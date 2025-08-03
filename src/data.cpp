// src/data.cpp
/*
 * Модуль data.cpp
 *
 * Реализует stream_extractor для VLC-плагина vlc-bittorrent:
 *  - Чтение данных из торрента (через Download).
 *  - Обработка запросов VLC: чтение, seek, control.
 *  - Регистрация собственного слушателя алертов libtorrent
 *    (VLCStatusUpdater), который обновляет строку статуса VLC.
 */
#include <libtorrent/alert_types.hpp>  // metadata_progress_alert, metadata_received_alert

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <memory>
#include <stdexcept>
#include <algorithm>
#include <sstream>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_variables.h>

#include <libtorrent/alert_types.hpp>  // для state_update_alert
#include "data.h"
#include "download.h"
#include "session.h"
#include "vlc.h"

#define MIN_CACHING_TIME 10000

// МИНИМУМ: обработчик метадаты
class VLCMetadataUpdater : public Alert_Listener {
public:
    explicit VLCMetadataUpdater(vlc_object_t* input)
        : m_input(input) {}
    ~VLCMetadataUpdater() override = default;

    void handle_alert(lt::alert* a) override {
        // прогресс получения метаданных (magnet)
        if (auto* mp = lt::alert_cast<lt::metadata_progress_alert>(a)) {
            int p = int(mp->progress * 100);
            char buf[64];
            snprintf(buf, sizeof(buf), "Meta: %d%%", p);
            var_SetString(m_input, "title", buf);
        }
        // метаданные получены — можно переключиться на VLCStatusUpdater
        else if (auto* mr = lt::alert_cast<lt::metadata_received_alert>(a)) {
            var_SetString(m_input, "title", "Meta OK, starting...");
        }
    }
};

// Обёртка над Alert_Listener, которая на state_update_alert пишет в title
class VLCStatusUpdater : public Alert_Listener {
public:
    explicit VLCStatusUpdater(vlc_object_t* input)
        : m_input(input) {}
    ~VLCStatusUpdater() override = default;

    void handle_alert(libtorrent::alert* a) override {
        if (auto* su = lt::alert_cast<lt::state_update_alert>(a)) {
            for (auto& st : su->status) {
                std::ostringstream oss;
                oss << "BT: D=" << (st.download_rate/1000) << "kB/s"
                    << " U=" << (st.upload_rate/1000)   << "kB/s"
                    << " Peers=" << st.num_peers
                    << " [" << int(st.progress * 100)   << "%]";
                var_SetString(m_input, "title", oss.str().c_str());
            }
        }
    }

private:
    vlc_object_t* m_input;
};

// Хранит состояние потока и нашего слушателя
struct data_sys {
    std::shared_ptr<Download> p_download;  // сам торрент
    int        i_file   = 0;               // текущий файл
    uint64_t   i_pos    = 0;               // позиция внутри файла
    Alert_Listener* p_meta_listener = nullptr;    // обновления метадаты
    Alert_Listener* p_stat_listener = nullptr;    // обновления статистики
};

// DataRead — VLC просит следующий кусок данных
static ssize_t
DataRead(stream_extractor_t* p_extractor, void* p_buf, size_t i_size)
{
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s || !s->p_download)
        return -1;
    try {
        ssize_t ret = s->p_download->read(
            s->i_file,
            static_cast<int64_t>(s->i_pos),
            static_cast<char*>(p_buf),
            i_size
        );
        if (ret > 0)
            s->i_pos += ret;
        else if (ret < 0)
            return 0;  // EOF
        return ret;
    } catch (const std::runtime_error& e) {
        msg_Dbg(p_extractor, "Read failed: %s", e.what());
    }
    return -1;
}

// DataSeek — VLC перемещает указатель
static int
DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos)
{
    if (!p_extractor || !p_extractor->p_sys)
        return VLC_EGENERIC;
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    s->i_pos = i_pos;
    return VLC_SUCCESS;
}

// DataControl — VLC интересуется нашими возможностями
static int
DataControl(stream_extractor_t* p_extractor, int i_query, va_list args)
{
    if (!p_extractor || !p_extractor->p_sys)
        return VLC_EGENERIC;
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s->p_download)
        return VLC_EGENERIC;

    switch (i_query) {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_FASTSEEK:
        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool*) = true;
            break;
        case STREAM_GET_PTS_DELAY: {
            int64_t net_cache = var_InheritInteger(p_extractor, "network-caching");
            int64_t use_cache = net_cache > MIN_CACHING_TIME
                              ? net_cache : MIN_CACHING_TIME;
            *va_arg(args, int64_t*) = use_cache * 1000LL;
            break;
        }
        case STREAM_SET_PAUSE_STATE:
            break;
        case STREAM_GET_SIZE:
            *va_arg(args, uint64_t*) =
                s->p_download->get_file(p_extractor->identifier).second;
            break;
        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

// DataOpen — инициализация потока
int DataOpen(vlc_object_t* p_obj)
{
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    msg_Info(p_extractor, "Opening %s", p_extractor->identifier);

    // 1) Читаем .torrent или magnet-метаданные
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0)
        return VLC_EGENERIC;

    // 2) Готовим data_sys
    auto* s = new data_sys();
    try {
        s->p_download = Download::get_download(
            md.get(), static_cast<size_t>(mdsz),
            get_download_directory(p_obj),
            get_keep_files(p_obj)
        );
        s->i_file = s->p_download
                        ->get_file(p_extractor->identifier)
                        .first;
    } catch (const std::runtime_error& e) {
        msg_Err(p_extractor, "Failed to add download: %s", e.what());
        delete s;
        return VLC_EGENERIC;
    }

    // 3) Регистрация колбэков VLC
    p_extractor->p_sys      = s;
    p_extractor->pf_read    = DataRead;
    p_extractor->pf_seek    = DataSeek;
    p_extractor->pf_control = DataControl;

    // 4) Сначала слушаем прогресс метадаты (если magnet)
    s->p_meta_listener = new VLCMetadataUpdater(p_obj);
    Session::get()->register_alert_listener(s->p_meta_listener);

     // 5) А затем, после получения метаданных, статистику загрузки
    s->p_stat_listener = new VLCStatusUpdater(p_obj);
    Session::get()->register_alert_listener(s->p_stat_listener);

    return VLC_SUCCESS;
}

// DataClose — закрытие потока
void DataClose(vlc_object_t* p_obj)
{
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s) return;

    // Убираем слушатели
    if (s->p_meta_listener) {
        Session::get()->unregister_alert_listener(s->p_meta_listener);
        delete s->p_meta_listener;
    }
    if (s->p_stat_listener) {
        Session::get()->unregister_alert_listener(s->p_stat_listener);
        delete s->p_stat_listener;
    }

    delete s;
    p_extractor->p_sys = nullptr;
}
