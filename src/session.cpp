// src/session.cpp
/*
 * Модуль session.cpp
 *
 *  - Конфигурирует libtorrent::session (alert_mask, DHT-узлы и т.п.).
 *  - Запускает отдельный поток, который:
 *      * ждёт алерты (wait_for_alert)
 *      * собирает их (pop_alerts)
 *      * раздаёт всем Alert_Listener::handle_alert()
 *  - Реализует add/remove torrent и register/unregister listener.
 */

#include "session.h"

#include <libtorrent/alert.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>  // для новых категорий
#include <chrono>
#include <vector>

#define D(x)     // отладочный макрос
#define DD(x)    // ещё один

// Какие алерты слушаем
#define LIBTORRENT_ADD_TORRENT_ALERTS \
    (lt::alert::storage_notification      \
     | lt::alert::block_progress_notification \
     | lt::alert::piece_progress_notification \
     | lt::alert::file_progress_notification  \
     | lt::alert::status_notification      \
     | lt::alert::error_notification)

// DHT-узлы для bootstrap
#define LIBTORRENT_DHT_NODES \
    ("router.bittorrent.com:6881," \
     "router.utorrent.com:6881,"   \
     "dht.transmissionbt.com:6881")

Session::Session(std::mutex& mtx)
    : m_lock(mtx)
    , m_session_thread_quit(false)
{
    D(printf("%s: ctor\n", __func__));

    lt::settings_pack sp = lt::default_settings();
    sp.set_int(sp.alert_mask, LIBTORRENT_ADD_TORRENT_ALERTS);
    sp.set_str(sp.dht_bootstrap_nodes, LIBTORRENT_DHT_NODES);

    // агрессивные настройки для быстрого старта
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

    // Запуск потока: каждую секунду запрашиваем свежие статусы
    m_session_thread = std::thread([this] {
        while (!m_session_thread_quit) {
            // 1) Получаем вектор torrent_status для всех торрентов
            std::vector<lt::torrent_status> stats =
                m_session->get_torrent_status(
                    [](lt::torrent_status const&) { return true; },  // все торренты
                    lt::status_flags::download_payload_rate
                  | lt::status_flags::upload_payload_rate
                  | lt::status_flags::num_peers
                  | lt::status_flags::progress
                );

            // 2) Рассылаем каждому слушателю
            {
                std::lock_guard<std::mutex> lg(m_listeners_mtx);
                for (auto& st : stats) {
                    for (auto* listener : m_listeners) {
                        // Приводим к нашему VLCStatusUpdater и шлём on_status
                        static_cast<VLCStatusUpdater*>(listener)->on_status(st);
                    }
                }
            }

            // 3) Ждём перед следующим циклом
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

Session::~Session()
{
    D(printf("%s: dtor\n", __func__));
    m_session_thread_quit = true;
    if (m_session_thread.joinable())
        m_session_thread.join();
}

void Session::register_alert_listener(Alert_Listener* al)
{
    D(printf("%s: register %p\n", __func__, al));
    std::unique_lock<std::mutex> lock(m_listeners_mtx);
    m_listeners.push_front(al);
}

void Session::unregister_alert_listener(Alert_Listener* al)
{
    D(printf("%s: unregister %p\n", __func__, al));
    std::unique_lock<std::mutex> lock(m_listeners_mtx);
    m_listeners.remove(al);
}

lt::torrent_handle Session::add_torrent(lt::add_torrent_params& atp)
{
    D(printf("%s: add torrent\n", __func__));
    return m_session->add_torrent(atp);
}

void Session::remove_torrent(lt::torrent_handle& th, bool k)
{
    D(printf("%s: remove torrent\n", __func__));
    if (k)
        m_session->remove_torrent(th);
    else
        m_session->remove_torrent(th, lt::session::delete_files);
}

std::shared_ptr<Session> Session::get()
{
    static std::mutex            inst_mtx;
    static std::weak_ptr<Session> inst;
    std::lock_guard<std::mutex> lg(inst_mtx);

    auto s = inst.lock();
    if (!s) {
        // Для блокировки создаём отдельный мьютекс
        static std::mutex mtx;
        s = std::shared_ptr<Session>(new Session(mtx));
        inst = s;
    }
    return s;
}
