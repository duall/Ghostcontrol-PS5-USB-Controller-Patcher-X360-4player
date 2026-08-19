#pragma once

#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/* Xbox 360 Wireless Receiver (xpad XTYPE_XBOX360W).  Same wrapped-report
 * protocol on both PIDs; not the bare-XInput 045e:028e dongles.
 *   045e:0719  genuine Microsoft Wireless Receiver for Windows
 *   045e:0291  common third-party clones (some reuse Microsoft's VID/PID)
 * Hardware-confirmed on 045e:0291 as interface class ff / subclass 5d /
 * protocol 81, with IN=0x81 and OUT=0x01 for the first pad. */
#define X360W_VID            0x045eu
#define X360W_PID            0x0291u
#define X360W_PID_GENUINE    0x0719u

/* The receiver is one USB device that hosts up to four wireless pads, each on
 * its own interface with its own interrupt pair.  The data interfaces are
 * interleaved with the headset interfaces, so consecutive pads are two
 * endpoint addresses apart: pad 0 is IN 0x81 / OUT 0x01, pad 1 is 0x83/0x03,
 * and so on.  Linux xpad never has to compute these because the kernel probes
 * it once per interface and it reads the endpoints out of that interface's
 * descriptor; we own the whole device from userspace and must address each pad
 * ourselves.  Receivers that expose fewer pads simply fail USB_FS_OPEN on the
 * higher addresses, which the caller treats as "this pad does not exist". */
#define X360W_MAX_PADS  4u
#define X360W_PAD_EP_IN(pad)   ((uint8_t)(0x81u + 2u * (unsigned)(pad)))
#define X360W_PAD_EP_OUT(pad)  ((uint8_t)(0x01u + 2u * (unsigned)(pad)))

typedef struct {
    int controller_present; /* -1 until the first receiver status frame */
    int presence_changed;
} x360w_state_t;

int         x360w_is_supported_vidpid(uint16_t vid, uint16_t pid);
const char *x360w_name(void);
void        x360w_state_init(x360w_state_t *state);

/* Ask the receiver to emit the presence status for one pad. The PS5 USB_FS
 * interface serializes endpoint transfers, so no IN request may be pending on
 * any endpoint while this OUT request is active. Returns only after the OUT
 * transfer completed, or after it was stopped and drained on failure. */
int         x360w_send_presence_inquiry(int fd, struct usb_fs_endpoint *out_ep,
                                        uint8_t ep_index);

/* Light the physical quadrant LED for this pad: it flashes briefly and then
 * stays lit. xpad_identify_controller uses (pad_nr % 4) + 2, where command 2
 * is the top-left quadrant, so pad 0 gets top-left. The receiver drops this
 * command unless a controller is actually paired, so it must be sent on the
 * presence transition rather than at startup. This is not the PS5 user
 * assignment. */
void        x360w_set_player_led(int fd, struct usb_fs_endpoint *out_ep,
                                 uint8_t ep_index, unsigned pad_nr);

/* Unwrap a wireless-receiver packet and map its inner Xbox 360 report into
 * ScePadData. Returns 1 only for an input report, or for a present->absent
 * transition where a neutral pad must be injected to prevent a stuck hold. */
int         x360w_handle_packet(const uint8_t *buf, uint32_t len,
                                x360w_state_t *state, ScePadData *out_pad);
