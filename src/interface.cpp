/*****************************************************************************
 * interface.cpp — BitTorrent status debug logger
 *
 * Выводит строку вида
 *   [BT] <hash> | D: <KiB/s> | U: <KiB/s> | Peers: <n> | Progress: <p>%
 * в отладочную консоль VLC (msg_Dbg).
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <array>
#include <cstdint>
#include <string>

#include "session.h"          // Session::get() и Alert_Listener

namespace lt = libtorrent;

/*-------------------------------------------------------
 * sha1_hash → hex-строка
 *------------------------------------------------------*/
static std::string sha1_to_hex(const lt::sha1_hash& h)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(40);
    for (std::uint8_t byte : h) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

/*-------------------------------------------------------
 * Слушатель алертов libtorrent
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* intf) : m_intf(intf)
    {
        Session::get()->register_alert_listener(this);
    }
    ~TorrentStatusLogger() override
    {
        Session::get()->unregister_alert_listener(this);
    }

    void handle_alert(lt::alert* a) override
    {
        if (auto* up = lt::alert_cast<lt::state_update_alert>(a)) {
            for (const lt::torrent_status& st : up->status) {
#if LIBTORRENT_VERSION_NUM >= 20000
                const lt::sha1_hash& ih = st.info_hashes.v1;
#else
                const lt::sha1_hash& ih = st.info_hash;
#endif
                std::string hash = sha1_to_hex(ih);
                int   dl = st.download_payload_rate / 1024;
                int   ul = st.upload_payload_rate   / 1024;
                float p  = st.progress * 100.0f;

                msg_Dbg(m_intf,
                        "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                        hash.c_str(), dl, ul, st.num_peers, p);
            }
        }
    }
private:
    vlc_object_t* m_intf;
};

/*-------------------------------------------------------
 * Системная структура VLC-интерфейса
 *------------------------------------------------------*/
struct intf_sys_t { TorrentStatusLogger* logger; };

/*-------------------------------------------------------
 * CALLBACKS, видимые из module.cpp
 *------------------------------------------------------*/
static int InterfaceOpen(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);

    auto* sys = static_cast<intf_sys_t*>(malloc(sizeof(intf_sys_t)));
    if (!sys) return VLC_ENOMEM;

    sys->logger = new (std::nothrow) TorrentStatusLogger(obj);
    if (!sys->logger) { free(sys); return VLC_ENOMEM; }

    intf->p_sys = sys;
    return VLC_SUCCESS;
}

static void InterfaceClose(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);
    auto* sys = static_cast<intf_sys_t*>(intf->p_sys);

    delete sys->logger;
    free(sys);
}

/*-------------------------------------------------------
 * Экспортируем имена для module.cpp
 *------------------------------------------------------*/
extern "C" {
    int  InterfaceOpen (vlc_object_t*) __attribute__((visibility("default")));
    void InterfaceClose(vlc_object_t*) __attribute__((visibility("default")));
}
