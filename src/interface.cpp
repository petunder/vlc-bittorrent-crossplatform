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

// Системная структура для хранения состояния интерфейса
struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
};

static void* Run(void* data);

// Функция инициализации интерфейсного плагина
int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys)
        return VLC_ENOMEM;
    p_intf->p_sys = p_sys;
    p_sys->thread_killed = false;
    
    msg_Dbg(p_intf, "Torrent status interface plugin started");
    
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_intf, "Failed to start torrent status thread");
        delete p_sys;
        return VLC_EGENERIC;
    }
    
    return VLC_SUCCESS;
}

// Функция закрытия и очистки ресурсов интерфейса
void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    msg_Dbg(p_intf, "Closing torrent status interface plugin");
    
    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    delete p_sys;
}

// Основной цикл работы плагина, выполняющийся в отдельном потоке
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

        // Проверяем, не сменился ли элемент
        bool item_changed = (p_item != last_item);
        if (item_changed) {
            if (last_status) {
                free(last_status);
                last_status = NULL;
            }
            last_item = p_item;
        }

        // Читаем статус торрента
        char* status_str = var_GetString(VLC_OBJECT(p_input), "bittorrent-status-string");
        
        // Проверяем, активен ли DVB-режим
        bool current_dvb_mode = var_GetBool(p_input, "program");
        
        if (status_str && strlen(status_str) > 0) {
            // Проверяем, изменился ли статус
            bool status_changed = (last_status == NULL || 
                                 strcmp(status_str, last_status) != 0);
            
            // Если это DVB-режим, пытаемся его отключить
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
                
                // СПОСОБ 1: УСТАНАВЛИВАЕМ ПЕРЕМЕННУЮ "status-text" - ПРЯМОЙ СПОСОБ
                var_SetString(VLC_OBJECT(p_input), "status-text", status_str);
                
                // СПОСОБ 2: ИЗМЕНЯЕМ ИМЯ ЭЛЕМЕНТА С ФЛАГОМ "item-change"
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
                
                // Устанавливаем флаг, что имя изменено пользователем (а не системой)
                var_SetInteger(p_input, "item-change", 1);
            }
        } else if (last_status) {
            // Если статус пропал, но раньше был
            msg_Dbg(p_intf, "Restoring original status bar text");
            free(last_status);
            last_status = NULL;
            
            // Восстанавливаем оригинальное имя
            char* original_name = input_item_GetMeta(p_item, vlc_meta_Title);
            if (original_name) {
                input_item_SetName(p_item, original_name);
                free(original_name);
            }
            
            // Сбрасываем переменную status-text
            var_SetString(VLC_OBJECT(p_input), "status-text", "");
            var_SetInteger(p_input, "item-change", 0);
        }
        
        // Если мы были в DVB-режиме, но теперь его отключили
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
