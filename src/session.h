/*
 * src/session.h
 *
 * Модуль session.h
 *
 * Обёртка над libtorrent::session. Реализован как синглтон.
 *
 * Роль в проекте: Простой и надежный диспетчер алертов.
 * Его единственная задача - запустить сессию libtorrent в отдельном потоке,
 * ловить все асинхронные события (алерты) и пересылать их всем
 * зарегистрированным подписчикам (Alert_Listener).
 * Он не содержит никакой логики, специфичной для плагина, что делает его
 * универсальным и стабильным.
 */

#ifndef VLC_BITTORRENT_LIBTORRENT_H
#define VLC_BITTORRENT_LIBTORRENT_H

#include <forward_list>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#include <libtorrent/alert.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/sha1_hash.hpp>
#pragma GCC diagnostic pop

// Интерфейс для получения алертов из libtorrent
struct Alert_Listener {
    virtual ~Alert_Listener() = default;
    virtual void handle_alert(lt::alert* a) = 0;
};

class Session {
public:
    static std::shared_ptr<Session> get();
    ~Session();

    // Alert-API
    void register_alert_listener(Alert_Listener* al);
    void unregister_alert_listener(Alert_Listener* al);

    // Torrent-API
    lt::torrent_handle add_torrent(lt::add_torrent_params& atp);
    void remove_torrent(lt::torrent_handle& th, bool keep);

private:
    Session(); // Конструктор теперь приватный для синглтона Мейерса

    void session_thread();

    std::unique_ptr<lt::session>       m_session;
    std::thread                        m_session_thread;
    std::atomic<bool>                  m_quit{false};
    std::forward_list<Alert_Listener*> m_listeners;
    std::mutex                         m_listeners_mtx;
};

#endif // VLC_BITTORRENT_LIBTORRENT_H
