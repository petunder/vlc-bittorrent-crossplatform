// src/session.h
/*
 * Модуль session.h
 *
 * Обёртка над libtorrent::session:
 *  - Запускает фоновый поток polling’а алертов.
 *  - Хранит список Alert_Listener* для всех подписчиков.
 *  - Предоставляет API для добавления/удаления торрентов и слушателей алертов.
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

// Интерфейс для получения любых алертов libtorrent
struct Alert_Listener {
    virtual ~Alert_Listener() { }
    virtual void handle_alert(lt::alert* alert) = 0;
};

class Session {
public:
    // Конструктор блокирует переданный мьютекс
    Session(std::mutex& mtx);
    // Останавливает поток и очищает ресурсы
    ~Session();

    // Подписаться/отписаться на все алерты
    void register_alert_listener(Alert_Listener* al);
    void unregister_alert_listener(Alert_Listener* al);

    // Добавить/удалить торрент
    lt::torrent_handle add_torrent(lt::add_torrent_params& atp);
    void remove_torrent(lt::torrent_handle& th, bool keep_files);

    // Singleton
    static std::shared_ptr<Session> get();

private:
    // Фоновой поток poll’ит алерты
    void session_thread();

    std::unique_lock<std::mutex>               m_lock;
    std::unique_ptr<lt::session>               m_session;
    std::thread                                m_session_thread;
    std::atomic<bool>                          m_session_thread_quit;
    std::forward_list<Alert_Listener*>         m_listeners;
    std::mutex                                 m_listeners_mtx;
};

#endif // VLC_BITTORRENT_LIBTORRENT_H
