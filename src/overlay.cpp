/*****************************************************************************
 * overlay.cpp — Автономный видеофильтр для статуса BitTorrent
 * Copyright 2025 petunder
 *
 * --- РОЛЬ ФАЙЛА В ПРОЕКТЕ ---
 * Этот файл является полностью самодостаточным плагином VLC. Он компилируется
 * в отдельный файл (например, libbittorrent_overlay.so) и отвечает
 * исключительно за отображение оверлея.
 *
 * --- АРХИТЕКТУРНОЕ РЕШЕНИЕ ---
 * 1.  **Независимый модуль:** Файл содержит собственный описатель
 *     `vlc_module_begin()`, объявляя себя как `video_filter`. Это позволяет
 *     VLC находить и загружать его как отдельный, независимый плагин.
 *
 * 2.  **Общий синглтон:** Для получения данных о статусе торрента, этот
 *     плагин обращается к тому же синглтону `Session::get()`, что и
 *     основной плагин доступа к данным. Это позволяет им безопасно
 *     обмениваться информацией без прямых зависимостей.
 *
 * 3.  **Прямой рендеринг:** Плагин не использует внешние зависимости
 *     (вроде dynamicoverlay). Он самостоятельно рисует текст прямо на
 *     видеокадре с помощью API рендеринга субтитров VLC 3.x.
 *
 * Этот подход является каноническим, надежным и решает проблему, когда
 * VLC не мог найти под-модуль внутри уже загруженного плагина.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "vlc.h"
#include "session.h"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/sha1_hash.hpp>

#include <mutex>
#include <vector>
#include <string>
#include <atomic>

// --- Описатель модуля VLC ---
// Предварительно объявляем функции, чтобы использовать их в set_callbacks.
int Open(vlc_object_t*);
void Close(vlc_object_t*);
picture_t* Filter(filter_t*, picture_t*);

vlc_module_begin()
    set_shortname("bittorrent_overlay")
    set_description("BitTorrent status overlay")
    set_category   (CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video_filter", 10)
    set_callbacks(Open, Close)
vlc_module_end()
// --- Конец описателя ---


namespace { // Скрываем детали реализации в анонимном пространстве имен

static std::string sha1_to_hex(const lt::sha1_hash& h) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(40);
    for (uint8_t b : h) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

// Класс-поставщик статуса, который подписывается на алерты
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

} // Конец анонимного пространства имен

// Системная структура для хранения состояния нашего фильтра
struct filter_sys_t {
    StatusProvider* provider;
};

// Функция, вызываемая VLC при создании экземпляра фильтра
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
    // Регистрируем нашу функцию Filter как обработчик видео
    p_filter->pf_video_filter = Filter;
    
    msg_Dbg(p_filter, "BitTorrent overlay filter created successfully");
    return VLC_SUCCESS;
}

// Функция, вызываемая VLC при уничтожении экземпляра фильтра
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

// Главная функция-фильтр, вызываемая для каждого видеокадра
picture_t* Filter(filter_t* p_filter, picture_t* p_pic)
{
    if (!p_pic) return nullptr;

    auto* p_sys = (filter_sys_t*)p_filter->p_sys;
    std::string text = p_sys->provider->get_status();

    if (text.empty()) {
        return p_pic;
    }

    // Создаем "холст" для субтитров
    subpicture_t* p_subpicture = filter_NewSubpicture(p_filter);
    if (!p_subpicture) {
        return p_pic;
    }
    
    // Создаем текстовый сегмент
    text_segment_t* p_segment = text_segment_New(text.c_str());
    if (!p_segment) {
        subpicture_Delete(p_subpicture);
        return p_pic;
    }
    
    // Создаем регион, где будет отображаться текст
    subpicture_region_t* p_region = subpicture_region_New(&p_pic->format);
    if (!p_region) {
        text_segment_Delete(p_segment);
        subpicture_Delete(p_subpicture);
        return p_pic;
    }

    // Настраиваем регион
    p_region->i_x = 20;
    p_region->i_y = 20;
    p_region->p_text = p_segment;

    // Прикрепляем регион к холсту субтитров
    p_subpicture->p_region = p_region;

    // "Впечатываем" наш холст с текстом на видеокадр
    picture_BlendSubpicture(p_pic, p_filter, p_subpicture);

    // Очищаем память, выделенную для субтитра
    subpicture_Delete(p_subpicture);

    return p_pic;
}
