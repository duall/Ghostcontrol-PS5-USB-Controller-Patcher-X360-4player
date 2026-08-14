/*
 * Xbox 360 Wireless Receiver R&D payload for the exact Microsoft/Xbox
 * receiver profile observed by TESTER-B: VID 045e, PID 0291.
 *
 * This probe is USB-only. It never creates a PS5 virtual pad, injects input,
 * or disconnects physical MBus devices. It opens the receiver, sends the
 * documented Xbox 360 wireless presence inquiry, and reports pairing/status
 * plus wrapped controller input through both PS5 notifications and the log.
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
#define X360W_VID    0x045eu
#define X360W_PID    0x0291u
#define RUN_MS       180000u
#define INQUIRY_MS   5000u

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
    klog_printf("[X360W-R&D] %s", msg);
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
                struct usb_interface_descriptor id;
                memset(&id, 0, sizeof(id));
                if (ioctl(fd, USB_GET_RX_INTERFACE_DESC, &id) == 0) {
                    rd_log("receiver: %s class=0x%02x sub=0x%02x proto=0x%02x\n",
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

static int open_first_endpoint(int fd, uint8_t ep_index, const uint8_t *candidates,
                               size_t candidate_count, struct usb_fs_open *out) {
    for (size_t i = 0; i < candidate_count; i++) {
        memset(out, 0, sizeof(*out));
        out->ep_index = ep_index;
        out->ep_no = candidates[i];
        out->max_bufsize = 64;
        out->max_frames = 1;
        if (ioctl(fd, USB_FS_OPEN, out) == 0) return 0;
        rd_log("endpoint probe ep=0x%02x rejected errno=%d\n", candidates[i], errno);
    }
    return -1;
}

static int send_presence_inquiry(int fd, struct usb_fs_endpoint *out_ep) {
    static const uint8_t inquiry[12] = {
        0x08, 0x00, 0x0f, 0xc0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    int ret = usb_send_out(fd, out_ep, inquiry, sizeof(inquiry), "x360w-presence");
    rd_log("presence inquiry ret=%d\n", ret);
    return ret;
}

static void log_wrapped_input(const uint8_t *buf, uint32_t len, uint32_t number) {
    if (len < 18) {
        rd_log("input[%u] too short len=%u\n", (unsigned)number, (unsigned)len);
        return;
    }
    const uint8_t *p = buf + 4;
    rd_log("input[%u] inner=%02x %02x b2=%02x b3=%02x lt=%02x rt=%02x "
           "lx=%02x%02x ly=%02x%02x\n", (unsigned)number,
           p[0], p[1], p[2], p[3], p[4], p[5], p[7], p[6], p[9], p[8]);
}

static void read_receiver(int fd, struct usb_fs_endpoint *eps, int out_opened) {
    uint8_t buf[64];
    void *buffers[1] = { buf };
    uint32_t lengths[1] = { sizeof(buf) };
    struct usb_fs_start start;
    struct usb_fs_complete complete;
    struct usb_fs_stop stop;
    int last_present = -1;
    int input_announced = 0;
    uint32_t status_count = 0;
    uint32_t input_count = 0;
    uint64_t deadline = now_ms() + RUN_MS;
    uint64_t next_inquiry = now_ms();
    uint64_t next_wait_notice = now_ms() + 30000u;

    while (!g_stop && now_ms() < deadline) {
        if (out_opened && now_ms() >= next_inquiry) {
            send_presence_inquiry(fd, &eps[1]);
            next_inquiry = now_ms() + INQUIRY_MS;
        }
        if (!input_announced && now_ms() >= next_wait_notice) {
            notify("Xbox 360 receiver: still waiting for paired controller");
            rd_log("status: still no wrapped controller input\n");
            next_wait_notice = now_ms() + 30000u;
        }

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
        if ((buf[0] & 0x08u) != 0) {
            int present = (buf[1] & 0x80u) != 0;
            int headset = (buf[1] & 0x40u) != 0;
            status_count++;
            rd_log("presence[%u] len=%u flags=%02x %02x controller=%d headset=%d\n",
                   (unsigned)status_count, (unsigned)len, buf[0], buf[1],
                   present, headset);
            if (present != last_present) {
                last_present = present;
                if (present)
                    notify("Xbox 360 receiver: controller paired - press A/B/X/Y");
                else
                    notify("Xbox 360 receiver: controller not paired/disconnected");
            }
        }

        if (buf[1] == 0x01u) {
            input_count++;
            if (!input_announced) {
                input_announced = 1;
                notify("Xbox 360 receiver: input active - test buttons and sticks");
            }
            if (input_count <= 80) log_wrapped_input(buf, len, input_count);
        } else if (status_count <= 20) {
            uint8_t b2 = len > 2 ? buf[2] : 0;
            uint8_t b3 = len > 3 ? buf[3] : 0;
            rd_log("packet len=%u: %02x %02x %02x %02x\n", (unsigned)len,
                   buf[0], buf[1], b2, b3);
        }
    }
    rd_log("SUMMARY status=%u wrapped_inputs=%u controller_present=%d\n",
           (unsigned)status_count, (unsigned)input_count, last_present);
}

int main(void) {
    int owns_pid = 0;
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    rd_log("Ghost-Control Xbox 360 Wireless Receiver R&D #1 starting (045e:0291 USB-only)\n");
    notify("Xbox 360 receiver R&D: starting - no PS5 controller injection");

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
        rd_log("receiver 045e:0291 not found\n");
        notify("Xbox 360 receiver R&D: 045e:0291 not found");
        goto done;
    }

    notify("Xbox 360 receiver detected - opening receiver now");
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

    static const uint8_t in_candidates[] = { 0x81, 0x82, 0x84 };
    static const uint8_t out_candidates[] = { 0x01, 0x02, 0x03 };
    if (open_first_endpoint(fd, 0, in_candidates, sizeof(in_candidates), &in_open) != 0) {
        rd_log("no usable receiver IN endpoint\n");
        memset(&uninit, 0, sizeof(uninit));
        ioctl(fd, USB_FS_UNINIT, &uninit);
        close(fd);
        goto done;
    }
    rd_log("receiver IN ep=0x%02x maxpkt=%u\n", in_open.ep_no,
           (unsigned)in_open.max_packet_length);
    int out_opened = open_first_endpoint(fd, 1, out_candidates,
                                         sizeof(out_candidates), &out_open) == 0;
    if (out_opened) {
        rd_log("receiver OUT ep=0x%02x maxpkt=%u\n", out_open.ep_no,
               (unsigned)out_open.max_packet_length);
    } else {
        rd_log("receiver OUT unavailable; pairing status may not refresh\n");
    }

    notify("Xbox 360 receiver ready: press receiver sync, then controller sync");
    read_receiver(fd, eps, out_opened);

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
    rd_log("R&D complete: USB released\n");
    notify("Xbox 360 receiver R&D complete - send gc_status.log");
    if (g_log_fd >= 0) close(g_log_fd);
    return 0;
}
