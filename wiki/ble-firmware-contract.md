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
Characteristic UUID: 0000ff01-0000-1000-8000-00805f9b34fb  (commands + notifications)
OTA Characteristic:  0000ff02-0000-1000-8000-00805f9b34fb  (firmware data, write/write-no-response)
```

Short form:

```text
Service 0x00FF
Characteristic 0xFF01 (commands/notify)
Characteristic 0xFF02 (OTA data)
```

The command characteristic supports notifications and writes. Android falls back
to the first notifiable/writable characteristic if exact UUIDs are unavailable.
The OTA characteristic is write-only (`WRITE` + `WRITE_NR`); firmware advertises
a local MTU of 517 so OTA chunks can be up to 514 bytes.

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
0 = restart ESP32
1 = enable low-power mode
2 = disable low-power mode / normal reading
3 = report firmware version (notifies "VER=<PROJECT_VER>")
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
- `3` notifies `VER=<version>` (from `PROJECT_VER` in `firmware/CMakeLists.txt`).

## OTA Firmware Update (BLE)

The firmware can be updated over the air through the existing BLE connection.
Flash layout uses two app slots (`ota_0`/`ota_1`, 1.875 MB each, see
`firmware/partitions_ota.csv`); the transfer writes the inactive slot and swaps
the boot partition on success. Rollback is enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): a new image that fails to reach
`app_main`'s BLE init is rolled back on the next reset.

Protocol (app = GATT client):

1. App writes `OTA:BEGIN:<size_bytes>` to `0xFF01` (the raw `.bin` size).
2. Firmware allocates the inactive OTA slot and notifies `OTA=READY`.
3. App streams the `.bin` as consecutive write-no-response chunks (≤ MTU-3
   bytes) to `0xFF02`. Firmware buffers them in a 16 KB ring buffer and writes
   flash from a dedicated task; sensor readings/notifications pause during the
   transfer.
4. Firmware notifies `OTA=PROG,<pct>` every 10%.
5. After the last byte: image is validated (`esp_ota_end`), boot partition
   swapped, `OTA=OK` notified, and the device restarts ~1.5 s later.

Failure notifications: `OTA=ERRO,<reason>` (`part`, `size`, `busy`, `mem`,
`task`, `begin`, `write`, `verify`, `boot`, `timeout`) and `OTA=ABORTED`
(after `OTA:ABORT` from the app, ring-buffer overflow, or disconnect). A 30 s
gap with no chunks also aborts. On the new image's first boot, the firmware
calls `esp_ota_mark_app_valid_cancel_rollback()` once BLE is up.

Android side: `BLEManager.startFirmwareUpdate(bytes)` negotiates MTU 517,
drives the transfer, and reports through `OtaListener` →
`onOtaProgress`/`onOtaStatus`, which reach the WebView as
`FilterTrackBridge.onOtaProgress/onOtaStatus`. The UI lives in the Settings
tab ("Firmware" tile): pick the `.bin` via the system file picker (validated
to start with the ESP image magic `0xE9`), confirm, watch progress, optional
cancel. Verify the result afterwards with command `3` (`VER=...`).

The first deployment of this OTA-capable layout must be flashed by cable
(partition table changed); see
[firmware-build-and-flash.md](firmware-build-and-flash.md).

## Hardware Pins

Current pin assignments (ESP32-C6 carrier board):

```text
Ultrasonic trigger: GPIO1
Ultrasonic echo:    GPIO0
Sensor power:       (disabled, SENSOR_PWR_ENABLED 0)
Accel I2C SDA:      GPIO13
Accel I2C SCL:      GPIO12
Red LED:            GPIO3
Green LED:          GPIO4
Yellow LED:         GPIO2
```

### ESP32-C6 USB pin conflict (GPIO12/GPIO13)

On the ESP32-C6, **GPIO12 (D-) and GPIO13 (D+) are the native USB-Serial/JTAG
lines**. The accelerometer's I2C bus (SDA=GPIO13 / SCL=GPIO12) shares those two
pins with the native USB connector, which can interfere with I2C (accelerometer
reads `RAW=ERRO`) when the USB PHY or a USB host is driving D+/D-.

Firmware workarounds for this (disabling the secondary USB-Serial/JTAG console,
releasing the USB PHY pads at startup, 50 kHz I2C clock, split write/read
transactions, BLE `DIAG;` messages) were tried in a previous session and have
been **reverted** — they did not resolve the wrong accelerometer behavior. If
the conflict needs solving, the cleanest fix is to move the I2C bus to two free
GPIOs (e.g. GPIO6/GPIO7) in both the wiring and the firmware defines.

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
