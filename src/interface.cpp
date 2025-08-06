/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`) и теперь играет
 * центральную роль в отображении статуса торрент-загрузки.
 *
 * Архитектура и роль в проекте:
 * 1.  **Долгоживущий компонент:** В отличие от `stream_extractor`, этот модуль
 *     загружается при старте VLC и живёт до его закрытия.
 * 2.  **Подписчик на алерты:** Он содержит класс `VLCStatusUpdater`, который
 *     реализует интерфейс `Alert_Listener`. Этот подписчик регистрируется
 *     в синглтоне `Session` и получает все алерты от libtorrent.
 * 3.  **Генерация статуса:** `VLCStatusUpdater` фильтрует алерты, отлавливая
 *     `state_update_alert` и `dht_stats_alert`, и на их основе формирует
 *     актуальную строку статуса.
 * 4.  **Агрессивное отображение:** Отдельный поток (`Run`) постоянно (каждые
 *     1000 мс) читает сгенерированную строку статуса и принудительно
 *     устанавливает её в качестве имени текущего медиа-элемента, решая
 *     проблему сброса заголовка интерфейсом VLC.
 * 5.  **Отслеживание активного торрента:** Поток `Run` также следит за
 *     переменной VLC "bittorrent-active-hash", чтобы знать, для какого
 *     торрента нужно отображать статус.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <atomic>
#include <string>
#include <sstream>
#include <map>

#include "vlc.h" // Unified header
#include "interface.h"
#include "session.h"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/sha1_hash.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/hex.hpp>

// --- НАЧАЛО ИЗМЕНЕНИЯ 1: ОБЪЯВЛЯЕМ ВНЕШНИЙ ФЛАГ ---
// Сообщаем этому файлу, что флаг g_is_in_blocking_read существует где-то еще.
extern std::atomic<bool> g_is_in_blocking_read;
// --- КОНЕЦ ИЗМЕНЕНИЯ 1 ---

class VLCStatusUpdater : public Alert_Listener {
public:
    // --- ИЗМЕНЕНИЕ 1: Конструктор теперь принимает vlc_object_t* ---
    VLCStatusUpdater(vlc_object_t* p_obj) : m_p_obj(p_obj), m_dht_nodes(0) {}

    void handle_alert(lt::alert* a) override {
        if (auto* su = lt::alert_cast<lt::state_update_alert>(a)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status_map.clear();
            for (const auto& st : su->status) {
                std::ostringstream oss;
                oss << "[ D: " << (st.download_payload_rate / 1024) << " kB/s | "
                    << "U: " << (st.upload_payload_rate / 1024)   << " kB/s | "
                    << "Peers: " << st.num_peers << " (" << st.list_seeds << ") | "
                    << "DHT: " << m_dht_nodes << " | "
                    << "Progress: " << static_cast<int>(st.progress * 100) << "% ]";
                
                lt::sha1_hash current_hash;
                #if LIBTORRENT_VERSION_NUM >= 20000
                    current_hash = st.info_hashes.v1;
                #else
                    current_hash = st.info_hash;
                #endif

                std::string hash_str = lt::aux::to_hex(current_hash.to_string());
                // --- ИЗМЕНЕНИЕ 2: Используем сохраненный указатель m_p_obj для логирования ---
                msg_Dbg(m_p_obj, "[BITTORRENT_DIAG] StatusUpdater: Received state_update_alert for hash %s. Status: %s", hash_str.c_str(), oss.str().c_str());
                m_status_map[current_hash] = oss.str();
            }
        } else if (auto* dht = lt::alert_cast<lt::dht_stats_alert>(a)) {
            int total_nodes = 0;
            for(const auto& bucket : dht->routing_table) {
                total_nodes += bucket.num_nodes;
            }
            m_dht_nodes = total_nodes;
        }
    }

    std::string get_status_string(lt::sha1_hash hash) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_status_map.find(hash);
        if (it != m_status_map.end()) {
            // --- ИЗМЕНЕНИЕ 2: Используем сохраненный указатель m_p_obj для логирования ---
            msg_Dbg(m_p_obj, "[BITTORRENT_DIAG] StatusUpdater: Found status for hash %s", lt::aux::to_hex(hash.to_string()).c_str());
            return it->second;
        }
        // --- ИЗМЕНЕНИЕ 2: Используем сохраненный указатель m_p_obj для логирования ---
        msg_Dbg(m_p_obj, "[BITTORRENT_DIAG] StatusUpdater: No status found for hash %s", lt::aux::to_hex(hash.to_string()).c_str());
        return "";
    }

private:
    // --- ИЗМЕНЕНИЕ 3: Добавляем член класса для хранения указателя на объект VLC ---
    vlc_object_t* m_p_obj;
    std::mutex m_mutex;
    std::map<lt::sha1_hash, std::string> m_status_map;
    std::atomic<int> m_dht_nodes;
};

struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
    char* original_name;
    input_item_t* last_item;
    VLCStatusUpdater* status_updater;
};

static void* Run(void* data);

int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    msg_Info(p_intf, "[BITTORRENT_DIAG] InterfaceOpen: Torrent status interface plugin is loading.");
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys) return VLC_ENOMEM;
    p_intf->p_sys = p_sys;
    p_sys->thread_killed = false;
    p_sys->original_name = NULL;
    p_sys->last_item = NULL;
    
    // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Увеличиваем счетчик ссылок для p_obj
    vlc_object_hold(p_obj);
    
    p_sys->status_updater = new VLCStatusUpdater(p_obj);
    Session::get()->register_alert_listener(p_sys->status_updater);
    msg_Info(p_intf, "[BITTORRENT_DIAG] InterfaceOpen: Status updater created and listener registered. Starting thread...");
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_intf, "[BITTORRENT_DIAG] InterfaceOpen: Failed to start torrent status thread!");
        Session::get()->unregister_alert_listener(p_sys->status_updater);
        delete p_sys->status_updater;
        vlc_object_release(p_obj);  // Уменьшаем счетчик ссылок, так как поток не запущен
        delete p_sys;
        return VLC_EGENERIC;
    }
    msg_Info(p_intf, "[BITTORRENT_DIAG] InterfaceOpen: Thread started successfully.");
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    msg_Dbg(p_intf, "Closing torrent status interface plugin");
    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    Session::get()->unregister_alert_listener(p_sys->status_updater);
    delete p_sys->status_updater;
    
    // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Уменьшаем счетчик ссылок для p_obj
    vlc_object_release(p_obj);
    
    if (p_sys->original_name) {
        free(p_sys->original_name);
    }
    
    // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Корректно освобождаем last_item
    if (p_sys->last_item) {
        input_item_Release(p_sys->last_item);
        p_sys->last_item = NULL;
    }
    
    delete p_sys;
}

static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    lt::sha1_hash active_hash;
    
    while (!p_sys->thread_killed) {
        msleep(1000000);
        
        // ПРОВЕРЯЕМ СВЕТОФОР!
        if (g_is_in_blocking_read) {
            msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: Main thread is in a blocking read. Skipping status update to prevent deadlock.");
            continue;
        }
        
        // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Дополнительная проверка флага завершения
        if (p_sys->thread_killed) break;
        
        msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: Loop tick.");
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist || p_sys->thread_killed) {
            if (p_playlist) vlc_object_release(p_playlist);
            continue;
        }
        
        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input || p_sys->thread_killed) {
            msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: No current input. Resetting state.");
            if(p_sys->last_item) {
                input_item_Release(p_sys->last_item);
                p_sys->last_item = NULL;
                if(p_sys->original_name) free(p_sys->original_name);
                p_sys->original_name = NULL;
                active_hash.clear();
            }
            vlc_object_release(p_playlist);
            continue;
        }
        
        input_item_t* p_item = input_GetItem(p_input);
        if (!p_item || p_sys->thread_killed) {
            msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: No input item. Skipping.");
            vlc_object_release(p_input);
            vlc_object_release(p_playlist);
            continue;
        }
        
        // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Проверка флага завершения перед сравнением
        if (p_sys->thread_killed) {
            input_item_Release(p_item);
            vlc_object_release(p_input);
            vlc_object_release(p_playlist);
            continue;
        }
        
        bool new_item = false;
        if (p_sys->last_item) {
            // Получаем URL для обоих элементов и сравниваем их
            char* last_url = input_item_GetURL(p_sys->last_item);
            char* current_url = input_item_GetURL(p_item);
            
            // Проверяем, что оба URL существуют и различаются
            new_item = (last_url == NULL || current_url == NULL || 
                        strcmp(last_url, current_url) != 0);
            
            // Освобождаем выделенную память
            if (last_url) free(last_url);
            if (current_url) free(current_url);
        } else {
            new_item = true;
        }
        
        if (new_item) {
            msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: New item detected. Updating info.");

            // Сначала удерживаем новый input_item, чтобы его память не ушла из-под нас
            input_item_Hold(p_item);
            
            // Освобождаем старый last_item
            if (p_sys->last_item) {
                input_item_Release(p_sys->last_item);
            }

            // Освобождаем старое имя
            if (p_sys->original_name) {
                free(p_sys->original_name);
            }
            
            //p_sys->original_name = input_item_GetName(p_item);
            // Теперь безопасно дублируем имя
            p_sys->original_name = input_item_GetName(p_item);
            
            // Удерживаем новый last_item
            //p_sys->last_item = input_item_Hold(p_item);

            // И сохраняем ссылку на input_item
            p_sys->last_item = p_item;
            
            char* hash_str = var_GetString(p_input, "bittorrent-active-hash");
            if(hash_str && strlen(hash_str) == 40) {
                msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: Found active hash variable: %s", hash_str);
                lt::from_hex(hash_str, 40, (char*)active_hash.data());
                free(hash_str);
            } else {
                msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: No active hash variable found or it's invalid.");
                active_hash.clear();
                if(hash_str) free(hash_str);
            }
        }
        
        // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Проверка флага завершения перед получением статуса
        if (p_sys->thread_killed) {
            input_item_Release(p_item);
            vlc_object_release(p_input);
            vlc_object_release(p_playlist);
            continue;
        }
        
        std::string status;
        if (!active_hash.is_all_zeros()) {
            msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: Active hash is set. Getting status string.");
            status = p_sys->status_updater->get_status_string(active_hash);
        }
        
        // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: Проверка флага завершения перед установкой имени
        if (p_sys->thread_killed) {
            input_item_Release(p_item);
            vlc_object_release(p_input);
            vlc_object_release(p_playlist);
            continue;
        }
        
        if (!status.empty() && p_sys->original_name) {
            std::string final_title = "VLC-bittorent-crossplatform plugin: ";
            final_title += p_sys->original_name;
            final_title += " ";
            final_title += status;
            msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: Setting item name to: \"%s\"", final_title.c_str());
            vlc_mutex_lock(&p_item->lock);
            input_item_SetName(p_item, final_title.c_str());
            vlc_mutex_unlock(&p_item->lock);
        } else {
            if (p_sys->original_name) {
                msg_Dbg(p_intf, "[BITTORRENT_DIAG] Run: Status is empty. Restoring original name: \"%s\"", p_sys->original_name);
                input_item_SetName(p_item, p_sys->original_name);
            }
        }
        
        input_item_Release(p_item);
        vlc_object_release(p_input);
        vlc_object_release(p_playlist);
    }
    
    msg_Info(p_intf, "[BITTORRENT_DIAG] Torrent status thread stopped");
    return NULL;
}
