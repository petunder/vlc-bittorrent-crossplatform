/*
 * src/data.cpp
 *
 * Этот модуль реализует логику потока данных (stream_extractor) для VLC.
 * Его задача - получать данные от libtorrent и публиковать строку статуса
 * в глобальную переменную VLC "bittorrent-status-string" для другого плагина.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <memory>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include <numeric>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_variables.h>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include "data.h"
#include "download.h"
#include "session.h"
#include "vlc.h"

#define MIN_CACHING_TIME 10000
static std::atomic<int> g_dht_nodes{0};

// Класс-слушатель для обновления статуса при получении метаданных
class VLCMetadataUpdater : public Alert_Listener {
public:
    explicit VLCMetadataUpdater(vlc_object_t* input) : m_input(input) {}
    ~VLCMetadataUpdater() override = default;
    void handle_alert(lt::alert* a) override {
        if (lt::alert_cast<lt::metadata_received_alert>(a)) {
            // Устанавливаем переменную на playlist
            playlist_t* p_playlist = pl_Get(m_input);
            if (p_playlist) {
                var_SetString(VLC_OBJECT(p_playlist), "bittorrent-status-string", "Metadata OK, starting download...");
            }
        } else if (auto* dht = lt::alert_cast<lt::dht_stats_alert>(a)) {
            int total_nodes = 0;
            for(const auto& bucket : dht->routing_table) {
                total_nodes += bucket.num_nodes;
            }
            g_dht_nodes = total_nodes;
        }
    }
private:
    vlc_object_t* m_input;
};

// Класс-слушатель для регулярного обновления строки статуса (скорость, пиры и т.д.)
class VLCStatusUpdater : public Alert_Listener {
public:
    explicit VLCStatusUpdater(vlc_object_t* input) : m_input(input) {}
    ~VLCStatusUpdater() override = default;
    void handle_alert(lt::alert* a) override {
        if (auto* su = lt::alert_cast<lt::state_update_alert>(a)) {
            if (su->status.empty()) return;
            const lt::torrent_status& st = su->status[0];
            std::ostringstream oss;
            oss << "[ D: " << (st.download_payload_rate / 1000) << " kB/s | "
                << "U: " << (st.upload_payload_rate / 1000)   << " kB/s | "
                << "Peers: " << st.num_peers << " (" << st.num_seeds << ") | "
                << "DHT: " << g_dht_nodes.load() << " | "
                << "Progress: " << static_cast<int>(st.progress * 100) << "% ]";
            
            // Устанавливаем переменную на playlist
            playlist_t* p_playlist = pl_Get(m_input);
            if (p_playlist) {
                var_SetString(VLC_OBJECT(p_playlist), "bittorrent-status-string", oss.str().c_str());
            }
        }
    }
private:
    vlc_object_t* m_input;
};

// Системная структура для хранения состояния stream_extractor
struct data_sys {
    std::shared_ptr<Download> p_download;
    int i_file = 0;
    uint64_t i_pos = 0;
    Alert_Listener* p_meta_listener = nullptr;
    Alert_Listener* p_stat_listener = nullptr;
};

// Функция чтения данных из торрента для VLC
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

// Функция перемотки
static int DataSeek(stream_extractor_t* p_extractor, uint64_t i_pos) {
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    s->i_pos = i_pos;
    return VLC_SUCCESS;
}

// Функция управления потоком (возможность перемотки, паузы и т.д.)
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

// Функция инициализации stream_extractor
int DataOpen(vlc_object_t* p_obj) {
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto md = std::make_unique<char[]>(0x100000);
    ssize_t mdsz = vlc_stream_Read(p_extractor->source, md.get(), 0x100000);
    if (mdsz < 0) return VLC_EGENERIC;
    auto* s = new data_sys();
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
    
    // Создаем глобальную переменную для хранения статуса на playlist
    playlist_t* p_playlist = pl_Get(p_obj);
    if (p_playlist) {
        var_Create(VLC_OBJECT(p_playlist), "bittorrent-status-string", VLC_VAR_STRING);
        var_SetString(VLC_OBJECT(p_playlist), "bittorrent-status-string", ""); // Инициализируем пустой строкой
    }

    s->p_meta_listener = new VLCMetadataUpdater(p_obj);
    Session::get()->register_alert_listener(s->p_meta_listener);
    s->p_stat_listener = new VLCStatusUpdater(p_obj);
    Session::get()->register_alert_listener(s->p_stat_listener);
    
    return VLC_SUCCESS;
}

// Функция закрытия и очистки ресурсов
void DataClose(vlc_object_t* p_obj) {
    auto* p_extractor = reinterpret_cast<stream_extractor_t*>(p_obj);
    auto* s = reinterpret_cast<data_sys*>(p_extractor->p_sys);
    if (!s) return;
    
    // Очищаем глобальную переменную статуса
    playlist_t* p_playlist = pl_Get(p_obj);
    if (p_playlist) {
        var_SetString(VLC_OBJECT(p_playlist), "bittorrent-status-string", "");
    }
    
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
