# Changelog

All notable changes to this project are documented here. Release tags use semver (`1.0.x`).

## Unreleased

### Added

- **EasySMX X10** hardware-tested support in Switch mode (`057e:2009`): nonstandard USB endpoints IN `0x84` / OUT `0x03`, using the existing Nintendo report parser.

### Fixed

- Hold the X10 descriptor FD through virtual-device assignment, preventing the PS5 `/dev/ugen` node timing race that previously caused repeated connect/disconnect attempts.
- Skip standard Nintendo initialization commands for the X10, which otherwise reset the controller.
- Complete the delayed physical-controller handoff after X10 assignment is confirmed, so games use the virtual controller instead of a concurrently connected DualSense.
- Normalize the X10's right-stick vertical axis for the virtual DualSense.

## [1.0.5] — 2026-07-05

### Added

- **8BitDo Ultimate 2C Wireless (81HD)** in XInput mode (`2dc8:310a`), including USB-C and 2.4G dongle ([PR #19](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher/pull/19), thanks @AdailtonSilva88).
- Reuses the existing Manba XUSB 20-byte report parser; device-specific USB endpoints IN `0x84` / OUT `0x05`.
- **`Supported_controllers.md`** — organized compatibility matrix linked from the README.

### Fixed

- **Probe ordering:** `2dc8:310a` is matched by VID:PID before the generic XInput interface branch, preventing mis-normalization to Manba `045e:028e` and wrong endpoint opens.

## [1.0.4] — 2026-06-11

- Third-party PS4-protocol controller table ([#5](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher/pull/5)).
- DualShock 3 / SIXAXIS support ([#6](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher/pull/6)).

## [1.0.3] — 2026-06-09

- Improved Xbox detection (sonik-br issue).

## [1.0.2] — 2026-06-08

- DS4 / HORIPAD / XIM4 controller support.

## [1.0.1] — 2026-06-08

- Early maintenance release.

## [1.0.0] — 2026-06-08

- Initial public release.
