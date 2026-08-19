/* controller_x360w.c - exact Microsoft Xbox 360 Wireless Receiver support */

#include "controller_x360w.h"
#include "controller_mamba.h"
#include "usb_helpers.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/klog.h>
#define LOG(...) klog_printf("[GC] " __VA_ARGS__)
#else
#include <stdio.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

int x360w_is_supported_vidpid(uint16_t vid, uint16_t pid) {
    return vid == X360W_VID &&
           (pid == X360W_PID || pid == X360W_PID_GENUINE);
}

const char *x360w_name(void) {
    return "Xbox 360 Wireless Receiver";
}

void x360w_state_init(x360w_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->controller_present = -1;
}

/* USB_FS_COMPLETE is _IOR: it reports whichever endpoint finished and ignores
 * the index we hand it, so a cancelled transfer can only be reaped by draining
 * the queue until it runs dry. */
static void x360w_stop_and_drain(int fd, uint8_t ep_index) {
    struct usb_fs_stop stop;
    struct usb_fs_complete complete;

    memset(&stop, 0, sizeof(stop));
    stop.ep_index = ep_index;
    ioctl(fd, USB_FS_STOP, &stop);
    for (int pass = 0; pass < 20; pass++) {
        memset(&complete, 0, sizeof(complete));
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            if (complete.ep_index == ep_index)
                break;
            continue;
        }
        if (errno != EBUSY)
            break;
        usleep(50000);
    }
}

int x360w_send_presence_inquiry(int fd, struct usb_fs_endpoint *out_ep,
                                uint8_t ep_index) {
    /* Linux xpad's XTYPE_XBOX360W receiver-presence inquiry.
     * USB_FS keeps these pointers after START returns, so they must not live
     * on this function's stack. Each pad's inquiry runs to completion before
     * the next one starts, so one shared pair of arrays is safe. */
    static const uint8_t inquiry[12] = {
        0x08, 0x00, 0x0f, 0xc0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static void *buffers[1];
    static uint32_t lengths[1];
    struct usb_fs_start start;
    struct usb_fs_complete complete;

    buffers[0] = (void *)inquiry;
    lengths[0] = sizeof(inquiry);

    out_ep->ppBuffer = buffers;
    out_ep->pLength = lengths;
    out_ep->nFrames = 1;
    out_ep->timeout = 150;
    out_ep->flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
    out_ep->aFrames = 0;
    out_ep->status = 0;
    memset(&start, 0, sizeof(start));
    start.ep_index = ep_index;
    if (ioctl(fd, USB_FS_START, &start) != 0) {
        int err = errno;
        LOG("Xbox 360 receiver presence START ep_index=%u errno=%d\n",
            (unsigned)ep_index, err);
        x360w_stop_and_drain(fd, ep_index);
        return -err;
    }
    for (int pass = 0; pass < 40; pass++) {
        memset(&complete, 0, sizeof(complete));
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            if (complete.ep_index != ep_index)
                continue;
            return 0;
        }
        if (errno != EBUSY) {
            int err = errno;
            LOG("Xbox 360 receiver presence COMPLETE ep_index=%u errno=%d\n",
                (unsigned)ep_index, err);
            x360w_stop_and_drain(fd, ep_index);
            return -err;
        }
        usleep(50000);
    }
    LOG("Xbox 360 receiver presence timeout ep_index=%u; endpoint stopped\n",
        (unsigned)ep_index);
    x360w_stop_and_drain(fd, ep_index);
    return -EBUSY;
}

void x360w_set_player_led(int fd, struct usb_fs_endpoint *out_ep,
                          uint8_t ep_index, unsigned pad_nr) {
    /* Linux xpad XTYPE_XBOX360W LED packet: 00 00 08 4N, where N is the
     * command. xpad_identify_controller uses (pad_nr % 4) + 2, so command 2
     * (top-left) through 5 (bottom-left) map to pads 0 through 3. */
    uint8_t led[12] = {
        0x00, 0x00, 0x08, 0x40, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    unsigned command = (pad_nr % 4u) + 2u;
    led[3] = (uint8_t)(0x40u + command);
    int ret = usb_send_out_ep(fd, out_ep, ep_index, led, sizeof(led),
                              "x360w-player-led");
    LOG("Xbox 360 receiver pad %u LED command=%u ret=%d\n",
        pad_nr, command, ret);
}

static void neutral_pad(ScePadData *pad) {
    memset(pad, 0, sizeof(*pad));
    pad->leftStick.x = 128;
    pad->leftStick.y = 128;
    pad->rightStick.x = 128;
    pad->rightStick.y = 128;
    pad->connected = 1;
    pad->quat.w = 1.0f;
}

int x360w_handle_packet(const uint8_t *buf, uint32_t len,
                        x360w_state_t *state, ScePadData *out_pad) {
    if (!buf || !state || !out_pad || len < 2)
        return 0;

    state->presence_changed = 0;

    /* Receiver status packet: bit 3 means a presence update and byte 1 bit 7
     * means the first wireless controller is connected.  xpad handles the
     * presence change and then falls through to the pad-data check rather than
     * returning, so a frame that carries both a status bit and input is still
     * parsed.  Do not inject a neutral state for a normal "still present"
     * frame: those arrive alongside held input and would spuriously release a
     * held button. */
    if ((buf[0] & 0x08u) != 0) {
        int present = (buf[1] & 0x80u) != 0;
        int was_present = state->controller_present;
        if (present != was_present) {
            state->controller_present = present;
            state->presence_changed = 1;
            LOG("Xbox 360 receiver presence controller=%d headset=%d\n",
                present, (buf[1] & 0x40u) != 0);
            if (!present && was_present > 0) {
                neutral_pad(out_pad);
                return 1;
            }
        }
    }

    /* A wireless receiver input wrapper has byte 1 == 01.  Linux xpad keys the
     * inner Xbox 360 report on inner[0] == 00 alone; the inner length byte is
     * firmware-dependent (0x13 and 0x14 both observed), so filtering on it
     * drops legitimate button edges. */
    if (buf[1] != 0x01u || len < 18u)
        return 0;
    const uint8_t *inner = buf + 4;
    if (inner[0] != 0x00u)
        return 0;

    /* The inner layout is the same Xbox 360 button/trigger/stick layout
     * already hardware-validated for the XInput parser. */
    mamba_xinput_parse_input(inner, out_pad);
    return 1;
}
