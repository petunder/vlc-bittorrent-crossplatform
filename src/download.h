/*
 * src/download.h
 *
 * Определяет класс Download, который инкапсулирует логику одной
 * торрент-загрузки. Он отвечает за чтение данных по частям (pieces),
 * управление приоритетами загрузки для плавного воспроизведения и
 * получение метаданных из торрент-файлов или magnet-ссылок.
 */


#ifndef VLC_BITTORRENT_DOWNLOAD_H
#define VLC_BITTORRENT_DOWNLOAD_H

#include <atomic>
#include <forward_list>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#include <libtorrent/alert.hpp>
#include <libtorrent/peer_request.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#pragma GCC diagnostic pop

#include "session.h"

namespace lt = libtorrent;

using MetadataProgressCb = std::function<void(float)>;
using DataProgressCb = std::function<void(float)>;

/* компактный срез статуса для оверлея */
struct BtOverlayStatus {
    double     progress_pct;      // 0..100
    long long  download_kib_s;    // КиБ/с
    long long  upload_kib_s;      // КиБ/с
    int        peers;             // активные пиры
};

class Download {

public:
    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;
    Download(std::mutex& mtx, lt::add_torrent_params& atp, bool k);
    ~Download();

    static std::shared_ptr<Download>
    get_download(char* metadata, size_t metadatalen, std::string save_path, bool keep);

    ssize_t
    read(int file, int64_t off, char* buf, size_t buflen, DataProgressCb progress_cb);

    ssize_t
    read(int file, int64_t off, char* buf, size_t buflen)
    {
        return read(file, off, buf, buflen, nullptr);
    }

    static std::vector<std::pair<std::string, uint64_t>>
    get_files(char* metadata, size_t metadatalen);

    std::vector<std::pair<std::string, uint64_t>>
    get_files();

    static std::shared_ptr<std::vector<char>>
    get_metadata(std::string url, std::string save_path, std::string cache_path,
                 MetadataProgressCb progress_cb);

    static std::shared_ptr<std::vector<char>>
    get_metadata(std::string url, std::string save_path, std::string cache_path)
    {
        return get_metadata(url, save_path, cache_path, nullptr);
    }

    std::shared_ptr<std::vector<char>>
    get_metadata(MetadataProgressCb progress_cb);

    std::shared_ptr<std::vector<char>>
    get_metadata()
    {
        return get_metadata(nullptr);
    }

    std::pair<int, uint64_t>
    get_file(std::string path);

    std::string get_name();
    std::string get_infohash();
    lt::torrent_handle get_handle();

    // публичный метод статуса для оверлея
    bool query_status(BtOverlayStatus &out);

    // ФУНКЦИЯ ДЛЯ ПЕРЕМОТКИ
    void set_piece_priority(int file, int64_t off, int size, int priority);

private:
    static std::shared_ptr<Download>
    get_download(lt::add_torrent_params& atp, bool k);

    void download_metadata(MetadataProgressCb cb);

    void download_metadata()
    {
        download_metadata(nullptr);
    }

    void download(lt::peer_request part, DataProgressCb cb);

    void download(lt::peer_request part)
    {
        download(part, nullptr);
    }

    ssize_t read(lt::peer_request part, char* buf, size_t buflen);

    // Старая функция остаётся приватной
    void set_piece_priority(int file, int64_t off, int size, libtorrent::download_priority_t prio);

    // Locks mutex passed to constructor
    std::unique_lock<std::mutex> m_lock;

    bool m_keep;
    std::shared_ptr<Session> m_session;
    lt::torrent_handle m_th;
};

#endif
