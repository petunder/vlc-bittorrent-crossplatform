/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`).
 * Его единственная задача - работать в фоновом потоке и обновлять строку статуса VLC,
 * используя информацию из переменной "bittorrent-status-string".
 *
 * РЕШЕНИЕ ПРОБЛЕМЫ ОТОБРАЖЕНИЯ:
 * Стандартный интерфейс VLC (Qt) игнорирует переменную "status-text" и всегда
 * отдает приоритет имени медиа-элемента (Title). Чтобы гарантированно
 * отобразить статус торрента, мы используем "агрессивный" подход:
 *
 * 1. Возвращаемся к потоковой модели с постоянным опросом (polling).
 * 2. В цикле с высокой частотой (каждые 400 мс) мы ПРИНУДИТЕЛЬНО
 *    устанавливаем имя текущего элемента (`input_item_SetName`) равным строке статуса.
 * 3. Это создает "гонку состояний" с интерфейсом VLC, в которой наш плагин
 *    побеждает, так как перезаписывает заголовок чаще, чем его может сбросить GUI.
 * 4. Мы также сохраняем оригинальное имя элемента, чтобы восстановить его,
 *    когда торрент-статус больше не доступен.
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

// --- НАЧАЛО ИЗМЕНЕНИЯ: ВОЗВРАЩАЕМ СТРУКТУРУ ДЛЯ ПОТОКА ---
// Старый код с колбэками был неэффективен.
/*
struct intf_sys_t {
    char* last_status;
    input_item_t* last_item;
};
*/
// Новый код: структура для управления потоком и состоянием.
struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
    char* original_name; // Для восстановления исходного имени
    input_item_t* last_item;
};
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

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
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ: ВОЗВРАЩАЕМ ПОТОК ---
    // Старый код с колбэками.
    /*
    var_AddCallback(VLC_OBJECT(p_intf), "bittorrent-status-string", OnStatusChanged, p_sys);
    */
    // Новый код: Запускаем поток для агрессивного опроса.
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_intf, "Failed to start torrent status thread");
        delete p_sys;
        return VLC_EGENERIC;
    }
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
    
    return VLC_SUCCESS;
}

void InterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    msg_Dbg(p_intf, "Closing torrent status interface plugin");
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ: ОСТАНАВЛИВАЕМ ПОТОК ---
    // Старый код с колбэками.
    /*
    var_DelCallback(VLC_OBJECT(p_intf), "bittorrent-status-string", OnStatusChanged, p_sys);
    */
    // Новый код: Устанавливаем флаг и ждем завершения потока.
    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    
    if (p_sys->original_name) {
        free(p_sys->original_name);
    }
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---
    delete p_sys;
}

// Основной цикл работы плагина, выполняющийся в отдельном потоке
static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    while (!p_sys->thread_killed) {
        msleep(400000); // Опрашиваем каждые 400 мс. Достаточно быстро, чтобы "победить" GUI.
        
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;
        
        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
             // Если воспроизведение остановлено, сбрасываем состояние.
            if(p_sys->last_item) {
                p_sys->last_item = NULL;
                free(p_sys->original_name);
                p_sys->original_name = NULL;
            }
            continue;
        }

        input_item_t* p_item = input_GetItem(p_input);
        if (!p_item) {
            vlc_object_release(p_input);
            continue;
        }

        // Если элемент сменился, сохраняем его оригинальное имя.
        if (p_item != p_sys->last_item) {
            if (p_sys->original_name) {
                free(p_sys->original_name);
            }
            p_sys->original_name = input_item_GetName(p_item);
            p_sys->last_item = p_item;
        }

        char* status_str = var_GetString(p_input, "bittorrent-status-string");
        
        if (status_str && strlen(status_str) > 0) {
            // ПРИНУДИТЕЛЬНО УСТАНАВЛИВАЕМ ИМЯ ЭЛЕМЕНТА
            // Мы не добавляем к имени, а полностью его заменяем.
            input_item_SetName(p_item, status_str);
            var_SetInteger(p_input, "item-change", 1); // Уведомляем интерфейс
        } else {
            // Если статус торрента пропал, а у нас есть сохраненное имя - восстанавливаем его.
            if (p_sys->original_name) {
                input_item_SetName(p_item, p_sys->original_name);
                // После восстановления оригинальное имя нам больше не нужно.
                free(p_sys->original_name);
                p_sys->original_name = NULL;
                var_SetInteger(p_input, "item-change", 0);
            }
        }
        
        if (status_str) free(status_str);
        vlc_object_release(p_input);
    }
    
    msg_Dbg(p_intf, "Torrent status thread stopped");
    return NULL;
}
