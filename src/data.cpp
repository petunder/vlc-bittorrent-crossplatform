/*
 * src/data.cpp
 *
 * Этот модуль реализует логику потока данных (stream_extractor) для VLC.
 * Его роль в проекте — быть "сигнальным механизмом":
 *
 * 1.  При открытии (DataOpen) он получает торрент-хэндл и **устанавливает
 *     глобальную переменную VLC "bittorrent-active-hash"**, содержащую
 *     infohash текущего торрента.
 * 2.  Предоставляет VLC стандартные функции для чтения данных (DataRead),
 *     перемотки (DataSeek) и управления (DataControl).
 * 3.  При закрытии (DataClose), когда VLC выгружает этот модуль, он
 *     **очищает переменную "bittorrent-active-hash"**.
 *
 * Таким образом, этот короткоживущий модуль просто сообщает долгоживущему
 * модулю `interface`, за каким торрентом нужно следить.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <memory>
#include <stdexcept>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_variables.h>

#include "data.h"
#include "download.h"
#include "vlc.h"

#define MIN_CACHING_TIME 10000

struct data_sys {
    std::shared_ptr<Download> p_download;
    int i_file = 0;
    uint64_t i_pos = 0;
};

static ssize_t DataRead(stream_extractor_t* p_extractor, void* p_buf, size_t i_size) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s || !s->p_download) return -1;
    try {
        ssize_t ret = s->p_download->read(s->i_file, (int64_t)s->i_pos, static_cast<char*>(p_buf), i_size);
        if (ret > 0) s->i_pos += ret;
        else if (ret < 0) return 0;
        return ret;
    } catch (const std::runtime_error& e) {
        msg_Dbg(p_extractor, "Read failed: %s", e.what());
    }
    return -1;
}

static int DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    s->i_pos = i_pos;
    return VLC_SUCCESS;
}

static int DataControl(stream_extractor_t* p_extractor, int i_query, va_list args) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s->p_download) return VLC_EGENERIC;
    switch (i_query) {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_FASTSEEK:
        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool*) = true;
            break;
        case STREAM_GET_PTS_DELAY: {
            int64_t nc = var_InheritInteger(p_extractor, "network-caching");
            int64_t delay = (nc > MIN_CACHING_TIME ? nc : MIN_CACHING_TIME) * 1000LL;
            *va_arg(args, int64_t*) = delay;
            break;
        }
        case STREAM_SET_PAUSE_STATE: break;
        case STREAM_GET_SIZE:
            *va_arg(args, uint64_t*) = s->p_download->get_file(p_extractor->identifier).second;
            break;
        default: return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int DataOpen(vlc_object_t* p_obj) {
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0) return VLC_EGENERIC;
    
    auto* s = new data_sys();
    try {
        s->p_download = Download::get_download(md.get(), (size_t)mdsz, get_download_directory(p_obj), get_keep_files(p_obj));
        s->i_file = s->p_download->get_file(p_extractor->identifier).first;

        std::string infohash = s->p_download->get_infohash();
        var_SetString(p_obj, "bittorrent-active-hash", infohash.c_str());
        msg_Dbg(p_obj, "Set active torrent hash: %s", infohash.c_str());

    } catch (const std::runtime_error& e) {
        msg_Err(p_extractor, "Failed to add download: %s", e.what());
        delete s;
        return VLC_EGENERIC;
    }
    
    p_extractor->p_sys = s;
    p_extractor->pf_read = DataRead;
    p_extractor->pf_seek = DataSeek;
    p_extractor->pf_control = DataControl;
    
    return VLC_SUCCESS;
}

void DataClose(vlc_object_t* p_obj) {
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    var_SetString(p_obj, "bittorrent-active-hash", "");
    msg_Dbg(p_obj, "Cleared active torrent hash.");
    
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s) return;
    
    delete s;
    p_extractor->p_sys = nullptr;
}
