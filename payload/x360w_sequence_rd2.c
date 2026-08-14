/*
 * Xbox 360 Wireless Receiver input-sequence R&D payload.
 *
 * Exact target: Microsoft Xbox 360 Wireless Receiver 045e:0291.
 * It sends exactly one receiver-presence query during setup, then closes OUT
 * before the sequence starts.  It never creates a virtual pad, injects PS5
 * input, rumbles, sets LEDs, or alters MBus devices.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>

#ifdef __PROSPERO__
#include <ps5/klog.h>
#endif

#define LOG_DIR       "/data/ghostpad"
#define LOG_PATH      "/data/ghostpad/gc_status.log"
#define PID_PATH      "/data/ghostpad/gc_main.pid"
#define HANDOFF_PATH  "/data/ghostpad/gc_handoff.ready"

#define X360W_VID     0x045eu
#define X360W_PID     0x0291u
#define X360W_EP_IN   0x81u
#define X360W_EP_OUT  0x01u

#define PHASE_MS      3000u
#define WAIT_INPUT_MS 60000u

extern int32_t sceKernelSendNotificationRequest(int unk0, void *req,
                                                size_t size, int unk1);
typedef struct { char _unk[45]; char message[3075]; } NotifyRequest;

typedef struct {
    const char *label;
    uint8_t b2_mask;
    uint8_t b3_mask;
    int control;
    int expect_set;
    int require_prior_press;
    int expect_all_clear;
} sequence_phase_t;

enum {
    CONTROL_A, CONTROL_B, CONTROL_X, CONTROL_Y,
    CONTROL_UP, CONTROL_DOWN, CONTROL_LEFT, CONTROL_RIGHT,
    CONTROL_COUNT
};

static const sequence_phase_t g_phases[] = {
    { "Release all controls",      0x00, 0x00, -1,            0, 0, 1 },
    { "Hold A",                    0x00, 0x10, CONTROL_A,    1, 0, 0 },
    { "Release A",                 0x00, 0x10, CONTROL_A,    0, 1, 0 },
    { "Hold B",                    0x00, 0x20, CONTROL_B,    1, 0, 0 },
    { "Release B",                 0x00, 0x20, CONTROL_B,    0, 1, 0 },
    { "Hold X",                    0x00, 0x40, CONTROL_X,    1, 0, 0 },
    { "Release X",                 0x00, 0x40, CONTROL_X,    0, 1, 0 },
    { "Hold Y",                    0x00, 0x80, CONTROL_Y,    1, 0, 0 },
    { "Release Y",                 0x00, 0x80, CONTROL_Y,    0, 1, 0 },
    { "Hold D-pad Up",             0x01, 0x00, CONTROL_UP,   1, 0, 0 },
    { "Release D-pad Up",          0x01, 0x00, CONTROL_UP,   0, 1, 0 },
    { "Hold D-pad Down",           0x02, 0x00, CONTROL_DOWN, 1, 0, 0 },
    { "Release D-pad Down",        0x02, 0x00, CONTROL_DOWN, 0, 1, 0 },
    { "Hold D-pad Left",           0x04, 0x00, CONTROL_LEFT, 1, 0, 0 },
    { "Release D-pad Left",        0x04, 0x00, CONTROL_LEFT, 0, 1, 0 },
    { "Hold D-pad Right",          0x08, 0x00, CONTROL_RIGHT,1, 0, 0 },
    { "Release D-pad Right",       0x08, 0x00, CONTROL_RIGHT,0, 1, 0 },
};

#define PHASE_COUNT ((int)(sizeof(g_phases) / sizeof(g_phases[0])))

typedef struct {
    int started;
    int completed;
    int phase;
    uint64_t phase_deadline_ms;
    uint32_t phase_start_input;
    int phase_pass[PHASE_COUNT];
    int press_seen[CONTROL_COUNT];
    uint32_t input_count;
    uint32_t gap_count;
    uint64_t last_input_ms;
    int have_last_buttons;
    uint8_t last_b2;
    uint8_t last_b3;
} sequence_state_t;

static volatile sig_atomic_t g_stop = 0;
static int g_log_fd = -1;

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static void rd_log(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
#ifdef __PROSPERO__
    klog_printf("[X360W-SEQUENCE] %s", msg);
#endif
    if (g_log_fd >= 0)
        write(g_log_fd, msg, strnlen(msg, sizeof(msg)));
}

static void notify(const char *fmt, ...) {
    NotifyRequest req;
    va_list ap;
    memset(&req, 0, sizeof(req));
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void request_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

static int pid_alive(pid_t pid) {
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static int handoff_acknowledged(pid_t expected_pid) {
    char buf[24] = {0};
    int fd = open(HANDOFF_PATH, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return len > 0 && (pid_t)atoi(buf) == expected_pid;
}

static void write_handoff_ack(void) {
    char buf[24];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    int fd = open(HANDOFF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        write(fd, buf, (size_t)len);
        close(fd);
    }
}

static int replace_previous_payload(void) {
    char buf[24] = {0};
    int fd = open(PID_PATH, O_RDONLY);
    if (fd < 0) return 0;
    read(fd, buf, sizeof(buf) - 1);
    close(fd);

    pid_t old = (pid_t)atoi(buf);
    if (old <= 0 || old == getpid()) return 0;

    unlink(HANDOFF_PATH);
    rd_log("handoff: requesting shutdown from pid=%d\n", old);
    if (kill(old, SIGTERM) != 0 && errno != ESRCH)
        rd_log("handoff: SIGTERM pid=%d errno=%d\n", old, errno);
    int released = 0;
    for (int pass = 0; pass < 50 && !g_stop; pass++) {
        if (handoff_acknowledged(old)) {
            rd_log("handoff: pid=%d confirmed USB release\n", old);
            released = 1;
            break;
        }
        if (!pid_alive(old)) {
            rd_log("handoff: pid=%d exited without acknowledgement\n", old);
            released = 1;
            break;
        }
        usleep(100000);
    }
    if (!released && pid_alive(old)) {
        rd_log("handoff: pid=%d did not exit in time; aborting R&D payload\n", old);
        notify("Xbox 360 R&D aborted: existing payload did not stop");
        return -1;
    }
    usleep(1500000);
    return 0;
}

static void write_our_pid(void) {
    char buf[24];
    int fd = open(PID_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        int len = snprintf(buf, sizeof(buf), "%d", getpid());
        write(fd, buf, (size_t)len);
        close(fd);
    }
}

static int find_receiver(char out_path[32]) {
    DIR *dp = opendir("/dev");
    if (!dp) return 0;
    struct dirent *ent;
    int found = 0;
    while (!g_stop && (ent = readdir(dp)) != NULL) {
        if (strncmp(ent->d_name, "ugen", 4) != 0) continue;
        const char *dot = strchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".1") == 0) continue;

        char path[32];
        snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        struct usb_device_descriptor desc;
        memset(&desc, 0, sizeof(desc));
        if (ioctl(fd, USB_GET_DEVICE_DESC, &desc) == 0) {
            uint16_t vid = UGETW(desc.idVendor);
            uint16_t pid = UGETW(desc.idProduct);
            rd_log("scan: %s VID=0x%04x PID=0x%04x\n", path, vid, pid);
            if (vid == X360W_VID && pid == X360W_PID) {
                strncpy(out_path, path, 31);
                out_path[31] = '\0';
                found = 1;
                close(fd);
                break;
            }
        }
        close(fd);
    }
    closedir(dp);
    return found;
}

/* The receiver does not necessarily forward controller input until it has
 * received its standard presence query.  Complete or explicitly stop every
 * transfer before this function returns, because its endpoint descriptors
 * live beyond the call but the transfer state must not remain active. */
static void stop_and_drain_endpoint(int fd, uint8_t ep_index) {
    struct usb_fs_stop stop;
    struct usb_fs_complete complete;

    memset(&stop, 0, sizeof(stop));
    stop.ep_index = ep_index;
    ioctl(fd, USB_FS_STOP, &stop);
    for (int pass = 0; pass < 20; pass++) {
        memset(&complete, 0, sizeof(complete));
        complete.ep_index = ep_index;
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0 || errno != EBUSY)
            break;
        usleep(50000);
    }
}

static int send_startup_presence_inquiry(int fd, struct usb_fs_endpoint *out_ep) {
    static const uint8_t inquiry[12] = {
        0x08, 0x00, 0x0f, 0xc0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    void *buffers[1] = { (void *)inquiry };
    uint32_t lengths[1] = { sizeof(inquiry) };
    struct usb_fs_start start;
    struct usb_fs_complete complete;

    memset(out_ep, 0, sizeof(*out_ep));
    out_ep->ppBuffer = buffers;
    out_ep->pLength = lengths;
    out_ep->nFrames = 1;
    out_ep->timeout = 150;
    out_ep->flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
    out_ep->aFrames = 0;
    out_ep->status = 0;
    memset(&start, 0, sizeof(start));
    start.ep_index = 1;
    if (ioctl(fd, USB_FS_START, &start) != 0) {
        int err = errno;
        rd_log("startup presence START failed errno=%d\n", err);
        stop_and_drain_endpoint(fd, 1);
        return -err;
    }

    for (int pass = 0; pass < 20; pass++) {
        memset(&complete, 0, sizeof(complete));
        complete.ep_index = 1;
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            rd_log("startup presence inquiry completed\n");
            return 0;
        }
        if (errno != EBUSY) {
            int err = errno;
            rd_log("startup presence COMPLETE failed errno=%d\n", err);
            stop_and_drain_endpoint(fd, 1);
            return -err;
        }
        usleep(50000);
    }

    rd_log("startup presence timeout - endpoint stopped\n");
    stop_and_drain_endpoint(fd, 1);
    return -EBUSY;
}

static void start_phase(sequence_state_t *state, uint64_t now) {
    const sequence_phase_t *phase = &g_phases[state->phase];
    state->phase_start_input = state->input_count;
    state->phase_deadline_ms = now + PHASE_MS;
    rd_log("PHASE %02d/%02d START: %s expected=%s b2mask=%02x b3mask=%02x\n",
           state->phase + 1, PHASE_COUNT, phase->label,
           phase->expect_all_clear ? "b2=00,b3=00" :
           phase->expect_set ? "bit set" : "bit clear",
           phase->b2_mask, phase->b3_mask);
    notify("Xbox 360 R&D %d/%d: %s", state->phase + 1, PHASE_COUNT,
           phase->label);
}

static void complete_sequence(sequence_state_t *state) {
    int pass_count = 0;
    rd_log("SEQUENCE COMPLETE: input_reports=%u report_gaps=%u\n",
           (unsigned)state->input_count, (unsigned)state->gap_count);
    for (int i = 0; i < PHASE_COUNT; i++) {
        if (state->phase_pass[i]) pass_count++;
        rd_log("SUMMARY PHASE %02d %s: %s\n", i + 1, g_phases[i].label,
               state->phase_pass[i] ? "PASS" : "FAIL");
    }
    rd_log("SUMMARY RESULT: %d/%d phases passed\n", pass_count, PHASE_COUNT);
    notify("Xbox 360 R&D complete: %d/%d phases passed - send gc_status.log",
           pass_count, PHASE_COUNT);
    state->completed = 1;
}

static void advance_phases(sequence_state_t *state, uint64_t now) {
    if (!state->started || state->completed) return;
    while (now >= state->phase_deadline_ms && !state->completed) {
        const sequence_phase_t *phase = &g_phases[state->phase];
        rd_log("PHASE %02d END: %s reports=%u result=%s\n",
               state->phase + 1, phase->label,
               (unsigned)(state->input_count - state->phase_start_input),
               state->phase_pass[state->phase] ? "PASS" : "FAIL");
        state->phase++;
        if (state->phase >= PHASE_COUNT) {
            complete_sequence(state);
            return;
        }
        start_phase(state, state->phase_deadline_ms);
    }
}

static void observe_input(sequence_state_t *state, uint8_t b2, uint8_t b3,
                          uint64_t now) {
    state->input_count++;
    if (state->last_input_ms != 0) {
        uint64_t gap = now - state->last_input_ms;
        if (gap > 50u) {
            rd_log("REPORT GAP %u: %llums before input=%u\n",
                   (unsigned)state->gap_count++, (unsigned long long)gap,
                   (unsigned)state->input_count);
        }
    }
    state->last_input_ms = now;

    if (!state->have_last_buttons || b2 != state->last_b2 || b3 != state->last_b3) {
        rd_log("RAW input=%u b2=%02x b3=%02x\n", (unsigned)state->input_count,
               b2, b3);
        state->last_b2 = b2;
        state->last_b3 = b3;
        state->have_last_buttons = 1;
    }

    if (!state->started) {
        state->started = 1;
        state->phase = 0;
        rd_log("first valid wrapped input received: starting 17-phase sequence\n");
        start_phase(state, now);
    }
    advance_phases(state, now);
    if (state->completed) return;

    const sequence_phase_t *phase = &g_phases[state->phase];
    int bit_set = (phase->b2_mask && (b2 & phase->b2_mask)) ||
                  (phase->b3_mask && (b3 & phase->b3_mask));
    int matched = 0;
    if (phase->expect_all_clear) {
        matched = (b2 == 0 && b3 == 0);
    } else if (phase->expect_set) {
        matched = bit_set;
        if (matched && phase->control >= 0)
            state->press_seen[phase->control] = 1;
    } else {
        matched = !bit_set && (!phase->require_prior_press ||
                               state->press_seen[phase->control]);
    }
    if (matched && !state->phase_pass[state->phase]) {
        state->phase_pass[state->phase] = 1;
        rd_log("PHASE %02d OBSERVED PASS: %s at input=%u b2=%02x b3=%02x\n",
               state->phase + 1, phase->label, (unsigned)state->input_count,
               b2, b3);
    }
}

static void read_sequence(int fd, struct usb_fs_endpoint *ep) {
    uint8_t buf[64];
    void *buffers[1] = { buf };
    uint32_t lengths[1] = { sizeof(buf) };
    struct usb_fs_start start;
    struct usb_fs_complete complete;
    struct usb_fs_stop stop;
    sequence_state_t state;
    memset(&state, 0, sizeof(state));
    uint64_t wait_deadline = now_ms() + WAIT_INPUT_MS;

    while (!g_stop && !state.completed) {
        uint64_t now = now_ms();
        if (!state.started && now >= wait_deadline) {
            rd_log("FAIL: no valid wrapped input within %ums\n", WAIT_INPUT_MS);
            notify("Xbox 360 R&D failed: no paired controller input");
            break;
        }
        advance_phases(&state, now);
        if (state.completed) break;

        memset(buf, 0, sizeof(buf));
        lengths[0] = sizeof(buf);
        ep->ppBuffer = buffers;
        ep->pLength = lengths;
        ep->nFrames = 1;
        ep->timeout = 50;
        ep->flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
        ep->aFrames = 0;
        ep->status = 0;
        memset(&start, 0, sizeof(start));
        start.ep_index = 0;
        if (ioctl(fd, USB_FS_START, &start) != 0) {
            if (errno == EBUSY) {
                memset(&stop, 0, sizeof(stop));
                stop.ep_index = 0;
                ioctl(fd, USB_FS_STOP, &stop);
                usleep(5000);
                continue;
            }
            rd_log("START failed errno=%d\n", errno);
            break;
        }

        int completed = 0;
        for (int pass = 0; pass < 60 && !g_stop; pass++) {
            memset(&complete, 0, sizeof(complete));
            complete.ep_index = 0;
            if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
                completed = 1;
                break;
            }
            if (errno != EBUSY) {
                rd_log("COMPLETE failed errno=%d\n", errno);
                break;
            }
            usleep(500);
        }
        if (!completed) {
            memset(&stop, 0, sizeof(stop));
            stop.ep_index = 0;
            ioctl(fd, USB_FS_STOP, &stop);
            continue;
        }
        if (lengths[0] < 2) continue;

        uint32_t len = lengths[0];
        if (buf[0] & 0x08u) {
            rd_log("PRESENCE len=%u controller=%d headset=%d\n", (unsigned)len,
                   (buf[1] & 0x80u) != 0, (buf[1] & 0x40u) != 0);
        }
        if (buf[1] != 0x01u || len < 18u) continue;

        const uint8_t *inner = buf + 4;
        if (inner[0] != 0x00u ||
            (inner[1] != 0x13u && inner[1] != 0x14u)) {
            rd_log("REJECT input len=%u inner=%02x %02x\n", (unsigned)len,
                   inner[0], inner[1]);
            continue;
        }
        observe_input(&state, inner[2], inner[3], now_ms());
    }

    if (!state.completed && state.started) {
        rd_log("SEQUENCE STOPPED before completion at phase=%d input_reports=%u\n",
               state.phase + 1, (unsigned)state.input_count);
    }
}

int main(void) {
    int owns_pid = 0;
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    rd_log("Ghost-Control Xbox 360 input-sequence R&D #3 starting (045e:0291 single-query)\n");
    notify("Xbox 360 input R&D: pair controller before test starts");

    if (replace_previous_payload() != 0)
        goto done;
    write_our_pid();
    owns_pid = 1;

    char path[32] = {0};
    for (int pass = 0; pass < 30 && !g_stop; pass++) {
        if (find_receiver(path)) break;
        usleep(1000000);
    }
    if (!path[0] || g_stop) {
        rd_log("FAIL: receiver 045e:0291 not found\n");
        notify("Xbox 360 input R&D: receiver 045e:0291 not found");
        goto done;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        rd_log("open %s failed errno=%d\n", path, errno);
        goto done;
    }
    struct usb_fs_endpoint eps[2];
    struct usb_fs_init init;
    struct usb_fs_open in_open;
    struct usb_fs_open out_open;
    struct usb_fs_close close_ep;
    struct usb_fs_uninit uninit;
    int input_open = 0;
    int output_open = 0;
    memset(eps, 0, sizeof(eps));
    memset(&init, 0, sizeof(init));
    init.pEndpoints = eps;
    init.ep_index_max = 2;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) {
        rd_log("FS_INIT failed errno=%d\n", errno);
        close(fd);
        goto done;
    }
    { int i; for (i = 0; i < 4; i++) { int i2 = i; ioctl(fd, USB_IFACE_DRIVER_DETACH, &i2); } }
    usleep(120000);

    memset(&in_open, 0, sizeof(in_open));
    in_open.ep_index = 0;
    in_open.ep_no = X360W_EP_IN;
    in_open.max_bufsize = 64;
    in_open.max_frames = 1;
    if (ioctl(fd, USB_FS_OPEN, &in_open) != 0) {
        rd_log("IN endpoint 0x81 failed errno=%d\n", errno);
        goto close_uninit;
    }
    input_open = 1;
    rd_log("receiver input claimed: %s IN=0x81 maxpkt=%u\n",
           path, (unsigned)in_open.max_packet_length);

    memset(&out_open, 0, sizeof(out_open));
    out_open.ep_index = 1;
    out_open.ep_no = X360W_EP_OUT;
    out_open.max_bufsize = 64;
    out_open.max_frames = 1;
    if (ioctl(fd, USB_FS_OPEN, &out_open) != 0) {
        rd_log("receiver OUT=0x01 open failed errno=%d\n", errno);
        goto close_uninit;
    }
    output_open = 1;
    rd_log("receiver OUT=0x01 opened for one startup presence query\n");
    if (send_startup_presence_inquiry(fd, &eps[1]) != 0) {
        rd_log("FAIL: startup presence query did not complete safely\n");
        goto close_uninit;
    }
    memset(&close_ep, 0, sizeof(close_ep));
    close_ep.ep_index = 1;
    ioctl(fd, USB_FS_CLOSE, &close_ep);
    output_open = 0;
    rd_log("receiver OUT closed; phase sequence is input-only\n");
    notify("Xbox 360 input R&D ready: pair, then wait for instructions");
    read_sequence(fd, &eps[0]);

close_uninit:
    if (output_open) {
        stop_and_drain_endpoint(fd, 1);
        memset(&close_ep, 0, sizeof(close_ep));
        close_ep.ep_index = 1;
        ioctl(fd, USB_FS_CLOSE, &close_ep);
    }
    if (input_open) {
        memset(&close_ep, 0, sizeof(close_ep));
        close_ep.ep_index = 0;
        ioctl(fd, USB_FS_CLOSE, &close_ep);
    }
    memset(&uninit, 0, sizeof(uninit));
    ioctl(fd, USB_FS_UNINIT, &uninit);
    close(fd);

done:
    if (owns_pid) {
        write_handoff_ack();
        unlink(PID_PATH);
    }
    if (g_log_fd >= 0) close(g_log_fd);
    return 0;
}
