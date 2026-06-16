# Firmware And BLE Contract

## Source Context

The firmware source is now in this repo:

```text
firmware/main/FilterTrackv3.c
```

Related project files:

```text
firmware/CMakeLists.txt
firmware/main/CMakeLists.txt
firmware/main/idf_component.yml
firmware/dependencies.lock
firmware/sdkconfig
```

Treat `firmware/build/` as generated ESP-IDF output and `firmware/managed_components/` as dependency/vendor code unless a task explicitly targets them.

Older workspace notes referenced external copies under:

```text
C:\Users\Isaque\Desktop\FilterTrackv3espc6\main\FilterTrackv3.c
C:\Users\Isaque\Desktop\FilterTrackv3espc3\main\FilterTrackv3.c
```

Use the in-repo file as the current implementation authority.

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

Firmware reads and notifies roughly every `100 ms` in normal mode.

Legacy payload format:

```text
DIST=%.2f
```

Current ESP32-C3 firmware can also send raw LSM303DLHC readings:

```text
DIST=%.2f;ACC_RAW=%d,%d,%d;MAG_RAW=%d,%d,%d
```

If one source fails, firmware can notify:

```text
DIST=%.2f;RAW=ERRO
DIST=ERRO;ACC_RAW=%d,%d,%d;MAG_RAW=%d,%d,%d
DIST=ERRO;RAW=ERRO
ERRO_TIMEOUT
```

The BLE notify helper splits long raw-measurement payloads into compact chunks:

```text
D=%.2f
A=%d,%d,%d
M=%d,%d,%d
RAW=ERRO
```

The WebView parser remains tolerant of old distance-only payloads, numeric strings, and JSON-like payloads. When `ACC_RAW` is present, Android computes the sensor Z-axis angle from the ground and uses `DIST * sin(angle)` as the corrected vertical distance for live flow/session samples. `MAG_RAW` is parsed for debug heading display; distance correction uses the accelerometer angle.

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

Firmware behavior around commands:

- Command `0` calls `esp_restart()`.
- Command `1` enables low-power mode, powers down the ultrasonic sensor through `SENSOR_PWR_PIN`, clears sensor error state, and keeps BLE active.
- Command `2` disables low-power mode and powers the ultrasonic sensor back on.

## Firmware Hardware Pins

Current pin assignments in `firmware/main/FilterTrackv3.c`:

```text
Ultrasonic trigger: GPIO20
Ultrasonic echo:    GPIO21
Sensor power:       GPIO10
LSM303 I2C SDA:     GPIO8
LSM303 I2C SCL:     GPIO9
Red LED:            GPIO3
Green LED:          GPIO4
Yellow LED:         GPIO2
```

Status LED behavior:

- Yellow on: low-power mode.
- Red on: Bluetooth, ultrasonic, or LSM303 error.
- Green on: BLE device connected.
- Red blinking: advertising or active sensor reading while disconnected.

## Sensor Filtering Expectations

Current app-side filtering assumes:

- Raw ultrasonic `DIST` values below `25 cm` are invalid.
- First reading after connect/command reset is ignored.
- Robust median/MAD filtering over recent samples is needed due to sensor noise.
- The flow window is 10 seconds.

Coordinate with firmware changes before altering these constants.
