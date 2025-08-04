/*
 * src/interface.h
 *
 * Заголовочный файл для интерфейсного плагина
 */
#ifndef VLC_BITTORRENT_INTERFACE_H
#define VLC_BITTORRENT_INTERFACE_H

#include <vlc_common.h>
#include <vlc_interface.h>

// Объявления функций
int TorrentStatusInterfaceOpen(vlc_object_t* p_obj);
void TorrentStatusInterfaceClose(vlc_object_t* p_obj);

#endif // VLC_BITTORRENT_INTERFACE_H
