cmake_minimum_required(VERSION 3.14)

# --- Начало патча для Windows ---
if (WIN32)
    # На Windows pkg-config недоступен, поэтому ищем библиотеки и заголовки вручную.
    # Путь к VLC SDK должен быть передан через -DCMAKE_PREFIX_PATH
    find_path(VLC_INCLUDE_DIRS
        NAMES vlc/vlc.h
        PATHS ${CMAKE_PREFIX_PATH}/include
        NO_DEFAULT_PATH
    )
    find_library(VLC_LIBRARIES
        NAMES libvlc libvlc.lib
        PATHS ${CMAKE_PREFIX_PATH}/lib
        NO_DEFAULT_PATH
    )

    if (NOT VLC_INCLUDE_DIRS OR NOT VLC_LIBRARIES)
        message(FATAL_ERROR
          "Не удалось найти VLC SDK. Убедитесь, что CMAKE_PREFIX_PATH указывает на корень VLC SDK "
          "(например, 'C:/path/to/vlc-3.0.21/sdk')"
        )
    endif()

    # Libtorrent будет найден через vcpkg toolchain
    find_package(LibtorrentRasterbar REQUIRED)
else()
    # Стандартная логика для Linux/macOS
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VlcPlugin REQUIRED IMPORTED_TARGET "vlc-plugin >= 3.0.0")
    pkg_check_modules(LibtorrentRasterbar REQUIRED IMPORTED_TARGET "libtorrent-rasterbar >= 1.2.0")
endif()
# --- Конец патча для Windows ---


# --- Объявление двух отдельных плагинов ---
# ПЛАГИН №1: Доступ к данным (access, stream_extractor)
add_library(
    access_bittorrent_plugin
    MODULE
        module.cpp
        metadata.cpp
        magnetmetadata.cpp
        data.cpp
        download.cpp
        session.cpp
        vlc.cpp
)
set_target_properties(access_bittorrent_plugin PROPERTIES PREFIX "lib" OUTPUT_NAME "access_bittorrent")

# ПЛАГИН №2: Видеофильтр-оверлей
add_library(
    overlay_plugin
    MODULE
        overlay.cpp
)
set_target_properties(overlay_plugin PROPERTIES PREFIX "lib" OUTPUT_NAME "bittorrent_overlay")


# --- Линковка для обоих плагинов ---
if (WIN32)
    # Линковка для Windows
    target_include_directories(access_bittorrent_plugin PRIVATE ${VLC_INCLUDE_DIRS} ${LibtorrentRasterbar_INCLUDE_DIRS})
    target_link_libraries(access_bittorrent_plugin PRIVATE ${VLC_LIBRARIES} LibtorrentRasterbar::torrent-rasterbar)
    set_target_properties(access_bittorrent_plugin PROPERTIES SUFFIX ".dll")

    target_include_directories(overlay_plugin PRIVATE ${VLC_INCLUDE_DIRS} ${LibtorrentRasterbar_INCLUDE_DIRS})
    target_link_libraries(overlay_plugin PRIVATE ${VLC_LIBRARIES} LibtorrentRasterbar::torrent-rasterbar)
    set_target_properties(overlay_plugin PROPERTIES SUFFIX ".dll")
else()
    # Линковка для Linux/macOS
    target_link_libraries(access_bittorrent_plugin PUBLIC PkgConfig::VlcPlugin PkgConfig::LibtorrentRasterbar atomic)
    target_link_libraries(overlay_plugin PUBLIC PkgConfig::VlcPlugin PkgConfig::LibtorrentRasterbar atomic)
endif()


# --- Директории установки ---
find_program(VLC_EXECUTABLE NAMES vlc)
execute_process(
    COMMAND readlink -f ${VLC_EXECUTABLE}
    OUTPUT_VARIABLE VLC_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (VLC_PATH MATCHES "/snap/")
    set(VLC_IS_SNAP TRUE)
    set(VLC_PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/lib/vlc/plugins/video_filter") # Устанавливаем все в одну папку для snap
else ()
    execute_process(COMMAND pkg-config --variable=pluginsdir vlc OUTPUT_VARIABLE VLC_BASE OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT VLC_BASE)
        set(VLC_BASE "/usr/lib/vlc/plugins")
    endif()
    set(VLC_ACCESS_PLUGIN_DIR "${VLC_BASE}/access")
    set(VLC_VIDEO_FILTER_PLUGIN_DIR "${VLC_BASE}/video_filter")
endif()

# --- Установка плагинов ---
if (VLC_IS_SNAP)
    install(TARGETS access_bittorrent_plugin overlay_plugin LIBRARY DESTINATION ${VLC_PLUGIN_INSTALL_DIR})
else()
    install(TARGETS access_bittorrent_plugin LIBRARY DESTINATION ${VLC_ACCESS_PLUGIN_DIR})
    install(TARGETS overlay_plugin LIBRARY DESTINATION ${VLC_VIDEO_FILTER_PLUGIN_DIR})
endif()


# --- post-install script ------------------------------------------------
install(CODE "
    execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory ${VLC_ACCESS_PLUGIN_DIR})
    execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory ${VLC_VIDEO_FILTER_PLUGIN_DIR})
    if (\"${VLC_IS_SNAP}\")
        # ... (логика для snap остается прежней)
    else ()
        message(STATUS \"Classic VLC → plugins copied to respective directories\")
        execute_process(
            COMMAND vlc-cache-gen ${VLC_BASE}
            RESULT_VARIABLE IGNORE ERROR_QUIET
        )
    endif()
")

# --- Настройки компиляции ---
# --- ИЗМЕНЕНИЕ: ДОБАВЛЕНЫ УНИКАЛЬНЫЕ ОПРЕДЕЛЕНИЯ MODULE_STRING ---
target_compile_definitions(
    access_bittorrent_plugin
    PRIVATE
        -DMODULE_STRING=\"bittorrent\" # Уникальное имя для плагина доступа
        -D__PLUGIN__
        -DPACKAGE=\"vlc-bittorrent\"
)
target_compile_definitions(
    overlay_plugin
    PRIVATE
        -DMODULE_STRING=\"bittorrent_overlay\" # Уникальное имя для плагина оверлея
        -D__PLUGIN__
        -DPACKAGE=\"vlc-bittorrent\"
)
# --- КОНЕЦ ИЗМЕНЕНИЯ ---

# --- Общие свойства для обоих плагинов ---
foreach(PLUGIN_TARGET access_bittorrent_plugin overlay_plugin)
    target_include_directories(
        ${PLUGIN_TARGET}
        PRIVATE
            ${CMAKE_BINARY_DIR}
    )
    target_compile_features(
        ${PLUGIN_TARGET}
        PUBLIC
            cxx_std_14
    )
    set_target_properties(
        ${PLUGIN_TARGET}
        PROPERTIES
            CXX_STANDARD 14
            CXX_STANDARD_REQUIRED YES
            CXX_VISIBILITY_PRESET hidden
    )
endforeach()
