/*
 * 8BitDo Arcade Stick USB R&D payload.
 *
 * This diagnostic deliberately does not create a virtual pad, inject input,
 * send an output report, or change the controller's mode. It is intended to
 * capture the USB identity, endpoint layout, and input reports before a
 * Ghost-Control support profile is authored.
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
#define BITDO_VID     0x2dc8u
#define SWITCH_VID    0x057eu
#define SWITCH_PID    0x2009u
#define XINPUT_VID    0x045eu
#define XINPUT_PID    0x028eu
#define CAPTURE_MS    75000u

#ifdef GC_BUTTON_MAP_PROBE
#define MAP_PHASE_MS  4000u
static const char *const g_map_controls[] = {
    "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left",
    "Y", "X", "B", "A", "L", "R", "ZL", "ZR",
    "Minus / Select", "Plus / Start", "Home", "Turbo", "P1", "P2"
};
#endif

extern int32_t sceKernelSendNotificationRequest(int unk0, void *req,
                                                size_t size, int unk1);
typedef struct { char _unk[45]; char message[3075]; } NotifyRequest;

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
    klog_printf("[8BitDo-AS-RD] %s", msg);
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

/* Give a previous Ghost-Control payload a chance to close its ugen handle. */
static void replace_previous_payload(void) {
    char buf[24] = {0};
    int fd = open(PID_PATH, O_RDONLY);
    if (fd < 0) return;
    read(fd, buf, sizeof(buf) - 1);
    close(fd);

    pid_t old = (pid_t)atoi(buf);
    if (old <= 0 || old == getpid()) return;
    unlink(HANDOFF_PATH);
    rd_log("handoff: requesting shutdown from pid=%d\n", old);
    if (kill(old, SIGTERM) != 0 && errno != ESRCH)
        rd_log("handoff: SIGTERM pid=%d errno=%d\n", old, errno);
    for (int pass = 0; pass < 50 && !g_stop; pass++) {
        if (handoff_acknowledged(old) || !pid_alive(old)) break;
        usleep(100000);
    }
    usleep(1500000);
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

static int is_target(uint16_t vid, uint16_t pid) {
#ifdef GC_XINPUT_MAP_PROBE
    return vid == XINPUT_VID && pid == XINPUT_PID;
#else
    return vid == BITDO_VID || (vid == SWITCH_VID && pid == SWITCH_PID);
#endif
}

static int find_target(char out_path[32], uint16_t *out_vid, uint16_t *out_pid) {
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
            rd_log("scan: %s VID=0x%04x PID=0x%04x class=0x%02x configs=%u\n",
                   path, vid, pid, desc.bDeviceClass,
                   (unsigned)desc.bNumConfigurations);
            if (is_target(vid, pid)) {
                struct usb_interface_descriptor id;
                memset(&id, 0, sizeof(id));
                if (ioctl(fd, USB_GET_RX_INTERFACE_DESC, &id) == 0) {
                    rd_log("target: interface=%u class=0x%02x sub=0x%02x proto=0x%02x endpoints=%u\n",
                           (unsigned)id.bInterfaceNumber, id.bInterfaceClass,
                           id.bInterfaceSubClass, id.bInterfaceProtocol,
                           (unsigned)id.bNumEndpoints);
                } else {
                    rd_log("target: interface descriptor unavailable errno=%d\n", errno);
                }
                strncpy(out_path, path, 31);
                out_path[31] = '\0';
                *out_vid = vid;
                *out_pid = pid;
                found = 1;
                close(fd);
                break;
            }
        } else {
            rd_log("scan: %s USB_GET_DEVICE_DESC errno=%d\n", path, errno);
        }
        close(fd);
    }
    closedir(dp);
    return found;
}

static int probe_first_input_endpoint(int fd) {
    static const uint8_t candidates[] = { 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87 };
    int selected = -1;
    for (size_t i = 0; i < sizeof(candidates); i++) {
        struct usb_fs_open ep;
        memset(&ep, 0, sizeof(ep));
        ep.ep_index = 0;
        ep.ep_no = candidates[i];
        ep.max_bufsize = 64;
        ep.max_frames = 1;
        if (ioctl(fd, USB_FS_OPEN, &ep) == 0) {
            rd_log("endpoint: IN 0x%02x opened maxpkt=%u\n", ep.ep_no,
                   (unsigned)ep.max_packet_length);
            if (selected < 0 && ep.max_packet_length > 0 && ep.max_packet_length <= 64)
                selected = ep.ep_no;
            struct usb_fs_close close_ep;
            memset(&close_ep, 0, sizeof(close_ep));
            close_ep.ep_index = 0;
            ioctl(fd, USB_FS_CLOSE, &close_ep);
        } else {
            rd_log("endpoint: IN 0x%02x rejected errno=%d\n", candidates[i], errno);
        }
    }
    return selected;
}

static void probe_output_endpoints(int fd) {
    static const uint8_t candidates[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
    for (size_t i = 0; i < sizeof(candidates); i++) {
        struct usb_fs_open ep;
        memset(&ep, 0, sizeof(ep));
        ep.ep_index = 1;
        ep.ep_no = candidates[i];
        ep.max_bufsize = 64;
        ep.max_frames = 1;
        if (ioctl(fd, USB_FS_OPEN, &ep) == 0) {
            rd_log("endpoint: OUT 0x%02x opened maxpkt=%u (not written)\n", ep.ep_no,
                   (unsigned)ep.max_packet_length);
            struct usb_fs_close close_ep;
            memset(&close_ep, 0, sizeof(close_ep));
            close_ep.ep_index = 1;
            ioctl(fd, USB_FS_CLOSE, &close_ep);
        } else {
            rd_log("endpoint: OUT 0x%02x rejected errno=%d\n", candidates[i], errno);
        }
    }
}

static void log_packet(const uint8_t *buf, uint32_t len, uint32_t number,
                       const char *phase) {
    char hex[3 * 64 + 1];
    size_t at = 0;
    uint32_t clipped = len > 64 ? 64 : len;
    for (uint32_t i = 0; i < clipped && at + 3 < sizeof(hex); i++) {
        int written = snprintf(hex + at, sizeof(hex) - at, "%02x%s", buf[i],
                               i + 1 == clipped ? "" : " ");
        if (written < 0) break;
        at += (size_t)written;
    }
    hex[at] = '\0';
    if (phase) {
        rd_log("input[%u] phase=%s len=%u: %s\n", (unsigned)number, phase,
               (unsigned)len, hex);
    } else {
        rd_log("input[%u] len=%u: %s\n", (unsigned)number, (unsigned)len, hex);
    }
}

/* Nintendo 0x30 reports increment byte 1 as a packet timer. It is not an
 * input change, so omit it when deciding whether a report merits a log line. */
static int input_changed(const uint8_t *previous, uint32_t previous_len,
                         const uint8_t *current, uint32_t current_len) {
    if (previous_len != current_len) return 1;
    uint32_t clipped = current_len > 64 ? 64 : current_len;
    for (uint32_t i = 0; i < clipped; i++) {
        if (i == 1 && current[0] == 0x30) continue;
        if (previous[i] != current[i]) return 1;
    }
    return 0;
}

static void capture_input(int fd, struct usb_fs_endpoint *eps) {
    uint8_t buf[64], last[64];
    void *buffers[1] = { buf };
    uint32_t lengths[1] = { sizeof(buf) };
    struct usb_fs_start start;
    struct usb_fs_complete complete;
    struct usb_fs_stop stop;
    uint32_t packets = 0, changed = 0;
    uint32_t last_len = 0;
    int have_last = 0;
    const char *phase_label = NULL;
#ifdef GC_BUTTON_MAP_PROBE
    size_t map_phase = (size_t)-1;
    uint64_t map_started = now_ms();
#endif
    uint64_t deadline = now_ms() + CAPTURE_MS;

#ifdef GC_BUTTON_MAP_PROBE
    rd_log("button-map: %u controls, %u ms each; press and hold the prompted control for one second, then release\n",
           (unsigned)(sizeof(g_map_controls) / sizeof(g_map_controls[0])),
           (unsigned)MAP_PHASE_MS);
    notify("8BitDo button map: follow each on-screen control prompt");
#else
    rd_log("capture: %u ms; press every stick direction and every labelled button once\n",
           (unsigned)CAPTURE_MS);
    notify("8BitDo Arcade Stick R&D: press every direction and button now");
#endif
    while (!g_stop && now_ms() < deadline) {
#ifdef GC_BUTTON_MAP_PROBE
        size_t next_phase = (size_t)((now_ms() - map_started) / MAP_PHASE_MS);
        if (next_phase < sizeof(g_map_controls) / sizeof(g_map_controls[0])) {
            phase_label = g_map_controls[next_phase];
            if (next_phase != map_phase) {
                map_phase = next_phase;
                rd_log("button-map: phase %u/%u — press %s now\n",
                       (unsigned)(map_phase + 1),
                       (unsigned)(sizeof(g_map_controls) / sizeof(g_map_controls[0])),
                       phase_label);
                notify("8BitDo button map: press %s", phase_label);
            }
        } else {
            phase_label = "sequence complete";
        }
#endif
        memset(buf, 0, sizeof(buf));
        lengths[0] = sizeof(buf);
        eps[0].ppBuffer = buffers;
        eps[0].pLength = lengths;
        eps[0].nFrames = 1;
        eps[0].timeout = 50;
        eps[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
        eps[0].aFrames = 0;
        eps[0].status = 0;
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
            rd_log("capture: START failed errno=%d\n", errno);
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
                rd_log("capture: COMPLETE failed errno=%d\n", errno);
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
        if (lengths[0] == 0) continue;
        packets++;
        int packet_changed = !have_last ||
                             input_changed(last, last_len, buf, lengths[0]);
        if (packets <= 8 || packet_changed) {
            if (packet_changed) changed++;
            log_packet(buf, lengths[0], packets, phase_label);
        }
        memcpy(last, buf, lengths[0] > 64 ? 64 : lengths[0]);
        last_len = lengths[0];
        have_last = 1;
    }
    rd_log("capture: SUMMARY packets=%u changed=%u\n", (unsigned)packets,
           (unsigned)changed);
}

int main(void) {
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
#ifdef GC_XINPUT_MAP_PROBE
    rd_log("8BitDo Arcade Stick XInput R&D starting (USB-only; no virtual pad or output reports)\n");
    notify("8BitDo Arcade Stick XInput R&D starting");
#else
    rd_log("8BitDo Arcade Stick R&D starting (USB-only; no virtual pad or output reports)\n");
    notify("8BitDo Arcade Stick R&D starting");
#endif

    replace_previous_payload();
    write_our_pid();

    char path[32] = {0};
    uint16_t vid = 0, pid = 0;
    for (int pass = 0; pass < 30 && !g_stop; pass++) {
        if (find_target(path, &vid, &pid)) break;
        usleep(1000000);
    }
    if (!path[0] || g_stop) {
#ifdef GC_XINPUT_MAP_PROBE
        rd_log("target not found: expected Arcade Stick XInput mode 045e:028e\n");
        notify("8BitDo Arcade Stick XInput R&D: target not found; send gc_status.log");
#else
        rd_log("target not found: expected 8BitDo vendor 0x2dc8 or Switch mode 057e:2009\n");
        notify("8BitDo Arcade Stick R&D: target not found; send gc_status.log");
#endif
        goto done;
    }
    rd_log("target selected: %s VID=0x%04x PID=0x%04x\n", path, vid, pid);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        rd_log("open %s failed errno=%d\n", path, errno);
        goto done;
    }
    struct usb_fs_endpoint eps[2];
    struct usb_fs_init init;
    struct usb_fs_uninit uninit;
    memset(eps, 0, sizeof(eps));
    memset(&init, 0, sizeof(init));
    init.pEndpoints = eps;
    init.ep_index_max = 2;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) {
        rd_log("USB_FS_INIT failed errno=%d\n", errno);
        close(fd);
        goto done;
    }
    for (int iface = 0; iface < 4; iface++) ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
    usleep(120000);

    int in_ep = probe_first_input_endpoint(fd);
    probe_output_endpoints(fd);
    if (in_ep < 0) {
        rd_log("no usable interrupt-IN endpoint; no reports captured\n");
    } else {
        struct usb_fs_open open_ep;
        memset(&open_ep, 0, sizeof(open_ep));
        open_ep.ep_index = 0;
        open_ep.ep_no = (uint8_t)in_ep;
        open_ep.max_bufsize = 64;
        open_ep.max_frames = 1;
        if (ioctl(fd, USB_FS_OPEN, &open_ep) == 0) {
            rd_log("capture: using IN 0x%02x maxpkt=%u\n", open_ep.ep_no,
                   (unsigned)open_ep.max_packet_length);
            capture_input(fd, eps);
            struct usb_fs_close close_ep;
            memset(&close_ep, 0, sizeof(close_ep));
            close_ep.ep_index = 0;
            ioctl(fd, USB_FS_CLOSE, &close_ep);
        } else {
            rd_log("capture: reopen IN 0x%02x failed errno=%d\n", in_ep, errno);
        }
    }
    memset(&uninit, 0, sizeof(uninit));
    ioctl(fd, USB_FS_UNINIT, &uninit);
    close(fd);

done:
    write_handoff_ack();
    unlink(PID_PATH);
    rd_log("R&D complete: USB released; reload normal payload only after unplug/replug if needed\n");
    notify("8BitDo Arcade Stick R&D complete - send gc_status.log");
    if (g_log_fd >= 0) close(g_log_fd);
    return 0;
}
