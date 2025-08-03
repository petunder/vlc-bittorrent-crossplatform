// src/session.cpp
/*
 * Модуль session.cpp
 *
 * Реализует:
 * - Конструктор Session: настраивает параметры libtorrent::session,
 *   в том числе alert_mask и DHT-узлы, и запускает фоновой поток.
 * - Деструктор: корректно завершает поток опроса алертов.
 * - Session::get(): возвращает синглтон-экземпляр.
 * - Методы add/remove_status_listener: регистрация слушателей.
 * - session_thread(): каждую секунду вызывает wait_for_alert,
 *   извлекает state_update_alert и передаёт каждому StatusListener.
 * - Класс VLCStatusUpdater: конкретный StatusListener,
 *   формирующий текст «BT: D=…kB/s U=…kB/s Peers=… […]»
 *   и записывающий его в var_SetString(p_input, "title", …).
 *
 * Место в архитектуре:
 * Ядро обработки событий торрента — непрерывный поллинг и рассылка
 * обновлений плагину (data.cpp) для динамического отображения статуса.
 */

#include "session.h"
#include <vlc_variables.h>
#include <libtorrent/alert.hpp>
#include <sstream>
#include <chrono>

// Какие категории алертов слушать
#define LIBTORRENT_ADD_TORRENT_ALERTS \
    (lt::alert::storage_notification       \
     | lt::alert::block_progress_notification \
     | lt::alert::piece_progress_notification \
     | lt::alert::file_progress_notification  \
     | lt::alert::status_notification      \
     | lt::alert::error_notification)

// Список DHT-бутстрап-узлов
#define LIBTORRENT_DHT_NODES \
    ("router.bittorrent.com:6881,"            \
     "router.utorrent.com:6881,"              \
     "dht.transmissionbt.com:6881")

// Конкретный слушатель, обновляющий VLC 'title' при каждом статусе
class VLCStatusUpdater : public StatusListener {
public:
    explicit VLCStatusUpdater(vlc_object_t* input)
        : m_input(input) {}

    // Формирует строку вида "BT: D=256kB/s U=12kB/s Peers=8 [43%]"
    // и записывает её в переменную title текущего VLC-потока.
    void on_status(const lt::torrent_status& st) override {
        std::ostringstream oss;
        oss << "BT: D=" << (st.download_rate / 1000) << "kB/s"
            << " U=" << (st.upload_rate / 1000)   << "kB/s"
            << " Peers=" << st.num_peers
            << " [" << int(st.progress * 100)   << "%]";
        var_SetString(m_input, "title", oss.str().c_str());
    }

private:
    vlc_object_t* m_input; // VLC-объект, у которого меняем title
};

std::shared_ptr<Session> Session::get() {
    static std::weak_ptr<Session> inst;
    static std::mutex             inst_mtx;
    std::lock_guard<std::mutex> lg(inst_mtx);
    auto s = inst.lock();
    if (!s) {
        s = std::shared_ptr<Session>(new Session());
        inst = s;
    }
    return s;
}

// Конструктор: настраиваем libtorrent::session и запускаем поток
Session::Session() {
    lt::settings_pack sp = lt::default_settings();
    // Подписываемся на нужные алерты
    sp.set_int(sp.alert_mask, LIBTORRENT_ADD_TORRENT_ALERTS);
    // Задаём DHT-узлы
    sp.set_str(sp.dht_bootstrap_nodes, LIBTORRENT_DHT_NODES);

    m_session = std::make_unique<lt::session>(sp);

    // Запускаем фоновой поток обработки алертов
    m_thread = std::thread(&Session::session_thread, this);
}

// Деструктор: мягко завершаем поток
Session::~Session() {
    m_quit = true;
    if (m_thread.joinable())
        m_thread.join();
}

// Регистрация слушателя: создаём VLCStatusUpdater
void Session::add_status_listener(vlc_object_t* p_input) {
    std::lock_guard<std::mutex> lg(m_mtx);
    if (m_listeners.count(p_input)) return; // уже есть
    m_listeners[p_input] = std::make_shared<VLCStatusUpdater>(p_input);
}

// Удаляем слушателя, когда плагин закрывается
void Session::remove_status_listener(vlc_object_t* p_input) {
    std::lock_guard<std::mutex> lg(m_mtx);
    m_listeners.erase(p_input);
}

// Фоновая функция: ждем алерты и рассылаем их слушателям
void Session::session_thread() {
    while (!m_quit) {
        // Ждём до 1 секунды, пока не придут алерты
        m_session->wait_for_alert(std::chrono::seconds(1));

        std::vector<lt::alert*> alerts;
        m_session->pop_alerts(&alerts);

        std::lock_guard<std::mutex> lg(m_mtx);
        // Ищем только state_update_alert, содержащий вектор torrent_status
        for (auto* a : alerts) {
            if (auto* su = lt::alert_cast<lt::state_update_alert>(a)) {
                for (auto& st : su->status) {
                    // Рассылаем каждому зарегистрированному слушателю
                    for (auto& kv : m_listeners)
                        kv.second->on_status(st);
                }
            }
        }
    }
}
