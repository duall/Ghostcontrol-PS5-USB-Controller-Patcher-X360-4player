#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

int  usb_send_out(int fd, struct usb_fs_endpoint *ep,
                  const uint8_t *data, uint32_t len, const char *tag);

/* Same as usb_send_out but for a device whose OUT endpoint is not at USB_FS
 * ep_index 1 — the Xbox 360 receiver hosts four pads and needs one OUT index
 * per pad. Blocks until the transfer completes, and stops the endpoint on
 * failure so the kernel does not retain a pointer to a caller buffer that is
 * about to go out of scope. */
int  usb_send_out_ep(int fd, struct usb_fs_endpoint *ep, uint8_t ep_index,
                     const uint8_t *data, uint32_t len, const char *tag);
int  usb_send_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b);
