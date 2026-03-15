/*
 * test_harness.c — Win32 test program for gen_mpris.dll
 *
 * Loads the plugin, acts as a fake WACUP window by responding to
 * WM_WA_IPC queries, and cycles through simulated playback states.
 *
 * Run under Wine:
 *   wine test_harness.exe
 *
 * (gen_mpris_host must already be running, or will be launched by the plugin)
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "sdk/wa_ipc.h"

#define CYCLE_TIMER_ID  0xCAFE
#define CYCLE_INTERVAL  3000   /* ms between track changes */
#define CMD_TIMER_ID    0xF00D   /* must not collide with DLL's TIMER_ID=0xBEEF */
#define CMD_INTERVAL    1500   /* ms between simulated media key presses */
#define QUIT_TIMER_ID   0xDEAD
#define QUIT_AFTER_MS   (NUM_TRACKS * CYCLE_INTERVAL + 2000)  /* all tracks + 2s margin */

/* Simulated tracks */
static const struct {
    const wchar_t *title;   /* Winamp "Artist - Title" format */
    int length_s;
    int status;
} g_tracks[] = {
    { L"The Beatles - Come Together",  259, 1 },
    { L"Pink Floyd - Wish You Were Here", 314, 1 },
    { L"David Bowie - Heroes",          360, 3 },  /* paused */
    { L"Led Zeppelin - Stairway to Heaven", 482, 1 },
};
#define NUM_TRACKS (int)(sizeof(g_tracks) / sizeof(g_tracks[0]))

static int g_track_idx = 0;
static int g_position_ms = 0;
static int g_volume = 200;

/* Simulated media key commands sent via dbus-send */
#define MPRIS_DEST "org.mpris.MediaPlayer2.wacup"
#define MPRIS_OBJ  "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define DBUS "dbus-send --session --dest=" MPRIS_DEST " " MPRIS_OBJ " "

static const struct { const char *label; const char *cmd; } g_key_cmds[] = {
    { "Pause",          DBUS MPRIS_PLAYER ".Pause" },
    { "Play",           DBUS MPRIS_PLAYER ".Play" },
    { "Next",           DBUS MPRIS_PLAYER ".Next" },
    { "Previous",       DBUS MPRIS_PLAYER ".Previous" },
    { "Seek +30s",      DBUS MPRIS_PLAYER ".Seek int64:30000000" },
    { "Volume 50%",     DBUS "org.freedesktop.DBus.Properties.Set "
                             "string:'" MPRIS_PLAYER "' "
                             "string:'Volume' variant:double:0.5" },
    { "PlayPause",      DBUS MPRIS_PLAYER ".PlayPause" },
    { "Stop",           DBUS MPRIS_PLAYER ".Stop" },
};
#define NUM_KEY_CMDS (int)(sizeof(g_key_cmds) / sizeof(g_key_cmds[0]))
static int g_cmd_step = 0;

/* Plugin function pointer type */
typedef struct {
    int version;
    const char *description;
    int  (*init)(void);
    void (*config)(void);
    void (*quit)(void);
    HWND hwndParent;
    HINSTANCE hDllInstance;
} WinampPlugin;
typedef WinampPlugin* (*GetPluginFn)(void);

static HWND g_hwnd = NULL;
static WinampPlugin *g_plugin = NULL;

/* Run a shell command via /bin/sh (Wine's system() uses cmd.exe, not sh) */
static void run_unix(const char *cmd, int wait)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[2048];

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* argv[0] for sh is "sh", then -c and the command */
    snprintf(cmdline, sizeof(cmdline), "sh -c \"%s\"", cmd);

    if (CreateProcessA("/bin/sh", cmdline, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (wait)
            WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ── Fake WACUP window procedure ───────────────────────── */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_WA_IPC) {
        switch (lp) {
        case IPC_ISPLAYING:
            return g_tracks[g_track_idx].status;

        case IPC_GETOUTPUTTIME:
            if (wp == 0)
                return g_position_ms;       /* position in ms */
            if (wp == 1)
                return g_tracks[g_track_idx].length_s;  /* length in s */
            return -1;

        case IPC_GETLISTPOS:
            return g_track_idx;

        case IPC_GETPLAYLISTTITLEW:
            /* Return pointer to wide string — valid for lifetime of static */
            return (LRESULT)g_tracks[g_track_idx].title;

        case IPC_SETVOLUME:
            if ((int)wp == -666)
                return g_volume;
            g_volume = (int)wp;
            return 0;

        case IPC_JUMPTOTIME:
            g_position_ms = (int)wp;
            printf("[harness] Seek to %d ms\n", g_position_ms);
            fflush(stdout);
            return 0;
        }
        return 0;
    }

    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case 40045: printf("[harness] CMD: Play\n"); fflush(stdout); break;
        case 40046: printf("[harness] CMD: Pause\n"); fflush(stdout); break;
        case 40047: printf("[harness] CMD: Stop\n"); fflush(stdout); break;
        case 40048:
            g_track_idx = (g_track_idx + 1) % NUM_TRACKS;
            g_position_ms = 0;
            printf("[harness] CMD: Next → track %d\n", g_track_idx);
            fflush(stdout);
            break;
        case 40044:
            g_track_idx = (g_track_idx + NUM_TRACKS - 1) % NUM_TRACKS;
            g_position_ms = 0;
            printf("[harness] CMD: Prev → track %d\n", g_track_idx);
            fflush(stdout);
            break;
        }
        return 0;
    }

    if (msg == WM_TIMER && wp == CMD_TIMER_ID) {
        if (g_cmd_step < NUM_KEY_CMDS) {
            printf("[harness] → media key: %s\n", g_key_cmds[g_cmd_step].label);
            fflush(stdout);
            run_unix(g_key_cmds[g_cmd_step].cmd, 1);
            g_cmd_step++;
        } else {
            KillTimer(hwnd, CMD_TIMER_ID);
        }
        return 0;
    }

    if (msg == WM_TIMER && wp == QUIT_TIMER_ID) {
        printf("[harness] Test complete — quitting\n");
        fflush(stdout);
        DestroyWindow(hwnd);
        return 0;
    }

    if (msg == WM_TIMER && wp == CYCLE_TIMER_ID) {
        g_track_idx = (g_track_idx + 1) % NUM_TRACKS;
        g_position_ms = 0;
        printf("[harness] Auto-advance → track %d: %ls\n",
               g_track_idx, g_tracks[g_track_idx].title);
        fflush(stdout);
        return 0;
    }

    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void)
{
    HMODULE dll;
    GetPluginFn get_plugin;
    WNDCLASSA wc;
    MSG msg;

    printf("[harness] Starting gen_mpris test harness\n");
    fflush(stdout);

    /* Register window class */
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = "WinampTestHarness";
    RegisterClassA(&wc);

    /* Create fake Winamp main window */
    g_hwnd = CreateWindowExA(0, "WinampTestHarness", "WACUP Test",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 400, 100,
                              NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g_hwnd) {
        fprintf(stderr, "[harness] Failed to create window\n");
        return 1;
    }

    /* Load the plugin DLL */
    dll = LoadLibraryA("gen_mpris.dll");
    if (!dll) {
        fprintf(stderr, "[harness] Failed to load gen_mpris.dll (error %lu)\n",
                GetLastError());
        return 1;
    }

    get_plugin = (GetPluginFn)(void *)GetProcAddress(dll, "winampGetGeneralPurposePlugin");
    if (!get_plugin) {
        fprintf(stderr, "[harness] winampGetGeneralPurposePlugin not found\n");
        return 1;
    }

    g_plugin = get_plugin();
    g_plugin->hwndParent  = g_hwnd;
    g_plugin->hDllInstance = dll;

    printf("[harness] Plugin: %s\n", g_plugin->description);
    fflush(stdout);

    /* Init plugin */
    if (g_plugin->init() != 0) {
        fprintf(stderr, "[harness] Plugin init failed\n");
        return 1;
    }

    printf("[harness] Plugin init OK — starting playback simulation\n");
    printf("[harness] Tracks cycle every %d seconds\n", CYCLE_INTERVAL / 1000);
    fflush(stdout);

    /* Auto-advance tracks, then quit after all tracks have been shown */
    SetTimer(g_hwnd, CYCLE_TIMER_ID, CYCLE_INTERVAL, NULL);
    SetTimer(g_hwnd, CMD_TIMER_ID,   CMD_INTERVAL,   NULL);
    SetTimer(g_hwnd, QUIT_TIMER_ID,  QUIT_AFTER_MS,  NULL);
    printf("[harness] Will quit automatically in %d seconds\n", QUIT_AFTER_MS / 1000);
    printf("[harness] Sending %d simulated media key commands every %ds\n",
           NUM_KEY_CMDS, CMD_INTERVAL / 1000);
    fflush(stdout);

    /* Message loop — advance position on each iteration */
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* Advance position if playing */
        if (g_tracks[g_track_idx].status == 1)
            g_position_ms += 10;  /* rough approximation */

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_plugin->quit();
    FreeLibrary(dll);
    return 0;
}
