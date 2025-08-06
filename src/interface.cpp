/*****************************************************************************
 * interface.cpp — BitTorrent status debug logger + Video-OSD (fixed 1 Hz)
 *
 * Каждую секунду:
 *  • обновляем буфер из libtorrent (handle_alert);
 *  • выводим его в msg_Dbg;
 *  • и через встроенный sub-filter “marq” дублируем текст поверх видео.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "vlc.h"                 // единый header, тянет vlc_common.h и др.
#include <vlc_vout_osd.h>        // vout_OSDMessage / DEFAULT_CHAN

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "session.h"             // Session::get(), Alert_Listener

namespace lt = libtorrent;

/* sha1_hash → 40-символьная hex-строка */
static std::string to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string s;  s.reserve(40);
    for (uint8_t b : h) { s += hex[b >> 4]; s += hex[b & 0xF]; }
    return s;
}

/*-------------------------------------------------------
 * Логгер: собираем строки в handle_alert,
 * loop() раз в секунду пишет в консоль и OSD
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* intf)
        : m_intf(intf), m_live(true), m_thread(&TorrentStatusLogger::loop,this)
    { Session::get()->register_alert_listener(this); }

    ~TorrentStatusLogger() override
    {
        m_live = false;
        if (m_thread.joinable()) m_thread.join();
        Session::get()->unregister_alert_listener(this);
    }

    /* ловим state_update_alert → готовим буфер строк */
    void handle_alert(lt::alert* a) override
    {
        auto* up = lt::alert_cast<lt::state_update_alert>(a);
        if (!up) return;

        std::vector<std::string> tmp;
        tmp.reserve(up->status.size());

        for (auto const& st : up->status)
        {
#if LIBTORRENT_VERSION_NUM >= 20000
            auto const& ih = st.info_hashes.v1;
#else
            auto const& ih = st.info_hash;
#endif
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                          to_hex(ih).c_str(),
                          st.download_payload_rate / 1024,
                          st.upload_payload_rate   / 1024,
                          st.num_peers,
                          st.progress * 100.0f);
            tmp.emplace_back(buf);
        }

        std::lock_guard<std::mutex> g(m_mu);
        m_lines.swap(tmp);
    }

private:
    vlc_object_t*            m_intf;
    std::mutex               m_mu;
    std::vector<std::string> m_lines;

    std::atomic<bool>        m_live;
    std::thread              m_thread;

    void loop()
    {
        using namespace std::chrono_literals;
        while (m_live)
        {
            std::this_thread::sleep_for(1s);

            std::vector<std::string> lines;
            {
                std::lock_guard<std::mutex> g(m_mu);
                lines = m_lines;
            }
            if (lines.empty()) continue;

            /* 1) debug-консоль */
            for (auto const& l : lines) msg_Dbg(m_intf, "%s", l.c_str());

            /* 2) On-Screen Display (канал DEFAULT_CHAN) */
            std::string text;
            for (auto const& l : lines) { text += l; text += '\n'; }
            vout_OSDMessage(m_intf, DEFAULT_CHAN, "%s", text.c_str());
        }
    }
};

/*-------------------------------------------------------
 * VLC boilerplate
 *------------------------------------------------------*/
struct intf_sys_t { TorrentStatusLogger* log; };

extern "C" {

int InterfaceOpen(vlc_object_t* o)
{
    auto* s = static_cast<intf_sys_t*>(malloc(sizeof(intf_sys_t)));
    if (!s) return VLC_ENOMEM;

    s->log = new (std::nothrow) TorrentStatusLogger(o);
    if (!s->log) { free(s); return VLC_ENOMEM; }

    reinterpret_cast<intf_thread_t*>(o)->p_sys = s;
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* o)
{
    auto* s = static_cast<intf_sys_t*>(reinterpret_cast<intf_thread_t*>(o)->p_sys);
    delete s->log;
    free(s);
}

} // extern "C"
