# FilterTrack - Master Project Documentation
> **Single Source of Truth** for the FilterTrack Water Flow Monitoring System  
> **Primary Microcontroller:** ESP32-C6  
> **Document Status:** Complete & Authoritative  

---

## 1. Executive Summary & System Overview

**FilterTrack** is an end-to-end industrial IoT solution engineered for real-time water level and flow monitoring during sand-filter backwash operations at water treatment facilities (such as **Cagece**).

The system measures the rate of rise and fall of water levels inside filtration basins using an ultrasonic sensor backed by accelerometer-based tilt correction. Technicians capture field measurements using a custom Android mobile application connected via Bluetooth Low Energy (BLE). The mobile app processes real-time sensor streams, computes flow metrics, manages local measurement sessions, and syncs data to a cloud-hosted FastAPI backend with an integrated Business Intelligence (BI) dashboard.

```
       +-------------------------------------------------------------+
       |                  Physical Filter Basin                      |
       |  +-------------------------------------------------------+  |
       |  | 3D Enclosure (Case + Lid + Lock)                      |  |
       |  |  +-------------------------------------------------+  |  |
       |  |  | PCB (router.pdf layout)                         |  |  |
       |  |  |  - ESP32-C6 Microcontroller (ESP-IDF 5.5)      |  |  |
       |  |  |  - Waterproof Ultrasonic (AJ-SR04M / GPIO0,1)  |  |  |
       |  |  |  - Accelerometer LSM303 (I2C SDA/SCL)          |  |  |
       |  |  |  - Status LEDs (Yellow, Red, Green)            |  |  |
       |  |  +-------------------------------------------------+  |  |
       |  +-------------------------------------------------------+  |
       +------------------------------|------------------------------+
                                      | BLE (0x00FF / 0xFF01)
                                      v
       +-------------------------------------------------------------+
       |                  Android Field Mobile App                   |
       | - Executable: app-release.apk (~2.26 MB)                    |
       | - Native Kotlin Shell (BLEManager + JavaScript Bridge)        |
       | - WebView React UI (Minimal Sand-Filter UI + Full Tabs)     |
       | - Real-time Velocity (m/min), Flow (L/min / m³/h) & Wash Banner|
       +------------------------------|------------------------------+
                                      | HTTPS Sync (POST /sessions)
                                      v
       +-------------------------------------------------------------+
       |             Cloud Infrastructure (Fly.io - `gru`)           |
       | - FastAPI Backend (`https://filtertrack-api.fly.dev`)         |
       | - Persistent SQLite Database on Fly Volume (`/data`)        |
       | - Business Intelligence Dashboard (`/bi`)                   |
       +-------------------------------------------------------------+
```

---

## 2. Hardware Architecture & Main Microcontroller (ESP32-C6)

The definitive primary microcontroller for FilterTrack is the **Espressif ESP32-C6** (32-bit RISC-V single-core CPU up to 160 MHz, 4 MB Flash, supporting 2.4 GHz Wi-Fi 6, BLE 5.0, and 802.15.4 Thread/Zigbee).

### 2.1 Hardware Connections & Pinout

The ESP32-C6 carrier board interfaces with the ultrasonic transducer, tilt accelerometer, status LEDs, and power supply according to the following GPIO configuration:

| Peripheral / Component | Function | ESP32-C6 GPIO | Technical Description |
|---|---|---|---|
| **Ultrasonic Sensor (AJ-SR04M)** | **Trigger** | `GPIO1` | Output pulse (~10µs) to initiate measurement |
| **Ultrasonic Sensor (AJ-SR04M)** | **Echo** | `GPIO0` | Input pulse width proportional to target distance |
| **Status LED - Yellow** | **Low-Power** | `GPIO2` | High when ESP32-C6 is in low-power standby mode |
| **Status LED - Red** | **Error / Adv** | `GPIO3` | Blinks when advertising/disconnected; Solid on error |
| **Status LED - Green** | **BLE Connected**| `GPIO4` | Solid high when BLE GATT client (Android) is connected |
| **Accelerometer (LSM303DLHC)** | **I2C SDA** | `GPIO13` | I2C Data line for 3-axis accelerometer / magnetometer |
| **Accelerometer (LSM303DLHC)** | **I2C SCL** | `GPIO12` | I2C Clock line for 3-axis accelerometer / magnetometer |
| **Sensor Power Control** | **Power Enable** | *Disabled* | `SENSOR_PWR_ENABLED 0` (direct continuous rail) |

> [!WARNING]
> **ESP32-C6 Native USB Pin Conflict Notice (GPIO12 / GPIO13):**  
> On the ESP32-C6 silicon, **GPIO12 (D-)** and **GPIO13 (D+)** are hardwired to the native USB-Serial/JTAG controller. Using these pins for the accelerometer I2C bus (SDA/SCL) creates a physical bus conflict whenever a USB cable or USB host is connected, leading to `RAW=ERRO` payload responses.  
> **Recommended Fix for Future PCB Revisions:** Reassign I2C SDA/SCL to un-multiplexed GPIO pins such as **GPIO6** and **GPIO7**.

### 2.2 ESP32-C6 Flash Partition Layout (`partitions_ota.csv`)

The ESP32-C6 utilizes a 4 MB flash layout optimized for dual Over-The-Air (OTA) firmware updates and on-chip raw sensor logging:

| Partition Label | Subtype | Flash Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | `nvs` | `0x009000` | 16 KB | Non-Volatile Storage (sand-mode state, calibration, logs) |
| `otadata` | `ota` | `0x00D000` | 8 KB | OTA selection state & bootloader pointers |
| `phy_init` | `phy` | `0x00F000` | 4 KB | PHY initialization data |
| `ota_0` | `ota_0` | `0x010000` | 1344 KB | **Primary Application Slot A** (~28% headroom) |
| `ota_1` | `ota_1` | `0x160000` | 1344 KB | **Secondary Application Slot B** (OTA target slot) |
| `storage` | `undefined` | `0x2B0000` | 1344 KB | **Raw Sensor Log Partition** (Direct partition read/write/erase) |

### 2.3 Firmware Compilation & Flashing Instructions

**Build Environment:** ESP-IDF v5.5.1 with `riscv32-esp-elf` cross-toolchain on Windows.

1. **Building for ESP32-C6:**
   ```powershell
   # Navigate to the firmware directory
   Set-Location C:\Users\Isaque\AndroidStudioProjects\FilterTrack\firmware

   # Execute ESP-IDF build targeting ESP32-C6
   & "C:\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe" `
     "C:\Espressif\frameworks\esp-idf-v5.5.1\tools\idf.py" `
     -B build-esp32c6 -DSDKCONFIG=sdkconfig_esp32c6 build
   ```
   *Output binary:* `firmware/build-esp32c6/FilterTrackv3.bin`

2. **Flashing via Serial Cable (`esptool.py`):**
   ```powershell
   python -m esptool --chip esp32c6 -p COM3 -b 460800 `
     --before default-reset --after hard-reset write-flash `
     --flash-mode dio --flash-size 4MB --flash-freq 80m `
     0x0     build-esp32c6\bootloader\bootloader.bin `
     0x8000  build-esp32c6\partition_table\partition-table.bin `
     0xd000  build-esp32c6\ota_data_initial.bin `
     0x10000 build-esp32c6\FilterTrackv3.bin
   ```

---

## 3. Printed Circuit Board (PCB) Routing & Physical Design (`router.pdf`)

The printed circuit board for the FilterTrack ESP32-C6 sensor unit was custom designed and routed by Isaque (composite routing diagram documented in [`router.pdf`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/router.pdf)).

### 3.1 Composite Drawing Details (`router.pdf`)
* **Document Name:** [`router.pdf`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/router.pdf)
* **Title:** Composite Drawing (Printed Circuit Board Copper Routing)
* **Author:** Isaque (Created 2026-07-07)
* **Content & Layout:**
  - Standard dual-layer PCB copper track layout connecting the ESP32-C6 microcontroller dev module headers to external peripherals.
  - Direct trace routing for **AJ-SR04M / JSN-SR04T** waterproof ultrasonic module connector (`GPIO1` Trigger, `GPIO0` Echo).
  - Dedicated header connections for **LSM303DLHC** 6-DOF accelerometer module (`GPIO13` SDA, `GPIO12` SCL).
  - Integrated current-limiting resistors and pads for status indicator LEDs (`GPIO2` Yellow, `GPIO3` Red, `GPIO4` Green).
  - On-board power management traces delivering regulated +5V and +3.3V DC to sensor components with decoupling capacitors for ultrasonic noise reduction.

---

## 4. 3D-Printed Protective Enclosure (.STL Files)

FilterTrack includes a complete 3-piece weather-resistant 3D-printed enclosure designed to protect the ESP32-C6 PCB, sensors, and power source when deployed over water filter basins.

| STL File Name | File Size | Triangle Count | Approximate Dimensions (L × W × H) | Mechanical Function |
|---|---|---|---|---|
| [`FilterTrackCase.stl`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/FilterTrackCase.stl) | 91.9 KB | 1,838 | **100 mm × 110 mm × 50 mm** | **Main Body Housing:** Encloses the ESP32-C6 carrier PCB, battery compartment, status LED viewports, and ultrasonic sensor port. |
| [`FilterTrackLid.stl`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/FilterTrackLid.stl) | 27.0 KB | 540 | **100 mm × 110 mm × 6 mm** | **Top Protection Cover:** Snaps/screws onto the main case to seal against water splashes and debris. |
| [`FilterTrackLock.stl`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/FilterTrackLock.stl) | 948.7 KB | 18,974 | **100 mm × 90 mm × 90 mm** | **Mounting Latch / Clamp:** High-precision heavy-duty locking bracket for securing the unit onto filter basin railings. |

### 4.1 3D Printing Guidelines
* **Recommended Material:** PETG or ABS (for outdoor UV resistance, splash resistance, and structural durability).
* **Layer Height:** 0.20 mm (Standard resolution).
* **Infill:** 20% to 30% (Grid or Gyroid infill for structural strength on the mounting clamp).
* **Wall Perimeter Count:** Minimum 3 to 4 walls (1.2 mm - 1.6 mm thickness).

---

## 5. Firmware BLE Contract & Communication Protocol

### 5.1 BLE Service & Characteristics
* **Advertised Device Name:** `FilterTrackV3`
* **Primary Service UUID:** `000000ff-0000-1000-8000-00805f9b34fb` (`0x00FF`)
* **Command & Notification Characteristic:** `0000ff01-0000-1000-8000-00805f9b34fb` (`0xFF01`)
* **OTA Data Characteristic:** `0000ff02-0000-1000-8000-00805f9b34fb` (`0xFF02`)

### 5.2 Notification Data Formats (~100 ms Interval)
1. **Standard Reading (with raw tilt accelerometer):**
   ```text
   DIST=%.2f;ACC_RAW=%d,%d,%d
   ```
   *Compact split chunk format:* `D=35.47;A=12,-8,102` (reassembled in `BLEManager.kt` upon receiving `D=`).

2. **Tilt Distance Correction Formula:**  
   When tilt accelerometer data is available, Android computes true vertical distance using:
   $$\text{Vertical Distance} = \text{DIST} \times \sin\left(\arccos(|g_z|)\right)$$

### 5.3 Control Commands (Android to ESP32-C6 over `0xFF01`)

| Command String | Firmware Response / Action | Application Usage |
|---|---|---|
| `0` | Calls `esp_restart()` | Reboots the ESP32-C6 microcontroller |
| `1` | Enables low-power standby; turns off ultrasonic sensor | Saves power between operational shifts |
| `2` | Disables low-power standby; powers ultrasonic sensor back on | Resumes active measurement |
| `3` | Notifies `VER=<PROJECT_VER>` | Query current firmware version |
| `4` | Runs full flash self-test on `storage` partition | Hardware validation (destroys logged raw data) |
| `5` | Enables sand-filter wash detection mode (`SAND=1`) | Enforces automatic wash detection state machine |
| `6` | Disables sand-filter mode (`SAND=0`) | Aborts open wash state without logging records |
| `LOG:COUNT` | Notifies `WL=CNT,<count>,<boot>,<uptime>` | Queries stored wash log entry count |
| `LOG:READ` | Streams `WL=R,...` records followed by `WL=END` | Dumps logged wash events to Android app |
| `LOG:ACK:<n>`| Erases log if count matches `<n>` (`WL=CLR`) | Confirms log sync and frees on-chip storage |
| `CAL:SET:<cm>`| Calibrates sensor tilt factor based on known water level | Calculates tilt factor $f = \frac{\text{real}}{\text{measured}}$ |

---

## 6. Android Mobile Application & Release APK (`app-release.apk`)

The field technician mobile client is built as a hybrid native/web Android application.

### 6.1 Executable `.apk` and Package Information
* **Production Release APK File:** [`app/build/outputs/apk/release/app-release.apk`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/app/build/outputs/apk/release/app-release.apk)
  * **File Size:** **2.26 MB** (2,265,043 bytes)
  * **Build Target:** Release Signed (`versionCode=2`, `versionName=1.0`)
* **Development Debug APK File:** [`app/build/outputs/apk/debug/app-debug.apk`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/app/build/outputs/apk/debug/app-debug.apk) (14.75 MB)
* **Google Play Bundle:** [`app/build/outputs/bundle/release/app-release.aab`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/app/build/outputs/bundle/release/app-release.aab) (`versionCode=3`, `versionName=1.1`)
* **Package Name:** `com.filtertrack`

### 6.2 Application Architecture & Native-JS Bridge
The app features a native Kotlin shell (`MainActivity.kt`, `BLEManager.kt`, `BiDashboardActivity.kt`) hosting an embedded WebView UI ([`app/src/main/assets/index.html`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/app/src/main/assets/index.html)).

* **JavaScript Bridge Name:** `window.Android`
* **Exposed Native Commands:** `startScan()`, `stopScan()`, `connect(address)`, `disconnect()`, `sendCommand(cmd)`, `openBiDashboard()`, `isBluetoothEnabled()`, `startFirmwareUpdate(bytes)`.
* **Native Callbacks to JS:** `window.FilterTrackBridge.onDeviceFound(...)`, `onDataReceived(...)`, `onConnectionStateChanged(...)`, `onOtaProgress(...)`.

```
               Android Native Kotlin Shell
   +--------------------------------------------------+
   |  BLEManager.kt  <--->  MainActivity.kt           |
   +--------------------------|-----------------------+
                              | window.Android / window.FilterTrackBridge
                              v
               WebView Layer (index.html)
   +--------------------------------------------------+
   |  - Minimal Mode (Single-screen sand filter UI)   |
   |  - Full App Mode (5 Tabs: Monitor, Sessoes,      |
   |    Filtros, Ajustes, Dados)                      |
   +--------------------------------------------------+
```

### 6.3 Dual User Interface Modes

1. **Minimal Mode (Default Field Interface):**
   * Optimized for quick field operation by plant technicians.
   * Auto-sends command `5` (`SAND=1`) on connect to enable ESP32-C6 wash detection.
   * Shows a clean single-screen interface:
     - **Velocidade de subida (Water Velocity):** Calculated in **m/min** (2 decimal places) over rolling 10-second analysis windows.
     - **Vazão (Water Flow Rate):** Displayed in **L/min** (or toggled to **m³/h**).
     - **Flow Direction Badge:** **Subindo (Green / Rising)** when level increases; **Descendo (Red / Falling)** when level decreases.
     - **Wash State Banner:** Listens for `WL=WSTART` and `WL=WEVT` notifications to announce active filter backwash sessions.
   * Includes a button `"Abrir app completo"` to access advanced tools.

2. **Full App Mode (Multi-Tab Navigation):**
   * **Monitor Tab:** Full real-time metric cards, velocity history chart, flow direction, and selected filter metadata.
   * **Sessões Tab:** Local session archives, session detail chart modal, manual cloud sync triggering, and compact JSON exports.
   * **Filtros Tab:** Filter catalog browser and filter selection picker with station/area details.
   * **Ajustes Tab:** Firmware OTA update utility, sensor tilt calibration (`CAL:`), wash detection parameter configuration (`CFG:`), low-power control, and ESP restart button.
   * **Dados Tab:** Launches the integrated Business Intelligence dashboard.

### 6.4 Data Filtering Rules & Signal Processing Pipeline
* **Minimum Distance Threshold:** Distance readings $< 25\text{ cm}$ are discarded as invalid sensor noise.
* **Outlier Filtering:** 2-second median/MAD windowing.
* **Maximum Velocity Cap:** Velocities $> 10\text{ m/min}$ are rejected as physical spikes.
* **Movement Threshold:** Water level displacement $< 0.1\text{ cm}$ over 10 seconds is treated as stationary ($0.0\text{ m/min}$).

---

## 7. Cloud Backend & BI Dashboard

### 7.1 Backend Services & Hosting
* **Framework:** FastAPI (Python 3.11+)
* **Database:** SQLite on persistent Fly.io Volume (`/data/filtertrack.db`)
* **Production API Base URL:** `https://filtertrack-api.fly.dev`
* **BI Dashboard Base URL:** `https://filtertrack-api.fly.dev/bi`

### 7.2 Session Upload Endpoint (`POST /filtertrack/sessions`)
Field sessions are synced using a compact array serialization scheme (`distance_cm_x100_v1`) to minimize cellular payload sizes:

```json
{
  "schemaVersion": 1,
  "app": "FilterTrack",
  "uploadedAt": "2026-08-07T10:40:00Z",
  "session": {
    "id": "session-1770000000000",
    "startedAt": "2026-08-07T10:30:00Z",
    "endedAt": "2026-08-07T10:40:00Z",
    "endReason": "manual_stop",
    "device": { "name": "FilterTrackV3", "address": "AA:BB:CC:DD:EE:FF" },
    "filter": {
      "id": "ETA_GAVIÃO_FILTRO_01",
      "name": "Filtro 01",
      "areaM2": 7.07,
      "station": "ETA Gavião",
      "businessUnit": "UN-METROPOLITANA"
    },
    "samples": {
      "format": "distance_cm_x100_v1",
      "t0": 1770000000000,
      "dtUnit": "ms",
      "distanceUnit": "cm",
      "scale": 100,
      "t": [0, 100, 200, 300],
      "d": [3547, 3546, 3544, 3542]
    }
  }
}
```

### 7.3 Integrated BI Dashboard (`backend-fastapi/app/static/bi.html`)
The BI dashboard provides management and operational teams with comprehensive insights:
* **Key Performance Indicators (KPIs):** Total sessions, active filters monitored, average net backwash flow rates, total backwash volume.
* **Interactive Charts:** Average Net Flow by Filter (L/min), Flow Direction Distribution, Daily Backwash Volume Trends, Samples by Water Treatment Station.
* **Data Correction Workflows:** Allows operators to submit proposals for re-assigning filter metadata or splitting multi-phase sessions into discrete time slices. Admin role approves or rejects proposals.

---

## 8. File & Directory Reference Catalog

| Directory / File | Description & Purpose |
|---|---|
| [`DOCUMENTATION.md`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/DOCUMENTATION.md) | **This master documentation file (Single Source of Truth)** |
| [`router.pdf`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/router.pdf) | Printed Circuit Board (PCB) composite copper routing diagram |
| [`FilterTrackCase.stl`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/FilterTrackCase.stl) | Main body 3D enclosure STL file (100 × 110 × 50 mm) |
| [`FilterTrackLid.stl`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/FilterTrackLid.stl) | Top cover 3D lid STL file (100 × 110 × 6 mm) |
| [`FilterTrackLock.stl`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/FilterTrackLock.stl) | Heavy-duty mounting latch/clamp STL file (100 × 90 × 90 mm) |
| [`app/build/outputs/apk/release/app-release.apk`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/app/build/outputs/apk/release/app-release.apk) | **Production executable release APK** (~2.26 MB) |
| [`firmware/main/FilterTrackv3.c`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/firmware/main/FilterTrackv3.c) | Core ESP-IDF C source code for ESP32-C6 |
| [`firmware/partitions_ota.csv`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/firmware/partitions_ota.csv) | ESP32-C6 flash partition table (OTA slots + sensor storage) |
| [`app/src/main/assets/index.html`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/app/src/main/assets/index.html) | Native Android WebView user interface (Minimal + Full modes) |
| [`backend-fastapi/app/static/bi.html`](file:///C:/Users/Isaque/AndroidStudioProjects/FilterTrack/backend-fastapi/app/static/bi.html) | Static BI dashboard served at `https://filtertrack-api.fly.dev/bi` |

---

## 9. Conclusion & Verification

This master document represents the single, consolidated, true specification for the **FilterTrack** system as of August 2026. All hardware pinouts, software boundaries, physical enclosure files, and communication protocols defined herein reflect the active production system.
