// src/session.h
/*
 * Модуль session.h
 *
 * Определяет класс Session — обёртку над libtorrent::session,
 * запускающую фоновый поток обработки алертов, и интерфейс StatusListener
 * для получения обновлений состояния торрента.
 *
 * Место в архитектуре:
 * - Session хранит глобальную торрент-сессию и пуллистенеров.
 * - DataOpen регистрирует StatusListener для обновлений,
 *   а session.cpp раз в секунду извлекает алерты и рассылвает их слушателям.
 */

#ifndef VLC_BITTORRENT_LIBTORRENT_H
#define VLC_BITTORRENT_LIBTORRENT_H

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <libtorrent/session.hpp>
#include <libtorrent/alert.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

// Интерфейс для получения уведомлений о статусе торрента.
// Реализуется, чтобы обновлять, например, строку статуса VLC.
class StatusListener {
public:
    virtual ~StatusListener() = default;
    // Вызывается при каждом обновлении статуса (state_update_alert)
    virtual void on_status(const lt::torrent_status& st) = 0;
};

// Класс, оборачивающий libtorrent::session и обеспечивающий
// фоновой опрос алертов и рассылку их зарегистрированным слушателям.
class Session {
public:
    // Возвращает синглтон-экземпляр Session.
    static std::shared_ptr<Session> get();

    // Регистрирует слушателя, которому будут
    // передаваться результаты опроса состояния торрента.
    void add_status_listener(vlc_object_t* p_input);

    // Убирает слушателя из списка.
    void remove_status_listener(vlc_object_t* p_input);

private:
    Session();   // Конфигурирует libtorrent и запускает поток
    ~Session();  // Останавливает поток

    // Функция, выполняемая в отдельном потоке:
    // ждёт алерты и пересылает их слушателям.
    void session_thread();

    std::unique_ptr<lt::session> m_session; // Внутренний libtorrent::session
    std::thread                  m_thread;  // Поток polling'а алертов
    bool                         m_quit = false; // Флаг остановки

    std::mutex                                           m_mtx;       // Защита map-списка
    std::map<vlc_object_t*, std::shared_ptr<StatusListener>> m_listeners; // Слушатели
};

#endif // VLC_BITTORRENT_LIBTORRENT_H
