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
 *     400 мс) читает сгенерированную строку статуса и принудительно
 *     устанавливает её в качестве имени текущего медиа-элемента, решая
 *     проблему сброса заголовка интерфейсом VLC.
 * 5.  **Отслеживание активного торрента:** Поток `Run` также следит за
 *     переменной VLC "bittorrent-active-hash", чтобы знать, для какого
 *     торрента нужно отображать статус.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include "interface.h"
#include "session.h"
#include <atomic>
#include <string>
#include <sstream>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_meta.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/sha1_hash.hpp>
#include <libtorrent/torrent_status.hpp>

// --- НАЧАЛО ИЗМЕНЕНИЯ: ЛОГИКА СТАТУСА ПЕРЕЕХАЛА СЮДА ---
class VLCStatusUpdater : public Alert_Listener {
public:
    VLCStatusUpdater() : m_dht_nodes(0) {}

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
                m_status_map[st.info_hash] = oss.str();
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
            return it->second;
        }
        return "";
    }

private:
    std::mutex m_mutex;
    std::map<lt::sha1_hash, std::string> m_status_map;
    std::atomic<int> m_dht_nodes;
};
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

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
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys) return VLC_ENOMEM;
    
    p_intf->p_sys = p_sys;
    p_sys->thread_killed = false;
    p_sys->original_name = NULL;
    p_sys->last_item = NULL;
    
    p_sys->status_updater = new VLCStatusUpdater();
    Session::get()->register_alert_listener(p_sys->status_updater);
    
    msg_Dbg(p_intf, "Torrent status interface plugin started");
    
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_intf, "Failed to start torrent status thread");
        Session::get()->unregister_alert_listener(p_sys->status_updater);
        delete p_sys->status_updater;
        delete p_sys;
        return VLC_EGENERIC;
    }
    
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

    if (p_sys->original_name) {
        free(p_sys->original_name);
    }
    delete p_sys;
}

static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    lt::sha1_hash active_hash;

    while (!p_sys->thread_killed) {
        msleep(400000);
        
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;
        
        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            if(p_sys->last_item) {
                p_sys->last_item = NULL;
                if(p_sys->original_name) free(p_sys->original_name);
                p_sys->original_name = NULL;
                active_hash.clear();
            }
            continue;
        }

        input_item_t* p_item = input_GetItem(p_input);
        if (!p_item) {
            vlc_object_release(p_input);
            continue;
        }

        if (p_item != p_sys->last_item) {
            if (p_sys->original_name) free(p_sys->original_name);
            p_sys->original_name = input_item_GetName(p_item);
            p_sys->last_item = p_item;

            char* hash_str = var_GetString(p_input, "bittorrent-active-hash");
            if(hash_str) {
                lt::from_hex(hash_str, 20, (char*)&active_hash[0]);
                free(hash_str);
            } else {
                active_hash.clear();
            }
        }

        std::string status;
        if (!active_hash.is_all_zeros()) {
            status = p_sys->status_updater->get_status_string(active_hash);
        }
        
        const char* status_str = status.c_str();
        
        if (status_str && strlen(status_str) > 0) {
            input_item_SetName(p_item, status_str);
        } else {
            if (p_sys->original_name) {
                char* current_name = input_item_GetName(p_item);
                if(current_name && strcmp(current_name, p_sys->original_name) != 0) {
                    input_item_SetName(p_item, p_sys->original_name);
                }
                free(current_name);
            }
        }
        
        vlc_object_release(p_input);
    }
    
    msg_Dbg(p_intf, "Torrent status thread stopped");
    return NULL;
}
