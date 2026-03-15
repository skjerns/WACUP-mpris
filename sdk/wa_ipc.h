/*
 * Minimal Winamp IPC definitions needed for gen_mpris.
 * Based on the public Winamp SDK (wa_ipc.h).
 */

#ifndef WINAMP_WA_IPC_H
#define WINAMP_WA_IPC_H

/* Main window message for Winamp IPC */
#define WM_WA_IPC (WM_USER)

/* IPC message IDs (sent as wParam to WM_WA_IPC, or lParam depending on call) */

/* SendMessage(hwnd, WM_WA_IPC, 0, IPC_ISPLAYING)
 * Returns: 1=playing, 3=paused, 0=stopped */
#define IPC_ISPLAYING        104

/* SendMessage(hwnd, WM_WA_IPC, 0, IPC_GETOUTPUTTIME)
 * Returns: position in milliseconds
 * SendMessage(hwnd, WM_WA_IPC, 1, IPC_GETOUTPUTTIME)
 * Returns: track length in seconds (-1 if not available) */
#define IPC_GETOUTPUTTIME    105

/* SendMessage(hwnd, WM_WA_IPC, 0, IPC_GETLISTPOS)
 * Returns: current playlist position (0-based) */
#define IPC_GETLISTPOS       125

/* SendMessage(hwnd, WM_WA_IPC, position, IPC_GETPLAYLISTTITLEW)
 * Returns: pointer to wide string title for playlist entry */
#define IPC_GETPLAYLISTTITLEW 213

/* SendMessage(hwnd, WM_WA_IPC, position, IPC_GETPLAYLISTFILEW)
 * Returns: pointer to wide string filename for playlist entry */
#define IPC_GETPLAYLISTFILEW 214

/* Volume: SendMessage(hwnd, WM_WA_IPC, vol, IPC_SETVOLUME)
 * vol: 0-255, or -666 to get current volume */
#define IPC_SETVOLUME        122

/* WM_COMMAND IDs for playback control */
#define WINAMP_CMD_PREV      40044
#define WINAMP_CMD_NEXT      40048
#define WINAMP_CMD_PLAY      40045
#define WINAMP_CMD_PAUSE     40046
#define WINAMP_CMD_STOP      40047

/* Seek: SendMessage(hwnd, WM_WA_IPC, position_ms, IPC_JUMPTOTIME) */
#define IPC_JUMPTOTIME       106

#endif /* WINAMP_WA_IPC_H */
