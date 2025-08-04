/*
 * src/download.cpp
 * Copyright 2016-2018 Johan Gunnarsson
 * Copyright 2025 petunder
 *
 * Этот файл является ядром логики BitTorrent-клиента в плагине.
 * Его основные задачи:
 *
 * 1.  **Управление загрузками (Download):**
 *     - Создание и управление объектами `Download`, представляющими одну торрент-сессию.
 *     - Реализация синглтона для объектов `Download` для предотвращения дублирования загрузок одного и того же торрента.
 *
 * 2.  **Обработка метаданных:**
 *     - Получение метаданных как из `.torrent` файлов, так и по magnet-ссылкам.
 *     - Реализация механизма кеширования метаданных для magnet-ссылок.
 *     - **Использование резервного списка публичных трекеров, если в magnet-ссылке они отсутствуют.**
 *
 * 3.  **Чтение данных:**
 *     - Реализация функции `read()`, которая позволяет VLC-плагину запрашивать фрагменты (piece) файла.
 *     - Управление приоритетами загрузки частей файла для обеспечения плавного воспроизведения.
 *
 * 4.  **Взаимодействие с libtorrent:**
 *     - Добавление торрентов в сессию `libtorrent::session`.
 *     - Использование `Alert_Listener` для асинхронной обработки событий от libtorrent (например, окончание загрузки куска, получение метаданных).
 *
 * Этот модуль абстрагирует сложность работы с libtorrent, предоставляя простое API
 * для верхнеуровневых модулей плагина (data.cpp, metadata.cpp, magnetmetadata.cpp).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <chrono>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <iterator> // для std::ostream_iterator
#include <vector>   // для std::vector

#include "download.h"
#include "session.h"
#include "vlc.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/peer_request.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/sha1_hash.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/torrent_flags.hpp>
#pragma GCC diagnostic pop

#define D(x)
#define DD(x)

#define kB (1024)
#define MB (1024 * kB)

#define PRIO_HIGHEST 7
#define PRIO_HIGHER 6
#define PRIO_HIGH 5

namespace lt = libtorrent;

template <typename T> class vlc_interrupt_guard {
public:
    vlc_interrupt_guard(T& pr)
    {
        vlc_interrupt_register(abort, &pr);
    }

    ~vlc_interrupt_guard()
    {
        vlc_interrupt_unregister();
    }

private:
    static void abort(void* data)
    {
        try {
            static_cast<T*>(data)->set_exception(
                std::make_exception_ptr(std::runtime_error("vlc interrupted")));
        } catch (...) {}
    }
};

template <typename T> class AlertSubscriber {
public:
    AlertSubscriber(std::shared_ptr<Session> dl, T* pr)
        : m_session(dl)
        , m_promise(pr)
    {
        m_session->register_alert_listener(m_promise);
    }

    ~AlertSubscriber()
    {
        m_session->unregister_alert_listener(m_promise);
    }

private:
    std::shared_ptr<Session> m_session;
    T* m_promise;
};

using ReadValue = std::pair<boost::shared_array<char>, int>;

class ReadPiecePromise : public std::promise<ReadValue>, public Alert_Listener {
public:
    ReadPiecePromise(lt::sha1_hash ih, int p)
        : m_ih(ih)
        , m_piece(p)
    {
    }

    void handle_alert(lt::alert* a) override
    {
        if (auto* x = lt::alert_cast<lt::read_piece_alert>(a)) {
            if (x->handle.info_hash() != m_ih) return;
            if (x->piece != m_piece) return;

            if (x->error) {
                set_exception(
                    std::make_exception_ptr(std::runtime_error("read failed")));
            } else {
                set_value(std::make_pair(x->buffer, x->size));
            }
        }
    }

private:
    lt::sha1_hash m_ih;
    int m_piece;
};

class DownloadPiecePromise : public std::promise<void>, public Alert_Listener {
public:
    DownloadPiecePromise(lt::sha1_hash ih, int p)
        : m_ih(ih)
        , m_piece(p)
    {
    }

    void handle_alert(lt::alert* a) override
    {
        if (auto* x = lt::alert_cast<lt::piece_finished_alert>(a)) {
            if (x->handle.info_hash() != m_ih) return;
            if (x->piece_index != m_piece) return;
            set_value();
        }
    }

private:
    lt::sha1_hash m_ih;
    int m_piece;
};

class MetadataDownloadPromise : public std::promise<void>, public Alert_Listener {
public:
    MetadataDownloadPromise(lt::sha1_hash ih)
        : m_ih(ih)
    {
    }

    void handle_alert(lt::alert* a) override
    {
        if (auto* x = lt::alert_cast<lt::torrent_error_alert>(a)) {
            if (x->handle.info_hash() != m_ih) return;
            set_exception(
                std::make_exception_ptr(std::runtime_error("metadata failed")));
        } else if (auto* x = lt::alert_cast<lt::metadata_failed_alert>(a)) {
            if (x->handle.info_hash() != m_ih) return;
            set_exception(
                std::make_exception_ptr(std::runtime_error("metadata failed")));
        } else if (auto* x = lt::alert_cast<lt::metadata_received_alert>(a)) {
            if (x->handle.info_hash() != m_ih) return;
            set_value();
        }
    }

private:
    lt::sha1_hash m_ih;
};

class RemovePromise : public std::promise<void>, public Alert_Listener {
public:
    RemovePromise(lt::sha1_hash ih)
        : m_ih(ih)
    {
    }

    void handle_alert(lt::alert* a) override
    {
        if (auto* x = lt::alert_cast<lt::torrent_removed_alert>(a)) {
            if (x->info_hashes.v1 != m_ih) return;
            set_value();
        }
    }

private:
    lt::sha1_hash m_ih;
};

Download::Download(std::mutex& mtx, lt::add_torrent_params& atp, bool k)
    : m_lock(mtx)
    , m_keep(k)
    , m_session(Session::get())
{
    D(printf("%s:%d: %s (from atp)\n", __FILE__, __LINE__, __func__));
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код (неудачная попытка асинхронности).
    // m_session->async_add_torrent(atp); // ОШИБКА: метод не существует в обертке Session
    // Новый код: Возвращаемся к оригинальному, синхронному вызову,
    // так как остальная логика зависит от немедленного получения torrent_handle.
    m_th = m_session->add_torrent(atp);
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
    
    if (!m_th.is_valid())
        throw std::runtime_error("Failed to add torrent");
    
    // Используем replace_trackers - это правильное и современное решение.
    if (m_th.is_valid() && !atp.trackers.empty()) {
        std::vector<lt::announce_entry> announce_entries;
        for (const auto& url : atp.trackers) {
            announce_entries.emplace_back(url);
        }
        m_th.replace_trackers(announce_entries);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

Download::~Download()
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));

    if (m_th.is_valid()) {
        RemovePromise rmprom(m_th.info_hash());
        AlertSubscriber<RemovePromise> sub(m_session, &rmprom);
        auto f = rmprom.get_future();
        m_session->remove_torrent(m_th, m_keep);
        f.wait_for(std::chrono::seconds(5));
    }
}

ssize_t Download::read(int file, int64_t fileoff, char* buf, size_t buflen,
    DataProgressCb progress_cb)
{
    D(printf("%s:%d: %s(%d, %lu, %p, %lu)\n", __FILE__, __LINE__, __func__,
        file, fileoff, buf, buflen));

    download_metadata();

    auto ti = m_th.torrent_file();
    auto fs = ti->files();
    if (file < 0 || file >= fs.num_files())
        throw std::runtime_error("File not found");
    if (fileoff < 0)
        throw std::runtime_error("File offset negative");
    int64_t filesz = fs.file_size(file);
    if (fileoff >= filesz) return 0;

    auto part = ti->map_file(file, fileoff,
        (int)std::min({ (int64_t)std::numeric_limits<int>::max(),
                        (int64_t)buflen, filesz - fileoff }));
    if (part.length <= 0) return 0;

    set_piece_priority(file, fileoff, part.length, PRIO_HIGHEST);

    int64_t p01 = std::max(
        std::min((int64_t)std::numeric_limits<int>::max(), filesz / 1000),
        (int64_t)128 * kB);
    set_piece_priority(file, 0, (int)p01, PRIO_HIGHER);
    set_piece_priority(file, filesz - p01, (int)p01, PRIO_HIGHER);

    int64_t p5 = std::max(
        std::min((int64_t)std::numeric_limits<int>::max(), 5 * filesz / 100),
        (int64_t)32 * MB);
    set_piece_priority(file, fileoff, (int)p5, PRIO_HIGH);

    if (!m_th.have_piece(part.piece))
        download(part, progress_cb);

    return read(part, buf, buflen);
}

void Download::set_piece_priority(int file, int64_t off, int size,
    libtorrent::download_priority_t prio)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();

    auto ti = m_th.torrent_file();
    auto fs = ti->files();
    int64_t filesz = fs.file_size(file);
    off = std::min(off, filesz);
    size = (int)std::min({ (int64_t)std::numeric_limits<int>::max(),
                           (int64_t)size, filesz - off });

    auto part = ti->map_file(file, off, size);
    for (; part.length > 0; part.length -= ti->piece_size(part.piece++)) {
        if (!m_th.have_piece(part.piece) &&
            m_th.piece_priority(part.piece) < prio)
            m_th.piece_priority(part.piece, prio);
    }
}

std::vector<std::pair<std::string, uint64_t>> Download::get_files()
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();

    std::vector<std::pair<std::string, uint64_t>> files;
    const lt::file_storage& fs = m_th.torrent_file()->files();
    for (int i = 0; i < fs.num_files(); i++)
        files.emplace_back(fs.file_path(i), fs.file_size(i));
    return files;
}

// static
std::vector<std::pair<std::string, uint64_t>>
Download::get_files(char* metadata, size_t metadatasz)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));

    lt::error_code ec;
    lt::torrent_info ti(metadata, (int)metadatasz, ec);
    if (ec) throw std::runtime_error("Failed to parse metadata");

    std::vector<std::pair<std::string, uint64_t>> files;
    const lt::file_storage& fs = ti.files();
    for (int i = 0; i < fs.num_files(); i++)
        files.emplace_back(fs.file_path(i), fs.file_size(i));
    return files;
}

// static
std::shared_ptr<std::vector<char>> Download::get_metadata(
    std::string url, std::string save_path,
    std::string cache_path, MetadataProgressCb cb)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));

    static const std::vector<std::string> public_trackers = {
        "udp://tracker.openbittorrent.com:6969/announce",
        "udp://tracker.opentrackr.org:1337/announce",
        "udp://open.demonii.com:1337/announce",
        "udp://tracker.coppersurfer.tk:6969/announce",
        "udp://tracker.leechers-paradise.org:6969/announce",
        "udp://exodus.desync.com:6969/announce",
        "udp://tracker.torrent.eu.org:451/announce",
        "udp://tracker.moeking.me:6969/announce",
        "udp://valakas.rollo.dnsabr.com:2710/announce",
        "udp://p4p.arenabg.com:1337/announce"
    };

    lt::add_torrent_params atp;
    atp.save_path = save_path;

    using namespace lt::torrent_flags;
    atp.flags &= ~auto_managed;
    atp.flags &= ~paused;

    lt::error_code ec;
    lt::parse_magnet_uri(url, atp, ec);
    if (ec) {
        // Not a magnet link, assume it's a path to a .torrent file or URL
        lt::error_code ec2;
#if LIBTORRENT_VERSION_NUM < 10200
        atp.ti = boost::make_shared<lt::torrent_info>(url, boost::ref(ec2));
#else
        atp.ti = std::make_shared<lt::torrent_info>(url, std::ref(ec2));
#endif
        if (ec2) throw std::runtime_error("Failed to parse metadata from file or magnet");
    } else {
        // It's a magnet-link. If no trackers are present, add the public ones.
        if (atp.trackers.empty()) {
            atp.trackers = public_trackers;
        }

        // Try to find the .torrent file in cache.
        std::string path = cache_path + DIR_SEP
            + atp.info_hashes.v1.to_string() + ".torrent";

        lt::error_code ec_cache;
#if LIBTORRENT_VERSION_NUM < 10200
        atp.ti = boost::make_shared<lt::torrent_info>(path, boost::ref(ec_cache));
#else
        atp.ti = std::make_shared<lt::torrent_info>(path, std::ref(ec_cache));
#endif
        if (ec_cache) {
            // Not in cache, we need to download it.
            atp.ti = nullptr;
            auto metadata = Download::get_download(atp, true)->get_metadata(cb);

            // Save the downloaded metadata to cache for next time
            std::ofstream os(path, std::ios::binary | std::ios::trunc);
            os.write(metadata->data(), metadata->size());
            os.close();

            return metadata;
        }
    }

    if (atp.ti && !atp.trackers.empty()) {
        for (auto const& tracker : atp.trackers) {
            atp.ti->add_tracker(tracker);
        }
    }

    auto entry = lt::create_torrent(*atp.ti).generate();
    auto metadata = std::make_shared<std::vector<char>>();
    lt::bencode(std::back_inserter(*metadata), entry);
    return metadata;
}

// static
std::shared_ptr<Download> Download::get_download(
    lt::add_torrent_params& atp, bool k)
{
    D(printf("%s:%d: %s (from atp)\n", __FILE__, __LINE__, __func__));

    lt::sha1_hash ih = atp.ti ? atp.ti->info_hash()
                              : atp.info_hashes.v1;

    static std::mutex mtx;
    std::unique_lock<std::mutex> lock(mtx);

    static std::map<lt::sha1_hash, std::weak_ptr<Download>> dls;
    static std::map<lt::sha1_hash, std::mutex> dls_mtx;
    std::shared_ptr<Download> dl = dls[ih].lock();
    if (!dl)
        dls[ih] = dl = std::make_shared<Download>(dls_mtx[ih], atp, k);
    return dl;
}

// static
std::shared_ptr<Download> Download::get_download(
    char* md, size_t mdsz, std::string sp, bool k)
{
    D(printf("%s:%d: %s (from buf)\n", __FILE__, __LINE__, __func__));

    lt::add_torrent_params atp;
    atp.save_path = sp;

    using namespace lt::torrent_flags;
    atp.flags &= ~auto_managed;
    atp.flags &= ~paused;
    atp.flags &= ~duplicate_is_error;

    lt::error_code ec;
#if LIBTORRENT_VERSION_NUM < 10200
    atp.ti = boost::make_shared<lt::torrent_info>(md, mdsz, boost::ref(ec));
#else
    atp.ti = std::make_shared<lt::torrent_info>(md, mdsz, std::ref(ec));
#endif
    if (ec) throw std::runtime_error("Failed to parse metadata");
    return Download::get_download(atp, k);
}

std::pair<int, uint64_t> Download::get_file(std::string path)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();

    const lt::file_storage& fs = m_th.torrent_file()->files();
    for (int i = 0; i < fs.num_files(); i++) {
        if (fs.file_path(i) == path)
            return std::make_pair(i, (uint64_t)fs.file_size(i));
    }
    throw std::runtime_error("Failed to find file");
}

std::string Download::get_name()
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();
    return m_th.torrent_file()->name();
}

std::string Download::get_infohash()
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();
    
    return m_th.torrent_file()->info_hash().to_string();
}

std::shared_ptr<std::vector<char>> Download::get_metadata(
    MetadataProgressCb cb)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata(cb);

    auto entry = lt::create_torrent(*m_th.torrent_file()).generate();
    auto buffer = std::make_shared<std::vector<char>>();
    lt::bencode(std::back_inserter(*buffer), entry);
    return buffer;
}

void Download::download_metadata(MetadataProgressCb cb)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));

    if (m_th.status().has_metadata)
        return;

    MetadataDownloadPromise dlprom(m_th.info_hash());
    AlertSubscriber<MetadataDownloadPromise> sub(m_session, &dlprom);
    vlc_interrupt_guard<MetadataDownloadPromise> intrguard(dlprom);

    auto f = dlprom.get_future();
    if (cb) cb(0.0);

    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: блокирующий цикл ожидания.
    /*
    while (!m_th.status().has_metadata) {
        auto r = f.wait_for(std::chrono::seconds(1));
        if (r == std::future_status::ready) {
            f.get();
            break;
        }
    }
    */
    // Новый код: используем `f.get()` который будет ждать, пока promise не будет выполнен.
    // Это более чистый способ дождаться завершения асинхронной операции.
    f.get();
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---

    if (cb) cb(100.0);
}

void Download::download(lt::peer_request part, DataProgressCb cb)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();

    if (m_th.have_piece(part.piece))
        return;

    DownloadPiecePromise dlprom(m_th.info_hash(), part.piece);
    AlertSubscriber<DownloadPiecePromise> sub(m_session, &dlprom);
    vlc_interrupt_guard<DownloadPiecePromise> intrguard(dlprom);

    auto f = dlprom.get_future();
    if (cb) cb(0.0);
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: блокирующий цикл ожидания.
    /*
    while (!m_th.have_piece(part.piece)) {
        auto r = f.wait_for(std::chrono::seconds(1));
        if (r == std::future_status::ready) {
            f.get();
            break;
        }
    }
    */
    // Новый код: используем `f.get()` для ожидания завершения.
    f.get();
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---

    if (cb) cb(100.0);
}

ssize_t Download::read(lt::peer_request part, char* buf, size_t buflen)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: использование `torrent_handle::read_piece`, который является блокирующим.
    /*
    ReadPiecePromise rdprom(m_th.info_hash(), part.piece);
    AlertSubscriber<ReadPiecePromise> sub(m_session, &rdprom);
    vlc_interrupt_guard<ReadPiecePromise> intrguard(rdprom);

    auto f = rdprom.get_future();
    m_th.read_piece(part.piece);
    */
    // Новый код: используем `async_read_piece` для асинхронного чтения, чтобы не блокировать поток.
    // Однако, для простоты и сохранения остальной структуры, оставляем блокирующий вызов,
    // но отмечаем, что это место для улучшения.
    ReadPiecePromise rdprom(m_th.info_hash(), part.piece);
    AlertSubscriber<ReadPiecePromise> sub(m_session, &rdprom);
    vlc_interrupt_guard<ReadPiecePromise> intrguard(rdprom);
    
    m_th.read_piece(part.piece);
    auto f = rdprom.get_future();
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---

    boost::shared_array<char> piece_buffer;
    int piece_size;
    std::tie(piece_buffer, piece_size) = f.get();

    int len = std::min({ (int)(piece_size - part.start),
                         (int)buflen,
                         part.length });
    if (len < 0) return -1;

    memcpy(buf, piece_buffer.get() + part.start, (size_t)len);
    return (ssize_t)len;
}
