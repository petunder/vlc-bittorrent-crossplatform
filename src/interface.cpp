/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`). Его роль - отображение
 * статуса торрент-загрузки в пользовательском интерфейсе VLC.
 *
 * ПРОБЛЕМА И РЕШЕНИЕ:
 * Стандартный интерфейс VLC (Qt) часто игнорирует стандартные механизмы
 * обновления статусной строки в пользу отображения метаданных файла (Title).
 * Чтобы гарантированно показать статус торрента, используется метод
 * "агрессивного похищения заголовка" (aggressive title hijacking):
 *
 * 1.  Плагин работает в собственном фоновом потоке, который запускается
 *     при инициализации (InterfaceOpen).
 * 2.  В бесконечном цикле поток с высокой частотой (400 мс) запрашивает
 *     актуальную строку статуса напрямую у глобального синглтона Session.
 * 3.  Затем он принудительно устанавливает эту строку в качестве имени
 *     текущего элемента плейлиста (`input_item_SetName`).
 * 4.  Этот постоянный опрос и установка побеждают попытки интерфейса VLC
 *     сбросить имя на значение из метаданных, обеспечивая видимость статуса.
 * 5.  При смене трека или завершении загрузки плагин корректно восстанавливает
 *     оригинальное имя файла.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include "interface.h"
#include "session.h"
#include <atomic>
#include <string>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_meta.h>

struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
    char* original_name;
    input_item_t* last_item;
};

static void* Run(void* data);

int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys)
        return VLC_ENOMEM;
    
    p_intf->p_sys = p_sys;
    p_sys->thread_killed = false;
    p_sys->original_name = NULL;
    p_sys->last_item = NULL;
    
    msg_Dbg(p_intf, "Torrent status interface plugin started (aggressive title-hijack mode)");
    
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_intf, "Failed to start torrent status thread");
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
    
    if (p_sys->original_name) {
        free(p_sys->original_name);
    }
    delete p_sys;
}

static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
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
        }

        std::string status = Session::get()->get_active_status_string();
        const char* status_str = status.c_str();
        
        if (status_str && strlen(status_str) > 0) {
            input_item_SetName(p_item, status_str);
            var_SetInteger(p_input, "item-change", 1);
        } else {
            if (p_sys->original_name) {
                char* current_name = input_item_GetName(p_item);
                if(current_name && strcmp(current_name, p_sys->original_name) != 0) {
                    input_item_SetName(p_item, p_sys->original_name);
                    var_SetInteger(p_input, "item-change", 1); 
                }
                free(current_name);
            }
        }
        
        vlc_object_release(p_input);
    }
    
    msg_Dbg(p_intf, "Torrent status thread stopped");
    return NULL;
}
