/*****************************************************************************
 * overlay.cpp — Видеофильтр для отображения статуса BitTorrent
 * Copyright 2025 petunder
 *
 * --- РОЛЬ ФАЙЛА В ПРОЕКТЕ ---
 * Этот файл реализует модуль видеофильтра (`vfilter`). Его единственная
 * задача — отображать оверлей со статусом загрузки поверх видео.
 *
 * --- АРХИТЕКТУРНОЕ РЕШЕНИЕ ---
 * В отличие от предыдущей неверной реализации (interface), этот модуль
 * правильно интегрируется в видео-конвейер VLC.
 *
 * 1.  **Тип модуля:** Объявлен как `video_filter`, а не `interface`. Это
 *     позволяет ему работать одновременно со стандартным интерфейсом Qt.
 *
 * 2.  **Активация:** Фильтр активируется автоматически из `data.cpp` при
 *     открытии торрент-потока.
 *
 * 3.  **Логика:** При создании фильтра (`Open`) запускается класс
 *     `TorrentStatusLogger`, который в отдельном потоке получает данные от
 *     libtorrent и пишет их в FIFO-канал для оверлея.
 *
 * 4.  **Обработка видео:** Основная функция фильтра (`Filter`) просто
 *     пропускает видеокадры дальше без изменений. Его существование
 *     нужно лишь для того, чтобы удерживать `TorrentStatusLogger` в памяти.
 *
 * Этот подход является каноническим для создания оверлеев в VLC.
 *****************************************************************************/

// src/overlay.cpp

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "overlay.h" 
#include "session.h" // Для подписки на алерты

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/sha1_hash.hpp>

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace { // Используем анонимное пространство имен для сокрытия деталей реализации

/* sha1_hash → 40-символьная hex-строка */
static std::string sha1_to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(40);
    for (uint8_t b : h) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
      : m_intf(obj), m_running(true), m_fifo_fd(-1), m_thread(&TorrentStatusLogger::loop, this)
    {
        m_fifo_path = "/tmp/vlc-bittorrent-overlay.fifo";
        msg_Dbg(m_intf, "Status logger initialized. FIFO path: %s", m_fifo_path.c_str());

        prepare_fifo();
        var_SetString(m_intf, "dynamicoverlay-file", m_fifo_path.c_str());

        Session::get()->register_alert_listener(this);
    }

    ~TorrentStatusLogger() override
    {
        Session::get()->unregister_alert_listener(this);
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
        if (m_fifo_fd >= 0)
            ::close(m_fifo_fd);
        ::unlink(m_fifo_path.c_str());
    }

    void handle_alert(lt::alert* a) override
    {
        if (auto* up = lt::alert_cast<lt::state_update_alert>(a)) {
            std::vector<std::string> tmp;
            tmp.reserve(up->status.size());
            for (auto const& st : up->status) {
#if LIBTORRENT_VERSION_NUM >= 20000
                auto const& ih = st.info_hashes.v1;
#else
                auto const& ih = st.info_hash;
#endif
                char buf[128];
                int len = std::snprintf(buf, sizeof(buf),
                    "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Prg: %.1f%%",
                    sha1_to_hex(ih).c_str(),
                    st.download_payload_rate/1024,
                    st.upload_payload_rate/1024,
                    st.num_peers,
                    st.progress*100.0f);
                if (len > 0)
                    tmp.emplace_back(buf, static_cast<size_t>(len));
            }
            if (!tmp.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lines.swap(tmp);
            }
        }
    }

private:
    vlc_object_t*                m_intf;
    std::mutex                   m_mutex;
    std::vector<std::string>     m_lines;
    std::atomic<bool>            m_running;
    std::thread                  m_thread;
    std::string                  m_fifo_path;
    int                          m_fifo_fd;

    void prepare_fifo() {
        if (m_fifo_fd >= 0) ::close(m_fifo_fd);
        ::unlink(m_fifo_path.c_str());
        if (::mkfifo(m_fifo_path.c_str(), 0666) != 0 && errno != EEXIST) {
            msg_Err(m_intf, "mkfifo failed: %s", strerror(errno));
            m_fifo_fd = -1;
            return;
        }
        m_fifo_fd = ::open(m_fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
        if (m_fifo_fd < 0) msg_Warn(m_intf, "open FIFO failed: %s. Will retry.", strerror(errno));
    }

    void loop()
    {
        using namespace std::chrono_literals;
        while (m_running) {
            std::this_thread::sleep_for(1s);
            if (m_fifo_fd < 0) {
                prepare_fifo();
                if (m_fifo_fd < 0) continue;
            }
            std::vector<std::string> snap;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snap = m_lines;
            }
            for (auto const& line : snap) {
                std::string cmd = "0 " + line + "\n";
                ssize_t written = ::write(m_fifo_fd, cmd.c_str(), cmd.size());
                if (written < 0) {
                    prepare_fifo();
                    break;
                }
            }
        }
    }
};

} // конец анонимного пространства имен

struct filter_sys_t {
    TorrentStatusLogger* logger;
};

int Open(vlc_object_t* p_this)
{
    filter_t* p_filter = (filter_t*)p_this;
    auto* p_sys = new (std::nothrow) filter_sys_t();
    if (!p_sys) return VLC_ENOMEM;

    p_sys->logger = new (std::nothrow) TorrentStatusLogger(p_this);
    if (!p_sys->logger) {
        delete p_sys;
        return VLC_ENOMEM;
    }
    
    p_filter->p_sys = p_sys;
    p_filter->pf_video_filter = Filter;
    
    return VLC_SUCCESS;
}

void Close(vlc_object_t* p_this)
{
    filter_t* p_filter = (filter_t*)p_this;
    auto* p_sys = (filter_sys_t*)p_filter->p_sys;
    delete p_sys->logger;
    delete p_sys;
}

picture_t* Filter(filter_t* p_filter, picture_t* p_pic)
{
    // Мы не модифицируем видеопоток, поэтому просто возвращаем картинку как есть.
    // Эта функция нужна только для того, чтобы VLC считал нас валидным видеофильтром.
    (void)p_filter;
    return p_pic;
}
