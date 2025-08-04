/*
 * src/session.cpp
 *
 * Этот модуль управляет глобальной сессией libtorrent. Он является синглтоном,
 * который инициализируется при первом обращении.
 *
 * Основные задачи:
 * - Конфигурация и запуск сессии libtorrent (настройки DHT, маска алертов).
 * - Запуск отдельного потока для обработки алертов от libtorrent
 *   и **генерации строки статуса для активного торрента**.
 * - Предоставление интерфейса для регистрации/отмены регистрации слушателей алертов
 *   и установки/сброса активного торрента.
 */

#include "session.h"
#include <libtorrent/alert.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp> // Для lt::torrent_status
// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// ОШИБКА: Этот заголовок (и API) относится к libtorrent 2.0.x, тогда как проект,
// вероятно, использует 1.x.
// #include <libtorrent/read_session_params.hpp>
// --- КОНЕЦ ИЗМЕНЕНИЯ ---
#include <chrono>
#include <vector>
#include <sstream>

#define LIBTORRENT_ADD_TORRENT_ALERTS \
    (lt::alert::storage_notification          \
     | lt::alert::block_progress_notification \
     | lt::alert::piece_progress_notification \
     | lt::alert::file_progress_notification  \
     | lt::alert::status_notification         \
     | lt::alert::tracker_notification        \
     | lt::alert::dht_notification            \
     | lt::alert::error_notification)

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
    m_active_torrent_hash = lt::sha1_hash();

    m_session_thread = std::thread(&Session::session_thread, this);
}

Session::~Session()
{
    m_quit = true;
    m_session->abort();
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

void Session::set_active_torrent(lt::torrent_handle th) {
    if (th.is_valid()) {
        m_active_torrent_hash = th.info_hash();
    }
}

void Session::clear_active_torrent() {
    m_active_torrent_hash = lt::sha1_hash(); // Сбрасываем хеш
}

std::string Session::get_active_status_string() {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_status_string;
}

void Session::session_thread()
{
    while (!m_quit) {
        m_session->wait_for_alert(std::chrono::seconds(1));
        if (m_quit) break;

        // --- НАЧАЛО ИЗМЕНЕНИЯ ---
        // Запрашиваем актуальный статус торрентов и DHT
        m_session->post_torrent_updates();
        m_session->post_dht_stats();
        // --- КОНЕЦ ИЗМЕНЕНИЯ ---

        std::vector<lt::alert*> alerts;
        m_session->pop_alerts(&alerts);

        {
            std::lock_guard<std::mutex> lg(m_listeners_mtx);
            for (auto* a : alerts) {
                for (auto* h : m_listeners) {
                    try { h->handle_alert(a); }
                    catch (...) {}
                }
            }
        }
        
        lt::sha1_hash active_hash = m_active_torrent_hash.load();
        std::string new_status_string = "";

        if (active_hash != lt::sha1_hash()) {
            lt::torrent_handle th = m_session->find_torrent(active_hash);
            if (th.is_valid()) {
                lt::torrent_status st = th.status();
                if (st.has_metadata) {
                     std::ostringstream oss;
                     oss << "[ D: " << (st.download_payload_rate / 1024) << " kB/s | "
                         << "U: " << (st.upload_payload_rate / 1024)   << " kB/s | "
                         << "Peers: " << st.num_peers << " (" << st.list_seeds << ") | "
                         << "Progress: " << static_cast<int>(st.progress * 100) << "% ]";
                     new_status_string = oss.str();
                } else {
                     new_status_string = "[ Downloading metadata... ]";
                }
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_status_string = new_status_string;
        }
    }
}

std::shared_ptr<Session> Session::get()
{
    static std::shared_ptr<Session> inst = []{
        static std::mutex global_mtx;
        return std::shared_ptr<Session>(new(std::nothrow) Session(global_mtx));
    }();
    return inst;
}
