// src/data.cpp
/*
 * Модуль data.cpp
 *
 * Реализует stream_extractor для VLC-плагина vlc-bittorrent.
 * - Организует чтение данных из torrent (через Download).
 * - Обрабатывает запросы VLC: чтение, перемотку, управление потоком.
 * - Регистрирует слушателя статуса (Session) для обновления строки статуса VLC.
 *
 * Место в архитектуре:
 * Этот файл — «мост» между VLC (stream_extractor) и libtorrent (через Download и Session).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <memory>
#include <stdexcept>
#include <algorithm>           // для std::max
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_variables.h>

#include "data.h"
#include "download.h"
#include "session.h"
#include "vlc.h"

// Минимальное кэширование в мс
#define MIN_CACHING_TIME 10000
#define D(x)  // макрос для отладки, при желании можно раскомментировать

// Вспомогательная структура для хранения состояния потока
struct data_sys {
    std::shared_ptr<Download> p_download; // Объект загрузки torrent-файла
    int        i_file    = 0;            // Индекс текущего файла в торренте
    uint64_t   i_pos     = 0;            // Текущая позиция чтения внутри файла
    vlc_object_t* p_input = nullptr;     // VLC-объект для обновления переменных
};

// DataRead — вызывается VLC, когда нужно получить очередной блок данных
static ssize_t
DataRead(stream_extractor_t* p_extractor, void* p_buf, size_t i_size)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    if (!p_sys || !p_sys->p_download)
        return -1; // нет загрузчика — возвращаем ошибку

    try {
        // Читаем из торрента: файл i_file, смещение i_pos
        ssize_t ret = p_sys->p_download->read(
            p_sys->i_file,
            static_cast<int64_t>(p_sys->i_pos),
            static_cast<char*>(p_buf),
            i_size
        );
        if (ret > 0) {
            p_sys->i_pos += ret; // смещаем указатель
        } else if (ret < 0) {
            // ret < 0 трактуем как конец потока
            return 0;
        }
        return ret;
    } catch (const std::runtime_error& e) {
        msg_Dbg(p_extractor, "Read failed: %s", e.what());
    }
    return -1;
}

// DataSeek — VLC запрашивает переместить указатель чтения
static int
DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos)
{
    D(printf("%s:%d: %s(%lu)\n", __FILE__, __LINE__, __func__, i_pos));
    if (!p_extractor || !p_extractor->p_sys)
        return VLC_EGENERIC;
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    p_sys->i_pos = i_pos; // сохраняем новую позицию
    return VLC_SUCCESS;
}

// DataControl — отвечает на дополнительные запросы VLC о возможностях потока
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
            *va_arg(args, bool*) = true;   // поддерживаем seek
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
            // указываем задержку буфера на основе network-caching
            *va_arg(args, int64_t*) =
                1000LL * std::max(
                    MIN_CACHING_TIME,
                    var_InheritInteger(p_extractor, "network-caching")
                );
            break;
        case STREAM_SET_PAUSE_STATE:
            // управление паузой не обрабатываем
            break;
        case STREAM_GET_SIZE:
            // возвращаем полный размер выбранного файла
            *va_arg(args, uint64_t*) =
                p_sys->p_download
                     ->get_file(p_extractor->identifier)
                     .second;
            break;
        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

// DataOpen — инициализация потока: читаем метаданные, создаём Download,
// настраиваем колбеки, подписываемся на обновления статуса
int
DataOpen(vlc_object_t* p_obj)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    auto* p_extractor = static_cast<stream_extractor_t*>(p_obj);
    msg_Info(p_extractor, "Opening %s", p_extractor->identifier);

    // 1) Читаем метаданные торрента в буфер
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0)
        return VLC_EGENERIC;

    // 2) Создаём структуру состояния
    auto* p_sys = new data_sys();
    p_sys->p_input = p_obj;

    try {
        // Инициализируем загрузку (парсим метаданные, создаём сессию)
        p_sys->p_download = Download::get_download(
            md.get(), static_cast<size_t>(mdsz),
            get_download_directory(p_obj),
            get_keep_files(p_obj)
        );
        msg_Dbg(p_extractor, "Added download for %s", p_extractor->identifier);

        // Определяем, какой файл будем читать
        p_sys->i_file = p_sys->p_download
                           ->get_file(p_extractor->identifier)
                           .first;
        msg_Dbg(p_extractor, "Reading file index %d", p_sys->i_file);
    } catch (const std::runtime_error& e) {
        msg_Err(p_extractor, "Failed to add download: %s", e.what());
        delete p_sys;
        return VLC_EGENERIC;
    }

    // 3) Настраиваем функции обратного вызова для VLC
    p_extractor->p_sys      = p_sys;
    p_extractor->pf_read    = DataRead;
    p_extractor->pf_seek    = DataSeek;
    p_extractor->pf_control = DataControl;

    // 4) Подписываемся на обновления статуса для динамического title
    Session::get()->add_status_listener(p_sys->p_input);

    return VLC_SUCCESS;
}

// DataClose — завершаем поток: отписываем слушателя, освобождаем память
void
DataClose(vlc_object_t* p_obj)
{
    D(printf("%s:%d: %s()\n", __FILE__, __LINE__, __func__));
    auto* p_extractor = static_cast<stream_extractor_t*>(p_obj);
    auto* p_sys = static_cast<data_sys*>(p_extractor->p_sys);
    if (!p_sys)
        return;

    // Убираем слушателя, чтобы больше не обновлять закрытый плеер
    Session::get()->remove_status_listener(p_sys->p_input);

    delete p_sys;
    p_extractor->p_sys = nullptr;
}
