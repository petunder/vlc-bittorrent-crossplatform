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
 * Реализация потока с использованием механизма кеширования VLC.
 * Гибридный подход: блокировка на старте, асинхронное чтение после.
 *
 * Ключевые моменты:
 *  - НЕТ vlc_stream_Seek() по source в DataSeek().
 *  - В DataRead() НИКОГДА не возвращаем 0, кроме реального EOF.
 *  - Большой PTS delay для сетевой буферизации (>= 10s).
 *  - Пауза: STREAM_CAN_CONTROL_PACE=false → VLC сам перестаёт читать;
 *    торрент продолжает качать.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <memory>
#include <stdexcept>
#include <atomic>
#include <stdio.h>          // snprintf
#include <inttypes.h>       // PRIu64, PRId64

#include "vlc.h"
#include <vlc_variables.h>
#include "data.h"
#include "download.h"

struct data_sys {
    std::shared_ptr<Download> p_download;
    int i_file = 0;
    uint64_t i_pos = 0;

    libvlc_int_t* libvlc = nullptr;
    mtime_t       last_pub = 0;

    std::atomic<bool> is_initial_buffer_filled{false};

    // Для диагностики паузы (read() на это НЕ смотрит)
    std::atomic<bool> paused{false};
};

static ssize_t DataRead(stream_extractor_t* p_extractor, void* p_buf, size_t i_size) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s || !s->p_download) {
        return -1;
    }

    // EOF
    auto file_info = s->p_download->get_file(p_extractor->identifier);
    if (s->i_pos >= file_info.second) {
        return 0;
    }

    // Инициализация libVLC указателя
    if (s->libvlc == nullptr) {
        s->libvlc = p_extractor->obj.libvlc;
    }

    // Периодическая публикация реального статуса торрента
    mtime_t now = mdate();
    if (now - s->last_pub >= 500000) { // ~0.5 сек
        BtOverlayStatus st{};
        if (s->p_download->query_status(st)) {
            char ovbuf[256];
            // Скорости — реальные из libtorrent; прогресс — общий прогресс торрента
            snprintf(ovbuf, sizeof(ovbuf),
                     "[BT] D:%lld KiB/s  U:%lld KiB/s  Peers:%d  Progress:%.2f%%",
                     st.download_kib_s, st.upload_kib_s, st.peers, st.progress_pct);
            var_SetString(VLC_OBJECT(s->libvlc), "bt_overlay_text", ovbuf);
        }
        s->last_pub = now;
    }

    // Основное чтение
    try {
        ssize_t ret = s->p_download->read(s->i_file, (int64_t)s->i_pos,
                                          static_cast<char*>(p_buf), i_size);
        if (ret > 0) {
            s->i_pos += ret;
            if (!s->is_initial_buffer_filled.load()) {
                s->is_initial_buffer_filled = true;
                msg_Dbg(p_extractor, "Initial buffer filled, playback starting.");
            }
        }
        return ret;
    } catch (const std::runtime_error& e) {
        msg_Err(p_extractor, "Fatal error during read (timeout?): %s", e.what());
        return -1;
    }
}

static int DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    msg_Dbg(p_extractor, "Seek requested to position %" PRIu64, i_pos);

    if (vlc_stream_Seek(p_extractor->source, i_pos)) {
        return VLC_EGENERIC;
    }

    s->i_pos = i_pos;
    s->is_initial_buffer_filled = false;
    msg_Dbg(p_extractor, "Resetting buffer status for seeking.");

    if (s->p_download) {
        s->p_download->set_piece_priority(s->i_file, (int64_t)s->i_pos, 50 * 1024 * 1024, 7);
    }

    return VLC_SUCCESS;
}

static int DataControl(stream_extractor_t* p_extractor, int i_query, va_list args) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s || !s->p_download) return VLC_EGENERIC;

    switch (i_query) {
        case STREAM_CAN_SEEK:
            *va_arg(args, bool*) = true;
            return VLC_SUCCESS;

        case STREAM_GET_SIZE:
            *va_arg(args, uint64_t*) = s->p_download->get_file(p_extractor->identifier).second;
            return VLC_SUCCESS;

        case STREAM_GET_PTS_DELAY: {
            int64_t caching_ms = var_InheritInteger(p_extractor, "network-caching");
            if (caching_ms < 10000) caching_ms = 10000;
            int64_t delay_us = caching_ms * 1000;
            *va_arg(args, int64_t*) = delay_us;
            msg_Dbg(p_extractor, "Reporting PTS delay of %" PRId64 " us for network caching.", delay_us);
            return VLC_SUCCESS;
        }

        case STREAM_CAN_PAUSE:
            *va_arg(args, bool*) = true;
            return VLC_SUCCESS;

        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool*) = true;   // критично оставить true, как в «рабочем» коде
            return VLC_SUCCESS;

        case STREAM_SET_PAUSE_STATE: {
            bool b_pause = *va_arg(args, bool*);
            s->paused = b_pause;           // только для логов/диагностики
            msg_Dbg(p_extractor, "Pause state set to: %s", b_pause ? "paused" : "playing");
            // Ничего больше не делаем: VLC сам перестаёт вызывать pf_read во время паузы.
            return VLC_SUCCESS;
        }

        default:
            return vlc_stream_vaControl(p_extractor->source, i_query, args);
    }
}

int DataOpen(vlc_object_t* p_obj) {
    // Инициализация переменной на libVLC
    libvlc_int_t *libvlc = p_obj->obj.libvlc;
    var_Create(VLC_OBJECT(libvlc), "bt_overlay_text", VLC_VAR_STRING);
    var_SetString(VLC_OBJECT(libvlc), "bt_overlay_text", "[BT] Starting...");

    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0) return VLC_EGENERIC;

    auto s = new (std::nothrow) data_sys();
    if (!s) return VLC_ENOMEM;

    try {
        s->p_download = Download::get_download(md.get(), (size_t)mdsz,
                                               get_download_directory(p_obj),
                                               get_keep_files(p_obj));
        s->i_file = s->p_download->get_file(p_extractor->identifier).first;
    } catch (const std::runtime_error& e) {
        msg_Err(p_extractor, "Failed to add download: %s", e.what());
        delete s;
        return VLC_EGENERIC;
    }

    p_extractor->p_sys = s;
    p_extractor->pf_read = DataRead;
    p_extractor->pf_seek = DataSeek;
    p_extractor->pf_control = DataControl;

    msg_Dbg(p_obj, "BitTorrent data stream opened successfully.");
    return VLC_SUCCESS;
}

void DataClose(vlc_object_t* p_obj) {
    // Очистим строку оверлея
    libvlc_int_t *libvlc = p_obj->obj.libvlc;
    var_SetString(VLC_OBJECT(libvlc), "bt_overlay_text", "");

    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (s) {
        delete s;
    }
    p_extractor->p_sys = nullptr;
    msg_Dbg(p_obj, "BitTorrent data stream closed.");
}
