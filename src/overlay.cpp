/*****************************************************************************
 * overlay.cpp — Видеофильтр для отображения статуса BitTorrent
 * Copyright 2025 petunder
 *
 * --- РОЛЬ ФАЙЛА В ПРОЕКТЕ ---
 * Этот файл реализует модуль видеофильтра (`vfilter`). Его единственная
 * задача — отображать оверлей со статусом загрузки поверх видео.
 *
 * --- АРХИТЕКТУРНОЕ РЕШЕНИЕ ---
 * В отличие от предыдущей неверной реализации (interface), этот модуль
 * правильно интегрируется в видео-конвейер VLC.
 *
 * 1.  **Тип модуля:** Объявлен как `video_filter`, а не `interface`. Это
 *     позволяет ему работать одновременно со стандартным интерфейсом Qt.
 *
 * 2.  **Активация:** Фильтр активируется автоматически из `data.cpp` при
 *     открытии торрент-потока.
 *
 * 3.  **Логика:** При создании фильтра (`Open`) запускается класс
 *     `TorrentStatusLogger`, который в отдельном потоке получает данные от
 *     libtorrent и пишет их в FIFO-канал для оверлея.
 *
 * 4.  **Обработка видео:** Основная функция фильтра (`Filter`) просто
 *     пропускает видеокадры дальше без изменений. Его существование
 *     нужно лишь для того, чтобы удерживать `TorrentStatusLogger` в памяти.
 *
 * Этот подход является каноническим для создания оверлеев в VLC.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "overlay.h"
#include "session.h"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/sha1_hash.hpp>

#include <mutex>
#include <vector>
#include <string>
#include <atomic>

// Объявляем нашу главную функцию-фильтр
extern "C" picture_t* Filter(filter_t* p_filter, picture_t* p_pic);

namespace { // Скрываем детали реализации

static std::string sha1_to_hex(const lt::sha1_hash& h) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(40);
    for (uint8_t b : h) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

class StatusProvider final : public Alert_Listener {
public:
    StatusProvider() {
        Session::get()->register_alert_listener(this);
    }
    ~StatusProvider() override {
        Session::get()->unregister_alert_listener(this);
    }

    void handle_alert(lt::alert* a) override {
        if (auto* up = lt::alert_cast<lt::state_update_alert>(a)) {
            std::string status_text;
            for (auto const& st : up->status) {
#if LIBTORRENT_VERSION_NUM >= 20000
                auto const& ih = st.info_hashes.v1;
#else
                auto const& ih = st.info_hash;
#endif
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "[BT] D: %lld KiB/s | U: %lld KiB/s | Peers: %d | Progress: %.2f%%\n",
                    (long long)st.download_payload_rate/1024,
                    (long long)st.upload_payload_rate/1024,
                    st.num_peers,
                    st.progress*100.0f);
                status_text += buf;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status_text = status_text;
        }
    }

    std::string get_status() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_status_text;
    }

private:
    std::mutex m_mutex;
    std::string m_status_text;
};

} // конец анонимного пространства имен

struct filter_sys_t {
    StatusProvider* provider;
};

int Open(vlc_object_t* p_this)
{
    filter_t* p_filter = (filter_t*)p_this;
    auto* p_sys = new (std::nothrow) filter_sys_t();
    if (!p_sys) return VLC_ENOMEM;

    p_sys->provider = new (std::nothrow) StatusProvider();
    if (!p_sys->provider) {
        delete p_sys;
        return VLC_ENOMEM;
    }
    
    p_filter->p_sys = p_sys;
    p_filter->pf_video_filter = Filter;
    
    msg_Dbg(p_filter, "BitTorrent overlay filter created");
    return VLC_SUCCESS;
}

void Close(vlc_object_t* p_this)
{
    filter_t* p_filter = (filter_t*)p_this;
    auto* p_sys = (filter_sys_t*)p_filter->p_sys;
    if (p_sys) {
        delete p_sys->provider;
        delete p_sys;
    }
    msg_Dbg(p_filter, "BitTorrent overlay filter destroyed");
}

picture_t* Filter(filter_t* p_filter, picture_t* p_pic)
{
    if (!p_pic) return nullptr;

    auto* p_sys = (filter_sys_t*)p_filter->p_sys;
    std::string text = p_sys->provider->get_status();

    if (text.empty()) {
        return p_pic;
    }

    subpicture_t* p_subpicture = filter_NewSubpicture(p_filter);
    if (!p_subpicture) {
        return p_pic;
    }
    
    text_segment_t* p_segment = text_segment_New(text.c_str());
    if (!p_segment) {
        subpicture_Delete(p_subpicture);
        return p_pic;
    }
    
    subpicture_region_t* p_region = subpicture_region_New(&p_pic->format);
    if (!p_region) {
        text_segment_Delete(p_segment);
        subpicture_Delete(p_subpicture);
        return p_pic;
    }

    p_region->i_x = 20;
    p_region->i_y = 20;
    p_region->p_text = p_segment;

    // --- ИЗМЕНЕНИЕ 1: Правильное имя поля ---
    // В VLC 3.x поле называется p_region, а не p_first_region
    p_subpicture->p_region = p_region;

    // --- ИЗМЕНЕНИЕ 2: Правильное количество аргументов ---
    // picture_BlendSubpicture требует ТРИ аргумента: картинку, фильтр и субтитр
    picture_BlendSubpicture(p_pic, p_filter, p_subpicture);

    // Очищаем subpicture ПОСЛЕ того, как он был использован для смешивания
    subpicture_Delete(p_subpicture);

    return p_pic;
}
