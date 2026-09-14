/*
 * qtscrcpy_raw_input_daemon
 *
 * Persistent on-device raw-input daemon. Implements
 * docs/daemon-implementation-plan.md section 1 and the protocol/behaviour
 * documented in docs/raw-input-daemon-design.md.
 *
 * Shape ported from the BlueStacks `uinput_daemon.c` prototype referenced by
 * the design doc: daemonize (double-fork, detach), open a listening socket,
 * accept one client at a time, read newline-delimited text commands, write()
 * directly into already-open device fds. What's different from that
 * prototype (real driver nodes instead of a synthetic /dev/uinput device,
 * multi-fd, slot-aware touch, real axis ranges, on-daemon rotation
 * transform) is called out inline below and mirrors §1.2 of the plan.
 *
 * Build: cross-compile with the Android NDK for aarch64. See
 * build_raw_input_daemon.sh in this directory. The resulting static binary
 * is bundled at src/third_party/raw_input_daemon/qtscrcpy_raw_input_daemon
 * and pushed to the device by RawInputDaemonSession (PC side) at connection
 * start, exactly like the existing adb.exe/scrcpy-server bundling.
 *
 * NOTE ON THE `struct input_event` ABI (design doc, open question 1): this
 * target is always aarch64 Android (64-bit time_t everywhere), so the
 * `<linux/input.h>` struct layout pulled in by the NDK sysroot for this
 * target is unambiguous - no 32-bit/64-bit time_t split to reconcile here,
 * unlike a hypothetical x86 host build. Not assumed - verified by the target
 * being aarch64-only by construction (the daemon is never built for any
 * other ABI).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#endif
#ifndef ABS_MT_POSITION_Y
#define ABS_MT_POSITION_Y 0x36
#endif
#ifndef BTN_TOUCH
#define BTN_TOUCH 0x14a
#endif
#ifndef KEY_HOME
#define KEY_HOME 102
#endif
#ifndef KEY_BACK
#define KEY_BACK 158
#endif
#ifndef KEY_MENU
#define KEY_MENU 139
#endif
#ifndef KEY_POWER
#define KEY_POWER 116
#endif
#ifndef KEY_VOLUMEUP
#define KEY_VOLUMEUP 115
#endif
#ifndef KEY_VOLUMEDOWN
#define KEY_VOLUMEDOWN 114
#endif

/* ---- §1.2: real panel node + real axis ranges (not uinput placeholders) ---- */
#define TOUCH_DEVICE_PATH "/dev/input/event6" /* fts, per docs/real-device-adb-sendevent.md */
#define TOUCH_X_MAX 14399
#define TOUCH_Y_MAX 31999
#define TOUCH_REQUIRES_BTN_TOUCH 1

/* ---- §1.2: multi-fd - all five nodes opened at startup, protocol-only follow-up later ---- */
#define HOME_BACK_MENU_DEVICE_PATH "/dev/input/event1" /* uinput-goodix */
#define POWER_DEVICE_PATH "/dev/input/event2"          /* pmic_pwrkey */
#define VOLUP_DEVICE_PATH "/dev/input/event0"          /* gpio-keys */
#define VOLDOWN_DEVICE_PATH "/dev/input/event3"        /* pmic_resin */

/* v1 scope (plan §0): only the touch fd is actively driven by the wire
 * protocol below. The other four are opened up front anyway so that wiring
 * HOME/BACK/MENU/POWER/VOL commands in later is a protocol-only change, not
 * a daemon restructuring - see plan §6 "explicitly deferred to v2". */

#define MAX_SLOTS 10 /* 0..8 game keymap contacts + 9 reserved mouse slot - see Controller::kMouseTouchSlot */
#define LISTEN_BACKLOG 1
#define LINE_BUF_SIZE 256
#define SOCK_NAME "qtscrcpy_raw_input_daemon" /* bound in the abstract namespace - see main() */
#define LOG_PATH "/data/local/tmp/qtscrcpy_raw_input_daemon.log"
#define ROTATION_POLL_INTERVAL_MS 200 /* matches Controller's own poll cadence */

typedef enum
{
    ROTATION_0 = 0,
    ROTATION_90 = 90,
    ROTATION_180 = 180,
    ROTATION_270 = 270,
    ROTATION_UNKNOWN = -1
} device_rotation_t;

typedef struct
{
    int touch_fd;
    int home_back_menu_fd;
    int power_fd;
    int volup_fd;
    int voldown_fd;

    /* per-slot state so BTN_TOUCH can be raised/lowered exactly once across
     * however many concurrent contacts are actually down at once, rather
     * than per-event, matching the "requiresBtnTouch" behaviour the PC-side
     * AdbSendEventSession already implements for its single-slot case. */
    int slot_active[MAX_SLOTS];
    int active_slot_count;

    /* frame->panel mapping state, set by the FRAME command and used by
     * DOWN/MOVE (see design doc "Rotation transform owned here"). */
    int frame_width;
    int frame_height;
    device_rotation_t rotation;
    time_t rotation_last_poll;
} daemon_state_t;

static FILE *g_log = NULL;

static void log_msg(const char *fmt, ...)
{
    if (!g_log) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    fprintf(g_log, "[%02d:%02d:%02d] ", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}

/* ---- §1.2: setenforce owned by the daemon itself, not assumed pre-done ---- */
static void ensure_selinux_permissive(void)
{
    int rc = system("setenforce 0");
    log_msg("setenforce 0 -> %d", rc);
}

static int open_node(const char *path)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_msg("open(%s) failed: %s", path, strerror(errno));
    }
    return fd;
}

static void emit(int fd, unsigned short type, unsigned short code, int value)
{
    if (fd < 0) {
        return;
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    /* Zero timestamp: the driver stamps its own on dispatch, matching what
     * the existing `sendevent`-based AdbSendEventSession path already does
     * (it never sets a timestamp either). */
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t n = write(fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
        log_msg("write to fd %d failed: %s", fd, strerror(errno));
    }
}

static void syn(int fd)
{
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* ---- Rotation polling: daemon owns it now (design doc "Decision") ---- */
static device_rotation_t parse_rotation(const char *dumpsys_output)
{
    const char *needle = "mCurrentRotation=ROTATION_";
    const char *p = strstr(dumpsys_output, needle);
    if (!p) {
        return ROTATION_UNKNOWN;
    }
    int val = atoi(p + strlen(needle));
    switch (val) {
    case 0:
        return ROTATION_0;
    case 90:
        return ROTATION_90;
    case 180:
        return ROTATION_180;
    case 270:
        return ROTATION_270;
    default:
        return ROTATION_UNKNOWN;
    }
}

/* Ported as-is from `Controller::pollDeviceRotation()`'s command, just run
 * locally via popen() instead of a PC-side adb round trip - see plan §1.3
 * step 3. */
static device_rotation_t poll_device_rotation(void)
{
    FILE *fp = popen("dumpsys window", "r");
    if (!fp) {
        log_msg("popen(dumpsys window) failed: %s", strerror(errno));
        return ROTATION_UNKNOWN;
    }
    char buf[4096];
    size_t total = 0;
    device_rotation_t result = ROTATION_UNKNOWN;
    while (total < sizeof(buf) - 1) {
        size_t n = fread(buf + total, 1, sizeof(buf) - 1 - total, fp);
        if (n == 0) {
            break;
        }
        total += n;
        buf[total] = '\0';
        result = parse_rotation(buf);
        if (result != ROTATION_UNKNOWN) {
            break;
        }
    }
    buf[total] = '\0';
    if (result == ROTATION_UNKNOWN) {
        result = parse_rotation(buf);
    }
    pclose(fp);
    return result;
}

static void ensure_rotation_fresh(daemon_state_t *st)
{
    time_t now = time(NULL);
    if (st->rotation != ROTATION_UNKNOWN &&
        (now - st->rotation_last_poll) * 1000 < ROTATION_POLL_INTERVAL_MS) {
        return;
    }
    device_rotation_t r = poll_device_rotation();
    if (r != ROTATION_UNKNOWN) {
        st->rotation = r;
    }
    st->rotation_last_poll = now;
}

/* Point-reflection relationship between ROTATION_90 and ROTATION_270 is
 * ported verbatim from `Controller::mapFrameToRawTouch()` - this is a port,
 * not a re-derivation (plan §1.2). Frame-space in, real panel-space out. */
static void map_frame_to_panel(daemon_state_t *st, int frame_x, int frame_y, int *out_x, int *out_y)
{
    if (st->frame_width <= 0 || st->frame_height <= 0) {
        *out_x = 0;
        *out_y = 0;
        return;
    }

    ensure_rotation_fresh(st);

    double rx, ry;
    int frame_is_landscape = st->frame_width > st->frame_height;
    if (frame_is_landscape) {
        double rx270 = frame_y * (double)TOUCH_X_MAX / st->frame_height;
        double ry270 = TOUCH_Y_MAX - (frame_x * (double)TOUCH_Y_MAX / st->frame_width);
        if (st->rotation == ROTATION_90) {
            rx = TOUCH_X_MAX - rx270;
            ry = TOUCH_Y_MAX - ry270;
        } else {
            /* ROTATION_270, and the fallback for 0/180/unknown reported
             * while the frame is still landscape - matches the verified
             * formula, same fallback behaviour as the PC-side version. */
            rx = rx270;
            ry = ry270;
        }
    } else {
        rx = frame_x * (double)TOUCH_X_MAX / st->frame_width;
        ry = frame_y * (double)TOUCH_Y_MAX / st->frame_height;
    }

    int ix = (int)(rx + 0.5);
    int iy = (int)(ry + 0.5);
    if (ix < 0) ix = 0;
    if (ix > TOUCH_X_MAX) ix = TOUCH_X_MAX;
    if (iy < 0) iy = 0;
    if (iy > TOUCH_Y_MAX) iy = TOUCH_Y_MAX;
    *out_x = ix;
    *out_y = iy;
}

/* ---- Slot-aware touch primitives (design doc "Slot-aware touch protocol") ---- */

static void touch_btn_on_first_down(daemon_state_t *st)
{
    if (TOUCH_REQUIRES_BTN_TOUCH && st->active_slot_count == 0) {
        emit(st->touch_fd, EV_KEY, BTN_TOUCH, 1);
    }
}

static void touch_btn_on_last_up(daemon_state_t *st)
{
    if (TOUCH_REQUIRES_BTN_TOUCH && st->active_slot_count == 0) {
        emit(st->touch_fd, EV_KEY, BTN_TOUCH, 0);
    }
}

static void handle_down(daemon_state_t *st, int slot, int track_id, int frame_x, int frame_y)
{
    if (slot < 0 || slot >= MAX_SLOTS) {
        log_msg("DOWN: slot %d out of range", slot);
        return;
    }
    int px, py;
    map_frame_to_panel(st, frame_x, frame_y, &px, &py);

    touch_btn_on_first_down(st);
    if (!st->slot_active[slot]) {
        st->slot_active[slot] = 1;
        st->active_slot_count++;
    }

    emit(st->touch_fd, EV_ABS, ABS_MT_SLOT, slot);
    emit(st->touch_fd, EV_ABS, ABS_MT_TRACKING_ID, track_id);
    emit(st->touch_fd, EV_ABS, ABS_MT_POSITION_X, px);
    emit(st->touch_fd, EV_ABS, ABS_MT_POSITION_Y, py);
    syn(st->touch_fd);
}

static void handle_move(daemon_state_t *st, int slot, int frame_x, int frame_y)
{
    if (slot < 0 || slot >= MAX_SLOTS) {
        log_msg("MOVE: slot %d out of range", slot);
        return;
    }
    int px, py;
    map_frame_to_panel(st, frame_x, frame_y, &px, &py);

    emit(st->touch_fd, EV_ABS, ABS_MT_SLOT, slot);
    emit(st->touch_fd, EV_ABS, ABS_MT_POSITION_X, px);
    emit(st->touch_fd, EV_ABS, ABS_MT_POSITION_Y, py);
    syn(st->touch_fd);
}

static void handle_up(daemon_state_t *st, int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) {
        log_msg("UP: slot %d out of range", slot);
        return;
    }

    emit(st->touch_fd, EV_ABS, ABS_MT_SLOT, slot);
    emit(st->touch_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);

    if (st->slot_active[slot]) {
        st->slot_active[slot] = 0;
        if (st->active_slot_count > 0) {
            st->active_slot_count--;
        }
    }
    touch_btn_on_last_up(st);
    syn(st->touch_fd);
}

static void handle_frame(daemon_state_t *st, int width, int height)
{
    st->frame_width = width;
    st->frame_height = height;
}

/* Resets touch state between client connections so a dropped/reconnected
 * PC session never leaves a stuck slot or a stuck BTN_TOUCH down. */
static void reset_touch_state(daemon_state_t *st)
{
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (st->slot_active[i]) {
            emit(st->touch_fd, EV_ABS, ABS_MT_SLOT, i);
            emit(st->touch_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
            st->slot_active[i] = 0;
        }
    }
    if (st->active_slot_count > 0 && TOUCH_REQUIRES_BTN_TOUCH) {
        emit(st->touch_fd, EV_KEY, BTN_TOUCH, 0);
    }
    st->active_slot_count = 0;
    syn(st->touch_fd);
}

/* ---- Wire protocol parsing (plan §3) ---- */

static void handle_line(daemon_state_t *st, char *line, int *quit)
{
    /* Trim trailing CR/LF. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    char cmd[16] = {0};
    int a = 0, b = 0, c = 0, d = 0;

    if (sscanf(line, "%15s", cmd) != 1) {
        return;
    }

    if (strcmp(cmd, "DOWN") == 0 && sscanf(line, "%*s %d %d %d %d", &a, &b, &c, &d) == 4) {
        handle_down(st, a, b, c, d);
    } else if (strcmp(cmd, "MOVE") == 0 && sscanf(line, "%*s %d %d %d", &a, &b, &c) == 3) {
        handle_move(st, a, b, c);
    } else if (strcmp(cmd, "UP") == 0 && sscanf(line, "%*s %d", &a) == 1) {
        handle_up(st, a);
    } else if (strcmp(cmd, "FRAME") == 0 && sscanf(line, "%*s %d %d", &a, &b) == 2) {
        handle_frame(st, a, b);
    } else if (strcmp(cmd, "PING") == 0) {
        /* no-op keepalive, useful for the PC side's startup handshake */
    } else if (strcmp(cmd, "QUIT") == 0) {
        *quit = 1;
    } else {
        log_msg("unrecognized command: %s", line);
    }
}

/* ---- Socket setup: Unix domain, abstract namespace ---- */
/* Abstract-namespace sockets need no filesystem entry (no /data/local/tmp
 * writable-socket-path concerns, no leftover socket file to clean up across
 * daemon restarts) and are exactly what `adb forward tcp:PORT
 * localabstract:NAME` expects on the other end. */
static int create_listen_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Leading NUL byte selects the abstract namespace. */
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, SOCK_NAME, sizeof(addr.sun_path) - 2);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));

    if (bind(fd, (struct sockaddr *)&addr, addr_len) < 0) {
        log_msg("bind() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        log_msg("listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- Daemonize: double-fork + detach, reusable as-is (design doc) ---- */
static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        _exit(1);
    }
    if (pid > 0) {
        _exit(0); /* first parent exits immediately */
    }

    if (setsid() < 0) {
        _exit(1);
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        _exit(1);
    }
    if (pid > 0) {
        _exit(0); /* second parent exits - child is now fully detached */
    }

    umask(0);
    if (chdir("/") != 0) {
        /* Non-fatal: worst case relative paths (none in this program)
         * resolve oddly. Silence the warn_unused_result nag explicitly
         * rather than ignoring it implicitly. */
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
}

/* ---- Accept loop: single current client, select()-based (reusable as-is) ---- */
static void run_accept_loop(daemon_state_t *st, int listen_fd)
{
    char linebuf[LINE_BUF_SIZE];
    size_t linelen = 0;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_msg("select() (accept) failed: %s", strerror(errno));
            break;
        }
        if (!FD_ISSET(listen_fd, &rfds)) {
            continue;
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            log_msg("accept() failed: %s", strerror(errno));
            continue;
        }
        log_msg("client connected");
        linelen = 0;

        int quit = 0;
        for (;;) {
            fd_set cfds;
            FD_ZERO(&cfds);
            FD_SET(client_fd, &cfds);
            rc = select(client_fd + 1, &cfds, NULL, NULL, NULL);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (!FD_ISSET(client_fd, &cfds)) {
                continue;
            }

            char chunk[LINE_BUF_SIZE];
            ssize_t n = read(client_fd, chunk, sizeof(chunk));
            if (n <= 0) {
                break; /* client disconnected or error */
            }
            for (ssize_t i = 0; i < n; ++i) {
                if (linelen < sizeof(linebuf) - 1) {
                    linebuf[linelen++] = chunk[i];
                }
                if (chunk[i] == '\n') {
                    linebuf[linelen] = '\0';
                    handle_line(st, linebuf, &quit);
                    linelen = 0;
                    if (quit) {
                        break;
                    }
                }
            }
            if (quit) {
                break;
            }
        }

        close(client_fd);
        reset_touch_state(st);
        log_msg("client disconnected, touch state reset");

        if (quit) {
            break;
        }
    }
}

int main(void)
{
    g_log = fopen(LOG_PATH, "a");

    daemonize();

    /* Re-open the log after daemonizing since stdio was redirected to
     * /dev/null across the double-fork. */
    if (g_log) {
        fclose(g_log);
    }
    g_log = fopen(LOG_PATH, "a");
    log_msg("qtscrcpy_raw_input_daemon starting");

    ensure_selinux_permissive();

    daemon_state_t st;
    memset(&st, 0, sizeof(st));
    st.rotation = ROTATION_UNKNOWN;
    st.rotation_last_poll = 0;

    st.touch_fd = open_node(TOUCH_DEVICE_PATH);
    st.home_back_menu_fd = open_node(HOME_BACK_MENU_DEVICE_PATH);
    st.power_fd = open_node(POWER_DEVICE_PATH);
    st.volup_fd = open_node(VOLUP_DEVICE_PATH);
    st.voldown_fd = open_node(VOLDOWN_DEVICE_PATH);

    if (st.touch_fd < 0) {
        log_msg("fatal: could not open touch device node, exiting");
        return 1;
    }

    int listen_fd = create_listen_socket();
    if (listen_fd < 0) {
        log_msg("fatal: could not create listen socket, exiting");
        return 1;
    }

    log_msg("listening on abstract socket '%s'", SOCK_NAME);
    run_accept_loop(&st, listen_fd);

    log_msg("qtscrcpy_raw_input_daemon exiting");
    close(listen_fd);
    if (st.touch_fd >= 0) close(st.touch_fd);
    if (st.home_back_menu_fd >= 0) close(st.home_back_menu_fd);
    if (st.power_fd >= 0) close(st.power_fd);
    if (st.volup_fd >= 0) close(st.volup_fd);
    if (st.voldown_fd >= 0) close(st.voldown_fd);
    if (g_log) fclose(g_log);
    return 0;
}
