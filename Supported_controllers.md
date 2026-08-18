# Supported controllers

Compatibility matrix for **Ghostcontrol — Manba V2 NBJr USB patch** (`payload/`). A device is *supported* only when Ghostcontrol can identify it, detach the USB driver, open the correct endpoints, parse input into `ScePadData`, and inject through the virtual DualSense (VDI) path.

For how to add new devices, see [othercontrollersGuide.md](othercontrollersGuide.md). Build and deploy: [README.md](README.md).

---

## Hardware-tested

| Controller | Mode | VID:PID | Parser / path | Notes |
|------------|------|---------|---------------|-------|
| 8BitDo Ultimate 2 | Nintendo Switch Pro | `057e:2009` | Nintendo / Manba Switch | Original confirmed path; IN `0x81`, 64-byte reports |
| EasySMX X10 | Switch mode (rear switch) | `057e:2009` | Nintendo parser, nonstandard endpoint path | IN `0x84`, OUT `0x03`, 64-byte reports. The normal Nintendo init commands are skipped; a USB descriptor FD is held through virtual-pad assignment to avoid a PS5 `/dev/ugen` timing race. Right-stick Y is normalized for the virtual DualSense. After X10 assignment is confirmed, Ghostcontrol releases the competing physical DualSense for that user. |
| EasySMX X10 | 2.4G receiver (rear switch) | `045e:028e` | XInput / Manba XUSB | Verified with the supplied receiver: IN `0x82`, OUT `0x02`, 20-byte XInput reports. The receiver's existing radio pairing is retained; Ghostcontrol does not send the Manba-specific enable packet. |
| DualShock 4 v1 | Direct wired USB | `054c:05c4` | DS4 USB parser | Verified official v1 only: IN `0x84`, OUT `0x03`, 64-byte report `0x01`. Includes a one-time connection-confirmation rumble after input streaming begins. Bluetooth, the Sony wireless adaptor, DS4 v2, and third-party DS4-layout pads are outside this tested scope. |
| 8BitDo Ultimate 2C Wireless (81HD) | XInput | `2dc8:310a` | Manba XUSB (reuse) | Composite device: IN `0x84`, OUT `0x05` (not classic `0x81`/`0x01`). USB-C cable and 2.4G dongle. Merged in [#19](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher/pull/19). |
| Xbox 360 Wireless Receiver | 2.4G, up to 4 pads | `045e:0291` (tested) / `045e:0719` | `controller_x360w.c` → XInput parser | All four pads working simultaneously, each as a separate console controller. `0291` is the common clone ID and was hardware-tested; `0719` is the genuine Microsoft receiver and uses the same xpad `XTYPE_XBOX360W` protocol. Not the bare-XInput `045e:028e` dongles (e.g. EasySMX X10). See [Xbox 360 Wireless Receiver](#xbox-360-wireless-receiver-four-pads) below. |

---

## Documented / untested on PS5

| Controller | Mode | VID:PID | Status |
|------------|------|---------|--------|
| 8BitDo Ultimate 2 | Native (8BitDo USB) | `2dc8:310b` | Listed in README; **not** routed in this Manba-focused build until a dedicated path is added |

---

## Manba V2 NBJr (this patch)

| Mode | Detection | Endpoints (typical) | Status |
|------|-----------|---------------------|--------|
| PC / XInput | `045e:028e` or USB interface subclass `0x5d`, protocol `0x01` | IN `0x81`, OUT `0x02` or `0x01` | Routed via `controller_mamba.c` |
| Switch USB | `057e:2009` | Usually IN `0x81`, 64-byte HID | Standard Switch Pro clones use the normal path. The EasySMX X10 uses the same VID:PID but is identified by its usable IN `0x84` / OUT `0x03` endpoint pair at runtime. |

---

## Ignored (safe skip)

| Device | VID:PID | Reason |
|--------|---------|--------|
| Manba receiver idle / update | `1a34:f517` | Not a playable controller |
| Xbox One / Series (GIP) | Interface `0x47` / `0xd0` | Ignored in this Manba patch build after identification |
| Unknown USB devices | — | Descriptor read only; no destructive endpoint probe when VID:PID is unrecognized |

---

## XInput endpoint selection (EasySMX X10, 8BitDo 2C, and Manba)

Classic Manba XInput uses IN `0x81` and OUT `0x02` (fallback `0x01`). The EasySMX X10 receiver presents as `045e:028e` but uses IN `0x82`, OUT `0x02`; that profile is verified by hardware and skips the Manba-only enable command. The 8BitDo Ultimate 2C Wireless uses IN `0x84` and OUT `0x05` because it is a **composite** device (XInput + HID keyboard/mouse). `probe_one_path()` matches `2dc8:310a` **before** the generic XInput interface normalizes to `045e:028e`, so the correct endpoints are opened in `usb_hid_thread()`.

---

## Xbox 360 Wireless Receiver (four pads)

Unlike every other supported device, this is **one** USB device hosting up to four wireless pads. Five things make all four work at once:

- **Per-pad endpoints.** Pad interfaces are interleaved with headset interfaces, so pads sit two addresses apart: IN `0x81`/OUT `0x01`, `0x83`/`0x03`, `0x85`/`0x05`, `0x87`/`0x07`. All eight open under one `USB_FS_INIT` with `ep_index_max = 8`; a receiver with fewer pads simply fails `USB_FS_OPEN` on the higher addresses.
- **Completions are dispatched, not polled.** `USB_FS_COMPLETE` is `_IOR` — `ep_index` is an *output* naming whichever endpoint finished, and the value passed in is discarded. Polling it per pad credits one pad's report to another and the next arm fails with `EBUSY`. The loop reads one completion and hands it to the pad owning that endpoint.
- **INs stay armed with no timeout.** The receiver reports *edges*, not state, so an unarmed endpoint loses a press permanently. Each pad re-arms with `timeout = 0` before its report is parsed, and a repeat injector resends held buttons.
- **One virtual DualSense per pad.** Each pad claims its own slot and VDA on pairing, so the console treats it as a separate controller and shows its normal profile screen instead of taking over pad 1.
- **LED on the presence edge.** The receiver drops the quadrant-LED command unless a pad is paired, so it is sent on the present transition. PS5 `USB_FS` rejects an OUT while interrupt INs are armed, so the INs are stopped and drained for that one OUT.

---

## Sources

- `payload/gc_main.c` — scan, probe, USB threads
- `payload/controller_mamba.h` / `controller_mamba.c` — Manba + shared XUSB parser
- `payload/controller_nintendo.c` — Switch Pro protocol
- `payload/controller_ds4.c` — wired DualShock 4 input parser
- `payload/controller_x360w.c` — Xbox 360 Wireless Receiver packet unwrap, presence, LED
- `README.md` — quick reference table
