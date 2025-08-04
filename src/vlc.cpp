/*
 * src/vlc.cpp
 *
 * Этот файл является вспомогательным модулем, который предоставляет
 * обертки для функций VLC API, используемых в других частях плагина.
 *
 * Основные задачи:
 * - Получение и создание директорий для загрузок и кэша,
 *   используя стандартные пути VLC.
 * - Инкапсуляция логики работы с конфигурационными переменными VLC,
 *   такими как `bittorrent-download-path` и `bittorrent-keep-files`.
 * - Обеспечение консистентной обработки ошибок при работе с файловой
 *   системой в стиле, принятом в VLC.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <string>

// --- НАЧАЛО ИЗМЕНЕНИЯ ---
// #include <filesystem> // ОШИБКА: Этот заголовок требует C++17, а проект использует C++14.
// namespace fs = std::filesystem;
// --- КОНЕЦ ИЗМЕНЕНИЯ ---
#include "vlc.h"

std::string
get_download_directory(vlc_object_t* p_this)
{
    std::string dldir;

    std::unique_ptr<char, decltype(&free)> dir(
        var_InheritString(p_this, DLDIR_CONFIG), free);
    if (!dir) {
        std::unique_ptr<char, decltype(&free)> user_dir(
            config_GetUserDir(VLC_DOWNLOAD_DIR), free);
        if (!user_dir) {
            // --- НАЧАЛО ИЗМЕНЕНИЯ ---
            // Старый код: бросал исключение, которое не обрабатывалось.
            // throw std::runtime_error("Failed to find download directory");
            // Новый код: Используем логгер VLC и возвращаем пустую строку.
            msg_Err(p_this, "Failed to find user download directory");
            return "";
            // --- КОНЕЦ ИЗМЕНЕНИЯ ---
        }

        dldir = std::string(user_dir.get());
        
        // --- НАЧАЛО ИЗМЕНЕНИЯ ---
        // Старый код с std::filesystem, который не компилируется.
        /*
        try {
            fs::create_directories(dldir);
        } catch (const fs::filesystem_error& e) {
            msg_Err(p_this, "Failed to create directory (%s): %s", dldir.c_str(), e.what());
            return "";
        }
        */
        // Новый код: Возвращаемся к использованию vlc_mkdir, как в оригинале.
        if (vlc_mkdir(dldir.c_str(), 0777) && errno != EEXIST) {
             msg_Err(p_this, "Failed to create directory (%s): %s", dldir.c_str(), strerror(errno));
             return "";
        }
        // --- КОНЕЦ ИЗМЕНЕНИЯ ---

        dldir += DIR_SEP;
        dldir += PACKAGE;
    } else {
        dldir = std::string(dir.get());
    }
    
    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код с std::filesystem.
    /*
    try {
        fs::create_directories(dldir);
    } catch (const fs::filesystem_error& e) {
        msg_Err(p_this, "Failed to create directory (%s): %s", dldir.c_str(), e.what());
        return "";
    }
    */
    // Новый код: Возвращаемся к vlc_mkdir и улучшаем обработку ошибок.
    if (vlc_mkdir(dldir.c_str(), 0777) && errno != EEXIST) {
        msg_Err(p_this, "Failed to create directory (%s): %s", dldir.c_str(), strerror(errno));
        return "";
    }
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---

    return dldir;
}

std::string
get_cache_directory(vlc_object_t* p_this)
{
    std::string cachedir;

    std::unique_ptr<char, decltype(&free)> dir(
        config_GetUserDir(VLC_CACHE_DIR), free);
    if (!dir) {
        msg_Err(p_this, "Failed to find cache directory");
        return "";
    }

    cachedir = std::string(dir.get());

    // --- НАЧАЛО ИЗМЕНЕНИЯ ---
    // Старый код с std::filesystem.
    /*
    try {
        fs::create_directories(cachedir);
    } catch (const fs::filesystem_error& e) {
        msg_Err(p_this, "Failed to create directory (%s): %s", cachedir.c_str(), e.what());
        return "";
    }
    */
    // Новый код: Возвращаемся к vlc_mkdir.
    if (vlc_mkdir(cachedir.c_str(), 0777) && errno != EEXIST) {
        msg_Err(p_this, "Failed to create cache directory (%s): %s", cachedir.c_str(), strerror(errno));
        return "";
    }
    // --- КОНЕЦ ИЗМЕНЕНИЯ ---

    return cachedir;
}

bool
get_keep_files(vlc_object_t* p_this)
{
    return var_InheritBool(p_this, KEEP_CONFIG);
}
