# 8BitDo Arcade Stick tester capture

This payload is a read-only USB diagnostic for the 8BitDo Arcade Stick. It
does **not** create a PS5 virtual controller, inject inputs, or send any output
report to the stick. Its only stateful action is stopping an already-running
Ghost-Control payload so the USB endpoint can be released safely.

The receiver changes identity with the stick's hardware mode: the observed
2.4G Switch mode is `057e:2009`, while 2.4G XInput mode is `045e:028e`. The
earlier reported `2dc8:3207` identity was not observed on the PS5 and is not a
support profile.

Switch mode has HID IN `0x81`, OUT `0x02`, and 64-byte Nintendo `0x30`
reports. The revised diagnostic ignores that report's rolling timer byte so
returned logs contain control changes rather than one line per packet.

## Before each pass

1. Connect only the Arcade Stick's included 2.4G receiver directly to the PS5.
   Disconnect other USB controllers, receivers, drives, and hubs for this
   capture. The receiver—not the stick itself—is the USB device we must profile.
2. Pair and power on the stick through that receiver. Do not connect the stick
   by USB-C or use Bluetooth for these captures.
3. Set the stick's hardware mode to the pass being tested, then unplug and
   reconnect the receiver before launching the payload. This makes each mode a
   clean USB enumeration and keeps the returned log unambiguous.

## Send the diagnostic payload

For Switch mode, send `payload/ghost-control-8bitdo-arcade-stick-rd.elf`. For
XInput mode, send
`payload/ghost-control-8bitdo-arcade-stick-xinput-button-map.elf`. Use the same
ELF loader normally used for Ghost-Control (TCP port 9021).

The payload will show a notification when it finds the stick. For about 75
seconds, leave the stick neutral for two seconds, then press and release each
control one at a time: every joystick direction, each face button, Start,
Select, Home, Turbo, P1, and P2 (where present). Keep the controller plugged in
until the completion notification appears.

Retrieve and send back this file after each pass:

```
/data/ghostpad/gc_status.log
```

Use the PS5 FTP service if available (typically port 2121), or copy the matching
`[8BitDo-AS-RD]` klog lines. Do not send the normal Ghost-Control log from an
earlier session; this diagnostic truncates the file at launch.

## Required passes

Run these as separate captures and label each returned log with the mode:

1. `2.4G Switch mode`
2. `2.4G PC / XInput mode`

If the stick has only one available receiver mode, send that one log and a photo
of the mode-switch labels. If a pass says `target not found`, send that log
anyway: its scan lines identify the actual USB VID:PID we need.

## What the log gives us

The capture records the stick's VID:PID, USB interface class/subclass/protocol,
which IN and OUT endpoints open, their maximum packet lengths, and raw reports
that changed while controls were pressed. That is enough to choose the correct
Ghost-Control parser and write a named hardware profile. Support will remain
unverified until this capture is followed by a PS5 gameplay test covering all
controls, unplug/replug, and coexistence with a physical DualSense.
