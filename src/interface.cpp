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

#include "vlc.h"                 // unified VLC headers
#include "interface.h"           // декларации InterfaceOpen/InterfaceClose

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

// --- НАЧАЛО ИЗМЕНЕНИЯ 1: ДЕЛАЕМ ГЛОБАЛЬНЫЙ ФЛАГ ВИДИМЫМ ---
// Объявляем флаг из download.cpp как внешний (extern),
// чтобы компоновщик знал, где его найти.
extern std::atomic<bool> g_is_in_blocking_read;
// --- КОНЕЦ ИЗМЕНЕНИЯ 1 ---

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

/*-------------------------------------------------------
 * Слушатель алертов libtorrent + динамический оверлей
 *------------------------------------------------------*/
class TorrentStatusLogger final : public Alert_Listener
{
public:
    explicit TorrentStatusLogger(vlc_object_t* obj)
      : m_intf(obj), m_running(true), m_thread(&TorrentStatusLogger::loop, this)
    {
        // --- НАЧАЛО ИЗМЕНЕНИЯ 2: ИСПРАВЛЕНА ЛОГИКА СОЗДАНИЯ FIFO ---
        // 1) Определить путь к FIFO
        m_fifo_path = "/tmp/vlc-bittorrent-overlay.fifo";

        // 2) Удалить старый файл, если он существует
        ::unlink(m_fifo_path.c_str());

        // 3) Создать FIFO
        if (::mkfifo(m_fifo_path.c_str(), 0666) != 0) {
            msg_Err(m_intf, "mkfifo(%s): %s", m_fifo_path.c_str(), strerror(errno));
            m_fifo_fd = -1;
        } else {
            m_fifo_fd = ::open(m_fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
            if (m_fifo_fd < 0)
                msg_Warn(m_intf, "open(%s): %s", m_fifo_path.c_str(), strerror(errno));
        }

        // 4) Сообщить VLC путь к файлу
        var_SetString(m_intf, "dynamicoverlay-file", m_fifo_path.c_str());

        // 5) И только теперь включить сам фильтр
        var_SetString(m_intf, "sub-filter", "dynamicoverlay");
        // --- КОНЕЦ ИЗМЕНЕНИЯ 2 ---

        // Подписка на алерты
        Session::get()->register_alert_listener(this);
    }

    ~TorrentStatusLogger() override
    {
        Session::get()->unregister_alert_listener(this);
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
        if (m_fifo_fd >= 0)
            ::close(m_fifo_fd);
        // --- НАЧАЛО ИЗМЕНЕНИЯ 3: ОЧИСТКА ---
        // Удаляем FIFO при закрытии
        ::unlink(m_fifo_path.c_str());
        // --- КОНЕЦ ИЗМЕНЕНИЯ 3 ---
    }

    // вызывается в потоке libtorrent
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
    std::string                  m_fifo_path;
    int                          m_fifo_fd = -1; // Инициализируем

    // loop: 1 Hz отправляет snapshot в FIFO
    void loop()
    {
        using namespace std::chrono_literals;
        while (m_running) {
            std::this_thread::sleep_for(1s);

            // --- НАЧАЛО ИЗМЕНЕНИЯ 4: ПРОВЕРКА ФЛАГА БЛОКИРОВКИ ---
            // Если сейчас идет блокирующее скачивание, не делаем ничего,
            // чтобы не мешать основному потоку.
            if (g_is_in_blocking_read) {
                continue;
            }
            // --- КОНЕЦ ИЗМЕНЕНИЯ 4 ---

            std::vector<std::string> snap;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snap = m_lines;
            }
            if (m_fifo_fd < 0) continue;

            for (auto const& line : snap) {
                std::string cmd = "0 " + line + "\n";
                ssize_t written = ::write(m_fifo_fd, cmd.c_str(), cmd.size());
                // Игнорируем ошибки записи, например, если VLC еще не успел прочитать
                (void)written;
            }
        }
    }
};

/*-------------------------------------------------------
 * VLC interface boilerplate
 *------------------------------------------------------*/
struct intf_sys_t { TorrentStatusLogger* logger; };

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
