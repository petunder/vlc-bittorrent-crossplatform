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
)

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

# --- Начало патча для линковки ---
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
    target_link_libraries(
        access_bittorrent_plugin
        PUBLIC
            PkgConfig::VlcPlugin
            PkgConfig::LibtorrentRasterbar
    )
endif()
# --- Конец патча для линковки ---

set_target_properties(
    access_bittorrent_plugin
    PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
        CXX_VISIBILITY_PRESET hidden
)
