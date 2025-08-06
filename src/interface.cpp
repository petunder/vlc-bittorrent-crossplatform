/*****************************************************************************
 * interface.cpp — BitTorrent status debug logger (fixed 1 Hz)
 *
 * Каждую секунду (±микросекунда) выводит в msg_Dbg
 * строку(и) вида:
 *   [BT] <hash> | D: <KiB/s> | U: <KiB/s> | Peers: <n> | Progress: <p>%
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>

#include "session.h"   // Session::get() и Alert_Listener

namespace lt = libtorrent;

/*-------------------------------------------------------
 * Преобразование info-hash → hex-строка (40 символов)
 *------------------------------------------------------*/
static std::string sha1_to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(40);
    for (auto byte : h) {
        out.push_back(hex[(byte >> 4) & 0xF]);
        out.push_back(hex[ byte       & 0xF]);
    }
    return out;
}

/*-------------------------------------------------------
 * Логгер статуса: ловит state_update_alert и
 * сохраняет строки в буфер для печати
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
        : m_intf(obj)
        , m_running(true)
        , m_thread(&TorrentStatusLogger::loop, this)
    {
        // Подписываемся на алерты libtorrent
        Session::get()->register_alert_listener(this);  :contentReference[oaicite:3]{index=3}
    }

    ~TorrentStatusLogger() override
    {
        // Останавливаем поток
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();

        Session::get()->unregister_alert_listener(this);
    }

    // Обновляем буфер при каждом state_update_alert
    void handle_alert(lt::alert* a) override
    {
        auto* up = lt::alert_cast<lt::state_update_alert>(a);
        if (!up) return;

        std::vector<std::string> tmp;
        tmp.reserve(up->status.size());
        for (auto const& st : up->status)
        {
#if LIBTORRENT_VERSION_NUM >= 20000
            const lt::sha1_hash& ih = st.info_hashes.v1;
#else
            const lt::sha1_hash& ih = st.info_hash;
#endif
            std::string hash = sha1_to_hex(ih);
            int dl = st.download_payload_rate / 1024;
            int ul = st.upload_payload_rate   / 1024;
            float prog = st.progress * 100.0f;

            tmp.emplace_back(
                "[BT] " + hash +
                " | D: " + std::to_string(dl) + " KiB/s" +
                " | U: " + std::to_string(ul) + " KiB/s" +
                " | Peers: " + std::to_string(st.num_peers) +
                " | Progress: " + (std::to_string(prog).substr(0, std::to_string(prog).find('.') + 3)) + "%"
            );
        }

        // Критическая секция: заменяем буфер разом
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lines.swap(tmp);
        }
    }

private:
    vlc_object_t*                m_intf;
    std::mutex                   m_mutex;
    std::vector<std::string>     m_lines;

    std::atomic<bool>            m_running;
    std::thread                  m_thread;

    // Печать ровно раз в секунду
    void loop()
    {
        using namespace std::chrono;
        while (m_running)
        {
            std::this_thread::sleep_for(seconds(1));

            std::vector<std::string> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snapshot = m_lines;
            }
            for (auto& line : snapshot)
            {
                msg_Dbg(m_intf, "%s", line.c_str());
            }
        }
    }
};

/*-------------------------------------------------------
 * Системная структура VLC-интерфейса
 *------------------------------------------------------*/
struct intf_sys_t
{
    TorrentStatusLogger* logger;
};

/*-------------------------------------------------------
 * Эти функции вызываются из module.cpp
 * (имена должны совпадать!)
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
