/*
 * src/session.cpp
 *
 * Этот модуль управляет глобальной сессией libtorrent. Он является синглтоном,
 * который инициализируется при первом обращении.
 *
 * Основные задачи:
 * - Конфигурация и запуск сессии libtorrent (настройки DHT, маска алертов).
 * - Запуск отдельного потока для обработки алертов от libtorrent.
 * - **Периодический запрос обновлений статуса (`post_torrent_updates`)**, чтобы получать
 *   информацию о скоростях, пирах и т.д.
 * - Предоставление интерфейса для регистрации/отмены регистрации слушателей алертов.
 */

#include "session.h"
#include <libtorrent/alert.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/session_params.hpp> // Для write_session_params
#include <libtorrent/read_session_params.hpp> // Для read_session_params
#include <chrono>
#include <vector>

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// Старая маска алертов не включала dht_notification, что не позволяло получать статистику по DHT.
/*
#define LIBTORRENT_ADD_TORRENT_ALERTS \
    (lt::alert::storage_notification          \
     | lt::alert::block_progress_notification \
     | lt::alert::piece_progress_notification \
     | lt::alert::file_progress_notification  \
     | lt::alert::status_notification         \
     | lt::alert::tracker_notification        \
     | lt::alert::error_notification)
*/
// Новая маска добавляет dht_notification для получения dht_stats_alert.
// Также добавляем session_log_notification для более детальной отладки.
#define LIBTORRENT_ADD_TORRENT_ALERTS \
    (lt::alert::storage_notification          \
     | lt::alert::block_progress_notification \
     | lt::alert::piece_progress_notification \
     | lt::alert::file_progress_notification  \
     | lt::alert::status_notification         \
     | lt::alert::tracker_notification        \
     | lt::alert::dht_notification            \
     | lt::alert::session_log_notification    \
     | lt::alert::error_notification)
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

#define LIBTORRENT_DHT_NODES \
    ("router.bittorrent.com:6881,"    \
     "router.utorrent.com:6881,"      \
     "dht.transmissionbt.com:6881")

Session::Session(std::mutex& global_mtx)
    : m_lock(global_mtx)
{
    lt::settings_pack sp = lt::default_settings();
    sp.set_int(sp.alert_mask, LIBTORRENT_ADD_TORRENT_ALERTS);
    sp.set_str(sp.dht_bootstrap_nodes, LIBTORRENT_DHT_NODES);

    sp.set_bool(sp.strict_end_game_mode, false);
    sp.set_bool(sp.announce_to_all_trackers, true);
    sp.set_bool(sp.announce_to_all_tiers, true);
    sp.set_int(sp.stop_tracker_timeout, 1);
    sp.set_int(sp.request_timeout, 2);
    sp.set_int(sp.whole_pieces_threshold, 5);
    sp.set_int(sp.request_queue_time, 1);
    sp.set_int(sp.urlseed_pipeline_size, 2);
#if LIBTORRENT_VERSION_NUM >= 10102
    sp.set_int(sp.urlseed_max_request_bytes, 100 * 1024);
#endif

    m_session = std::make_unique<lt::session>(sp);

    m_session_thread = std::thread(&Session::session_thread, this);
}

Session::~Session()
{
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: мог приводить к гонкам при завершении.
    /*
    m_quit = true;
    if (m_session_thread.joinable())
        m_session_thread.join();
    */
    // Новый код: более безопасное завершение сессии.
    // Сначала посылаем команду на остановку, затем ждем поток.
    m_quit = true;
    m_session->abort(); // Прерываем ожидание алертов
    if (m_session_thread.joinable()) {
        m_session_thread.join();
    }
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
}

void Session::register_alert_listener(Alert_Listener* al)
{
    std::lock_guard<std::mutex> lg(m_listeners_mtx);
    m_listeners.push_front(al);
}

void Session::unregister_alert_listener(Alert_Listener* al)
{
    std::lock_guard<std::mutex> lg(m_listeners_mtx);
    m_listeners.remove(al);
}

lt::torrent_handle Session::add_torrent(lt::add_torrent_params& atp)
{
    return m_session->add_torrent(atp);
}

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// Новый асинхронный метод, как более современная альтернатива.
void Session::async_add_torrent(lt::add_torrent_params& atp) {
    m_session->async_add_torrent(atp);
}
// --- КОНЕЦ ИЗМЕНЕНИЯ ---


void Session::remove_torrent(lt::torrent_handle& th, bool keep)
{
    if (keep)
        m_session->remove_torrent(th);
    else
        m_session->remove_torrent(th, lt::session::delete_files);
}

void Session::session_thread()
{
    while (!m_quit) {
        // --- НАЧАЛО ИЗМЕНЕНИЯ ---
        // Старый код: простое ожидание.
        // m_session->wait_for_alert(std::chrono::seconds(1));
        
        // Новый код: обрабатываем nullptr, который может вернуть wait_for_alert при вызове abort().
        lt::alert* a = m_session->wait_for_alert(std::chrono::seconds(1));
        if (a == nullptr) continue; // Сессия завершается
        // --- КОНЕЦ ИЗМЕНЕНИЯ ---

        // Явно запрашиваем у libtorrent прислать нам алерты
        // о состоянии торрентов и DHT.
        m_session->post_torrent_updates();
        m_session->post_dht_stats();

        std::vector<lt::alert*> alerts;
        m_session->pop_alerts(&alerts);

        std::lock_guard<std::mutex> lg(m_listeners_mtx);
        for (auto* alert_item : alerts) {
            for (auto* h : m_listeners) {
                try { h->handle_alert(alert_item); }
                catch (...) {}
            }
        }
    }
}

std::shared_ptr<Session> Session::get()
{
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: синглтон с внешней синхронизацией, подверженный проблемам.
    /*
    static std::mutex             inst_mtx;
    static std::weak_ptr<Session> inst;
    std::lock_guard<std::mutex>   lg(inst_mtx);
    auto s = inst.lock();
    if (!s) {
        static std::mutex global_mtx;
        s = std::shared_ptr<Session>(new Session(global_mtx));
        inst = s;
    }
    return s;
    */
    // Новый код: потокобезопасный синглтон Мейерса (C++11 и новее).
    // Он лениво инициализируется при первом вызове и гарантированно создается один раз.
    static std::shared_ptr<Session> inst = []{
        static std::mutex global_mtx;
        // Используем new(std::nothrow) для большей безопасности
        return std::shared_ptr<Session>(new(std::nothrow) Session(global_mtx));
    }();
    return inst;
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
}
