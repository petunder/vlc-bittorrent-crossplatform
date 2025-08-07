/*
 * module.cpp – корневой описатель VLC-плагина «vlc-bittorrent»
 *
 * Содержит один главный модуль категории INPUT (stream_directory)
 * и три под-модуля:
 *   • stream_extractor  – чтение данных торрент-файла;
 *   • access            – извлечение .torrent из magnet-ссылки;
 *   • interface         – ЛОГ-гер статуса (выводит в msg_Dbg).
 *
 * Copyright (C) 2016-2025
 *   Johan Gunnarsson <johan.gunnarsson@gmail.com>,
 *   другие авторы — смотрите git-историю проекта.
 *
 * Распространяется под лицензией GPL-3.0-or-later.
 */

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include "vlc.h"              // единый заголовок с include-порядком VLC

#include "metadata.h"         // stream_directory
#include "data.h"             // stream_extractor
#include "magnetmetadata.h"   // access
#include "overlay.h"          // overlay (Logger)

// ────────────────────────────────────────────────────────────
//                    Описание VLC-модуля
// ────────────────────────────────────────────────────────────
vlc_module_begin()
    /* ────────────────── главный модуль ─────────────────── */
    set_shortname("bittorrent")
    set_category   (CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_STREAM_FILTER)
    set_description("BitTorrent metadata access")
    set_capability("stream_directory", 99)
    set_callbacks  (MetadataOpen, NULL)

    /* Конфигурация загрузок */
    add_directory(DLDIR_CONFIG, NULL, "Downloads",
                  "Directory where VLC will put downloaded files.", false)
    add_bool(KEEP_CONFIG, false, "Don't delete files",
             "Don't delete files after download.", true)

    /* ──────────────── под-модуль: stream_extractor ─────────────── */
    add_submodule()
        set_description("BitTorrent data access")
        set_capability("stream_extractor", 99)
        set_callbacks(DataOpen, DataClose)

    /* ──────────────── под-модуль: access (magnet) ──────────────── */
    add_submodule()
        set_description("BitTorrent magnet metadata access")
        set_capability("access", 60)
        add_shortcut("file", "magnet")
        set_callbacks(MagnetMetadataOpen, MagnetMetadataClose)

vlc_module_end()
