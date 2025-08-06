/*****************************************************************************
 * interface.cpp — **BitTorrent status logger**
 *
 * Мини-интерфейс VLC-плагина.  Модуль лишь раз в секунду запрашивает у
 * libtorrent актуальное состояние всех торрентов и пишет человекочитаемую
 * строку в debug-консоль (`msg_Dbg`).  
 *
 * Собирается как обычный VLC-interface-плагин, но не меняет плейлист,
 * заголовки треков и т. д. — только логирует.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include "session.h"   /* singleton-обёртка над lt::session */

/*==============================================================
 * Утилита: красивый вывод скорости (в KiB/s или MiB/s)
 *============================================================*/
static std::string human_rate(int bytes_per_sec)
{
    constexpr double kibi = 1024.0;
    constexpr double mebi = 1024.0 * 1024.0;

    std::ostringstream os;
    os.setf(std::ios::fixed, std::ios::floatfield);
    os.precision(1);

    if (bytes_per_sec >= mebi)
        os << bytes_per_sec / mebi << " MiB/s";
    else
        os << bytes_per_sec / kibi << " KiB/s";

    return os.str();
}

/*==============================================================
 * StatusLogger — слушает алерты и пишет статус в лог
 *============================================================*/
class StatusLogger final : public Alert_Listener
{
public:
    explicit StatusLogger(intf_thread_t* intf)
        : m_intf(intf), m_running(true),
          m_thread([this] { this->loop(); })
    {
        Session::get().register_alert_listener(this);
    }

    ~StatusLogger() override
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
        Session::get().unregister_alert_listener(this);
    }

    /* Получаем алерты от Session */
    void handle_alert(lt::alert const* a) override
    {
        using namespace lt;
        if (auto const* sa = alert_cast<state_update_alert>(a))
            for (torrent_status const& st : sa->status)
                print_status(st);
    }

private:
    /* Основной цикл: раз в секунду просим libtorrent прислать апдейт */
    void loop()
    {
        using namespace std::chrono_literals;
        while (m_running)
        {
            Session::get().post_torrent_updates();
            std::this_thread::sleep_for(1s);
        }
    }

    /* Формирование и вывод строки */
    void print_status(lt::torrent_status const& st) const
    {
        double progress = st.progress_ppm / 10000.0;   // 0-100 %

        std::ostringstream os;
        os.setf(std::ios::fixed, std::ios::floatfield);
        os.precision(1);

        os << "[ D: "   << human_rate(st.download_payload_rate)
           << " | U: "  << human_rate(st.upload_payload_rate)
           << " | Peers: " << st.num_peers
           << " | Progress: " << progress << "% ] "
           << (st.name.empty() ? st.info_hashes.to_string() : st.name);

        msg_Dbg(m_intf, "%s", os.str().c_str());
    }

    intf_thread_t*   m_intf;
    std::atomic_bool m_running;
    std::thread      m_thread;
};

/*==============================================================
 * VLC glue
 *============================================================*/
static int Open(vlc_object_t* obj)
{
    auto* intf = reinterpret_cast<intf_thread_t*>(obj);

    auto* logger = new (std::nothrow) StatusLogger(intf);
    if (!logger)
        return VLC_ENOMEM;

    intf->p_sys = logger;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t* obj)
{
    auto* intf = reinterpret_cast<intf_thread_t*>(obj);
    delete static_cast<StatusLogger*>(intf->p_sys);
}

vlc_module_begin()
    set_shortname("BT-Status")
    set_description("BitTorrent status logger (console only)")
    set_capability("interface", 0)
    set_callbacks(Open, Close)
vlc_module_end()
