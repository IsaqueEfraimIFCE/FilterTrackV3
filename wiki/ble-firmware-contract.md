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
4 = run storage partition self-test (destructive; notifies "ST=OK,<bytes>" or "ST=ERRO,<stage>")
5 = enable sand/fixed-filter mode (persisted in NVS; notifies "SAND=1")
6 = disable sand/fixed-filter mode (notifies "SAND=0"; aborts an open wash without records)
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
- `4` runs the storage-partition self-test in a background task (see "Sensor
  Data Storage Partition" below). Sensor readings pause while it runs. It
  **erases the whole `storage` partition**, so don't send it once real sensor
  data lives there.
- `5`/`6` toggle sand/fixed-filter mode (the wash-detection state machine
  below). The state persists in NVS (`ftstore/sandmode`) and survives
  reboots; every write replies with the current state (`SAND=1`/`SAND=0`),
  even when nothing changed. `6` aborts an open wash without writing records.
  The app's minimal mode enforces sand mode: it sends `5` shortly after
  connect and re-sends it whenever a `SAND=0` notification arrives.

### Sand-mode wash detection (firmware >= 1.6.0, generic + configurable)

A wash is a **transient** that can move the level in either direction
(ascending or descending filters) and is influenced by other factors, so
every threshold is a runtime-adjustable parameter (see `CFG:` below):

- **Start**: |distance − baseline| > `start_mm` sustained for `start_s`
  (either direction), or a stable window already that far away → opens a
  wash, notifies `WL=WSTART`.
- **Events**: inside the wash, each max↔min excursion of the level with
  amplitude ≥ `ev_mm` is an excursion; consecutive excursions whose
  throughput (mm/min) is within ±`ev_tol`% of the group mean are aggregated
  into one **event**. When a group closes it is written to flash and
  `WL=WEVT,<min_mm>,<max_mm>,<rate_mm_min>,<n_excursions>` is notified.
- **End**: the wash closes when the level has been continuously stable
  (windows of `stable_s` with range ≤ `stable_mm`, medians close) for
  `end_s`, after at least `min_s`; forced close at `max_s`. Summary record
  written; `WL=EVT,<before>,<after>,<n_events>` notified; baseline := after;
  re-arms after `cool_s`.
- Outside a wash, stable windows only track baseline drift > `drift_mm`.
- Command `6` (sand mode off) aborts an open wash without records.

### Wash-log record v2 (`WASHLOG_VERSION 2`, 20 bytes/slot)

`WL=R,<boot>,<uptime_s>,<type>,<a_mm>,<b_mm>,<count>,<aux>`

- `type 0` (wash summary): a=before, b=after, count=nº of events,
  aux=duration in s.
- `type 1` (aggregated event): a=min level, b=max level, count=nº of
  excursions aggregated, aux=mean throughput in mm/min.

Version bump reformats the partition (old records discarded on first boot).

### Wash-log transfer (`LOG:` commands)

The app pulls and clears the persisted wash log over `0xFF01`:

```text
LOG:COUNT     -> WL=CNT,<count>,<boot_count>,<uptime_s>
LOG:READ      -> dump task: WL=CNT,... then one WL=R,... per record, then WL=END
LOG:ACK:<n>   -> erases the log if <n> == current count (WL=CLR);
                 WL=ERR,count if a new record arrived after the dump (re-read first)
LOG:TEST      -> appends one synthetic event + wash record (WL=ADD,<count>)
```

Errors notify `WL=ERR,<reason>` (`busy`, `task`, `count`, `write`, `erase`).
`LOG:READ`/`LOG:ACK` are refused while a dump/erase task or the storage
self-test is running (`WL=ERR,busy`).

Android minimal mode runs this silently on connect: `LOG:COUNT` ~4 s after
connect, then `LOG:READ`, stores the records in local storage
(`filtertrack.washLog.v1`), and acknowledges with `LOG:ACK:<n>` so the
sensor's log is cleared once safely copied.

### Detection configuration (`CFG:` commands, firmware >= 1.6.0)

```text
CFG:GET               -> one notify per item:
                         CFG=<key>,<val>,<default>,<min>,<max>,<description>
                         ... then CFG=END
CFG:SET:<key>:<value> -> CFG=OK,<key>,<value> | CFG=ERR,faixa,... | CFG=ERR,chave
CFG:RESET             -> all keys back to defaults, CFG=OK,reset,0
```

Keys (persisted in NVS): `start_mm, start_s, stable_mm, stable_s, end_s,
min_s, max_s, cool_s, ev_mm, ev_tol, drift_mm`. The app's Ajustes tab has a
generic "Detecção de lavagem" card that renders whatever `CFG:GET` returns,
so new firmware keys appear automatically.

App behavior: `WL=WSTART` finalizes the current minimal-mode session
("wash_started") and shows a "Lavagem em andamento" banner with a live count
of `WL=WEVT` events; `WL=EVT` clears the banner and starts the next session.
The banner self-clears after 35 min or on disconnect in case `WL=EVT` is
lost. During the wash the minimal-mode chart restarts at `WL=WSTART` and
accumulates points every 2 s (no inversion resets, no display-threshold cut)
so all wash events are visible, while the velocity/vazão tiles stay frozen
at the latest above-threshold reading — see
[android-app.md](android-app.md) "Minimal Mode" for the full display rules.

### Sensor tilt calibration (`CAL:` commands, firmware >= 1.4.0)

If the sensor is mounted with a tilt, the ultrasonic distance reads longer
than the true vertical distance (`measured = real / cos(θ)`). The app sends
the known real distance while the water level is steady (filter not being
washed); the firmware computes `factor = real / measured` (= `cos θ`), stores
it in NVS (`calppm`, factor × 10⁶) and multiplies **every** distance reading
by it from then on — including the sand-mode wash detection.

```text
CAL:SET:<real_cm>  -> applies factor = real_cm / last raw reading
                      replies CAL=OK,<factor 4dp>,<angle_deg 1dp>
                      or CAL=ERR,valor | CAL=ERR,semleitura | CAL=ERR,faixa
CAL:GET            -> replies CAL=<factor>,<angle_deg>
CAL:CLEAR          -> resets factor to 1.0; replies CAL=OK,1.0000,0.0
```

Rules:

- `CAL:SET` uses the last valid **raw** reading (kept internally), which must
  be less than 5 s old (`CAL=ERR,semleitura` otherwise), so recalibrating with
  a factor already active still works.
- Accepted factor range is 0.50–1.05 (`CAL=ERR,faixa` outside it); values
  slightly above 1.0 are clamped to 1.0 since tilt can only lengthen the path.
- Applying a new factor rescales the persisted sand-mode baseline by
  `new/old` and resets the stability window, so a calibration change is not
  misread as a wash event.
- The UI lives in the full app's Ajustes tab ("Calibração de inclinação");
  the app sends `CAL:GET` on connect to display the current factor/angle.

## Sensor Data Storage Partition

The ESP32-C6 4 MB layout (`firmware/partitions_ota.csv`) reserves a raw data
partition for sensor data logging:

```text
nvs       0x9000    16 KB
otadata   0xd000     8 KB
phy_init  0xf000     4 KB
ota_0     0x10000   1344 KB (app slot A)
ota_1     0x160000  1344 KB (app slot B)
storage   0x2b0000  1344 KB (raw data, subtype undefined, label "storage")
```

The app binary is ~960 KB, so each 1344 KB OTA slot keeps ~28% headroom.
`storage` is currently raw (no filesystem); access it via
`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
"storage")` and `esp_partition_read/write/erase_range`.

### Storage self-test

`storage_selftest_task` in `FilterTrackv3.c` verifies the entire partition:
erases it all, writes a deterministic xorshift32 pattern (per-4KB-block seed)
across every byte, reads everything back, and compares. Serial log reports
progress and `Storage: SELF-TEST OK — <bytes> bytes ...` at the end; over BLE
it notifies `ST=OK,<bytes>` / `ST=ERRO,<stage>` (`part`, `mem`, `erase`,
`write`, `read`, `verify`, `busy`, `ota`, `task`).

It runs automatically once on the first boot after flashing (completion marker
`ftstore/selftest` in NVS) and on demand via BLE command `4`. OTA transfers and
the self-test are mutually exclusive (`OTA=ERRO,busy` / `ST=ERRO,ota`).

## OTA Firmware Update (BLE)

The firmware can be updated over the air through the existing BLE connection.
Flash layout uses two app slots (`ota_0`/`ota_1`, 1344 KB each, see
`firmware/partitions_ota.csv` and the partition table above); the transfer
writes the inactive slot and swaps
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
