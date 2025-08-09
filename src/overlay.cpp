/*****************************************************************************
 * overlay.cpp — BitTorrent status overlay (SUB SOURCE) для VLC 3.0.x
 * Совместимо с VLC 3.0.18 (ABI 3_0_0f). Компилируется как C++.
 * Источник текста берём из переменной libVLC: "bt_overlay_text".
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

// Заголовки VLC
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_subpicture.h>
#include <vlc_text_style.h>
#include <vlc_variables.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_NAME 3_0_0f
#ifndef MODULE_STRING
# define MODULE_STRING "bittorrent_overlay"
#endif

// ────────────────────────────────────────────────────────────
// Прототипы
// ────────────────────────────────────────────────────────────
static int           Open  (vlc_object_t *);
static void          Close (vlc_object_t *);
static subpicture_t* Render(filter_t *, mtime_t);

// ────────────────────────────────────────────────────────────
// Состояние фильтра
// ────────────────────────────────────────────────────────────
typedef struct filter_sys_t {
    libvlc_int_t*  p_libvlc;     // корневой объект (один на процесс)
    text_style_t*  style;
    int            margin;
    unsigned       render_calls;
} filter_sys_t;

// ────────────────────────────────────────────────────────────
// Описание модуля VLC как SUB SOURCE
// ────────────────────────────────────────────────────────────
vlc_module_begin()
    set_shortname("BitTorrent Overlay")
    set_description("Display BitTorrent status as subpicture overlay")
    set_category   (CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_SUBPIC)
    set_capability ("sub source", 0)
    add_shortcut   (MODULE_STRING)
    set_callbacks  (Open, Close)
vlc_module_end()

// ────────────────────────────────────────────────────────────
// Open: инициализация
// ────────────────────────────────────────────────────────────
static int Open(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;

    // Назначаем рендерер субкартинки
    p_filter->pf_sub_source = Render;

    // Выделяем состояние
    filter_sys_t* p_sys = (filter_sys_t*)calloc(1, sizeof(filter_sys_t));
    if (!p_sys) return VLC_ENOMEM;

    // Получаем libVLC через поле obj.libvlc
    p_sys->p_libvlc = p_filter->obj.libvlc;
    if (!p_sys->p_libvlc) {
        free(p_sys);
        return VLC_EGENERIC;
    }

    // Гарантируем наличие переменной (если не создана — создадим пустую строковую)
    var_Create(VLC_OBJECT(p_sys->p_libvlc), "bt_overlay_text", VLC_VAR_STRING);

    p_sys->style = text_style_New();
    if (!p_sys->style) {
        free(p_sys);
        return VLC_ENOMEM;
    }
    p_sys->style->i_font_size = 24;   // при желании подстройте
    p_sys->margin = 12;
    p_sys->render_calls = 0;

    p_filter->p_sys = p_sys;
    msg_Dbg(p_filter, MODULE_STRING " sub source opened (using libVLC var 'bt_overlay_text')");
    return VLC_SUCCESS;
}

// ────────────────────────────────────────────────────────────
// Close: освобождение
// ────────────────────────────────────────────────────────────
static void Close(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t* p_sys = (filter_sys_t*)p_filter->p_sys;

    if (p_sys) {
        if (p_sys->style) text_style_Delete(p_sys->style);
        p_filter->p_sys = NULL;
        free(p_sys);
    }
    msg_Dbg(p_filter, MODULE_STRING " sub source closed");
}

// ────────────────────────────────────────────────────────────
// Render: читаем текст из libVLC переменной и рисуем субкартинку
// ────────────────────────────────────────────────────────────
static subpicture_t* Render(filter_t *p_filter, mtime_t date)
{
    filter_sys_t* p_sys = (filter_sys_t*)p_filter->p_sys;
    ++p_sys->render_calls;

    // Берём текущий текст (var_GetString аллоцирует — нужно free())
    char* s = var_GetString(VLC_OBJECT(p_sys->p_libvlc), "bt_overlay_text");

    if (s == NULL || s[0] == '\0') {
        if ((p_sys->render_calls % 120) == 0)
            msg_Dbg(p_filter, "[overlay] no text in 'bt_overlay_text' yet");
        free(s);
        return NULL; // ничего не рисуем, пока нет данных
    }

    // Создаём субкартинку
    subpicture_t *spu = subpicture_New(NULL);
    if (!spu) {
        free(s);
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
    if (!r) { subpicture_Delete(spu); free(s); return NULL; }

    text_segment_t *seg = text_segment_New(s);
    free(s); // строку больше не держим
    if (!seg) { subpicture_region_Delete(r); subpicture_Delete(spu); return NULL; }

    if (p_sys->style)
        seg->style = text_style_Duplicate(p_sys->style);

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
