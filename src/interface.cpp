/*****************************************************************************
 * interface.cpp — BitTorrent status debug‐logger (interface only)
 *
 * Этот плагин не трогает плейлист или заголовки в VLC —
 * он только регистрируется на state_update_alert и каждый раз
 * при получении шлёт в msg_Dbg строку вида:
 *   [BT] <info-hash> | D: <KiB/s> | U: <KiB/s> | Peers: <n> | Progress: <%>
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

#include "session.h"   // Alert_Listener и Session API

namespace lt = libtorrent;

//------------------------------------------------------------
// Класс-слушатель алертов libtorrent
//------------------------------------------------------------
class TorrentStatusLogger : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
        : m_intf(obj)
    {
        // Регистрируемся в глобальной сессии
        Session::get()->register_alert_listener(this);
    }

    ~TorrentStatusLogger() override
    {
        Session::get()->unregister_alert_listener(this);
    }

    // Обработка всех алертов; нас интересует только state_update_alert
    void handle_alert(lt::alert* a) override
    {
        if (auto* su = lt::alert_cast<lt::state_update_alert>(a))
        {
            for (auto const& st : su->status)
            {
                // Инфо-хеш в hex
                std::string hash;
#if LIBTORRENT_VERSION_NUM >= 20000
                hash = lt::to_hex(st.info_hashes.v1);
#else
                hash = lt::to_hex(st.info_hash);
#endif
                // Скорости и прогресс
                int dl = st.download_payload_rate / 1024;
                int ul = st.upload_payload_rate   / 1024;
                double prog = st.progress * 100.0;

                // Вывод в debug-консоль VLC
                msg_Dbg(m_intf,
                        "[BT] %s | D: %d KiB/s | U: %d KiB/s | Peers: %d | Progress: %.1f%%",
                        hash.c_str(), dl, ul, st.num_peers, prog);
            }
        }
    }

private:
    vlc_object_t* m_intf;
};

//------------------------------------------------------------
// Определяем собственную структуру для p_sys
//------------------------------------------------------------
struct intf_sys_t
{
    TorrentStatusLogger* logger;
};

//------------------------------------------------------------
// Open — точка входа VLC для интерфейса
//------------------------------------------------------------
static int Open(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);

    // Аллоцируем нашу структуру
    intf_sys_t* sys = static_cast<intf_sys_t*>(
        malloc(sizeof(intf_sys_t))
    );
    if (!sys)
        return VLC_ENOMEM;

    // Создаём логгер
    sys->logger = new (std::nothrow) TorrentStatusLogger(obj);
    if (!sys->logger)
    {
        free(sys);
        return VLC_ENOMEM;
    }

    intf->p_sys = sys;
    return VLC_SUCCESS;
}

//------------------------------------------------------------
// Close — точка выхода VLC для интерфейса
//------------------------------------------------------------
static void Close(vlc_object_t* obj)
{
    intf_thread_t* intf = reinterpret_cast<intf_thread_t*>(obj);
    intf_sys_t* sys = reinterpret_cast<intf_sys_t*>(intf->p_sys);

    delete sys->logger;
    free(sys);
}

vlc_module_begin()
    set_shortname("BT-Logger")
    set_description("BitTorrent status debug logger")
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_capability("interface", 0)
    set_callbacks(Open, Close)
vlc_module_end()
