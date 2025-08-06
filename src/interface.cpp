/*****************************************************************************
 * interface.cpp — BitTorrent status debug logger + Video-OSD (fixed 1 Hz)
 *
 * Каждую секунду:
 *  • обновляем буфер из libtorrent (handle_alert);
 *  • выводим его в msg_Dbg;
 *  • и через встроенный sub-filter “marq” дублируем текст поверх видео.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include "vlc.h"                  // unified VLC headers
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "session.h"              // Session::get(), Alert_Listener

namespace lt = libtorrent;

/* sha1_hash → 40-символьная hex-строка */
static std::string sha1_to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string s;  s.reserve(40);
    for (uint8_t b : h) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 0xF]); }
    return s;
}

/*-------------------------------------------------------
 * Логгер libtorrent → буфер; loop() печать + OSD
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* intf)
        : m_intf(intf), m_running(true), m_thread(&TorrentStatusLogger::loop, this)
    {
        Session::get()->register_alert_listener(this);
    }
    ~TorrentStatusLogger() override
    {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        Session::get()->unregister_alert_listener(this);
    }

    /* ловим state_update_alert, собираем строки */
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
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                          sha1_to_hex(ih).c_str(),
                          st.download_payload_rate / 1024,
                          st.upload_payload_rate   / 1024,
                          st.num_peers,
                          st.progress * 100.0f);
            tmp.emplace_back(buf);
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        m_lines.swap(tmp);
    }

private:
    vlc_object_t*            m_intf;
    std::mutex               m_mutex;
    std::vector<std::string> m_lines;

    std::atomic<bool>        m_running;
    std::thread              m_thread;
    bool                     m_marq_on = false;

    /* поток: ровно раз в секунду вывод + OSD */
    void loop()
    {
        using namespace std::chrono_literals;
        while (m_running)
        {
            std::this_thread::sleep_for(1s);

            std::vector<std::string> lines;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                lines = m_lines;
            }
            if (lines.empty()) continue;

            /* 1. лог в консоль */
            for (auto const& l : lines) msg_Dbg(m_intf, "%s", l.c_str());

            /* 2. overlay-OSD через фильтр “marq” */
            playlist_t* pl = pl_Get(m_intf);             /* <-- штатное API */
            if (!pl) continue;

            if (!m_marq_on)                              /* включаем один раз */
            {
                var_SetString (pl, "sub-filter",  "marq");
                var_SetInteger(pl, "marq-position", 3);  /* top-left */
                var_SetInteger(pl, "marq-opacity", 200);
                var_SetInteger(pl, "marq-size",    24);
                var_SetInteger(pl, "marq-timeout", 0);   /* навсегда */
                var_SetInteger(pl, "marq-refresh", 1000);
                m_marq_on = true;
            }

            std::string text;
            for (auto const& l : lines) { text += l; text.push_back('\n'); }
            var_SetString(pl, "marq-marquee", text.c_str());
        }
    }
};

/*-------------------------------------------------------
 * VLC boilerplate
 *------------------------------------------------------*/
struct intf_sys_t { TorrentStatusLogger* log; };

int InterfaceOpen(vlc_object_t* o)
{
    auto* sys = static_cast<intf_sys_t*>(malloc(sizeof(intf_sys_t)));
    if (!sys) return VLC_ENOMEM;
    sys->log = new (std::nothrow) TorrentStatusLogger(o);
    if (!sys->log) { free(sys); return VLC_ENOMEM; }

    reinterpret_cast<intf_thread_t*>(o)->p_sys = sys;
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* o)
{
    auto* sys = static_cast<intf_sys_t*>(reinterpret_cast<intf_thread_t*>(o)->p_sys);
    delete sys->log;
    free(sys);
}
