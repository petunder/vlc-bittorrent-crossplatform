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
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <memory>
#include <stdexcept>
#include <atomic>

#include "vlc.h"
#include "data.h"
#include "download.h"

struct data_sys {
    std::shared_ptr<Download> p_download;
    int i_file = 0;
    uint64_t i_pos = 0;

    // Флаг, который гарантирует, что блокирующая логика
    // для начальной буферизации сработает только один раз.
    std::atomic<bool> is_initial_buffer_filled{false};
};

static ssize_t DataRead(stream_extractor_t* p_extractor, void* p_buf, size_t i_size) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s || !s->p_download) {
        return -1; // Критическая ошибка
    }

    // Проверка на конец файла
    auto file_info = s->p_download->get_file(p_extractor->identifier);
    if (s->i_pos >= file_info.second) {
        return 0; // EOF
    }

    // --- ГЛАВНАЯ ЛОГИКА ---
    // Функция Download::read теперь всегда блокирующая, с внутренним тайм-аутом.
    // На старте (когда is_initial_buffer_filled = false), VLC вызывает нас
    // из своего потока кеширования. Мы блокируемся и ждем первый кусок.
    // После старта, VLC вызывает нас из потока воспроизведения. Мы снова вызываем
    // тот же read(), но т.к. libtorrent уже качает куски в фоне,
    // он, скорее всего, вернет данные мгновенно или с минимальной задержкой.
    // Если данных нет, он вернет 0, что на этом этапе уже безопасно.
    try {
        ssize_t ret = s->p_download->read(s->i_file, (int64_t)s->i_pos,
                                          static_cast<char*>(p_buf), i_size);

        if (ret > 0) {
            s->i_pos += ret;
            // После первого успешного чтения, считаем, что буфер заполнен.
            if (!s->is_initial_buffer_filled.load()) {
                s->is_initial_buffer_filled = true;
                msg_Dbg(p_extractor, "Initial buffer filled, playback starting.");
            }
        }
        // Возвращаем ret как есть (может быть > 0 или 0, если данные еще не готовы)
        return ret;

    } catch (const std::runtime_error& e) {
        // Это произойдет в основном при тайм-ауте на старте.
        msg_Err(p_extractor, "Fatal error during read (timeout?): %s", e.what());
        return -1; // Сигнализируем VLC, что источник неисправен.
    }
}

static int DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    msg_Dbg(p_extractor, "Seek requested to position %" PRIu64, i_pos);

    if (vlc_stream_Seek(p_extractor->source, i_pos)) {
        return VLC_EGENERIC;
    }

    s->i_pos = i_pos;

    // После перемотки нам снова нужно будет дождаться данных с новой позиции.
    // Сбрасываем флаг, чтобы DataRead снова использовал блокирующую логику
    // для заполнения буфера с нового места.
    s->is_initial_buffer_filled = false;
    msg_Dbg(p_extractor, "Resetting buffer status for seeking.");

    // Даем команду libtorrent качать с нового места с высшим приоритетом.
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
            break;
        case STREAM_GET_SIZE:
            *va_arg(args, uint64_t*) = s->p_download->get_file(p_extractor->identifier).second;
            break;

        // --- КЛЮЧЕВОЙ МОМЕНТ АРХИТЕКТУРЫ ---
        case STREAM_GET_PTS_DELAY: {
            // Запрашиваем у VLC большое время на кеширование.
            // Это заставит его вызвать DataRead из своего потока буферизации,
            // что делает нашу блокировку на старте безопасной.
            int64_t caching_ms = var_InheritInteger(p_extractor, "network-caching");
            // Просим минимум 10 секунд (10000 мс) кеширования.
            int64_t delay_us = (caching_ms > 10000 ? caching_ms : 10000) * 1000LL;
            *va_arg(args, int64_t*) = delay_us;
            msg_Dbg(p_extractor, "Reporting PTS delay of %" PRId64 " us for network caching.", delay_us);
            break;
        }

        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool*) = true;
            break;

        default:
            return vlc_stream_vaControl(p_extractor->source, i_query, args);
    }
    return VLC_SUCCESS;
}

int DataOpen(vlc_object_t* p_obj) {
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0) return VLC_EGENERIC;

    auto s = new (std::nothrow) data_sys();
    if (!s) return VLC_ENOMEM;

    try {
        s->p_download = Download::get_download(md.get(), (size_t)mdsz, get_download_directory(p_obj), get_keep_files(p_obj));
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
    
    // Автоматически включаем оверлей и стандартный фильтр dynamicoverlay
    var_SetString(p_obj, "sub-filter", "bittorrent_overlay:dynamicoverlay");

    msg_Dbg(p_obj, "BitTorrent data stream opened successfully.");
    return VLC_SUCCESS;
}

void DataClose(vlc_object_t* p_obj) {
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (s) {
        delete s;
    }
    p_extractor->p_sys = nullptr;
    msg_Dbg(p_obj, "BitTorrent data stream closed.");
}
