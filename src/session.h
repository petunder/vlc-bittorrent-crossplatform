/*
 * src/session.h
 *
 * Модуль session.h
 *
 * Обёртка над libtorrent::session:
 *  - Хранит единственную сессию в синглтоне (глобальный объект).
 *  - Позволяет регистрировать Alert_Listener для любых алертов.
 *  - Обеспечивает методы add/remove torrent.
 *  - **Ключевая роль в проекте:** Отвечает за отслеживание "активного"
 *    торрента, который в данный момент воспроизводится. Генерирует и
 *    хранит его строку статуса, которую могут запрашивать другие модули.
 *    Это решает проблему короткого жизненного цикла stream_extractor'а.
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
    // Получить синглтон (инициализирует при первом вызове)
    static std::shared_ptr<Session> get();

    // Деструктор — публичный, чтобы shared_ptr мог корректно уничтожить объект
    ~Session();

    // Alert-API
    void register_alert_listener(Alert_Listener* al);
    void unregister_alert_listener(Alert_Listener* al);

    // Torrent-API
    lt::torrent_handle add_torrent(lt::add_torrent_params& atp);
    void remove_torrent(lt::torrent_handle& th, bool keep);

    // --- НАЧАЛО ИЗМЕНЕНИЯ: НОВЫЕ ПУБЛИЧНЫЕ МЕТОДЫ ---
    // Устанавливает торрент как активный для мониторинга статуса.
    void set_active_torrent(lt::torrent_handle th);
    // Сбрасывает активный торрент.
    void clear_active_torrent();
    // Получает сгенерированную строку статуса.
    std::string get_active_status_string();
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---

private:
    Session(std::mutex& global_mtx);

    // Цикл polling’а алертов и обновления статуса
    void session_thread();

    std::unique_lock<std::mutex>       m_lock;
    std::unique_ptr<lt::session>       m_session;
    std::thread                        m_session_thread;
    std::atomic<bool>                  m_quit{false};
    std::forward_list<Alert_Listener*> m_listeners;
    std::mutex                         m_listeners_mtx;

    // --- НАЧАЛО ИЗМЕНЕНИЯ: НОВЫЕ ПРИВАТНЫЕ ПОЛЯ ---
    std::atomic<lt::sha1_hash> m_active_torrent_hash; // Потокобезопасный хеш активного торрента
    std::string m_status_string; // Строка статуса
    std::mutex m_status_mutex;   // Мьютекс для защиты строки
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
};

#endif // VLC_BITTORRENT_LIBTORRENT_H
