/*
 * src/module.cpp
 *
 * This file is part of vlc-bittorrent.
 *
 * vlc-bittorrent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vlc-bittorrent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vlc-bittorrent. If not, see <http://www.gnu.org/licenses/>.
 */
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_url.h>

#include "data.h"
#include "metadata.h"
#include "magnetmetadata.h"

#define DLDIR_CONFIG "bittorrent-downloads"

vlc_module_begin()
    set_shortname("BitTorrent")
    set_description("BitTorrent access and stream input")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    add_string(DLDIR_CONFIG, NULL, "Download directory", "Directory where files will be downloaded", false)
    add_shortcut("bittorrent", "bt")
    set_capability("access", 55) // выше, чем у file, но ниже, чем у http
    set_callbacks(DataOpen, DataClose)
    
    // Добавляем отдельный модуль для метаданных
    add_submodule()
        set_shortname("BitTorrent Meta")
        set_description("BitTorrent metadata reader")
        set_category(CAT_META)
        set_subcategory(SUBCAT_META_READER)
        set_capability("meta reader", 10)
        set_callbacks(MetadataRead, NULL)
    
    // Добавляем отдельный модуль для магнитных ссылок
    add_submodule()
        set_shortname("Magnet Meta")
        set_description("Magnet link metadata reader")
        set_category(CAT_META)
        set_subcategory(SUBCAT_META_READER)
        set_capability("meta reader", 10)
        set_callbacks(MagnetMetadataRead, NULL)
vlc_module_end()
