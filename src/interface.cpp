/*****************************************************************************
 * interface.cpp — BitTorrent debug-logger (interface only)
 *
 * Этот плагин не трогает плейлист и заголовки в VLC — он только
 * регистрируется на state_update_alert и пишет статус торрентов
 * в msg_Dbg (консоль отладки).
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/hex.hpp>

#include "session.h"   // Alert_Listener и Session API :contentReference[oaicite:5]{index=5}

namespace lt = libtorrent;

/*--------------------------------------------------------------
 * Статус-логгер: подписывается на алерты и выводит их в консоль
 *-------------------------------------------------------------*/
class TorrentStatusLogger : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
        : m_intf(obj)
    {
        // регистрируем себя на получение алертов
        Session::get()->register_alert_listener(this);  :contentReference[oaicite:6]{index=6}
    }

    ~TorrentStatusLogger() override
    {
        Session::get()->unregister_alert_listener(this);
    }

    // Приводим сигнатуру ровно под Alert_Listener
    void handle_alert(lt::alert* a) override
    {
        if (auto* su = lt::alert_cast<lt::state_update_alert>(a))
        {
            for (auto const& st : su->status)
            {
                // Переводим хеш в hex
                std::string hash = lt::aux::to_hex(
                    #if LIBTORRENT_VERSION_NUM >= 20000
                        st.info_hashes.v1.to_string()
                    #else
                        st.info_hash.to_string()
                    #endif
                );

                // Скорости в KiB/s и прогресс в %
                int dl = st.download_payload_rate / 1024;
                int ul = st.upload_payload_rate   / 1024;
                double prog = st.progress * 100.0;

                // Выводим в debug-консоль
                msg_Dbg(m_intf,
                        "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                        hash.c_str(), dl, ul, st.num_peers, prog);
            }
        }
    }

private:
    vlc_object_t* m_intf;
};

/*--------------------------------------------------------------
 * VLC-модули: открытие и закрытие плагина
 *-------------------------------------------------------------*/
static int Open(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);

    auto* logger = new (std::nothrow) TorrentStatusLogger(obj);
    if (!logger)
        return VLC_ENOMEM;

    intf->p_sys = logger;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);
    delete static_cast<TorrentStatusLogger*>(intf->p_sys);
}

vlc_module_begin()
    set_shortname("BT-Logger")
    set_description("BitTorrent status debug logger")
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_capability("interface", 0)
    set_callbacks(Open, Close)
vlc_module_end()
