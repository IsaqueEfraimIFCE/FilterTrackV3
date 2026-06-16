# BLE And Firmware Contract

The firmware source is now in this repo:

```text
firmware/main/FilterTrackv3.c
```

Related firmware files:

```text
firmware/CMakeLists.txt
firmware/main/CMakeLists.txt
firmware/main/idf_component.yml
firmware/dependencies.lock
firmware/sdkconfig
```

Treat `firmware/build/` as generated output and `firmware/managed_components/`
as dependency/vendor code unless the task specifically concerns them.

Older notes referenced external desktop copies under:

```text
C:\Users\Isaque\Desktop\FilterTrackv3espc6\main\FilterTrackv3.c
C:\Users\Isaque\Desktop\FilterTrackv3espc3\main\FilterTrackv3.c
```

Use the in-repo file as the current implementation authority before changing BLE
protocol assumptions.

## Advertised Device

Observed advertised name:

```text
FilterTrackV3
```

The Android app accepts devices whose names start with `filtertrack` and devices
whose addresses were previously connected successfully.

## UUIDs

Expected service and characteristic:

```text
Service UUID:        000000ff-0000-1000-8000-00805f9b34fb
Characteristic UUID: 0000ff01-0000-1000-8000-00805f9b34fb
```

Short form:

```text
Service 0x00FF
Characteristic 0xFF01
```

The characteristic should support notifications and writes. Android falls back
to the first notifiable/writable characteristic if exact UUIDs are unavailable.

## Notifications

Normal mode reads and notifies roughly every `100 ms`.

Legacy payload:

```text
DIST=%.2f
```

Current payload includes accelerometer raw values:

```text
DIST=%.2f;ACC_RAW=%d,%d,%d
```

Error and fallback payloads can include:

```text
DIST=%.2f;RAW=ERRO
DIST=ERRO;ACC_RAW=%d,%d,%d
DIST=ERRO;RAW=ERRO
ERRO_TIMEOUT
```

The notify helper splits payloads longer than 20 bytes into successive BLE
notifications (`BLE_NOTIFY_PAYLOAD_MAX = 20`, `vTaskDelay(10 ms)` between
chunks). A typical two-chunk example:

```text
chunk 1 (20 B): D=35.47;A=12,-8,102
chunk 2  (1 B): 0
```

**`BLEManager` must reassemble chunks before dispatching to JS.**
`handleRawBleData()` in `BLEManager.kt` does this: it buffers bytes until
the next notification starting with `D=` arrives, then dispatches the
previous complete payload. Passing each chunk individually to `onDataReceived`
causes the continuation chunk to be parsed as a standalone distance reading
(wrong value, extra `distanceSamples` entries, inflated "Amostras 2s" count).

Compact BLE sub-fields used when splitting:

```text
D=%.2f
A=%d,%d,%d
RAW=ERRO
```

The WebView parser remains tolerant of:

- `DIST=...`
- distance plus `ACC_RAW`
- numeric strings
- JSON-like payloads

When `ACC_RAW` is present, Android computes the vertical distance using
`acos(|gz_normalized|)` as the angle from ground (see
[sensor-processing-flow.md](sensor-processing-flow.md) — Angle Correction section
for the full mounting explanation). Use `DIST * sin(acos(|gz|))`, **not**
`DIST * sin(asin(|gz|))`.

## Commands

Android sends single-character command strings through BLE writes:

```text
0 = restart ESP32-C3
1 = enable low-power mode
2 = disable low-power mode / normal reading
```

App behavior around commands:

- Low-power finalizes and saves the active session before sending `1`, even if
  the session has no samples.
- Restart finalizes and saves the active session, sends `0`, keeps scanning, and
  avoids reopening filter selection after automatic reconnect.
- Commands use the app modal confirmation system, not browser `window.confirm`.

Firmware behavior around commands:

- `0` calls `esp_restart()`.
- `1` enables low-power mode, powers down the ultrasonic sensor, clears sensor
  error state, and keeps BLE active.
- `2` disables low-power mode and powers the ultrasonic sensor back on.

## Hardware Pins

Current pin assignments:

```text
Ultrasonic trigger: GPIO20
Ultrasonic echo:    GPIO21
Sensor power:       GPIO10
LSM303 accel SDA:   GPIO8
LSM303 accel SCL:   GPIO9
Red LED:            GPIO3
Green LED:          GPIO4
Yellow LED:         GPIO2
```

LED behavior:

- Yellow on: low-power mode.
- Red on: Bluetooth, ultrasonic, or accelerometer error.
- Green on: BLE device connected.
- Red blinking: advertising or active sensor reading while disconnected.

## Filtering Expectations

Current app-side assumptions:

- Raw ultrasonic `DIST` below `25 cm` is invalid.
- First reading after connect/command reset is ignored.
- Robust median/MAD filtering over recent samples is required.
- Flow window is 10 seconds.

Coordinate firmware changes with [sensor-processing-flow.md](sensor-processing-flow.md).
