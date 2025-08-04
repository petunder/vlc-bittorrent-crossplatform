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

struct intf_sys_t {
    char* last_status;
    input_item_t* last_item;
};

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// Старый код с неверной сигнатурой (void).
/*
static void OnStatusChanged(vlc_object_t* p_obj, const char* var_name,
                            vlc_value_t old_val, vlc_value_t new_val, void* p_data) { ... }
*/
// Новый код: Сигнатура исправлена на `int` и добавлен `return VLC_SUCCESS`,
// как того требует тип `vlc_callback_t`.
static int OnStatusChanged(vlc_object_t* p_obj, const char* var_name,
                           vlc_value_t old_val, vlc_value_t new_val, void* p_data) {
// --- КОНЕЦ ИЗМЕНЕНИЯ ---
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_data;
    
    playlist_t* p_playlist = pl_Get(p_intf);
    if (!p_playlist) return VLC_SUCCESS;

    input_thread_t* p_input = playlist_CurrentInput(p_playlist);
    if (!p_input) return VLC_SUCCESS;

    input_item_t* p_item = input_GetItem(p_input);
    if (!p_item) {
        vlc_object_release(p_input);
        return VLC_SUCCESS;
    }

    if (p_item != p_sys->last_item) {
        if (p_sys->last_status) {
            free(p_sys->last_status);
            p_sys->last_status = NULL;
        }
        p_sys->last_item = p_item;
    }

    const char* status_str = new_val.psz_string;

    if (status_str && strlen(status_str) > 0) {
        if (!p_sys->last_status || strcmp(p_sys->last_status, status_str) != 0) {
            if (p_sys->last_status) free(p_sys->last_status);
            p_sys->last_status = strdup(status_str);
            
            msg_Dbg(p_intf, "Updating status bar via callback: %s", status_str);
            
            var_SetString(p_input, "status-text", status_str);
            
            char* original_name = input_item_GetName(p_item);
            char* new_name = (char*)malloc(strlen(original_name) + strlen(status_str) + 4);
            if (new_name) {
                sprintf(new_name, "%s (%s)", original_name, status_str);
                input_item_SetName(p_item, new_name);
                free(new_name);
            }
            free(original_name);
            var_SetInteger(p_input, "item-change", 1);
        }
    } else if (p_sys->last_status) {
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
    return VLC_SUCCESS;
}

int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys)
        return VLC_ENOMEM;
    p_intf->p_sys = p_sys;
    p_sys->last_status = NULL;
    p_sys->last_item = NULL;
    
    msg_Dbg(p_intf, "Torrent status interface plugin started");
    
    // Вместо потока используется колбэк - это современный и эффективный подход.
    var_AddCallback(VLC_OBJECT(p_intf), "bittorrent-status-string", OnStatusChanged, p_sys);
    
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    msg_Dbg(p_intf, "Closing torrent status interface plugin");
    
    var_DelCallback(VLC_OBJECT(p_intf), "bittorrent-status-string", OnStatusChanged, p_sys);
    
    if (p_sys->last_status) {
        free(p_sys->last_status);
    }
    delete p_sys;
}
