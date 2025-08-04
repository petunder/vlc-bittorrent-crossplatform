/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`).
 * Его единственная задача - работать в фоновом потоке, читать глобальную
 * переменную VLC "bittorrent-status-string" и обновлять заголовок (метаданные)
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
    
    libvlc_instance_t* p_libvlc = vlc_object_instance(VLC_OBJECT(p_intf));

    char* original_title = NULL;
    input_item_t* last_item = NULL;

    while (!p_sys->thread_killed) {
        msleep(1000000); // Ждем 1 секунду

        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;

        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            // Ничего не играет, восстанавливаем заголовок последнего элемента
            if (original_title && last_item) {
                input_item_SetMeta(last_item, vlc_meta_Title, original_title);
                input_item_SendEventMeta(last_item);
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

        // Если медиа-элемент сменился, сбрасываем состояние
        if (p_item != last_item) {
            if (original_title && last_item) {
                input_item_SetMeta(last_item, vlc_meta_Title, original_title);
                input_item_SendEventMeta(last_item);
            }
            if (original_title) free(original_title);
            original_title = NULL;
            last_item = p_item;
        }
        
        // Читаем глобальную переменную статуса
        char* status_str = NULL;
        if(p_libvlc)
            status_str = var_GetString(p_libvlc, "bittorrent-status-string");
        
        if (status_str && strlen(status_str) > 0) {
            // Если статус есть, сохраняем оригинальный заголовок (если еще не сделали)
            if (!original_title) {
                original_title = input_item_GetMeta(p_item, vlc_meta_Title);
                if (!original_title) 
                    original_title = strdup(p_item->psz_name ? p_item->psz_name : "");
            }

            // Формируем новую строку и обновляем метаданные
            std::string final_title = std::string(original_title ? original_title : "") + " " + std::string(status_str);
            input_item_SetMeta(p_item, vlc_meta_Title, final_title.c_str());
            input_item_SendEventMeta(p_item); // Уведомляем GUI об изменении
        } else {
             // Если статусная строка пуста, восстанавливаем оригинальный заголовок
             if (original_title) {
                input_item_SetMeta(p_item, vlc_meta_Title, original_title);
                input_item_SendEventMeta(p_item);
                free(original_title);
                original_title = NULL;
            }
        }
        
        if (status_str) free(status_str);
        vlc_object_release(p_input);
    }

    // При выходе из потока, восстанавливаем оригинальный заголовок
    if (original_title && last_item) {
        input_item_SetMeta(last_item, vlc_meta_Title, original_title);
        input_item_SendEventMeta(last_item);
        free(original_title);
    }

    return NULL;
}
