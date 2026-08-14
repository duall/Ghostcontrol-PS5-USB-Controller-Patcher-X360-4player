/*
 * XInput comparison R&D payload.
 *
 * This is intentionally a USB-only diagnostic. It takes over from a running
 * Ghost-Control process, but it never creates a virtual pad, injects input, or
 * disconnects physical MBus devices. It compares three startup conditions for
 * the exact direct XInput topology observed on the Xbox tester hardware:
 *
 *   A. no output command
 *   B. Linux xpad's read-only vendor inquiry
 *   C. Ghost-Control's legacy Manba enable packet
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

#include "usb_helpers.h"

#define LOG_DIR      "/data/ghostpad"
#define LOG_PATH     "/data/ghostpad/gc_status.log"
#define PID_PATH     "/data/ghostpad/gc_main.pid"
#define HANDOFF_PATH "/data/ghostpad/gc_handoff.ready"
#define XINPUT_VID   0x045eu
#define XINPUT_PID   0x028eu
#define PHASE_MS     45000u

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
    klog_printf("[XInput-R&D] %s", msg);
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
        notify("XInput R&D aborted: existing payload did not stop");
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

static int find_target(char out_path[32]) {
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
            if (vid == XINPUT_VID && pid == XINPUT_PID) {
                struct usb_interface_descriptor id;
                memset(&id, 0, sizeof(id));
                if (ioctl(fd, USB_GET_RX_INTERFACE_DESC, &id) == 0) {
                    rd_log("target: %s interface class=0x%02x sub=0x%02x proto=0x%02x\n",
                           path, id.bInterfaceClass, id.bInterfaceSubClass,
                           id.bInterfaceProtocol);
                }
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

typedef struct {
    uint32_t total;
    uint32_t valid;
    uint32_t rejected;
    uint32_t events;
    uint32_t up, down, left, right, start, back, l3, r3;
    uint32_t lb, rb, guide, a, b, x, y;
    uint8_t last_b2;
    uint8_t last_b3;
    int have_last;
} phase_stats_t;

static void count_rising(uint8_t old_value, uint8_t new_value, uint8_t bit,
                         uint32_t *counter) {
    if (!(old_value & bit) && (new_value & bit)) (*counter)++;
}

static void capture_packet(const uint8_t *buf, uint32_t len, phase_stats_t *st,
                           const char *phase) {
    st->total++;
    if (len < 14 || buf[0] != 0x00u || buf[1] != 0x14u) {
        st->rejected++;
        if (st->rejected <= 12) {
            uint8_t b1 = len > 1 ? buf[1] : 0;
            uint8_t b2 = len > 2 ? buf[2] : 0;
            uint8_t b3 = len > 3 ? buf[3] : 0;
            rd_log("%s reject[%u] len=%u: %02x %02x %02x %02x\n", phase,
                   (unsigned)st->rejected, (unsigned)len, buf[0], b1, b2, b3);
        }
        return;
    }

    st->valid++;
    uint8_t b2 = buf[2], b3 = buf[3];
    if (!st->have_last || b2 != st->last_b2 || b3 != st->last_b3) {
        uint8_t old_b2 = st->have_last ? st->last_b2 : 0;
        uint8_t old_b3 = st->have_last ? st->last_b3 : 0;
        count_rising(old_b2, b2, 0x01u, &st->up);
        count_rising(old_b2, b2, 0x02u, &st->down);
        count_rising(old_b2, b2, 0x04u, &st->left);
        count_rising(old_b2, b2, 0x08u, &st->right);
        count_rising(old_b2, b2, 0x10u, &st->start);
        count_rising(old_b2, b2, 0x20u, &st->back);
        count_rising(old_b2, b2, 0x40u, &st->l3);
        count_rising(old_b2, b2, 0x80u, &st->r3);
        count_rising(old_b3, b3, 0x01u, &st->lb);
        count_rising(old_b3, b3, 0x02u, &st->rb);
        count_rising(old_b3, b3, 0x04u, &st->guide);
        count_rising(old_b3, b3, 0x10u, &st->a);
        count_rising(old_b3, b3, 0x20u, &st->b);
        count_rising(old_b3, b3, 0x40u, &st->x);
        count_rising(old_b3, b3, 0x80u, &st->y);
        st->events++;
        if (st->events <= 100) {
            rd_log("%s buttons[%u] b2=%02x b3=%02x lt=%02x rt=%02x\n", phase,
                   (unsigned)st->events, b2, b3, buf[4], buf[5]);
        }
        st->last_b2 = b2;
        st->last_b3 = b3;
        st->have_last = 1;
    }
}

static void log_summary(const char *phase, const phase_stats_t *st) {
    rd_log("%s SUMMARY total=%u valid=%u rejected=%u button_changes=%u\n", phase,
           (unsigned)st->total, (unsigned)st->valid, (unsigned)st->rejected,
           (unsigned)st->events);
    rd_log("%s COUNTS A=%u B=%u X=%u Y=%u Guide=%u LB=%u RB=%u Start=%u Back=%u "
           "Up=%u Down=%u Left=%u Right=%u L3=%u R3=%u\n", phase,
           (unsigned)st->a, (unsigned)st->b, (unsigned)st->x, (unsigned)st->y,
           (unsigned)st->guide, (unsigned)st->lb, (unsigned)st->rb,
           (unsigned)st->start, (unsigned)st->back, (unsigned)st->up,
           (unsigned)st->down, (unsigned)st->left, (unsigned)st->right,
           (unsigned)st->l3, (unsigned)st->r3);
}

static void wait_for_phase_start(const char *phase, const char *description) {
    rd_log("%s prepares: %s\n", phase, description);
    notify("XInput R&D %s in 5 sec: press A/B/X/Y/Guide repeatedly; do not use sticks", phase);
    for (int i = 0; i < 50 && !g_stop; i++) usleep(100000);
}

static void capture_phase(int fd, struct usb_fs_endpoint *eps, const char *phase) {
    uint8_t buf[64];
    void *buffers[1] = { buf };
    uint32_t lengths[1] = { sizeof(buf) };
    struct usb_fs_start start;
    struct usb_fs_complete complete;
    struct usb_fs_stop stop;
    phase_stats_t st;
    memset(&st, 0, sizeof(st));

    uint64_t deadline = now_ms() + PHASE_MS;
    rd_log("%s capture started for %u ms\n", phase, (unsigned)PHASE_MS);
    while (!g_stop && now_ms() < deadline) {
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
            rd_log("%s START failed errno=%d\n", phase, errno);
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
                rd_log("%s COMPLETE failed errno=%d\n", phase, errno);
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
        if (lengths[0] > 0) capture_packet(buf, lengths[0], &st, phase);
    }
    log_summary(phase, &st);
    notify("XInput R&D %s complete; next phase starts shortly", phase);
}

static int xpad_vendor_inquiry(int fd) {
    uint8_t response[20];
    struct usb_ctl_request req;
    memset(response, 0, sizeof(response));
    memset(&req, 0, sizeof(req));
    /* USB_TYPE_VENDOR | USB_DIR_IN | USB_RECIP_INTERFACE: no device state
     * write. This is the direct Xbox 360 inquiry used by Linux xpad. */
    req.ucr_request.bmRequestType = 0xc1u;
    req.ucr_request.bRequest = 0x01u;
    USETW(req.ucr_request.wValue, 0x0100u);
    USETW(req.ucr_request.wIndex, 0x0000u);
    USETW(req.ucr_request.wLength, sizeof(response));
    req.ucr_data = response;
    int ret = ioctl(fd, USB_DO_REQUEST, &req);
    rd_log("Phase B xpad vendor inquiry ret=%d errno=%d actlen=%u bytes: "
           "%02x %02x %02x %02x\n", ret, ret ? errno : 0,
           (unsigned)req.ucr_actlen, response[0], response[1], response[2], response[3]);
    return ret;
}

int main(void) {
    int owns_pid = 0;
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    rd_log("Ghost-Control XInput consolidated R&D #2 starting (USB-only)\n");
    notify("Ghost-Control: XInput consolidated R&D #2 starting");

    if (replace_previous_payload() != 0)
        goto done;
    write_our_pid();
    owns_pid = 1;

    char path[32] = {0};
    for (int pass = 0; pass < 30 && !g_stop; pass++) {
        if (find_target(path)) break;
        usleep(1000000);
    }
    if (!path[0] || g_stop) {
        rd_log("target 045e:028e not found; ending\n");
        notify("XInput R&D: 045e:028e controller not found");
        goto done;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        rd_log("open %s failed errno=%d\n", path, errno);
        goto done;
    }
    struct usb_fs_endpoint eps[2];
    struct usb_fs_init init;
    struct usb_fs_open open_ep;
    struct usb_fs_close close_ep;
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

    memset(&open_ep, 0, sizeof(open_ep));
    open_ep.ep_index = 0;
    open_ep.ep_no = 0x81u;
    open_ep.max_bufsize = 64;
    open_ep.max_frames = 1;
    if (ioctl(fd, USB_FS_OPEN, &open_ep) != 0) {
        rd_log("IN 0x81 open failed errno=%d\n", errno);
        memset(&uninit, 0, sizeof(uninit));
        ioctl(fd, USB_FS_UNINIT, &uninit);
        close(fd);
        goto done;
    }
    rd_log("IN 0x81 opened maxpkt=%u\n", (unsigned)open_ep.max_packet_length);

    memset(&open_ep, 0, sizeof(open_ep));
    open_ep.ep_index = 1;
    open_ep.ep_no = 0x02u;
    open_ep.max_bufsize = 64;
    open_ep.max_frames = 1;
    int out_opened = ioctl(fd, USB_FS_OPEN, &open_ep) == 0;
    rd_log("OUT 0x02 opened=%d errno=%d\n", out_opened, out_opened ? 0 : errno);

    wait_for_phase_start("Phase A", "baseline, no output command");
    capture_phase(fd, eps, "Phase A");

    if (!g_stop) {
        xpad_vendor_inquiry(fd);
        wait_for_phase_start("Phase B", "after read-only Xbox 360 vendor inquiry");
        capture_phase(fd, eps, "Phase B");
    }

    if (!g_stop) {
        if (out_opened) {
            static const uint8_t manba_enable[] = { 0x01, 0x03, 0x0e };
            int ret = usb_send_out(fd, &eps[1], manba_enable, sizeof(manba_enable),
                                   "xinput-rd2-manba-enable");
            rd_log("Phase C Manba enable ret=%d\n", ret);
        } else {
            rd_log("Phase C Manba enable skipped: OUT 0x02 unavailable\n");
        }
        wait_for_phase_start("Phase C", "after legacy Manba enable command");
        capture_phase(fd, eps, "Phase C");
    }

    memset(&close_ep, 0, sizeof(close_ep));
    close_ep.ep_index = 0;
    ioctl(fd, USB_FS_CLOSE, &close_ep);
    if (out_opened) {
        memset(&close_ep, 0, sizeof(close_ep));
        close_ep.ep_index = 1;
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
    rd_log("R&D complete: USB released, reload the normal payload to play\n");
    notify("XInput R&D complete - send gc_status.log");
    if (g_log_fd >= 0) close(g_log_fd);
    return 0;
}
