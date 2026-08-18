#include "usb_helpers.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/klog.h>
#define LOG(...) klog_printf("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

int usb_send_out_ep(int fd, struct usb_fs_endpoint *ep, uint8_t ep_index,
                    const uint8_t *data, uint32_t len, const char *tag) {
    void *bufs[1] = { (void *)data };
    uint32_t lens[1] = { len };
    struct usb_fs_start start;
    struct usb_fs_complete complete;
    struct usb_fs_stop stop;

    ep->ppBuffer = bufs;
    ep->pLength  = lens;
    ep->nFrames  = 1;
    ep->timeout  = 150;
    ep->flags    = 0;
    ep->aFrames  = 0;
    ep->status   = 0;

    memset(&start, 0, sizeof(start));
    start.ep_index = ep_index;
    if (ioctl(fd, USB_FS_START, &start) != 0) {
        LOG("OUT %s START fail errno=%d\n", tag, errno);
        return -errno;
    }
    /* USB_FS_COMPLETE is _IOR: ep_index is an output naming whichever endpoint
     * finished, and the value we pass in is discarded. On a device with several
     * endpoints armed at once another one can surface here first, so keep
     * reading until ours appears rather than claiming the first completion. */
    for (int w = 0; w < 20; w++) {
        memset(&complete, 0, sizeof(complete));
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            if (complete.ep_index == ep_index)
                return 0;
            continue;
        }
        if (errno != EBUSY) {
            LOG("OUT %s COMPLETE fail errno=%d\n", tag, errno);
            return -errno;
        }
        usleep(50000);
    }

    /* bufs/lens live on this frame. Cancel the transfer and wait for the
     * endpoint to release them before returning. */
    LOG("OUT %s timeout\n", tag);
    memset(&stop, 0, sizeof(stop));
    stop.ep_index = ep_index;
    ioctl(fd, USB_FS_STOP, &stop);
    for (int w = 0; w < 20; w++) {
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
    return -EBUSY;
}

int usb_send_out(int fd, struct usb_fs_endpoint *ep,
                 const uint8_t *data, uint32_t len, const char *tag) {
    return usb_send_out_ep(fd, ep, 1, data, len, tag);
}

int usb_send_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b) {
    uint8_t buf[2] = { a, b };
    char tag[8];
    snprintf(tag, sizeof(tag), "%02x%02x", a, b);
    return usb_send_out(fd, ep, buf, 2, tag);
}
