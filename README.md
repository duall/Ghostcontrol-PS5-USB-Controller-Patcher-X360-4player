# Ghostcontrol — by StonedModder

If you enjoy my work - please consider donating to my BTC address: 

`bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5`


Use third-party USB controllers on PS5. Reads USB HID input from a plugged-in controller and injects it into a virtual DualSense via the PS5's `scePadVirtualDeviceInsertData` path (Ghostpad VDI path).

**Hardware-tested controllers:** 8BitDo Ultimate 2, EasySMX X10 (wired Switch mode and its 2.4G receiver), and the original wired DualShock 4 v1.

---



https://github.com/user-attachments/assets/6583b1c2-3d3d-4f2e-9e79-689121fea4a3



## Features

- 60Hz input streaming from USB HID controller → virtual DualSense
- PS5 notifications: startup, controller connect/disconnect, detected controller type
- User assignment: virtual DualSense is bound to the foreground user on startup
- Full button mapping: face buttons, triggers, sticks, dpad, L3/R3, PS button
- Auto-reconnect on controller unplug/replug

---

## Requirements

- PS5 with kernel exploit (tested on jailbroken PS5)
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
- USB controller — see supported list below

---

## Supported Controllers

**Full matrix:** [Supported_controllers.md](Supported_controllers.md) — detection rules, endpoints, ignored devices, and Manba vs 8BitDo notes.

| Controller | Mode | VID:PID | Status |
|-----------|------|---------|--------|
| 8BitDo Ultimate 2 | Nintendo Switch Pro | 057e:2009 | ✅ Working |
| EasySMX X10 | Switch mode (rear switch) | 057e:2009 | ✅ Working |
| EasySMX X10 | 2.4G receiver (rear switch) | 045e:028e | ✅ Working |
| DualShock 4 v1 | Wired USB | 054c:05c4 | ✅ Working |
| 8BitDo Ultimate 2 | Native | 2dc8:310b | Untested |
| 8BitDo Ultimate 2C Wireless (81HD) | XInput | 2dc8:310a | ✅ Working |
| Xbox 360 Wireless Receiver | 4 pads, wireless | 045e:0291 / 045e:0719 | ✅ Working (`0291` tested) |

**Xbox 360 Wireless Receiver:** one USB device hosting up to four pads, so each pad gets its own endpoint pair (IN `0x81`/`0x83`/`0x85`/`0x87`), its own slot, and its own virtual DualSense — which is what gives every pad a normal profile screen and its matching quadrant LED. All four work at once. `045e:0291` is the common clone ID (hardware-tested); `045e:0719` is the genuine Microsoft receiver and uses the same protocol. See [Supported_controllers.md](Supported_controllers.md#xbox-360-wireless-receiver-four-pads).

See [othercontrollersGuide.md](othercontrollersGuide.md) for adding new controllers.

---

## Build

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
cd payload
make clean all
```

Output: `ghost-control-x10-ds4-x360w.elf`

## Deploy

```sh
# Deploy to PS5 (replace IP)
nc -w 5 192.168.1.xxx 9021 < payload/ghost-control-x10-ds4-x360w.elf
```

Or set `PS5_HOST` in your environment:
```sh
make -C payload deploy PS5_HOST=192.168.1.xxx
```

---

## How It Works

1. **VDA**: Creates a virtual DualSense via `scePadVirtualDeviceAddDevice(type=3)`
2. **klog capture**: Monitors klogsrv TCP to detect the `DEVICE_ADDED` event and get the device handle
3. **force_bind**: Binds the virtual device to the foreground user via ShellUI MBus IPC
4. **USB HID thread**: Detaches `usb_hid0` from the controller, opens raw USB FS endpoints, runs the Nintendo Switch Pro Controller USB handshake, then reads 60Hz input reports
5. **VDI inject**: Parses HID reports into `ScePadData` and calls `scePadVirtualDeviceInsertData` at 60Hz

See `ProControllerResearch.md` for the full research documentation on the USB HID protocol.

---

## Files

| File | Description |
|------|-------------|
| `gc_main.c` | Main payload — VDA, VDI, USB HID thread, button parsing |
| `shellui_pad.c` | ShellUI PT_ATTACH helper for force_bind via MBus |
| `shellui_pad.h` | Header for shellui_pad |
| `Makefile` | Build system |
| `ghost-control-ps5.elf` | Pre-compiled payload (deploy directly) |
| `ProControllerResearch.md` | Full USB protocol research for Nintendo Switch Pro Controller |
| `Supported_controllers.md` | Compatibility matrix (source of truth) |
| `CHANGELOG.md` | Release history |
| `othercontrollersGuide.md` | Guide for adding other USB HID controllers |

---

## License

GPL-3.0-or-later
