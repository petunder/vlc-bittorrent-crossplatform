// src/data.cpp
/*
Copyright 2016 Johan Gunnarsson <johan.gunnarsson@gmail.com>

This file is part of vlc-bittorrent.

vlc-bittorrent is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

vlc-bittorrent is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with vlc-bittorrent.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <memory>
#include <stdexcept>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_variables.h>

#include "data.h"
#include "download.h"
#include "session.h"
#include "vlc.h"

#define MIN_CACHING_TIME (10000)
#define D(x)

struct data_sys {
    std::shared_ptr<Download> p_download;
    // текущий файл и позиция
    int       i_file    = 0;
    uint64_t  i_pos     = 0;
    // VLC-объект для обновления title
    vlc_object_t* p_input = nullptr;
}

static ssize_t
DataRead(stream_extractor_t* p_extractor, void* p_buf, size_t i_size)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    if (!p_sys || !p_sys->p_download)
        return -1;

    try {
        ssize_t ret = p_sys->p_download->read(p_sys->i_file,
                                              (int64_t)p_sys->i_pos,
                                              static_cast<char*>(p_buf),
                                              i_size);
        if (ret > 0)
            p_sys->i_pos += ret;
        else if (ret < 0)
            return 0;
        return ret;
    } catch (const std::runtime_error& e) {
        msg_Dbg(p_extractor, "Read failed: %s", e.what());
    }
    return -1;
}

static int
DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos)
{
    D(printf("%s:%d: %s(%lu)\n", __FILE__, __LINE__, __func__, i_pos));
    if (!p_extractor || !p_extractor->p_sys)
        return VLC_EGENERIC;
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    p_sys->i_pos = i_pos;
    return VLC_SUCCESS;
}

static int
DataControl(stream_extractor_t* p_extractor, int i_query, va_list args)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    if (!p_extractor || !p_extractor->p_sys)
        return VLC_EGENERIC;
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    if (!p_sys->p_download)
        return VLC_EGENERIC;

    switch (i_query) {
        case STREAM_CAN_SEEK:
            *va_arg(args, bool*) = true;
            break;
        case STREAM_CAN_FASTSEEK:
            *va_arg(args, bool*) = true;
            break;
        case STREAM_CAN_PAUSE:
            *va_arg(args, bool*) = true;
            break;
        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool*) = true;
            break;
        case STREAM_GET_PTS_DELAY:
            *va_arg(args, int64_t*) =
                INT64_C(1000) * std::max(MIN_CACHING_TIME,
                    var_InheritInteger(p_extractor, "network-caching"));
            break;
        case STREAM_SET_PAUSE_STATE:
            break;
        case STREAM_GET_SIZE:
            *va_arg(args, uint64_t*) =
                p_sys->p_download->get_file(p_extractor->identifier).second;
            break;
        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int
DataOpen(vlc_object_t* p_obj)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    auto* p_extractor = static_cast<stream_extractor_t*>(p_obj);
    msg_Info(p_extractor, "Opening %s", p_extractor->identifier);

    // читаем метаданные
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0)
        return VLC_EGENERIC;

    // создаём системную часть
    auto* p_sys = new data_sys();
    p_sys->p_input = p_obj;

    try {
        p_sys->p_download = Download::get_download(
            md.get(), (size_t)mdsz,
            get_download_directory(p_obj),
            get_keep_files(p_obj)
        );
        msg_Dbg(p_extractor, "Added download");
        p_sys->i_file = p_sys->p_download
                           ->get_file(p_extractor->identifier)
                           .first;
        msg_Dbg(p_extractor, "Found file %d", p_sys->i_file);
    } catch (const std::runtime_error& e) {
        msg_Err(p_extractor, "Failed to add download: %s", e.what());
        delete p_sys;
        return VLC_EGENERIC;
    }

    p_extractor->p_sys     = p_sys;
    p_extractor->pf_read   = DataRead;
    p_extractor->pf_seek   = DataSeek;
    p_extractor->pf_control= DataControl;

    // подписываемся на статусы
    Session::get()->add_status_listener(p_sys->p_input);

    return VLC_SUCCESS;
}

void
DataClose(vlc_object_t* p_obj)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    auto* p_extractor = static_cast<stream_extractor_t*>(p_obj);
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    if (!p_sys)
        return;

    // отписываемся от статусов
    Session::get()->remove_status_listener(p_sys->p_input);

    delete p_sys;
    p_extractor->p_sys = nullptr;
}
