// src/session.h
/*
 * Модуль session.h
 *
 * Обёртка над libtorrent::session:
 *  - Хранит единственную сессию в синглтоне.
 *  - Позволяет регистрировать Alert_Listener для любых алертов.
 *  - Обеспечивает методы add/remove torrent.
 */

#ifndef VLC_BITTORRENT_LIBTORRENT_H
#define VLC_BITTORRENT_LIBTORRENT_H

#include <forward_list>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#include <libtorrent/alert.hpp>
#include <libtorrent/session.hpp>
#pragma GCC diagnostic pop

// Интерфейс для получения алертов из libtorrent
struct Alert_Listener {
    virtual ~Alert_Listener() { }
    virtual void handle_alert(lt::alert* a) = 0;
};

class Session {
public:
    // Получить синглтон (инициализирует при первом вызове)
    static std::shared_ptr<Session> get();

    // Alert-API
    void register_alert_listener(Alert_Listener* al);
    void unregister_alert_listener(Alert_Listener* al);

    // Torrent-API
    lt::torrent_handle add_torrent(lt::add_torrent_params& atp);
    void remove_torrent(lt::torrent_handle& th, bool keep);

private:
    Session(std::mutex& global_mtx);
    ~Session();

    // Цикл polling’а алертов
    void session_thread();

    std::unique_lock<std::mutex>       m_lock;
    std::unique_ptr<lt::session>       m_session;
    std::thread                        m_session_thread;
    std::atomic<bool>                  m_quit{false};
    std::forward_list<Alert_Listener*> m_listeners;
    std::mutex                         m_listeners_mtx;
};

#endif // VLC_BITTORRENT_LIBTORRENT_H
