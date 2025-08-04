/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`).
 * Его единственная задача - работать в фоновом потоке, читать переменную
 * "bittorrent-status-string" с объекта input_thread и обновлять метаданные
 * проигрываемого файла, чтобы отображать статус загрузки.
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
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        delete p_sys;
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

// Функция закрытия и очистки ресурсов интерфейса
void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    delete p_sys;
}

// Основной цикл работы плагина, выполняющийся в отдельном потоке
static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    char* original_name = NULL;
    input_item_t* last_item = NULL;

    while (!p_sys->thread_killed) {
        msleep(1000000); // Ждем 1 секунду
        
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;
        
        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            // Если ничего не играет, восстанавливаем оригинальное имя
            if (original_name && last_item) {
                input_item_SetName(last_item, original_name);
                free(original_name);
                original_name = NULL;
                last_item = NULL;
            }
            continue;
        }

        input_item_t* p_item = input_GetItem(p_input);
        if (!p_item) {
            vlc_object_release(p_input);
            continue;
        }

        // Если элемент сменился, сохраняем оригинальное имя
        if (p_item != last_item) {
            if (original_name) {
                free(original_name);
                original_name = NULL;
            }
            last_item = p_item;
            original_name = strdup(input_item_GetName(p_item));
        }

        // Читаем статус торрента
        char* status_str = var_GetString(VLC_OBJECT(p_input), "bittorrent-status-string");
        
        if (status_str && strlen(status_str) > 0) {
            // Формируем новое имя
            std::string final_name = std::string(original_name) + " " + std::string(status_str);
            input_item_SetName(p_item, final_name.c_str());
        } else if (original_name) {
            // Восстанавливаем оригинальное имя
            input_item_SetName(p_item, original_name);
        }
        
        if (status_str) free(status_str);
        vlc_object_release(p_input);
    }
    
    // При выходе восстанавливаем оригинальное имя
    if (original_name && last_item) {
        input_item_SetName(last_item, original_name);
        free(original_name);
    }
    
    return NULL;
}
