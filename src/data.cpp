/*
 * src/data.cpp
 *
 * Этот модуль реализует логику потока данных (stream_extractor) для VLC.
 * Его роль в проекте — быть "сигнальным механизмом" и обеспечивать
 * корректную работу с I/O операциями VLC.
 *
 * 1.  При открытии (DataOpen) он регистрирует активный торрент через
 *     переменную VLC.
 * 2.  Предоставляет VLC функции для чтения (DataRead) и управления (DataControl).
 * 3.  **Ключевая роль:** Реализует **правильную** функцию перемотки (DataSeek).
 *     Она не только обновляет внутреннюю позицию, но и:
 *      a) Вызывает vlc_stream_Seek(), чтобы уведомить ядро VLC о перемотке
 *         и сбросить внутренние часы (reference clock).
 *      b) Вызывает set_piece_priority(), чтобы приказать libtorrent немедленно
 *         начать загрузку данных с нового места с наивысшим приоритетом.
 * 4.  При закрытии (DataClose) он очищает переменную активного торрента.
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
// --- НАЧАЛО ИЗМЕНЕНИЯ: КОНСТАНТА ДЛЯ ОПТИМИЗАЦИИ ПЕРЕМОТКИ ---
#define SEEK_READAHEAD_SIZE (10 * 1024 * 1024) // 10 MB
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

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

// --- НАЧАЛО ИЗМЕНЕНИЯ: ПОЛНАЯ И ПРАВИЛЬНАЯ РЕАЛИЗАЦИЯ ПЕРЕМОТКИ ---
static int DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    msg_Dbg(p_extractor, "Seek requested to position %" PRIu64, i_pos);

    // ШАГ 1: Сообщаем нижележащему потоку VLC о перемотке.
    // Это ключевой вызов, который сбрасывает часы (reference clock) и буферы.
    if (vlc_stream_Seek(p_extractor->source, i_pos)) {
        msg_Err(p_extractor, "Underlying stream seek failed");
        return VLC_EGENERIC;
    }

    // ШАГ 2: Обновляем нашу внутреннюю позицию.
    s->i_pos = i_pos;

    // ШАГ 3 (Оптимизация): Приказываем libtorrent немедленно скачать
    // данные с нового места с наивысшим приоритетом.
    if (s->p_download) {
        msg_Dbg(p_extractor, "Setting piece priority for seeking");
        s->p_download->set_piece_priority(s->i_file, (int64_t)s->i_pos, SEEK_READAHEAD_SIZE, 7);
    }

    return VLC_SUCCESS;
}
// --- КОНЕЦ ИЗМЕНЕНИЯ ---

static int DataControl(stream_extractor_t* p_extractor, int i_query, va_list args) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s || !s->p_download)
        return VLC_EGENERIC;

    switch (i_query) {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_FASTSEEK:
            *va_arg(args, bool*) = true;
            break;

        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool*) = true;
            break;

        case STREAM_GET_PTS_DELAY: {
            int64_t nc    = var_InheritInteger(p_extractor, "network-caching");
            int64_t delay = (nc > MIN_CACHING_TIME ? nc : MIN_CACHING_TIME) * 1000LL;
            *va_arg(args, int64_t*) = delay;
            break;
        }

        case STREAM_SET_PAUSE_STATE:
            // здесь ничего не делаем, но и не возвращаем ошибку
            break;

        case STREAM_GET_SIZE:
            *va_arg(args, uint64_t*) =
                s->p_download->get_file(p_extractor->identifier).second;
            break;

        default:
            // Пробрасываем все остальные запросы (включая запросы по тайм-кодам)
            return vlc_stream_vaControl(p_extractor->source, i_query, args);
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
