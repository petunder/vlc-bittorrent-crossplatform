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
        interface.cpp
)

# линковка
if (WIN32)
    target_include_directories(
        access_bittorrent_plugin
        PRIVATE
            ${VLC_INCLUDE_DIRS}
            ${LibtorrentRasterbar_INCLUDE_DIRS}
    )
    target_link_libraries(
        access_bittorrent_plugin
        PRIVATE
            ${VLC_LIBRARIES}
            LibtorrentRasterbar::torrent-rasterbar
    )
    # Указываем правильное расширение для Windows
    set_target_properties(access_bittorrent_plugin PROPERTIES SUFFIX ".dll")
else()
    # --- НАЧАЛО ИЗМЕНЕНИЯ: ДОБАВЛЯЕМ `atomic` ---
    # Старый код не линковал libatomic, что вызывало ошибку "undefined symbol: __atomic_store".
    # target_link_libraries(
    #     access_bittorrent_plugin
    #     PUBLIC
    #         PkgConfig::VlcPlugin
    #         PkgConfig::LibtorrentRasterbar
    # )
    # Новый код добавляет `atomic` в список библиотек.
    target_link_libraries(
        access_bittorrent_plugin
        PUBLIC
            PkgConfig::VlcPlugin
            PkgConfig::LibtorrentRasterbar
            atomic
    )
    # --- КОНЕЦ ИЗМЕНЕНИЯ ---
endif()

# --- choose install dir -------------------------------------------------
find_program(VLC_EXECUTABLE NAMES vlc)
execute_process(
    COMMAND readlink -f ${VLC_EXECUTABLE}
    OUTPUT_VARIABLE VLC_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (VLC_PATH MATCHES "/snap/")
    # -------- snap -------------
    set(VLC_IS_SNAP TRUE)
    set(VLC_PLUGIN_DIR "$ENV{HOME}/.local/lib/vlc/plugins/access")
else ()
    # -------- classic ----------
    execute_process(
        COMMAND pkg-config --variable=pluginsdir vlc
        OUTPUT_VARIABLE VLC_BASE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (NOT VLC_BASE)
        # fallback для странных дистрибутивов
        set(VLC_BASE "/usr/lib/vlc/plugins")
    endif()
    set(VLC_PLUGIN_DIR "${VLC_BASE}/access")
endif()

# --- install target -----------------------------------------------------
install(
    TARGETS access_bittorrent_plugin
    LIBRARY DESTINATION ${VLC_PLUGIN_DIR}
)

# --- post-install script ------------------------------------------------
install(CODE "
    execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory ${VLC_PLUGIN_DIR})
    if (\"${VLC_IS_SNAP}\")
        message(STATUS \"VLC snap detected → plugin copied to ${VLC_PLUGIN_DIR}\")
        file(APPEND \$ENV{HOME}/.profile
             \"\\n# added by vlc-bittorrent\\nexport VLC_PLUGIN_PATH=${VLC_PLUGIN_DIR}:\$VLC_PLUGIN_PATH\\n\")
        message(STATUS \"Added VLC_PLUGIN_PATH to ~/.profile\")
    else ()
        message(STATUS \"Classic VLC → plugin copied to ${VLC_PLUGIN_DIR}\")
        execute_process(
            COMMAND vlc-cache-gen ${VLC_BASE}
            RESULT_VARIABLE IGNORE ERROR_QUIET
        )
    endif()
")

# инклюды для сборки плагина
target_include_directories(
    access_bittorrent_plugin
    PRIVATE
        ${CMAKE_BINARY_DIR}
)

target_compile_definitions(
    access_bittorrent_plugin
    PRIVATE
        -DMODULE_STRING=\"bittorrent\"
        -D__PLUGIN__
        -DPACKAGE=\"vlc-bittorrent\"
)

target_compile_features(
    access_bittorrent_plugin
    PUBLIC
        cxx_std_14
)

set_target_properties(
    access_bittorrent_plugin
    PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
        CXX_VISIBILITY_PRESET hidden
)
