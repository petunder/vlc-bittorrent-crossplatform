/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`).
 * Его единственная задача - работать в фоне и обновлять метаданные
 * проигрываемого файла, читая статус из переменной, которую
 * устанавливает плагин stream_extractor.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "interface.h"
#include <atomic> // Используем стандартный C++ atomic
#include <string>

#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_meta.h>
#include <vlc_module.h> // Для поиска нашего модуля

struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
};

static void* Run(void* data);

int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    // Используем new/delete для C++
    intf_sys_t* p_sys = new intf_sys_t();
    p_intf->p_sys = p_sys;

    p_sys->thread_killed = false;

    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        delete p_sys;
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;

    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    delete p_sys;
}

static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    vlc_object_t *p_vlc_obj = VLC_OBJECT(p_intf);

    char* original_title = NULL;
    input_item_t* last_item = NULL;

    while (!p_sys->thread_killed) {
        msleep(1000000); // Ждем 1 секунду

        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;

        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            // Если ничего не играет, сбрасываем сохраненный заголовок
            if (original_title) {
                free(original_title);
                original_title = NULL;
                last_item = NULL;
            }
            continue;
        }

        input_item_t* p_item = input_GetItem(p_input);
        if (!p_item) {
            vlc_object_release(p_input);
            continue;
        }

        // Если элемент сменился, сбрасываем сохраненный заголовок
        if (p_item != last_item) {
            if (original_title) free(original_title);
            original_title = NULL;
            last_item = p_item;
        }
        
        // Ищем наш stream_extractor по имени
        module_t *p_module = module_list_get_module_by_capability(p_vlc_obj, "stream_extractor", "bittorrent");
        
        if (p_module && p_module->p_obj) {
            char* status_str = var_GetString(p_module->p_obj, "bittorrent-status-string");
            
            if (status_str && strlen(status_str) > 0) {
                if (!original_title) {
                    original_title = input_item_GetMeta(p_item, vlc_meta_Title);
                    if (!original_title) original_title = strdup(p_item->psz_name ? p_item->psz_name : "");
                }

                std::string final_title = std::string(original_title) + " " + std::string(status_str);
                input_item_SetMeta(p_item, vlc_meta_Title, final_title.c_str());
            } else {
                 // Если статус пуст, но мы его меняли, восстанавливаем
                 if (original_title) {
                    input_item_SetMeta(p_item, vlc_meta_Title, original_title);
                    free(original_title);
                    original_title = NULL;
                 }
            }
            free(status_str);
        }
        
        vlc_object_release(p_input);
    }

    // Восстанавливаем оригинальный заголовок при выходе
    if (original_title && last_item) {
        input_item_SetMeta(last_item, vlc_meta_Title, original_title);
        free(original_title);
    }

    return NULL;
}
