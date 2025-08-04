/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`).
 * Его единственная задача - работать в фоновом потоке и обновлять строку статуса VLC,
 * используя информацию из переменной "bittorrent-status-string".
 * 
 * ВАЖНО: Это решение гарантирует отображение статуса торрента в строке статуса VLC,
 * потому что использует ОБА механизма:
 * 1. Установка переменной "status-text" (прямой способ для строки статуса)
 * 2. Изменение имени элемента с установкой флага "item-change"
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include "interface.h"
#include <atomic>
#include <string>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_meta.h>

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// Системная структура для старого подхода с потоком.
/*
struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
};
*/

// Новый подход не требует сложной системной структуры.
struct intf_sys_t {
    char* last_status;
    input_item_t* last_item;
};

// Старая функция потока опроса.
/*
static void* Run(void* data);
*/
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// Новый код: функция обратного вызова, которая реагирует на изменение статуса.
// Она будет вызвана VLC автоматически, когда плагин data.cpp обновит переменную "bittorrent-status-string".
static void OnStatusChanged(vlc_object_t* p_obj, const char* var_name,
                            vlc_value_t old_val, vlc_value_t new_val, void* p_data) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj; // p_obj - это и есть наш интерфейс
    intf_sys_t* p_sys = (intf_sys_t*)p_data;
    
    playlist_t* p_playlist = pl_Get(p_intf);
    if (!p_playlist) return;

    input_thread_t* p_input = playlist_CurrentInput(p_playlist);
    if (!p_input) return;

    input_item_t* p_item = input_GetItem(p_input);
    if (!p_item) {
        vlc_object_release(p_input);
        return;
    }

    // Сброс, если сменился медиа-элемент
    if (p_item != p_sys->last_item) {
        if (p_sys->last_status) {
            free(p_sys->last_status);
            p_sys->last_status = NULL;
        }
        p_sys->last_item = p_item;
    }

    const char* status_str = new_val.psz_string;

    if (status_str && strlen(status_str) > 0) {
        // Обновляем, только если статус изменился
        if (!p_sys->last_status || strcmp(p_sys->last_status, status_str) != 0) {
            if (p_sys->last_status) free(p_sys->last_status);
            p_sys->last_status = strdup(status_str);
            
            msg_Dbg(p_intf, "Updating status bar via callback: %s", status_str);
            
            // 1. Прямой способ через "status-text"
            var_SetString(p_input, "status-text", status_str);
            
            // 2. Обновление имени элемента для совместимости
            char* original_name = input_item_GetName(p_item);
            char* new_name = (char*)malloc(strlen(original_name) + strlen(status_str) + 4);
            if (new_name) {
                sprintf(new_name, "%s (%s)", original_name, status_str);
                input_item_SetName(p_item, new_name);
                free(new_name);
            }
            free(original_name);
            var_SetInteger(p_input, "item-change", 1); // Уведомляем интерфейс
        }
    } else if (p_sys->last_status) {
        // Статус был, но пропал - восстанавливаем исходное имя.
        free(p_sys->last_status);
        p_sys->last_status = NULL;
        
        char* original_name = input_item_GetMeta(p_item, vlc_meta_Title);
        if (original_name) {
            input_item_SetName(p_item, original_name);
            free(original_name);
        }
        var_SetString(p_input, "status-text", "");
        var_SetInteger(p_input, "item-change", 0);
    }
    
    vlc_object_release(p_input);
}
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys)
        return VLC_ENOMEM;
    p_intf->p_sys = p_sys;
    p_sys->last_status = NULL;
    p_sys->last_item = NULL;
    
    msg_Dbg(p_intf, "Torrent status interface plugin started");
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: запуск потока для опроса.
    /*
    p_sys->thread_killed = false;
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_intf, "Failed to start torrent status thread");
        delete p_sys;
        return VLC_EGENERIC;
    }
    */
    // Новый код: подписываемся на изменения переменной. Это эффективнее, чем опрос.
    // Мы будем отслеживать изменения на глобальном объекте VLC, так как текущий
    // input может меняться. В колбэке мы найдем нужный input.
    var_AddCallback(VLC_OBJECT(p_intf), "bittorrent-status-string", OnStatusChanged, p_sys);
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
    
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    msg_Dbg(p_intf, "Closing torrent status interface plugin");
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код: остановка потока.
    /*
    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    */
    // Новый код: отписываемся от колбэка.
    var_DelCallback(VLC_OBJECT(p_intf), "bittorrent-status-string", OnStatusChanged, p_sys);
    
    if (p_sys->last_status) {
        free(p_sys->last_status);
    }
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
    delete p_sys;
}

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// Старый код: функция потока, которая больше не нужна при использовании колбэков.
/*
static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    input_item_t* last_item = NULL;
    char* last_status = NULL;
    bool is_dvb_mode = false;

    while (!p_sys->thread_killed) {
        msleep(300000); // Ждем 0.3 секунды (быстрее, чем VLC может сбросить)
        
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;
        
        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            if (last_status) {
                free(last_status);
                last_status = NULL;
            }
            continue;
        }

        input_item_t* p_item = input_GetItem(p_input);
        if (!p_item) {
            vlc_object_release(p_input);
            continue;
        }

        bool item_changed = (p_item != last_item);
        if (item_changed) {
            if (last_status) {
                free(last_status);
                last_status = NULL;
            }
            last_item = p_item;
        }

        char* status_str = var_GetString(VLC_OBJECT(p_input), "bittorrent-status-string");
        
        bool current_dvb_mode = var_GetBool(p_input, "program");
        
        if (status_str && strlen(status_str) > 0) {
            bool status_changed = (last_status == NULL || 
                                 strcmp(status_str, last_status) != 0);
            
            if (current_dvb_mode && !is_dvb_mode) {
                msg_Dbg(p_intf, "Detected DVB mode, trying to disable it");
                var_SetBool(p_input, "program", false);
                var_SetBool(p_input, "is-sat-ip", false);
                var_SetBool(p_input, "is-dvb", false);
                is_dvb_mode = false;
            }
            
            if (status_changed) {
                if (last_status) free(last_status);
                last_status = strdup(status_str);
                
                msg_Dbg(p_intf, "Updating status bar: %s", status_str);
                
                var_SetString(VLC_OBJECT(p_input), "status-text", status_str);
                
                char* original_name = input_item_GetName(p_item);
                char* new_name = (char*)malloc(strlen(original_name) + strlen(status_str) + 2);
                if (new_name) {
                    strcpy(new_name, original_name);
                    strcat(new_name, " ");
                    strcat(new_name, status_str);
                    input_item_SetName(p_item, new_name);
                    free(new_name);
                }
                free(original_name);
                
                var_SetInteger(p_input, "item-change", 1);
            }
        } else if (last_status) {
            msg_Dbg(p_intf, "Restoring original status bar text");
            free(last_status);
            last_status = NULL;
            
            char* original_name = input_item_GetMeta(p_item, vlc_meta_Title);
            if (original_name) {
                input_item_SetName(p_item, original_name);
                free(original_name);
            }
            
            var_SetString(VLC_OBJECT(p_input), "status-text", "");
            var_SetInteger(p_input, "item-change", 0);
        }
        
        if (!current_dvb_mode && is_dvb_mode) {
            msg_Dbg(p_intf, "DVB mode disabled");
            is_dvb_mode = false;
        }
        
        if (status_str) free(status_str);
        vlc_object_release(p_input);
    }
    
    if (last_status) free(last_status);
    
    msg_Dbg(p_intf, "Torrent status thread stopped");
    return NULL;
}
*/
// --- КОНЕЦ ИЗМЕНЕНИЯ ---
