/*
 * gen_mpris IPC protocol
 *
 * Shared between the PE plugin (gen_mpris.dll) and the native
 * helper process (gen_mpris_host).
 *
 * Transport: TCP 127.0.0.1:39158
 * Message format: [uint8 type][uint16 payload_len (LE)][payload]
 */

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>

#define MPRIS_IPC_PORT     39158
#define MPRIS_IPC_ADDR     "127.0.0.1"

/* Message header */
#pragma pack(push, 1)

struct ipc_header {
    uint8_t  type;
    uint16_t payload_len;
};

#pragma pack(pop)

/* Message types */
#define MSG_STATE_UPDATE   0x01  /* DLL -> Host: playback state */
#define MSG_COMMAND        0x02  /* Host -> DLL: media command */
#define MSG_HEARTBEAT      0x03  /* Both directions: keepalive */
#define MSG_QUIT           0x04  /* DLL -> Host: shutdown */

/* Playback status values */
#define PLAYBACK_STOPPED   0
#define PLAYBACK_PLAYING   1
#define PLAYBACK_PAUSED    3

/* Command types */
#define CMD_NONE           0
#define CMD_PLAY           1
#define CMD_PAUSE          2
#define CMD_PLAYPAUSE      3
#define CMD_STOP           4
#define CMD_NEXT           5
#define CMD_PREV           6
#define CMD_SEEK           7
#define CMD_SETVOL         8

#pragma pack(push, 1)

struct mpris_state {
    uint8_t  playback_status;   /* PLAYBACK_STOPPED/PLAYING/PAUSED */
    int32_t  position_ms;
    int32_t  length_ms;
    uint8_t  volume;            /* 0-255 */
    char     title[512];        /* UTF-8 */
    char     artist[256];       /* UTF-8 */
};

struct mpris_command {
    uint8_t  type;              /* CMD_* */
    int32_t  param;             /* seek position (ms) or volume (0-255) */
};

#pragma pack(pop)

#endif /* IPC_PROTOCOL_H */
