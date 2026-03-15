/*
 * gen_mpris.dll — WACUP general-purpose plugin for MPRIS2 integration
 *
 * PE side: polls WACUP playback state via Winamp IPC, sends it over TCP
 * to the native gen_mpris_host process, and receives media commands back.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "sdk/gen.h"
#include "sdk/wa_ipc.h"
#include "ipc_protocol.h"

#define TIMER_ID        0xBEEF
#define TIMER_INTERVAL  250   /* ms */
#define CONNECT_RETRIES 10
#define CONNECT_DELAY   500   /* ms between retries */

/* Forward declarations */
static int  plugin_init(void);
static void plugin_config(void);
static void plugin_quit(void);

/* Plugin descriptor — exported */
static winampGeneralPurposePlugin plugin = {
    GPPHDR_VER,
    "MPRIS2 Integration (gen_mpris)",
    plugin_init,
    plugin_config,
    plugin_quit,
    0, /* hwndParent — filled by host */
    0  /* hDllInstance — filled by host */
};

static SOCKET     g_sock = INVALID_SOCKET;
static UINT_PTR   g_timer = 0;
static struct mpris_state g_last_state;
static int        g_connected = 0;
static int        g_was_connected = 0;   /* ever successfully connected */
static int        g_warned_disconnect = 0;
static int        g_reconnect_ticks = 0;

#define RECONNECT_TICKS 20  /* try reconnect every ~5s (20 × 250ms ticks) */

/* ── Networking helpers ────────────────────────────────── */

static int ipc_connect(void)
{
    struct sockaddr_in addr;
    int i;

    for (i = 0; i < CONNECT_RETRIES; i++) {
        g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_sock == INVALID_SOCKET)
            return -1;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MPRIS_IPC_PORT);
        addr.sin_addr.s_addr = inet_addr(MPRIS_IPC_ADDR);

        if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            /* Leave socket blocking — sends must not fail with WSAEWOULDBLOCK.
             * ipc_recv_command uses select() to do a non-blocking peek. */
            g_connected = 1;
            return 0;
        }

        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        Sleep(CONNECT_DELAY);
    }

    return -1;
}

static int ipc_send(uint8_t type, const void *payload, uint16_t len)
{
    struct ipc_header hdr;
    int ret;

    if (!g_connected || g_sock == INVALID_SOCKET)
        return -1;

    hdr.type = type;
    hdr.payload_len = len;

    /* Send header */
    ret = send(g_sock, (const char *)&hdr, sizeof(hdr), 0);
    if (ret != sizeof(hdr))
        goto fail;

    /* Send payload */
    if (len > 0 && payload) {
        ret = send(g_sock, (const char *)payload, len, 0);
        if (ret != len)
            goto fail;
    }

    return 0;

fail:
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    g_connected = 0;
    return -1;
}

static int ipc_recv_command(struct mpris_command *cmd)
{
    struct ipc_header hdr;
    fd_set rfds;
    struct timeval tv;
    int ret;

    if (!g_connected || g_sock == INVALID_SOCKET)
        return -1;

    /* Non-blocking check: is there data waiting? */
    FD_ZERO(&rfds);
    FD_SET(g_sock, &rfds);
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    if (select(0, &rfds, NULL, NULL, &tv) <= 0)
        return -1;  /* No data available */

    /* Data is ready — blocking read of header */
    ret = recv(g_sock, (char *)&hdr, sizeof(hdr), 0);
    if (ret <= 0) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        g_connected = 0;
        return -1;
    }

    if (ret != sizeof(hdr))
        return -1;

    if (hdr.type == MSG_COMMAND && hdr.payload_len == sizeof(*cmd)) {
        ret = recv(g_sock, (char *)cmd, sizeof(*cmd), 0);
        if (ret == sizeof(*cmd))
            return 0;
    } else if (hdr.type == MSG_HEARTBEAT && hdr.payload_len > 0) {
        char discard[64];
        recv(g_sock, discard, hdr.payload_len, 0);
    }

    return -1;
}

/* ── WACUP state gathering ──────────────────────────────── */

static void wide_to_utf8(const wchar_t *src, char *dst, int dst_size)
{
    if (!src || !*src) {
        dst[0] = '\0';
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL);
    dst[dst_size - 1] = '\0';
}

/*
 * Parse "Artist - Title" format used by Winamp/WACUP.
 * If no " - " separator found, entire string goes to title.
 */
static void parse_title_string(const char *raw, char *artist, int artist_size,
                                char *title, int title_size)
{
    const char *sep = strstr(raw, " - ");
    if (sep) {
        int alen = (int)(sep - raw);
        if (alen >= artist_size) alen = artist_size - 1;
        memcpy(artist, raw, alen);
        artist[alen] = '\0';

        const char *tstart = sep + 3;
        strncpy(title, tstart, title_size - 1);
        title[title_size - 1] = '\0';
    } else {
        artist[0] = '\0';
        snprintf(title, title_size, "%s", raw);
    }
}

static void gather_state(HWND hwnd, struct mpris_state *state)
{
    LRESULT res;

    memset(state, 0, sizeof(*state));

    /* Playback status */
    res = SendMessage(hwnd, WM_WA_IPC, 0, IPC_ISPLAYING);
    state->playback_status = (uint8_t)res;

    /* Position in ms */
    res = SendMessage(hwnd, WM_WA_IPC, 0, IPC_GETOUTPUTTIME);
    state->position_ms = (int32_t)res;

    /* Length in seconds -> convert to ms */
    res = SendMessage(hwnd, WM_WA_IPC, 1, IPC_GETOUTPUTTIME);
    state->length_ms = (res > 0) ? (int32_t)(res * 1000) : 0;

    /* Volume (0-255), pass -666 to query */
    res = SendMessage(hwnd, WM_WA_IPC, (WPARAM)-666, IPC_SETVOLUME);
    state->volume = (uint8_t)(res & 0xFF);

    /* Track title */
    res = SendMessage(hwnd, WM_WA_IPC, 0, IPC_GETLISTPOS);
    int pos = (int)res;

    res = SendMessage(hwnd, WM_WA_IPC, (WPARAM)pos, IPC_GETPLAYLISTTITLEW);
    if (res) {
        char raw_title[768];
        wide_to_utf8((const wchar_t *)res, raw_title, sizeof(raw_title));
        parse_title_string(raw_title, state->artist, sizeof(state->artist),
                          state->title, sizeof(state->title));
    }
}

static int state_changed(const struct mpris_state *a, const struct mpris_state *b)
{
    if (a->playback_status != b->playback_status) return 1;
    if (a->volume != b->volume) return 1;
    if (a->length_ms != b->length_ms) return 1;
    /* Position changes constantly during playback, but we still want to
       send updates so the host has current position data. We only skip
       if absolutely nothing changed. */
    if (a->position_ms != b->position_ms) return 1;
    if (strcmp(a->title, b->title) != 0) return 1;
    if (strcmp(a->artist, b->artist) != 0) return 1;
    return 0;
}

/* ── Command dispatch ───────────────────────────────────── */

static void dispatch_command(HWND hwnd, const struct mpris_command *cmd)
{
    switch (cmd->type) {
    case CMD_PLAY:
        SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_PLAY, 0);
        break;
    case CMD_PAUSE:
        SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_PAUSE, 0);
        break;
    case CMD_PLAYPAUSE: {
        LRESULT playing = SendMessage(hwnd, WM_WA_IPC, 0, IPC_ISPLAYING);
        if (playing == 1)
            SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_PAUSE, 0);
        else
            SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_PLAY, 0);
        break;
    }
    case CMD_STOP:
        SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_STOP, 0);
        break;
    case CMD_NEXT:
        SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_NEXT, 0);
        break;
    case CMD_PREV:
        SendMessage(hwnd, WM_COMMAND, WINAMP_CMD_PREV, 0);
        break;
    case CMD_SEEK:
        SendMessage(hwnd, WM_WA_IPC, (WPARAM)cmd->param, IPC_JUMPTOTIME);
        break;
    case CMD_SETVOL:
        SendMessage(hwnd, WM_WA_IPC, (WPARAM)(cmd->param & 0xFF), IPC_SETVOLUME);
        break;
    }
}

/* ── Disconnect warning (shown in a background thread to avoid blocking) ── */

static DWORD WINAPI warning_thread_fn(LPVOID arg)
{
    (void)arg;
    MessageBoxA(NULL,
        "gen_mpris_host has stopped.\n\n"
        "MPRIS media controls are unavailable.\n"
        "Restart WACUP to reconnect.",
        "gen_mpris", MB_OK | MB_ICONWARNING | MB_TASKMODAL);
    return 0;
}

/* ── Timer callback ─────────────────────────────────────── */

static void CALLBACK timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    struct mpris_state cur;
    struct mpris_command cmd;

    (void)hwnd; (void)msg; (void)id; (void)time;

    if (!g_connected) {
        /* Show a one-time warning when a working connection drops */
        if (g_was_connected && !g_warned_disconnect) {
            g_warned_disconnect = 1;
            HANDLE h = CreateThread(NULL, 0, warning_thread_fn, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }

        /* Non-blocking reconnect: try once every RECONNECT_TICKS ticks.
         * No Sleep() here — that would freeze WACUP's message loop. */
        if (++g_reconnect_ticks < RECONNECT_TICKS)
            return;
        g_reconnect_ticks = 0;

        {
            struct sockaddr_in addr;
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET)
                return;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(MPRIS_IPC_PORT);
            addr.sin_addr.s_addr = inet_addr(MPRIS_IPC_ADDR);
            if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                g_sock           = s;
                g_connected      = 1;
                g_was_connected  = 1;
                g_warned_disconnect = 0;
            } else {
                closesocket(s);
            }
        }
        return;
    }

    g_was_connected = 1;

    /* Gather and send state */
    gather_state(plugin.hwndParent, &cur);

    if (state_changed(&cur, &g_last_state)) {
        if (ipc_send(MSG_STATE_UPDATE, &cur, sizeof(cur)) == 0)
            g_last_state = cur;
    }

    /* Check for incoming commands */
    while (ipc_recv_command(&cmd) == 0) {
        dispatch_command(plugin.hwndParent, &cmd);
    }
}

/* ── Host process launch ────────────────────────────────── */

static void launch_host(void)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* Wine recognises /bin/sh as a native ELF binary and executes it directly.
     * exec replaces the shell with gen_mpris_host so it runs as a clean process. */
    if (CreateProcessA("/bin/sh",
                       "sh -c \"exec gen_mpris_host >/dev/null 2>&1\"",
                       NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ── Plugin interface ───────────────────────────────────── */

static int plugin_init(void)
{
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    memset(&g_last_state, 0, sizeof(g_last_state));

    /* Launch the native helper process */
    launch_host();

    /* Wait a moment for the host to start listening */
    Sleep(300);

    /* Connect to host */
    ipc_connect();

    /* Start polling timer */
    g_timer = SetTimer(plugin.hwndParent, TIMER_ID, TIMER_INTERVAL, timer_proc);

    return 0;
}

static void plugin_config(void)
{
    MessageBoxA(plugin.hwndParent,
                "gen_mpris — MPRIS2 integration for WACUP\n\n"
                "Exposes WACUP as an MPRIS2 media player on D-Bus,\n"
                "enabling desktop media controls and widgets.\n\n"
                "No configuration needed.",
                "gen_mpris", MB_OK | MB_ICONINFORMATION);
}

static void plugin_quit(void)
{
    /* Kill timer */
    if (g_timer) {
        KillTimer(plugin.hwndParent, TIMER_ID);
        g_timer = 0;
    }

    /* Tell host to shut down */
    if (g_connected)
        ipc_send(MSG_QUIT, NULL, 0);

    /* Close socket */
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    g_connected = 0;
    WSACleanup();
}

/* DLL export */
__declspec(dllexport) winampGeneralPurposePlugin *winampGetGeneralPurposePlugin(void)
{
    return &plugin;
}
