// src/overlay.h

#ifndef VLC_BITTORRENT_OVERLAY_H
#define VLC_BITTORRENT_OVERLAY_H

#include "vlc.h"

// Объявляем функции, которые будут вызываться VLC.
// extern "C" гарантирует C-совместимость имен функций, что важно для плагинов.
extern "C" {
    int Open(vlc_object_t*);
    void Close(vlc_object_t*);
    picture_t* Filter(filter_t*, picture_t*);
}

#endif // VLC_BITTORRENT_OVERLAY_H
