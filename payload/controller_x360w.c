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
    return vid == X360W_VID && pid == X360W_PID;
}

const char *x360w_name(void) {
    return "Xbox 360 Wireless Receiver";
}

void x360w_state_init(x360w_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->controller_present = -1;
}

static void x360w_stop_and_drain(int fd, uint8_t ep_index) {
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

int x360w_send_presence_inquiry(int fd, struct usb_fs_endpoint *out_ep) {
    /* Linux xpad's XTYPE_XBOX360W receiver-presence inquiry. */
    static const uint8_t inquiry[12] = {
        0x08, 0x00, 0x0f, 0xc0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    void *buffers[1] = { (void *)inquiry };
    uint32_t lengths[1] = { sizeof(inquiry) };
    struct usb_fs_start start;
    struct usb_fs_complete complete;

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
        LOG("Xbox 360 receiver presence START errno=%d\n", err);
        x360w_stop_and_drain(fd, 1);
        return -err;
    }
    for (int pass = 0; pass < 20; pass++) {
        memset(&complete, 0, sizeof(complete));
        complete.ep_index = 1;
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            LOG("Xbox 360 receiver presence inquiry completed\n");
            return 0;
        }
        if (errno != EBUSY) {
            int err = errno;
            LOG("Xbox 360 receiver presence COMPLETE errno=%d\n", err);
            x360w_stop_and_drain(fd, 1);
            return -err;
        }
        usleep(50000);
    }
    LOG("Xbox 360 receiver presence timeout; endpoint stopped\n");
    x360w_stop_and_drain(fd, 1);
    return -EBUSY;
}

void x360w_assign_player_one_led(int fd, struct usb_fs_endpoint *out_ep) {
    /* Linux xpad XTYPE_XBOX360W command 2: player 1/top-left blink, then on. */
    static const uint8_t player_one[12] = {
        0x00, 0x00, 0x08, 0x42, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    int ret = usb_send_out(fd, out_ep, player_one, sizeof(player_one),
                           "x360w-player-one");
    LOG("Xbox 360 receiver player-1 LED ret=%d\n", ret);
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
    state->face_changed = 0;

    /* Receiver status packet: bit 3 means a presence update and byte 1 bit 7
     * means the first wireless controller is connected.  Do not inject a
     * neutral state for a normal "still present" status frame: those arrive
     * alongside held input and would spuriously release a held button. */
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
        return 0;
    }

    /* A wireless receiver input wrapper has byte 1 == 01.  TESTER-B's
     * hardware captured the inner Xbox 360 report at offset 4 as 00 13;
     * accept that exact 19-byte variant as well as the conventional 00 14
     * variant, but never parse unrelated receiver traffic. */
    if (buf[1] != 0x01u || len < 18u)
        return 0;
    const uint8_t *inner = buf + 4;
    if (inner[0] != 0x00u ||
        (inner[1] != 0x13u && inner[1] != 0x14u))
        return 0;

    /* Trace the physical receiver's ABXY nibble separately from VDI. This is
     * intentionally bounded at 240 transitions so a tester can run a normal
     * game session without growing gc_status.log without limit. */
    uint8_t face = inner[3] & 0xf0u;
    if (!state->face_bits_valid || face != state->face_current) {
        state->face_previous = state->face_bits_valid ? state->face_current : 0u;
        state->face_current = face;
        state->face_bits_valid = 1;
        state->face_changed = 1;
        state->face_transition_count++;
        if (state->face_transition_count <= 240u)
            LOG("Xbox 360 receiver face[%u] input=%u prev=%02x now=%02x\n",
                (unsigned)state->face_transition_count,
                (unsigned)(state->input_count + 1u),
                state->face_previous, state->face_current);
    }

    /* The inner layout is the same Xbox 360 button/trigger/stick layout
     * already hardware-validated for the XInput parser. */
    mamba_xinput_parse_input(inner, out_pad);
    state->input_count++;
    return 1;
}

void x360w_connection_pulse(int fd, struct usb_fs_endpoint *out_ep, int slot) {
    /* Linux xpad XTYPE_XBOX360W packet format: a short strong/weak pulse,
     * followed by an explicit stop. It is sent only after an input report. */
    static const uint8_t rumble[12] = {
        0x00, 0x01, 0x0f, 0xc0, 0x00, 0x70, 0x50,
        0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t stop[12] = {
        0x00, 0x01, 0x0f, 0xc0, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00
    };
    int start_ret = usb_send_out(fd, out_ep, rumble, sizeof(rumble),
                                 "x360w-ready");
    usleep(180000);
    int stop_ret = usb_send_out(fd, out_ep, stop, sizeof(stop), "x360w-stop");
    LOG("slot[%d] Xbox 360 receiver ready rumble start=%d stop=%d\n",
        slot, start_ret, stop_ret);
}
