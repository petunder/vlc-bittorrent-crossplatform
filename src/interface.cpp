/*****************************************************************************
 * interface.cpp — BitTorrent status dynamic overlay (1 Hz)
 *
 * Каждую секунду:
 *  • собирает строки статуса из libtorrent;
 *  • пишет их в msg_Dbg;
 *  • и дублирует в динамический оверлей VLC через spu/dynamicoverlay.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "vlc.h"                 // unified VLC headers
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

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

#include "session.h"             // Session::get() и Alert_Listener

namespace lt = libtorrent;

/*-------------------------------------------------------
 * Преобразование sha1_hash → 40-символьная hex-строка
 *------------------------------------------------------*/
static std::string sha1_to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (auto byte : h) {
        out.push_back(hex[(byte >> 4) & 0xF]);
        out.push_back(hex[ byte        & 0xF]);
    }
    return out;
}

/*-------------------------------------------------------
 * TorrentStatusLogger:
 *  • ловит state_update_alert (handle_alert)
 *  • фоновой поток loop() каждую секунду пишет snapshot
 *    в FIFO для spu/dynamicoverlay (переменная dynamicoverlay-file)
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
        : m_intf(obj)
        , m_running(true)
        , m_thread(&TorrentStatusLogger::loop, this)
        , m_fifo_fd(-1)
    {
        // 1) Подписка на алерты libtorrent
        Session::get()->register_alert_listener(this);

        // 2) Находим playlist и настраиваем sub-filter=dynamicoverlay
        playlist_t* pl = pl_Get(reinterpret_cast<intf_thread_t*>(m_intf));
        var_SetString(pl, "sub-filter", "dynamicoverlay");

        // 3) Получаем путь к FIFO из переменной dynamicoverlay-file
        char* fifo_var = var_GetString(pl, "dynamicoverlay-file");
        m_fifo_path = fifo_var
            ? std::string(fifo_var)
            : std::string("/tmp/vlc-bittorrent-overlay.fifo");
        free(fifo_var);

        // 4) Создаём FIFO (если нужно) и открываем его на запись, non-blocking
        if (::mkfifo(m_fifo_path.c_str(), 0666) && errno != EEXIST) {
            msg_Err(m_intf, "Cannot mkfifo %s: %s",
                    m_fifo_path.c_str(), std::strerror(errno));
        }
        m_fifo_fd = ::open(m_fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
        if (m_fifo_fd < 0) {
            msg_Warn(m_intf, "Cannot open overlay FIFO %s: %s",
                     m_fifo_path.c_str(), std::strerror(errno));
        }
    }

    ~TorrentStatusLogger() override
    {
        // Отписываемся и останавливаем поток
        Session::get()->unregister_alert_listener(this);
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
        if (m_fifo_fd >= 0)
            ::close(m_fifo_fd);
    }

    // libtorrent thread
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
                    "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                    sha1_to_hex(ih).c_str(),
                    st.download_payload_rate / 1024,
                    st.upload_payload_rate   / 1024,
                    st.num_peers,
                    st.progress * 100.0f);
                if (len > 0)
                    tmp.emplace_back(buf, static_cast<size_t>(len));
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lines.swap(tmp);
        }
    }

private:
    vlc_object_t*            m_intf;
    std::mutex               m_mutex;
    std::vector<std::string> m_lines;
    std::atomic<bool>        m_running;
    std::thread              m_thread;
    std::string              m_fifo_path;
    int                      m_fifo_fd;

    void loop()
    {
        using namespace std::chrono_literals;
        while (m_running) {
            std::this_thread::sleep_for(1s);

            std::vector<std::string> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snapshot = m_lines;
            }
            if (m_fifo_fd < 0)
                continue;

            // Пишем каждую строку в FIFO вида "0 <text>\n"
            for (auto const& line : snapshot) {
                std::string cmd = "0 " + line + "\n";
                (void)::write(m_fifo_fd, cmd.c_str(), cmd.size());
            }
        }
    }
};

/*-------------------------------------------------------
 * VLC boilerplate: InterfaceOpen / InterfaceClose
 *------------------------------------------------------*/
struct intf_sys_t { TorrentStatusLogger* logger; };

extern "C" {

int InterfaceOpen(vlc_object_t* obj)
{
    auto* sys = static_cast<intf_sys_t*>(malloc(sizeof(intf_sys_t)));
    if (!sys) return VLC_ENOMEM;
    sys->logger = new (std::nothrow) TorrentStatusLogger(obj);
    if (!sys->logger) { free(sys); return VLC_ENOMEM; }
    reinterpret_cast<intf_thread_t*>(obj)->p_sys = sys;
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* obj)
{
    auto* sys = static_cast<intf_sys_t*>(
        reinterpret_cast<intf_thread_t*>(obj)->p_sys);
    delete sys->logger;
    free(sys);
}

} // extern "C"
