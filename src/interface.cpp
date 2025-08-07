/*****************************************************************************
 * interface.cpp — BitTorrent status dynamic overlay (1 Hz, C++ linkage)
 *
 * Каждую секунду:
 *  • собирает строки статуса из libtorrent;
 *  • пишет их в msg_Dbg;
 *  • дублирует в динамический оверлей VLC через spu/dynamicoverlay.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "vlc.h"
#include "interface.h"

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

#include "session.h"

namespace lt = libtorrent;

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
        var_SetString(m_intf, "sub-filter", "dynamicoverlay");

        Session::get()->register_alert_listener(this);
        msg_Dbg(m_intf, "Subscribed to libtorrent alerts.");
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
        msg_Dbg(m_intf, "Status logger destroyed.");
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
        ::unlink(m_fifo_path.c_str()); // Удаляем старый на всякий случай

        if (::mkfifo(m_fifo_path.c_str(), 0666) != 0 && errno != EEXIST) {
            msg_Err(m_intf, "mkfifo failed: %s", strerror(errno));
            m_fifo_fd = -1;
            return;
        }

        m_fifo_fd = ::open(m_fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
        if (m_fifo_fd < 0) {
            msg_Warn(m_intf, "open FIFO failed: %s. Will retry.", strerror(errno));
        } else {
            msg_Dbg(m_intf, "FIFO opened successfully.");
        }
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
            if (snap.empty()) continue;

            for (auto const& line : snap) {
                std::string cmd = "0 " + line + "\n";
                ssize_t written = ::write(m_fifo_fd, cmd.c_str(), cmd.size());
                if (written < 0) {
                    msg_Warn(m_intf, "FIFO write error: %s. Re-initializing.", strerror(errno));
                    prepare_fifo();
                    break;
                }
            }
        }
    }
};

/* VLC boilerplate */
struct intf_sys_t { TorrentStatusLogger* logger; };

int InterfaceOpen(vlc_object_t* obj) { /* ... */ }
void InterfaceClose(vlc_object_t* obj) { /* ... */ }
