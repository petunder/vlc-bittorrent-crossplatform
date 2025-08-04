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
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_meta.h>
#include <vlc_events.h>

struct intf_sys_t {
    vlc_thread_t thread;
    vlc_atomic_bool thread_killed;
};

static void* Run(void* data);

int InterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)calloc(1, sizeof(intf_sys_t));
    if (!p_sys) return VLC_ENOMEM;
    p_intf->p_sys = p_sys;

    vlc_atomic_init(&p_sys->thread_killed, false);

    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        free(p_sys);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;

    vlc_atomic_store(&p_sys->thread_killed, true);
    vlc_join(p_sys->thread, NULL);
    free(p_sys);
}

static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;

    char* original_title = NULL;

    while (!vlc_atomic_load(&p_sys->thread_killed)) {
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) {
            msleep(1000000);
            continue;
        }

        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            // Если ничего не играет, сбрасываем сохраненный заголовок
            if (original_title) {
                free(original_title);
                original_title = NULL;
            }
            msleep(1000000);
            continue;
        }
        
        // Ищем наш stream_extractor в цепочке фильтров
        stream_extractor_t* p_extractor = NULL;
        for (int i = 0; ; ++i) {
            vlc_object_t* p_filter = input_GetFilter(p_input, i);
            if (!p_filter) break;

            if (strcmp(p_filter->psz_object_name, "bittorrent") == 0) {
                p_extractor = (stream_extractor_t*)p_filter;
                vlc_object_release(p_filter);
                break;
            }
            vlc_object_release(p_filter);
        }

        input_item_t* p_item = input_GetItem(p_input);
        if (p_extractor && p_item) {
            if (!original_title) {
                original_title = input_item_GetMeta(p_item, vlc_meta_Title);
                if (!original_title) original_title = strdup(""); // Защита от NULL
            }

            char* status_str = var_GetString(p_extractor, "bittorrent-status-string");
            if (status_str && strlen(status_str) > 0) {
                std::string final_title = std::string(original_title) + " " + std::string(status_str);
                
                input_item_SetMeta(p_item, vlc_meta_Title, final_title.c_str());
                vlc_event_manager_t *p_em = input_item_GetEventManager(p_item);
                if (p_em) {
                    vlc_event_t event;
                    vlc_event_init(&event, vlc_InputItemMetaChanged);
                    event.u.input_item_meta_changed.meta_type = vlc_meta_Title;
                    vlc_event_send(p_em, &event);
                }
            }
            free(status_str);
        } else {
            // Если наш плагин неактивен, восстанавливаем исходный заголовок
             if (original_title && p_item) {
                input_item_SetMeta(p_item, vlc_meta_Title, original_title);
                vlc_event_manager_t *p_em = input_item_GetEventManager(p_item);
                if (p_em) {
                    vlc_event_t event;
                    vlc_event_init(&event, vlc_InputItemMetaChanged);
                    event.u.input_item_meta_changed.meta_type = vlc_meta_Title;
                    vlc_event_send(p_em, &event);
                }
                free(original_title);
                original_title = NULL;
            }
        }
        
        vlc_object_release(p_input);
        msleep(1000000); // Ждем 1 секунду
    }

    return NULL;
}
