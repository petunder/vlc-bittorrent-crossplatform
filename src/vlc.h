#ifndef VLC_BITTORRENT_VLC_H
#define VLC_BITTORRENT_VLC_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"

// --- НАЧАЛО ИЗМЕНЕНИЙ ---
// Unified VLC Headers.
// Порядок имеет решающее значение.

// 1. vlc_common.h должен быть первым. Он определяет базовые типы (mtime_t, uint8_t) и макросы (VLC_API).
#include <vlc_common.h>

// 2. vlc_plugin.h - основной заголовок для плагинов. Он также подключает vlc_objects.h.
#include <vlc_plugin.h>

// 3. Остальные специфичные для плагина заголовки.
// Не нужно повторно включать vlc_objects.h, так как он уже есть в vlc_plugin.h.
#include <vlc_access.h>
#include <vlc_demux.h>
#include <vlc_dialog.h>
#include <vlc_es.h>
#include <vlc_fs.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_interface.h>
#include <vlc_interrupt.h>
#include <vlc_meta.h>
#include <vlc_playlist.h>
#include <vlc_stream.h>
#include <vlc_stream_extractor.h>
#include <vlc_threads.h>
#include <vlc_url.h>
#include <vlc_variables.h>
// --- КОНЕЦ ИЗМЕНЕНИЙ ---

#pragma GCC diagnostic pop

#define DLDIR_CONFIG "bittorrent-download-path"
#define KEEP_CONFIG "bittorrent-keep-files"

std::string
get_download_directory(vlc_object_t* p_this);

std::string
get_cache_directory(vlc_object_t* p_this);

bool
get_keep_files(vlc_object_t* p_this);

#endif
