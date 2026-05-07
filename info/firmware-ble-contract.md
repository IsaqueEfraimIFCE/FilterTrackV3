# Firmware And BLE Contract

## Source Context

The firmware source is outside this repo. Previous workspace notes referenced:

```text
C:\Users\Isaque\Desktop\FilterTrackv3espc6\main\FilterTrackv3.c
```

Use this file if available when changing BLE protocol assumptions.

## Advertised Device

Observed firmware advertised name:

```text
FilterTrackV3
```

The Android app accepts devices whose names start with `filtertrack` and also devices whose addresses were previously connected successfully.

## BLE Service And Characteristic

`BLEManager.kt` expects:

```text
Service UUID:        000000ff-0000-1000-8000-00805f9b34fb
Characteristic UUID: 0000ff01-0000-1000-8000-00805f9b34fb
```

Short form:

```text
Service 0x00FF
Characteristic 0xFF01
```

The characteristic should support notifications and writes. The Android code falls back to the first notifiable/writable characteristic if the exact UUID is not found.

## Notifications

Firmware sends distance notifications roughly every `100 ms` in normal mode.

Observed payload format:

```text
DIST=%.2f
```

The WebView parser is tolerant and can also extract distances from numeric strings or JSON-like payloads.

## Commands

The Android app sends single-character command strings through BLE writes:

```text
0 = restart ESP32-C3
1 = enable low-power mode
2 = disable low-power mode / resume normal reading
```

App behavior around commands:

- Low-power command finalizes and saves any active session before sending command `1`, even if the session has no samples.
- Restart command finalizes and saves any active session, sends command `0`, keeps the app scanning for reconnection, and avoids reopening filter selection when the device reconnects automatically.
- Commands use the app's own confirmation modal rather than `window.confirm`.

## Sensor Filtering Expectations

Current app-side filtering assumes:

- Raw distances below `25 cm` are invalid.
- First reading after connect/command reset is ignored.
- Robust median/MAD filtering over recent samples is needed due to sensor noise.
- The flow window is 10 seconds.

Coordinate with firmware changes before altering these constants.
