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
/*****************************************************************************
 * overlay.cpp — BitTorrent status overlay (SUB SOURCE) для VLC 3.0.x
 * Совместимо с VLC 3.0.18 (ABI 3_0_0f). Компилируется как C++.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

// Заголовки VLC (НЕ оборачивать в extern "C")
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_subpicture.h>
#include <vlc_text_style.h>

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_NAME 3_0_0f
#ifndef MODULE_STRING
# define MODULE_STRING "bittorrent_overlay"
#endif

#include "session.h" // Session / Alert_Listener
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <mutex>
#include <string>
#include <new>

namespace lt = libtorrent;

// ────────────────────────────────────────────────────────────
// Прототипы
// ────────────────────────────────────────────────────────────
static int           Open  (vlc_object_t *);
static void          Close (vlc_object_t *);
static subpicture_t* Render(filter_t *, mtime_t);

class StatusProvider final : public Alert_Listener {
public:
    explicit StatusProvider(vlc_object_t* logger) : logger_(logger) {
        Session::get()->register_alert_listener(this);
        // просим мгновенный статус — если сессия живая, пойдёт первый state_update_alert
        Session::get()->post_torrent_updates();
        msg_Dbg(logger_, "[overlay] StatusProvider registered; requested immediate torrent updates");
    }
    ~StatusProvider() override {
        Session::get()->unregister_alert_listener(this);
        msg_Dbg(logger_, "[overlay] StatusProvider unregistered");
    }

    void handle_alert(lt::alert* a) override {
        if (auto* up = lt::alert_cast<lt::state_update_alert>(a)) {
            std::string s; s.reserve(256);
            for (auto const& st : up->status) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "[BT] D:%lld KiB/s  U:%lld KiB/s  Peers:%d  Progress:%.2f%%",
                         (long long)(st.download_payload_rate / 1024),
                         (long long)(st.upload_payload_rate   / 1024),
                         st.num_peers,
                         st.progress * 100.0f);
                if (!s.empty()) s.push_back('\n');
                s += buf;
            }
            {
                std::lock_guard<std::mutex> lock(m_);
                text_ = std::move(s);
            }
            msg_Dbg(logger_, "[overlay] state_update_alert: %zu torrent(s)", up->status.size());
        }
        else if (auto* er = lt::alert_cast<lt::error_alert>(a)) {
            msg_Warn(logger_, "[overlay] libtorrent error: %s", er->message().c_str());
        }
    }

    std::string text() const {
        std::lock_guard<std::mutex> lock(m_);
        return text_;
    }

private:
    vlc_object_t*     logger_;
    mutable std::mutex m_;
    std::string        text_;
};

typedef struct filter_sys_t {
    StatusProvider* provider;
    text_style_t*   style;
    int             margin;
    unsigned        render_calls;
} filter_sys_t;

vlc_module_begin()
    set_shortname("BitTorrent Overlay")
    set_description("Display BitTorrent status as subpicture overlay")
    set_category   (CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_SUBPIC)
    set_capability ("sub source", 0)
    add_shortcut   (MODULE_STRING)
    set_callbacks  (Open, Close)
vlc_module_end()

static int Open(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;

    p_filter->pf_sub_source = Render;

    auto *p_sys = (filter_sys_t*)calloc(1, sizeof(filter_sys_t));
    if (!p_sys) return VLC_ENOMEM;

    p_sys->provider = new (std::nothrow) StatusProvider(VLC_OBJECT(p_filter));
    if (!p_sys->provider) { free(p_sys); return VLC_ENOMEM; }

    p_sys->style = text_style_New();
    if (!p_sys->style) {
        delete p_sys->provider;
        free(p_sys);
        return VLC_ENOMEM;
    }
    p_sys->style->i_font_size = 24;
    p_sys->margin = 12;
    p_sys->render_calls = 0;

    p_filter->p_sys = p_sys;
    msg_Dbg(p_filter, MODULE_STRING " sub source opened");
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    auto *p_sys = (filter_sys_t*)p_filter->p_sys;

    if (p_sys) {
        if (p_sys->style)    text_style_Delete(p_sys->style);
        if (p_sys->provider) delete p_sys->provider;
        free(p_sys);
        p_filter->p_sys = NULL;
    }
    msg_Dbg(p_filter, MODULE_STRING " sub source closed");
}

static subpicture_t* Render(filter_t *p_filter, mtime_t date)
{
    auto *p_sys = (filter_sys_t*)p_filter->p_sys;
    ++p_sys->render_calls;

    const std::string text = p_sys->provider->text();

    if (text.empty()) {
        if ((p_sys->render_calls % 120) == 0) // раз в 120 вызовов (~периодически)
            msg_Dbg(p_filter, "[overlay] Render: no text yet (waiting for libtorrent updates)");
        return NULL;
    }

    subpicture_t *spu = subpicture_New(NULL);
    if (!spu) {
        msg_Warn(p_filter, "[overlay] subpicture_New failed");
        return NULL;
    }

    video_format_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.i_chroma = VLC_CODEC_TEXT;
    fmt.i_width = fmt.i_visible_width = 1280;
    fmt.i_height = fmt.i_visible_height = 720;
    fmt.i_sar_num = fmt.i_sar_den = 1;

    subpicture_region_t *r = subpicture_region_New(&fmt);
    if (!r) { subpicture_Delete(spu); return NULL; }

    text_segment_t *seg = text_segment_New(text.c_str());
    if (!seg) { subpicture_region_Delete(r); subpicture_Delete(spu); return NULL; }

    if (p_sys->style) seg->style = text_style_Duplicate(p_sys->style);

    r->p_text = seg;
    r->i_align = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
    r->i_x = p_sys->margin;
    r->i_y = p_sys->margin;

    spu->p_region = r;
    spu->i_start  = date;
    spu->i_stop   = date + 500000; // ~0.5 сек
    spu->b_ephemer = true;

    return spu;
}
