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

#include "vlc.h"               // unified includes: vlc_common.h, vlc_plugin.h, vlc_interface.h, vlc_playlist.h, vlc_variables.h…
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <cstdio>

#include "session.h"           // Session::get() и Alert_Listener

namespace lt = libtorrent;

/*-------------------------------------------------------
 * sha1_hash → 40-символьная hex-строка
 *------------------------------------------------------*/
static std::string sha1_to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(40);
    for (auto byte : h) {
        out.push_back(hex[(byte >> 4) & 0xF]);
        out.push_back(hex[ byte        & 0xF]);
    }
    return out;
}

/*-------------------------------------------------------
 * Логгер: ловим state_update_alert (handle_alert) и
 * сохраняем строки. loop() каждую секунду печатает и
 * обновляет marq-OSD.
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
        : m_intf(obj)
        , m_running(true)
        , m_thread(&TorrentStatusLogger::loop, this)
        , m_marq_enabled(false)
    {
        Session::get()->register_alert_listener(this);
    }

    ~TorrentStatusLogger() override
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
        Session::get()->unregister_alert_listener(this);
    }

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
            std::string hash = sha1_to_hex(ih);
            int dl = st.download_payload_rate / 1024;
            int ul = st.upload_payload_rate   / 1024;
            float prog = st.progress * 100.0f;

            char buf[128];
            int len = std::snprintf(buf, sizeof(buf),
                "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                hash.c_str(), dl, ul, st.num_peers, prog);
            tmp.emplace_back(buf, (len > 0 ? (size_t)len : 0));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.swap(tmp);
    }

private:
    vlc_object_t*                m_intf;
    std::mutex                   m_mutex;
    std::vector<std::string>     m_lines;

    std::atomic<bool>            m_running;
    std::thread                  m_thread;
    bool                         m_marq_enabled;

    void loop()
    {
        using namespace std::chrono_literals;
        while (m_running)
        {
            std::this_thread::sleep_for(1s);

            // Копируем текущий буфер
            std::vector<std::string> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snapshot = m_lines;
            }
            if (snapshot.empty())
                continue;

            // 1) Debug-вывод
            for (auto const& line : snapshot)
                msg_Dbg(m_intf, "%s", line.c_str());

            // 2) Marquee-OSD: получаем main-playlist
            playlist_t *playlist =
                vlc_intf_GetMainPlaylist(
                    reinterpret_cast<intf_thread_t*>(m_intf)
                );  // :contentReference[oaicite:2]{index=2}
            if (!playlist)
                continue;

            // Раз включаем фильтр “marq” один раз
            if (!m_marq_enabled)
            {
                var_SetString(playlist, "sub-filter",  "marq");
                var_SetInteger(playlist, "marq-position", 3);
                var_SetInteger(playlist, "marq-opacity", 200);
                var_SetInteger(playlist, "marq-size",    24);
                var_SetInteger(playlist, "marq-timeout", 0);
                var_SetInteger(playlist, "marq-refresh", 1000);
                m_marq_enabled = true;
            }

            // Собираем в одну строку и обновляем текст
            std::string text;
            for (auto const& line : snapshot)
            {
                text += line;
                text += '\n';
            }
            var_SetString(playlist, "marq-marquee", text.c_str()); // :contentReference[oaicite:3]{index=3}
        }
    }
};

/*-------------------------------------------------------
 * Системная структура VLC-интерфейса
 *------------------------------------------------------*/
struct intf_sys_t { TorrentStatusLogger* logger; };

/*-------------------------------------------------------
 * Открытие/закрытие интерфейса (из module.cpp)
 *------------------------------------------------------*/
int InterfaceOpen(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);
    auto* sys = static_cast<intf_sys_t*>(malloc(sizeof(intf_sys_t)));
    if (!sys) return VLC_ENOMEM;

    sys->logger = new (std::nothrow) TorrentStatusLogger(obj);
    if (!sys->logger) { free(sys); return VLC_ENOMEM; }

    intf->p_sys = sys;
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);
    auto* sys = static_cast<intf_sys_t*>(intf->p_sys);

    delete sys->logger;
    free(sys);
}
