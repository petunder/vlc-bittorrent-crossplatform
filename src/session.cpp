// src/session.cpp
/*
 * Модуль session.cpp
 *
 * На старте конфигурирует lt::session (alert_mask, DHT-узлы и т.п.),
 * затем в отдельном потоке ждёт алерты через pop_alerts()
 * и передаёт их всем зарегистрированным Alert_Listener.
 */

#include "session.h"
#include <libtorrent/alert.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <chrono>
#include <vector>

#define LIBTORRENT_ADD_TORRENT_ALERTS \
    (lt::alert::storage_notification       \
     | lt::alert::block_progress_notification \
     | lt::alert::piece_progress_notification \
     | lt::alert::file_progress_notification  \
     | lt::alert::status_notification       \
     | lt::alert::metadata_notification     \
     | lt::alert::error_notification)

#define LIBTORRENT_DHT_NODES \
    ("router.bittorrent.com:6881,"            \
     "router.utorrent.com:6881,"              \
     "dht.transmissionbt.com:6881")

Session::Session(std::mutex& global_mtx)
    : m_lock(global_mtx)
{
    lt::settings_pack sp = lt::default_settings();
    sp.set_int(sp.alert_mask, LIBTORRENT_ADD_TORRENT_ALERTS);
    sp.set_str(sp.dht_bootstrap_nodes, LIBTORRENT_DHT_NODES);

    // агрессивные настройки для скорого старта
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

    // Запуск цикла обработки алертов
    m_session_thread = std::thread(&Session::session_thread, this);
}

Session::~Session()
{
    m_quit = true;
    if (m_session_thread.joinable())
        m_session_thread.join();
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
        m_session->wait_for_alert(std::chrono::seconds(1));
        std::vector<lt::alert*> alerts;
        m_session->pop_alerts(&alerts);

        std::lock_guard<std::mutex> lg(m_listeners_mtx);
        for (auto* a : alerts) {
            for (auto* h : m_listeners) {
                try { h->handle_alert(a); }
                catch (...) {}
            }
        }
    }
}

std::shared_ptr<Session> Session::get()
{
    static std::mutex            inst_mtx;
    static std::weak_ptr<Session> inst;
    std::lock_guard<std::mutex> lg(inst_mtx);
    auto s = inst.lock();
    if (!s) {
        static std::mutex global_mtx;
        s = std::shared_ptr<Session>(new Session(global_mtx));
        inst = s;
    }
    return s;
}
