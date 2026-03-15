/*
 * gen_mpris_host — Native Linux helper for gen_mpris WACUP plugin
 *
 * Registers as an MPRIS2 player on the D-Bus session bus and communicates
 * with gen_mpris.dll (running inside WACUP/Wine) over TCP localhost.
 *
 * Build: gcc -o gen_mpris_host gen_mpris_host.c $(pkg-config --cflags --libs dbus-1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <dbus/dbus.h>

#include "ipc_protocol.h"

#define BUS_NAME       "org.mpris.MediaPlayer2.wacup"
#define OBJECT_PATH    "/org/mpris/MediaPlayer2"
#define IFACE_ROOT     "org.mpris.MediaPlayer2"
#define IFACE_PLAYER   "org.mpris.MediaPlayer2.Player"
#define IFACE_PROPS    "org.freedesktop.DBus.Properties"

static volatile int g_running = 1;

/* Forward declaration */
static const char *playback_status_string(uint8_t status);

/* Current state from the DLL */
static struct mpris_state g_state;
static struct mpris_state g_prev_state;
static int g_has_state = 0;

/* Pending commands to send to DLL */
#define CMD_QUEUE_SIZE 16
static struct mpris_command g_cmd_queue[CMD_QUEUE_SIZE];
static int g_cmd_head = 0;
static int g_cmd_tail = 0;

/* TCP server */
static int g_listen_fd = -1;
static int g_client_fd = -1;

/* D-Bus connection */
static DBusConnection *g_dbus = NULL;

/* ── Signal handling ────────────────────────────────────── */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── Command queue ──────────────────────────────────────── */

static void cmd_enqueue(uint8_t type, int32_t param)
{
    int next = (g_cmd_head + 1) % CMD_QUEUE_SIZE;
    if (next == g_cmd_tail)
        return;  /* queue full, drop */

    g_cmd_queue[g_cmd_head].type = type;
    g_cmd_queue[g_cmd_head].param = param;
    g_cmd_head = next;
}

static int cmd_dequeue(struct mpris_command *cmd)
{
    if (g_cmd_tail == g_cmd_head)
        return -1;

    *cmd = g_cmd_queue[g_cmd_tail];
    g_cmd_tail = (g_cmd_tail + 1) % CMD_QUEUE_SIZE;
    return 0;
}

/* ── TCP server ─────────────────────────────────────────── */

static int tcp_init(void)
{
    struct sockaddr_in addr;
    int opt = 1;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return -1;
    }

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MPRIS_IPC_PORT);
    addr.sin_addr.s_addr = inet_addr(MPRIS_IPC_ADDR);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, 1) < 0) {
        perror("listen");
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    /* Non-blocking for accept */
    fcntl(g_listen_fd, F_SETFL, O_NONBLOCK);

    fprintf(stderr, "gen_mpris_host: listening on %s:%d\n",
            MPRIS_IPC_ADDR, MPRIS_IPC_PORT);
    return 0;
}

static int tcp_send_command(const struct mpris_command *cmd)
{
    struct ipc_header hdr;
    ssize_t ret;

    if (g_client_fd < 0)
        return -1;

    hdr.type = MSG_COMMAND;
    hdr.payload_len = sizeof(*cmd);

    ret = write(g_client_fd, &hdr, sizeof(hdr));
    if (ret != sizeof(hdr))
        goto fail;

    ret = write(g_client_fd, cmd, sizeof(*cmd));
    if (ret != sizeof(*cmd))
        goto fail;

    return 0;

fail:
    close(g_client_fd);
    g_client_fd = -1;
    return -1;
}

/* Read exactly n bytes, returns 0 on success, -1 on error/short */
static int read_exact(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += r;
    }
    return 0;
}

static int tcp_handle_client(void)
{
    struct ipc_header hdr;
    ssize_t ret;

    /* Non-blocking read of header */
    ret = recv(g_client_fd, &hdr, sizeof(hdr), MSG_DONTWAIT);
    if (ret == 0) {
        /* Client disconnected */
        close(g_client_fd);
        g_client_fd = -1;
        return -1;
    }
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;  /* No more data — stop the read loop */
        close(g_client_fd);
        g_client_fd = -1;
        return -1;
    }
    if (ret != sizeof(hdr)) {
        close(g_client_fd);
        g_client_fd = -1;
        return -1;
    }

    switch (hdr.type) {
    case MSG_STATE_UPDATE:
        if (hdr.payload_len == sizeof(struct mpris_state)) {
            if (read_exact(g_client_fd, &g_state, sizeof(g_state)) == 0) {
                g_has_state = 1;
                fprintf(stderr, "gen_mpris_host: state update — %s | \"%s\" by \"%s\"\n",
                        playback_status_string(g_state.playback_status),
                        g_state.title, g_state.artist);
            } else {
                close(g_client_fd);
                g_client_fd = -1;
            }
        }
        return 1;  /* Processed a message — check for more */

    case MSG_HEARTBEAT:
        /* Discard payload if any */
        if (hdr.payload_len > 0) {
            char discard[64];
            uint16_t remain = hdr.payload_len;
            while (remain > 0) {
                uint16_t chunk = remain > sizeof(discard) ? sizeof(discard) : remain;
                if (read_exact(g_client_fd, discard, chunk) < 0)
                    break;
                remain -= chunk;
            }
        }
        return 1;

    case MSG_QUIT:
        fprintf(stderr, "gen_mpris_host: received QUIT\n");
        g_running = 0;
        return 1;

    default:
        /* Unknown message, skip payload */
        if (hdr.payload_len > 0) {
            char discard[64];
            uint16_t remain = hdr.payload_len;
            while (remain > 0) {
                uint16_t chunk = remain > sizeof(discard) ? sizeof(discard) : remain;
                if (read_exact(g_client_fd, discard, chunk) < 0)
                    break;
                remain -= chunk;
            }
        }
        return 1;
    }

    return 0;
}

/* ── D-Bus MPRIS2 helpers ───────────────────────────────── */

static const char *playback_status_string(uint8_t status)
{
    switch (status) {
    case PLAYBACK_PLAYING: return "Playing";
    case PLAYBACK_PAUSED:  return "Paused";
    default:               return "Stopped";
    }
}

static void append_metadata(DBusMessageIter *iter)
{
    DBusMessageIter dict, entry, variant, arr;
    char trackid[64];

    snprintf(trackid, sizeof(trackid), "/org/mpris/MediaPlayer2/Track/1");

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    /* mpris:trackid — object path */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "mpris:trackid";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &variant);
        const char *val = trackid;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* mpris:length — int64 microseconds */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "mpris:length";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "x", &variant);
        int64_t len_us = (int64_t)g_state.length_ms * 1000;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT64, &len_us);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* xesam:title — string */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "xesam:title";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = g_state.title;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* xesam:artist — array of strings */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "xesam:artist";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
        const char *val = g_state.artist;
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&variant, &arr);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(iter, &dict);
}

static void emit_properties_changed(void)
{
    DBusMessage *sig;
    DBusMessageIter args, dict, entry, variant, invalidated;
    int changed = 0;

    if (!g_dbus || !g_has_state)
        return;

    /* Check what actually changed */
    int status_changed = (g_state.playback_status != g_prev_state.playback_status);
    int meta_changed = (strcmp(g_state.title, g_prev_state.title) != 0 ||
                        strcmp(g_state.artist, g_prev_state.artist) != 0 ||
                        g_state.length_ms != g_prev_state.length_ms);
    int vol_changed = (g_state.volume != g_prev_state.volume);

    if (!status_changed && !meta_changed && !vol_changed)
        return;

    sig = dbus_message_new_signal(OBJECT_PATH, IFACE_PROPS, "PropertiesChanged");
    if (!sig)
        return;

    dbus_message_iter_init_append(sig, &args);

    /* Interface name */
    const char *iface = IFACE_PLAYER;
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface);

    /* Changed properties dict */
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    if (status_changed) {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "PlaybackStatus";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = playback_status_string(g_state.playback_status);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
        changed = 1;
    }

    if (meta_changed) {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "Metadata";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant);
        append_metadata(&variant);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
        changed = 1;
    }

    if (vol_changed) {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "Volume";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "d", &variant);
        double vol = (double)g_state.volume / 255.0;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_DOUBLE, &vol);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
        changed = 1;
    }

    dbus_message_iter_close_container(&args, &dict);

    /* Invalidated properties (empty array) */
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_close_container(&args, &invalidated);

    if (changed)
        dbus_connection_send(g_dbus, sig, NULL);

    dbus_message_unref(sig);

    g_prev_state = g_state;
}

/* ── D-Bus method/property handlers ─────────────────────── */

static DBusHandlerResult handle_root_get(DBusMessage *msg, DBusConnection *conn,
                                          const char *property)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter args, variant;

    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    dbus_message_iter_init_append(reply, &args);

    if (strcmp(property, "Identity") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = "WACUP";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "CanQuit") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = TRUE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "CanRaise") == 0 ||
               strcmp(property, "HasTrackList") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "DesktopEntry") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = "wacup";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "SupportedUriSchemes") == 0 ||
               strcmp(property, "SupportedMimeTypes") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "as", &variant);
        DBusMessageIter arr;
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
        dbus_message_iter_close_container(&variant, &arr);
        dbus_message_iter_close_container(&args, &variant);
    } else {
        dbus_message_unref(reply);
        reply = dbus_message_new_error_printf(msg,
            DBUS_ERROR_UNKNOWN_PROPERTY,
            "Property %s not found on " IFACE_ROOT, property);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_root_getall(DBusMessage *msg, DBusConnection *conn)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter args, dict, entry, variant;

    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    dbus_message_iter_init_append(reply, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    /* Identity */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "Identity";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = "WACUP";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* DesktopEntry — KDE uses GetAll to discover this, must be included here */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "DesktopEntry";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = "wacup";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* CanQuit */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "CanQuit";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = TRUE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* CanRaise */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "CanRaise";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* HasTrackList */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "HasTrackList";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* SupportedUriSchemes */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "SupportedUriSchemes";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
        DBusMessageIter arr;
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
        dbus_message_iter_close_container(&variant, &arr);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    /* SupportedMimeTypes */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "SupportedMimeTypes";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
        DBusMessageIter arr;
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
        dbus_message_iter_close_container(&variant, &arr);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&args, &dict);

    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_player_get(DBusMessage *msg, DBusConnection *conn,
                                            const char *property)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter args, variant;

    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    dbus_message_iter_init_append(reply, &args);

    if (strcmp(property, "PlaybackStatus") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = playback_status_string(g_state.playback_status);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "Metadata") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "a{sv}", &variant);
        append_metadata(&variant);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "Position") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "x", &variant);
        int64_t pos_us = (int64_t)g_state.position_ms * 1000;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT64, &pos_us);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "Volume") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "d", &variant);
        double vol = (double)g_state.volume / 255.0;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_DOUBLE, &vol);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "Rate") == 0 ||
               strcmp(property, "MinimumRate") == 0 ||
               strcmp(property, "MaximumRate") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "d", &variant);
        double val = 1.0;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_DOUBLE, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "CanGoNext") == 0 ||
               strcmp(property, "CanGoPrevious") == 0 ||
               strcmp(property, "CanPlay") == 0 ||
               strcmp(property, "CanPause") == 0 ||
               strcmp(property, "CanSeek") == 0 ||
               strcmp(property, "CanControl") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = TRUE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "LoopStatus") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &variant);
        const char *val = "None";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else if (strcmp(property, "Shuffle") == 0) {
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&args, &variant);
    } else {
        dbus_message_unref(reply);
        reply = dbus_message_new_error_printf(msg,
            DBUS_ERROR_UNKNOWN_PROPERTY,
            "Property %s not found on " IFACE_PLAYER, property);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Helper to add a single property entry to a dict */
#define ADD_PROP_STRING(dict, name, value) do { \
    DBusMessageIter _e, _v; \
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &_e); \
    const char *_k = (name); \
    dbus_message_iter_append_basic(&_e, DBUS_TYPE_STRING, &_k); \
    dbus_message_iter_open_container(&_e, DBUS_TYPE_VARIANT, "s", &_v); \
    const char *_s = (value); \
    dbus_message_iter_append_basic(&_v, DBUS_TYPE_STRING, &_s); \
    dbus_message_iter_close_container(&_e, &_v); \
    dbus_message_iter_close_container(dict, &_e); \
} while(0)

#define ADD_PROP_BOOL(dict, name, value) do { \
    DBusMessageIter _e, _v; \
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &_e); \
    const char *_k = (name); \
    dbus_message_iter_append_basic(&_e, DBUS_TYPE_STRING, &_k); \
    dbus_message_iter_open_container(&_e, DBUS_TYPE_VARIANT, "b", &_v); \
    dbus_bool_t _b = (value); \
    dbus_message_iter_append_basic(&_v, DBUS_TYPE_BOOLEAN, &_b); \
    dbus_message_iter_close_container(&_e, &_v); \
    dbus_message_iter_close_container(dict, &_e); \
} while(0)

#define ADD_PROP_DOUBLE(dict, name, value) do { \
    DBusMessageIter _e, _v; \
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &_e); \
    const char *_k = (name); \
    dbus_message_iter_append_basic(&_e, DBUS_TYPE_STRING, &_k); \
    dbus_message_iter_open_container(&_e, DBUS_TYPE_VARIANT, "d", &_v); \
    double _d = (value); \
    dbus_message_iter_append_basic(&_v, DBUS_TYPE_DOUBLE, &_d); \
    dbus_message_iter_close_container(&_e, &_v); \
    dbus_message_iter_close_container(dict, &_e); \
} while(0)

#define ADD_PROP_INT64(dict, name, value) do { \
    DBusMessageIter _e, _v; \
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &_e); \
    const char *_k = (name); \
    dbus_message_iter_append_basic(&_e, DBUS_TYPE_STRING, &_k); \
    dbus_message_iter_open_container(&_e, DBUS_TYPE_VARIANT, "x", &_v); \
    int64_t _x = (value); \
    dbus_message_iter_append_basic(&_v, DBUS_TYPE_INT64, &_x); \
    dbus_message_iter_close_container(&_e, &_v); \
    dbus_message_iter_close_container(dict, &_e); \
} while(0)

static DBusHandlerResult handle_player_getall(DBusMessage *msg, DBusConnection *conn)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter args, dict, entry, variant;

    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    dbus_message_iter_init_append(reply, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    ADD_PROP_STRING(&dict, "PlaybackStatus",
                    playback_status_string(g_state.playback_status));

    ADD_PROP_STRING(&dict, "LoopStatus", "None");
    ADD_PROP_DOUBLE(&dict, "Rate", 1.0);
    ADD_PROP_BOOL(&dict, "Shuffle", FALSE);
    ADD_PROP_DOUBLE(&dict, "Volume", (double)g_state.volume / 255.0);
    ADD_PROP_INT64(&dict, "Position", (int64_t)g_state.position_ms * 1000);
    ADD_PROP_DOUBLE(&dict, "MinimumRate", 1.0);
    ADD_PROP_DOUBLE(&dict, "MaximumRate", 1.0);
    ADD_PROP_BOOL(&dict, "CanGoNext", TRUE);
    ADD_PROP_BOOL(&dict, "CanGoPrevious", TRUE);
    ADD_PROP_BOOL(&dict, "CanPlay", TRUE);
    ADD_PROP_BOOL(&dict, "CanPause", TRUE);
    ADD_PROP_BOOL(&dict, "CanSeek", TRUE);
    ADD_PROP_BOOL(&dict, "CanControl", TRUE);

    /* Metadata */
    {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *key = "Metadata";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant);
        append_metadata(&variant);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&args, &dict);

    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_player_method(DBusMessage *msg, DBusConnection *conn)
{
    const char *member = dbus_message_get_member(msg);
    DBusMessage *reply;

    if (strcmp(member, "Play") == 0) {
        cmd_enqueue(CMD_PLAY, 0);
    } else if (strcmp(member, "Pause") == 0) {
        cmd_enqueue(CMD_PAUSE, 0);
    } else if (strcmp(member, "PlayPause") == 0) {
        cmd_enqueue(CMD_PLAYPAUSE, 0);
    } else if (strcmp(member, "Stop") == 0) {
        cmd_enqueue(CMD_STOP, 0);
    } else if (strcmp(member, "Next") == 0) {
        cmd_enqueue(CMD_NEXT, 0);
    } else if (strcmp(member, "Previous") == 0) {
        cmd_enqueue(CMD_PREV, 0);
    } else if (strcmp(member, "Seek") == 0) {
        int64_t offset_us = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_INT64, &offset_us,
                                  DBUS_TYPE_INVALID)) {
            int32_t new_pos_ms = g_state.position_ms + (int32_t)(offset_us / 1000);
            if (new_pos_ms < 0) new_pos_ms = 0;
            cmd_enqueue(CMD_SEEK, new_pos_ms);
        }
    } else if (strcmp(member, "SetPosition") == 0) {
        const char *trackid = NULL;
        int64_t pos_us = 0;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_OBJECT_PATH, &trackid,
                                  DBUS_TYPE_INT64, &pos_us,
                                  DBUS_TYPE_INVALID)) {
            cmd_enqueue(CMD_SEEK, (int32_t)(pos_us / 1000));
        }
    } else if (strcmp(member, "OpenUri") == 0) {
        /* Not supported — silently ignore */
    } else {
        reply = dbus_message_new_error_printf(msg,
            DBUS_ERROR_UNKNOWN_METHOD,
            "Method %s not found on " IFACE_PLAYER, member);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    reply = dbus_message_new_method_return(msg);
    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_root_method(DBusMessage *msg, DBusConnection *conn)
{
    const char *member = dbus_message_get_member(msg);
    DBusMessage *reply;

    if (strcmp(member, "Quit") == 0) {
        g_running = 0;
        reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (strcmp(member, "Raise") == 0) {
        /* Not supported, but don't error */
        reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    reply = dbus_message_new_error_printf(msg,
        DBUS_ERROR_UNKNOWN_METHOD,
        "Method %s not found on " IFACE_ROOT, member);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Handle Set for writable properties */
static DBusHandlerResult handle_property_set(DBusMessage *msg, DBusConnection *conn)
{
    const char *iface = NULL;
    const char *property = NULL;
    DBusMessageIter args, variant;
    DBusMessage *reply;

    if (!dbus_message_iter_init(msg, &args))
        goto err;
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        goto err;
    dbus_message_iter_get_basic(&args, &iface);

    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        goto err;
    dbus_message_iter_get_basic(&args, &property);

    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT)
        goto err;
    dbus_message_iter_recurse(&args, &variant);

    if (strcmp(iface, IFACE_PLAYER) == 0 && strcmp(property, "Volume") == 0) {
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_DOUBLE) {
            double vol;
            dbus_message_iter_get_basic(&variant, &vol);
            if (vol < 0.0) vol = 0.0;
            if (vol > 1.0) vol = 1.0;
            cmd_enqueue(CMD_SETVOL, (int32_t)(vol * 255.0));
        }
    }

    reply = dbus_message_new_method_return(msg);
    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
    }
    return DBUS_HANDLER_RESULT_HANDLED;

err:
    reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                   "Invalid arguments for Set");
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ── D-Bus message filter ───────────────────────────────── */

static DBusHandlerResult message_handler(DBusConnection *conn, DBusMessage *msg,
                                          void *user_data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    const char *path = dbus_message_get_path(msg);

    (void)user_data;

    if (!path || strcmp(path, OBJECT_PATH) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /* Handle Introspect */
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable",
                                     "Introspect")) {
        static const char *introspect_xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "  \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
            "    <method name=\"Introspect\">\n"
            "      <arg direction=\"out\" type=\"s\"/>\n"
            "    </method>\n"
            "  </interface>\n"
            "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
            "    <method name=\"Get\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"out\" type=\"v\"/>\n"
            "    </method>\n"
            "    <method name=\"GetAll\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"out\" type=\"a{sv}\"/>\n"
            "    </method>\n"
            "    <method name=\"Set\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"v\"/>\n"
            "    </method>\n"
            "    <signal name=\"PropertiesChanged\">\n"
            "      <arg type=\"s\"/>\n"
            "      <arg type=\"a{sv}\"/>\n"
            "      <arg type=\"as\"/>\n"
            "    </signal>\n"
            "  </interface>\n"
            "  <interface name=\"" IFACE_ROOT "\">\n"
            "    <method name=\"Quit\"/>\n"
            "    <method name=\"Raise\"/>\n"
            "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
            "    <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
            "  </interface>\n"
            "  <interface name=\"" IFACE_PLAYER "\">\n"
            "    <method name=\"Next\"/>\n"
            "    <method name=\"Previous\"/>\n"
            "    <method name=\"Pause\"/>\n"
            "    <method name=\"PlayPause\"/>\n"
            "    <method name=\"Stop\"/>\n"
            "    <method name=\"Play\"/>\n"
            "    <method name=\"Seek\">\n"
            "      <arg direction=\"in\" type=\"x\" name=\"Offset\"/>\n"
            "    </method>\n"
            "    <method name=\"SetPosition\">\n"
            "      <arg direction=\"in\" type=\"o\" name=\"TrackId\"/>\n"
            "      <arg direction=\"in\" type=\"x\" name=\"Position\"/>\n"
            "    </method>\n"
            "    <method name=\"OpenUri\">\n"
            "      <arg direction=\"in\" type=\"s\" name=\"Uri\"/>\n"
            "    </method>\n"
            "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"LoopStatus\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"Rate\" type=\"d\" access=\"read\"/>\n"
            "    <property name=\"Shuffle\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
            "    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
            "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
            "    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
            "    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
            "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
            "    <signal name=\"Seeked\">\n"
            "      <arg type=\"x\" name=\"Position\"/>\n"
            "    </signal>\n"
            "  </interface>\n"
            "</node>\n";

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_message_append_args(reply,
                DBUS_TYPE_STRING, &introspect_xml,
                DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Handle Properties interface */
    if (iface && strcmp(iface, IFACE_PROPS) == 0) {
        if (strcmp(member, "Get") == 0) {
            const char *req_iface = NULL;
            const char *property = NULL;
            if (dbus_message_get_args(msg, NULL,
                                      DBUS_TYPE_STRING, &req_iface,
                                      DBUS_TYPE_STRING, &property,
                                      DBUS_TYPE_INVALID)) {
                if (strcmp(req_iface, IFACE_ROOT) == 0)
                    return handle_root_get(msg, conn, property);
                if (strcmp(req_iface, IFACE_PLAYER) == 0)
                    return handle_player_get(msg, conn, property);
            }
        } else if (strcmp(member, "GetAll") == 0) {
            const char *req_iface = NULL;
            if (dbus_message_get_args(msg, NULL,
                                      DBUS_TYPE_STRING, &req_iface,
                                      DBUS_TYPE_INVALID)) {
                if (strcmp(req_iface, IFACE_ROOT) == 0)
                    return handle_root_getall(msg, conn);
                if (strcmp(req_iface, IFACE_PLAYER) == 0)
                    return handle_player_getall(msg, conn);
            }
        } else if (strcmp(member, "Set") == 0) {
            return handle_property_set(msg, conn);
        }
    }

    /* Handle Player methods */
    if (iface && strcmp(iface, IFACE_PLAYER) == 0) {
        return handle_player_method(msg, conn);
    }

    /* Handle Root methods */
    if (iface && strcmp(iface, IFACE_ROOT) == 0) {
        return handle_root_method(msg, conn);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ── D-Bus setup ────────────────────────────────────────── */

static int dbus_init(void)
{
    DBusError err;
    int ret;

    dbus_error_init(&err);

    g_dbus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "gen_mpris_host: D-Bus connection error: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (!g_dbus)
        return -1;

    /* Request the MPRIS bus name */
    ret = dbus_bus_request_name(g_dbus, BUS_NAME,
                                DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "gen_mpris_host: name request error: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "gen_mpris_host: could not acquire bus name %s\n", BUS_NAME);
        return -1;
    }

    fprintf(stderr, "gen_mpris_host: acquired bus name %s\n", BUS_NAME);

    /* Register object path handler */
    static const DBusObjectPathVTable vtable = {
        .unregister_function = NULL,
        .message_function = message_handler,
    };

    if (!dbus_connection_register_object_path(g_dbus, OBJECT_PATH, &vtable, NULL)) {
        fprintf(stderr, "gen_mpris_host: failed to register object path\n");
        return -1;
    }

    return 0;
}

/* ── Main loop ──────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    fd_set rfds;
    struct timeval tv;
    int dbus_fd = -1;

    (void)argc; (void)argv;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    memset(&g_state, 0, sizeof(g_state));
    memset(&g_prev_state, 0, sizeof(g_prev_state));

    /* Initialize TCP server */
    if (tcp_init() < 0) {
        fprintf(stderr, "gen_mpris_host: failed to initialize TCP server\n");
        return 1;
    }

    /* Initialize D-Bus */
    if (dbus_init() < 0) {
        fprintf(stderr, "gen_mpris_host: failed to initialize D-Bus\n");
        close(g_listen_fd);
        return 1;
    }

    /* Get D-Bus fd for select() */
    if (!dbus_connection_get_unix_fd(g_dbus, &dbus_fd)) {
        fprintf(stderr, "gen_mpris_host: could not get D-Bus fd\n");
        dbus_fd = -1;
    }

    fprintf(stderr, "gen_mpris_host: running\n");

    while (g_running) {
        int maxfd = -1;

        FD_ZERO(&rfds);

        /* Listen socket for new connections */
        if (g_client_fd < 0 && g_listen_fd >= 0) {
            FD_SET(g_listen_fd, &rfds);
            if (g_listen_fd > maxfd) maxfd = g_listen_fd;
        }

        /* Client socket for data */
        if (g_client_fd >= 0) {
            FD_SET(g_client_fd, &rfds);
            if (g_client_fd > maxfd) maxfd = g_client_fd;
        }

        /* D-Bus fd */
        if (dbus_fd >= 0) {
            FD_SET(dbus_fd, &rfds);
            if (dbus_fd > maxfd) maxfd = dbus_fd;
        }

        /* Timeout for periodic tasks */
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms */

        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (nready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Accept new client */
        if (g_listen_fd >= 0 && g_client_fd < 0 &&
            FD_ISSET(g_listen_fd, &rfds)) {
            g_client_fd = accept(g_listen_fd, NULL, NULL);
            if (g_client_fd >= 0) {
                /* Keep client fd blocking — read_exact needs blocking reads.
                 * The header recv uses MSG_DONTWAIT explicitly for the non-blocking peek. */
                fprintf(stderr, "gen_mpris_host: client connected\n");
            }
        }

        /* Handle client data */
        if (g_client_fd >= 0 && FD_ISSET(g_client_fd, &rfds)) {
            while (tcp_handle_client() > 0) {
                /* Process all available messages */
            }
        }

        /* Emit D-Bus signals for state changes */
        emit_properties_changed();

        /* Send pending commands to DLL */
        {
            struct mpris_command cmd;
            while (cmd_dequeue(&cmd) == 0) {
                tcp_send_command(&cmd);
            }
        }

        /* Process D-Bus events */
        while (dbus_connection_get_dispatch_status(g_dbus) ==
               DBUS_DISPATCH_DATA_REMAINS) {
            dbus_connection_dispatch(g_dbus);
        }
        dbus_connection_read_write(g_dbus, 0);
        while (dbus_connection_get_dispatch_status(g_dbus) ==
               DBUS_DISPATCH_DATA_REMAINS) {
            dbus_connection_dispatch(g_dbus);
        }
    }

    fprintf(stderr, "gen_mpris_host: shutting down\n");

    /* Cleanup */
    if (g_client_fd >= 0)
        close(g_client_fd);
    if (g_listen_fd >= 0)
        close(g_listen_fd);

    if (g_dbus) {
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_release_name(g_dbus, BUS_NAME, &err);
        dbus_error_free(&err);
        dbus_connection_unref(g_dbus);
    }

    return 0;
}
