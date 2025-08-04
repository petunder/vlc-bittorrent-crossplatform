/*
 * src/interface.cpp
 *
 * Этот модуль является интерфейсным плагином (`intf`).
 * Его единственная задача - напрямую обновлять строку статуса VLC через Qt,
 * минуя внутренние механизмы VLC, которые не работают в данном случае.
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
#include <vlc_variables.h>

// Для доступа к Qt
#include <QWidget>
#include <QStatusBar>
#include <QApplication>

// Системная структура для хранения состояния интерфейса
struct intf_sys_t {
    vlc_thread_t thread;
    std::atomic<bool> thread_killed;
};

static void* Run(void* data);

// Функция инициализации интерфейсного плагина
int TorrentStatusInterfaceOpen(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = new(std::nothrow) intf_sys_t();
    if (!p_sys)
        return VLC_ENOMEM;
    p_intf->p_sys = p_sys;
    p_sys->thread_killed = false;
    
    msg_Dbg(p_obj, "Torrent status interface plugin is loading");
    
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        msg_Err(p_obj, "Failed to start torrent status thread");
        delete p_sys;
        return VLC_EGENERIC;
    }
    
    return VLC_SUCCESS;
}

// Функция закрытия и очистки ресурсов интерфейса
void TorrentStatusInterfaceClose(vlc_object_t* p_obj) {
    intf_thread_t* p_intf = (intf_thread_t*)p_obj;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    
    msg_Dbg(p_obj, "Closing torrent status interface plugin");
    
    p_sys->thread_killed = true;
    vlc_join(p_sys->thread, NULL);
    delete p_sys;
}

// Основной цикл работы плагина, выполняющийся в отдельном потоке
static void* Run(void* data) {
    intf_thread_t* p_intf = (intf_thread_t*)data;
    intf_sys_t* p_sys = (intf_sys_t*)p_intf->p_sys;
    char* last_status = NULL;
    bool qt_initialized = false;

    msg_Dbg(p_intf, "Torrent status thread started");
    
    while (!p_sys->thread_killed) {
        msleep(300000); // Ждем 0.3 секунды
        
        // 1. Проверяем, есть ли активный поток
        playlist_t* p_playlist = pl_Get(p_intf);
        if (!p_playlist) continue;
        
        input_thread_t* p_input = playlist_CurrentInput(p_playlist);
        if (!p_input) {
            if (last_status) {
                free(last_status);
                last_status = NULL;
            }
            vlc_object_release(p_playlist);
            continue;
        }
        vlc_object_release(p_playlist);

        // 2. Читаем статус торрента
        char* status_str = var_GetString(VLC_OBJECT(p_input), "bittorrent-status-string");
        
        // 3. Получаем доступ к Qt-интерфейсу
        QWidget* p_main_window = (QWidget*)p_intf->p_sys->p_main_window;
        
        if (!p_main_window) {
            msg_Dbg(p_intf, "Main window not available yet");
            
            if (status_str) free(status_str);
            vlc_object_release(p_input);
            continue;
        }
        
        // Убедимся, что Qt инициализирован
        if (!qt_initialized) {
            msg_Dbg(p_intf, "Qt interface initialized");
            qt_initialized = true;
        }
        
        QStatusBar* p_status_bar = p_main_window->findChild<QStatusBar*>();
        if (!p_status_bar) {
            msg_Dbg(p_intf, "Status bar not found in Qt interface");
            
            if (status_str) free(status_str);
            vlc_object_release(p_input);
            continue;
        }

        // 4. Обновляем статус в зависимости от данных
        if (status_str && strlen(status_str) > 0) {
            // Проверяем, изменился ли статус
            bool status_changed = (last_status == NULL || 
                                 strcmp(status_str, last_status) != 0);
            
            if (status_changed) {
                msg_Dbg(p_intf, "Updating status bar: %s", status_str);
                
                if (last_status) free(last_status);
                last_status = strdup(status_str);
                
                // ПРЯМОЙ ВЫЗОВ Qt ДЛЯ ОБНОВЛЕНИЯ СТРОКИ СТАТУСА
                QString status = QString::fromUtf8(status_str);
                p_status_bar->showMessage(status);
            }
        } else if (last_status) {
            // Если статус пропал, но раньше был
            msg_Dbg(p_intf, "Restoring original status bar text");
            free(last_status);
            last_status = NULL;
            
            // Восстанавливаем оригинальный статус через VLC
            input_item_t* p_item = input_GetItem(p_input);
            if (p_item) {
                // Получаем оригинальное имя файла
                const char* original_name = input_item_GetName(p_item);
                if (original_name) {
                    QString status = QString::fromUtf8(original_name);
                    p_status_bar->showMessage(status);
                }
                input_item_Release(p_item);
            }
        }
        
        if (status_str) free(status_str);
        vlc_object_release(p_input);
    }
    
    if (last_status) free(last_status);
    
    msg_Dbg(p_intf, "Torrent status thread stopped");
    return NULL;
}

// Регистрация интерфейсного модуля
vlc_module_begin()
    set_shortname("torrentstatus")
    set_description("Displays torrent status in status bar")
    set_category(N_("Interface"))
    set_subcategory(N_("Control"))
    set_capability("interface", 0)
    set_callbacks(TorrentStatusInterfaceOpen, TorrentStatusInterfaceClose)
vlc_module_end()
