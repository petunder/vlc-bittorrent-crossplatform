/*
 * src/download.cpp
 * Copyright 2016-2018 Johan Gunnarsson
 * Copyright 2025 petunder
 *
 * Этот файл является ядром логики BitTorrent-клиента в плагине. Он
 * инкапсулирует одну торрент-сессию (загрузку) в классе `Download`. Этот
 * модуль напрямую взаимодействует с библиотекой libtorrent.
 *
 * Ключевая функция этого модуля — `Download::read()`. В финальной архитектуре
 * эта функция является **БЛОКИРУЮЩЕЙ С ТАЙМ-АУТОМ**. Она спроектирована
 * для работы в связке с механизмом сетевого кеширования VLC, который
 * запрашивается в `data.cpp`.
 *
 * Логика работы следующая:
 *
 * 1.  **Начало воспроизведения:** `data.cpp` через `DataControl` просит VLC
 *     выделить время на кеширование. VLC запускает специальный поток
 *     буферизации и из него вызывает `DataRead`.
 *
 * 2.  **Блокировка:** `DataRead` вызывает `Download::read()`. Эта функция
 *     проверяет, есть ли нужный кусок торрента на диске. Если нет, она
 *     **блокирует поток**, ожидая, пока libtorrent скачает этот кусок.
 *
 * 3.  **Защита от зависания:** Блокировка не вечна. Она ограничена
 *     тайм-аутом `PIECE_READ_TIMEOUT` (например, 60 секунд). Если за это
 *     время кусок не скачался (торрент "мертвый" или очень медленный),
 *     функция бросает исключение `std::runtime_error`.
 *
 * 4.  **Обработка ошибки:** Исключение перехватывается в `data.cpp`, который
 *     возвращает VLC ошибку (-1). VLC корректно сообщает пользователю,
 *     что не может открыть источник.
 *
 * 5.  **Успешное чтение:** Если кусок скачался вовремя, `read()` читает из
 *     него данные и возвращает их. VLC заполняет свой кеш и начинает
 *     воспроизведение, как только буфер заполнен.
 *
 * Этот подход решает главную дилемму: он позволяет дождаться данных на
 * старте (удовлетворяя требование VLC), но делает это в безопасном потоке,
 * не замораживая интерфейс плеера.
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
#include <iterator>
#include <vector>

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

// Тайм-аут в секундах для ожидания одного куска.
// Если за это время ничего не скачалось, read() вернет ошибку.
#define PIECE_READ_TIMEOUT 60

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
            #if LIBTORRENT_VERSION_NUM >= 20000
                if (x->handle.info_hashes().v1 != m_ih) return;
            #else
                if (x->handle.info_hash() != m_ih) return;
            #endif
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
            #if LIBTORRENT_VERSION_NUM >= 20000
                if (x->handle.info_hashes().v1 != m_ih) return;
            #else
                if (x->handle.info_hash() != m_ih) return;
            #endif
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
            #if LIBTORRENT_VERSION_NUM >= 20000
                if (x->handle.info_hashes().v1 != m_ih) return;
            #else
                if (x->handle.info_hash() != m_ih) return;
            #endif
            set_exception(
                std::make_exception_ptr(std::runtime_error("metadata failed")));
        } else if (auto* x = lt::alert_cast<lt::metadata_failed_alert>(a)) {
            #if LIBTORRENT_VERSION_NUM >= 20000
                if (x->handle.info_hashes().v1 != m_ih) return;
            #else
                if (x->handle.info_hash() != m_ih) return;
            #endif
            set_exception(
                std::make_exception_ptr(std::runtime_error("metadata failed")));
        } else if (auto* x = lt::alert_cast<lt::metadata_received_alert>(a)) {
            #if LIBTORRENT_VERSION_NUM >= 20000
                if (x->handle.info_hashes().v1 != m_ih) return;
            #else
                if (x->handle.info_hash() != m_ih) return;
            #endif
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
            #if LIBTORRENT_VERSION_NUM >= 20000
                if (x->info_hashes.v1 != m_ih) return;
            #else
                if (x->info_hash != m_ih) return;
            #endif
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

    m_th = m_session->add_torrent(atp);
    if (!m_th.is_valid())
        throw std::runtime_error("Failed to add torrent");

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

    // ШАГ 1: Устанавливаем приоритеты, чтобы libtorrent начал качать нужные данные.
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

    // ШАГ 2: Проверяем, есть ли уже нужная нам часть.
    if (!m_th.have_piece(part.piece)) {
        // Если части нет, мы БЛОКИРУЕМСЯ и ждем ее скачивания.
        DownloadPiecePromise dlprom(m_th.info_hash(), part.piece);
        AlertSubscriber<DownloadPiecePromise> sub(m_session, &dlprom);
        auto f = dlprom.get_future();

        if (progress_cb) progress_cb(0.0);

        // Ждем не дольше, чем указано в тайм-ауте.
        auto status = f.wait_for(std::chrono::seconds(PIECE_READ_TIMEOUT));
        if (status == std::future_status::timeout) {
            // Если время вышло, бросаем исключение, которое поймает data.cpp.
            throw std::runtime_error("Timeout waiting for piece to download");
        }

        // Если скачалось, проверяем на другие ошибки.
        f.get();
        if (progress_cb) progress_cb(100.0);
    }
    
    // Если данных по-прежнему нет (например, из-за ошибки в libtorrent),
    // возвращаем 0, чтобы не зацикливаться.
    if (!m_th.have_piece(part.piece)) {
        return 0;
    }

    // ШАГ 3: Если мы дошли сюда, значит часть скачана. Выполняем чтение с диска.
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

    atp.flags &= ~lt::torrent_flags::auto_managed;
    atp.flags &= ~lt::torrent_flags::paused;

    lt::error_code ec;
    lt::parse_magnet_uri(url, atp, ec);
    if (ec) {
        lt::error_code ec2;
#if LIBTORRENT_VERSION_NUM < 10200
        atp.ti = boost::make_shared<lt::torrent_info>(url, boost::ref(ec2));
#else
        atp.ti = std::make_shared<lt::torrent_info>(url, std::ref(ec2));
#endif
        if (ec2) throw std::runtime_error("Failed to parse metadata from file or magnet");
    } else {
        if (atp.trackers.empty()) {
            atp.trackers = public_trackers;
        }

        std::string info_hash_str;
        #if LIBTORRENT_VERSION_NUM >= 20000
            info_hash_str = lt::aux::to_hex(atp.info_hashes.v1.to_string());
        #else
            info_hash_str = lt::aux::to_hex(atp.info_hash.to_string());
        #endif
        std::string path = cache_path + DIR_SEP + info_hash_str + ".torrent";

        lt::error_code ec_cache;
#if LIBTORRENT_VERSION_NUM < 10200
        atp.ti = boost::make_shared<lt::torrent_info>(path, boost::ref(ec_cache));
#else
        atp.ti = std::make_shared<lt::torrent_info>(path, std::ref(ec_cache));
#endif
        if (ec_cache) {
            atp.ti.reset();
            auto metadata = Download::get_download(atp, true)->get_metadata(cb);

            std::ofstream os(path, std::ios::binary | std::ios::trunc);
            os.write(metadata->data(), (std::streamsize)metadata->size());
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

std::shared_ptr<Download> Download::get_download(
    lt::add_torrent_params& atp, bool k)
{
    D(printf("%s:%d: %s (from atp)\n", __FILE__, __LINE__, __func__));
    
    lt::sha1_hash ih;
    if (atp.ti) {
        #if LIBTORRENT_VERSION_NUM >= 20000
            ih = atp.ti->info_hashes().v1;
        #else
            ih = atp.ti->info_hash();
        #endif
    } else {
        #if LIBTORRENT_VERSION_NUM >= 20000
            ih = atp.info_hashes.v1;
        #else
            ih = atp.info_hash;
        #endif
    }

    static std::mutex mtx;
    std::unique_lock<std::mutex> lock(mtx);

    static std::map<lt::sha1_hash, std::weak_ptr<Download>> dls;
    static std::map<lt::sha1_hash, std::mutex> dls_mtx;
    std::shared_ptr<Download> dl = dls[ih].lock();
    if (!dl)
        dls[ih] = dl = std::make_shared<Download>(dls_mtx[ih], atp, k);
    return dl;
}

std::shared_ptr<Download> Download::get_download(
    char* md, size_t mdsz, std::string sp, bool k)
{
    D(printf("%s:%d: %s (from buf)\n", __FILE__, __LINE__, __func__));

    lt::add_torrent_params atp;
    atp.save_path = sp;

    atp.flags &= ~lt::torrent_flags::auto_managed;
    atp.flags &= ~lt::torrent_flags::paused;
    atp.flags &= ~lt::torrent_flags::duplicate_is_error;

    lt::error_code ec;
#if LIBTORRENT_VERSION_NUM < 10200
    atp.ti = boost::make_shared<lt::torrent_info>(md, (int)mdsz, boost::ref(ec));
#else
    atp.ti = std::make_shared<lt::torrent_info>(md, (int)mdsz, std::ref(ec));
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
    
    #if LIBTORRENT_VERSION_NUM >= 20000
        return lt::aux::to_hex(m_th.info_hashes().v1.to_string());
    #else
        return lt::aux::to_hex(m_th.info_hash().to_string());
    #endif
}

lt::torrent_handle Download::get_handle()
{
    return m_th;
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
    f.get();
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
    f.get();
    if (cb) cb(100.0);
}

ssize_t Download::read(lt::peer_request part, char* buf, size_t buflen)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    download_metadata();

    ReadPiecePromise rdprom(m_th.info_hash(), part.piece);
    AlertSubscriber<ReadPiecePromise> sub(m_session, &rdprom);
    vlc_interrupt_guard<ReadPiecePromise> intrguard(rdprom);

    auto f = rdprom.get_future();
    m_th.read_piece(part.piece);

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

void Download::set_piece_priority(int file, int64_t off, int size, int priority)
{
    set_piece_priority(file, off, size, (libtorrent::download_priority_t)priority);
}
