#pragma once

#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/* Microsoft Xbox 360 Wireless Receiver.  This is deliberately an exact
 * profile: it is not the direct-XInput 045e:028e device used by some
 * third-party receivers.  TESTER-B captured this receiver as interface
 * class ff/subclass 5d/protocol 81 with IN=81 and OUT=01. */
#define X360W_VID       0x045eu
#define X360W_PID       0x0291u
#define X360W_EP_IN     0x81u
#define X360W_EP_OUT    0x01u

typedef struct {
    int      controller_present; /* -1 until the first receiver status frame */
    int      presence_changed;
    uint32_t input_count;
    int      face_bits_valid;
    int      face_changed;
    uint8_t  face_previous;
    uint8_t  face_current;
    uint32_t face_transition_count;
} x360w_state_t;

int         x360w_is_supported_vidpid(uint16_t vid, uint16_t pid);
const char *x360w_name(void);
void        x360w_state_init(x360w_state_t *state);

/* Ask the receiver to emit its current controller-presence status. The PS5
 * USB_FS interface serializes endpoint transfers, so no IN request may be
 * pending while this OUT request is active. Returns only after the OUT
 * transfer completed, or after it was stopped and drained on failure. */
int         x360w_send_presence_inquiry(int fd, struct usb_fs_endpoint *out_ep);

/* Assign the exact receiver's first physical player LED: top-left flashes
 * briefly and then stays on. This is not the PS5 user assignment. */
void        x360w_assign_player_one_led(int fd, struct usb_fs_endpoint *out_ep);

/* Unwrap a 045e:0291 receiver packet and map its inner Xbox 360 report into
 * ScePadData. Returns 1 only for an input report, or for a present->absent
 * transition where a neutral pad must be injected to prevent a stuck hold. */
int         x360w_handle_packet(const uint8_t *buf, uint32_t len,
                                x360w_state_t *state, ScePadData *out_pad);

/* A short, standard receiver rumble packet used only after real input starts. */
void        x360w_connection_pulse(int fd, struct usb_fs_endpoint *out_ep,
                                   int slot);
