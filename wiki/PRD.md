# FilterTrack Product Requirements Document

**Version:** 1.0  
**Date:** 2026-05-11  
**Status:** Active Production

---

## 1. Overview

FilterTrack is a real-time water flow monitoring system designed for water treatment facility operators. The product measures water flow during sand filter backwash processes and provides immediate feedback to technicians and long-term analytics to management.

### Product Vision

Enable water treatment facilities to optimize filter maintenance by providing accurate, real-time flow measurements and historical session analytics for sand filter backwash operations.

---

## 2. User Personas

### Primary Users: Field Technicians

**Role:** Operate and maintain sand filters at water treatment facilities.

**Needs:**
- Quick connection to BLE sensors without complex setup
- Real-time flow visualization during backwash operations
- Immediate feedback on whether backwash is working correctly
- Ability to stop monitoring sessions and move between filters
- Access to local session history without internet

**Environment:**
- Field work with Android phones
- Variable network connectivity (may work offline)
- Time-pressured operations (backwash windows are limited)

### Secondary Users: Operations Management

**Role:** Review filter performance, approve filter definitions, and manage operational data.

**Needs:**
- Dashboard view of all historical sessions and flow patterns
- Ability to search and filter sessions by device, date, station
- Export data for compliance and analysis
- Propose corrections to session metadata (filter assignment, time ranges)
- Approve or reject data corrections from technicians
- View KPIs and trends across the facility

**Environment:**
- Desktop or tablet access
- Persistent internet connectivity
- Decision-making authority for filter metadata

---

## 3. Core Features

### 3.1 Android App: Field Collection

#### 3.1.1 Device Discovery and Connection

**Implemented:**
- BLE device scanning with Low Latency mode (`SCAN_MODE_LOW_LATENCY`)
- Auto-discovery of `FilterTrackV3` devices
- Device address persistence and fallback reconnection
- Connection recovery with continuous background scanning
- Multiple device handling (pauses auto-connect when >1 device visible; user chooses)
- Bluetooth state monitoring and error recovery

**User Flow:**
1. User opens app on Android phone
2. App scans for `FilterTrackV3` devices
3. Matching devices appear in a list with signal strength (RSSI)
4. User taps a device or app auto-connects if only one is available
5. Connection state shown in header (connected/disconnected/searching)
6. If connection drops unexpectedly, app warns with recovery options

#### 3.1.2 Filter Selection

**Implemented:**
- Filter catalog loaded locally in app
- Search by filter name, station, business unit
- Visual selection before measurement starts
- Filter metadata includes: ID, name, area (m²), station, location, business unit
- Filter definitions persist in browser localStorage

**User Flow:**
1. After connection, app prompts filter selection if not already chosen
2. User searches or scrolls filter list
3. Selected filter displays area and metadata
4. User confirms and starts monitoring

#### 3.1.3 Real-Time Flow Monitoring

**Implemented:**
- Live distance readings from ultrasonic sensor (every ~100ms)
- Real-time velocity calculation in m/min (10-second windows)
- Flow rate display in L/min with toggle to m³/h
- Visual flow direction indicator:
  - **Subindo (Rising):** green, water level increasing, positive flow
  - **Descendo (Falling):** red, water level decreasing, negative flow
- Data collection countdown and status during initial 10-second window
- Automatic refresh every 10 seconds
- Distance range validation (> 25cm)

**Display Metrics:**
- Velocidade de subida (velocity): m/min, 2 decimals
- Vazão (flow): L/min (default) or m³/h, 1 decimal
- Direction status with color coding

**Sensor Processing:**
- Raw payload parsing (supports `DIST=...`, JSON, accelerometer raw values)
- 2-second median/MAD filtering for outlier rejection
- Distance correction using accelerometer angle (Z-axis) when available
- Flow calculation: `flowLpm = areaM2 * velocityMPerMin * 1000`
- Invalid velocity threshold: > 10 m/min rejected
- Movement threshold: < 0.1 cm ignored

#### 3.1.4 Chart Accumulation and Flow Inversion

**Implemented:**
- Velocity and flow charts accumulate data from session start
- On flow inversion (velocity sign change):
  - History resets to current point
  - 10-second countdown timer begins
  - After countdown, resumes accumulation
- Uses `Math.sign(velocityMpm)` for inversion detection

**Purpose:** Distinguishes between:
- **Backwash phase (positive flow):** water rising in filter
- **Inversion:** flow direction changes
- **Next phase (negative flow):** water falling (drain phase)

#### 3.1.5 Session Management

**Implemented:**
- Automatic session start on device connection
- Manual session stop/end
- Sessions saved locally with:
  - Session ID (unique)
  - Start/end timestamps (ISO 8601)
  - Device address and name
  - Filter metadata (ID, name, area, station, location, business unit)
  - Raw distance samples (compact format)
  - End reason (manual_stop, connection_lost, etc.)
- Session continuity on unexpected disconnect:
  - If latest velocity > 0.1 m/min: keeps session open for auto-reconnect
  - If velocity ≤ 0.1 m/min: finalizes session, next connection starts new session

#### 3.1.6 Sessions Archive and Sync

**Implemented:**
- Local session archive stored in browser localStorage
- Pending sessions queue for offline scenarios
- Manual sync button to upload to backend
- Automatic sync tracking with timestamp
- Deduplication at backend (duplicate uploads detected by normalized key)

**Upload Endpoint:**
```
POST https://filtertrack-api.fly.dev/filtertrack/sessions
```

**Payload Schema:**
```json
{
  "schemaVersion": 1,
  "app": "FilterTrack",
  "uploadedAt": "<ISO-8601>",
  "session": {
    "id": "session-...",
    "startedAt": "<ISO-8601>",
    "endedAt": "<ISO-8601>",
    "endReason": "manual_stop",
    "device": {
      "name": "FilterTrackV3",
      "address": "AA:BB:CC:DD:EE:FF"
    },
    "filter": {
      "id": "filter-id",
      "name": "...",
      "areaM2": 7.07,
      "station": "...",
      "location": "...",
      "businessUnit": "..."
    },
    "samples": { /* compact format */ }
  }
}
```

#### 3.1.7 Compact Sample Storage

**Implemented:**
Distance samples stored as compact arrays to minimize payload size:

```json
{
  "format": "distance_cm_x100_v1",
  "t0": 1770000000000,
  "dtUnit": "ms",
  "distanceUnit": "cm",
  "scale": 100,
  "t": [0, 100, 200, ...],
  "d": [2428, 2429, 2430, ...]
}
```

- `t0`: first sample timestamp (Unix ms)
- `t`: offset array from t0 (ms)
- `d`: distance values scaled by 100 (e.g., 2428 = 24.28 cm)
- Reduces storage footprint for thousands of samples

#### 3.1.8 Debug Mode

**Implemented:**
- Toggle debug mode via Settings tab
- Persisted across sessions
- Debug panel displays:
  - Raw ESP payload log
  - Distance from sensor
  - Post-processed distance
  - Raw velocity/flow without filters
  - Processed velocity and 10-second flow
  - Movement metrics
  - Sample counts and buffer state
  - Distance chart
  - Processing rules reference

#### 3.1.9 Settings and Configuration

**Implemented:**
- Server URL configuration (default: production API)
- Debug mode toggle
- BLE sensor commands:
  - **Restart (0):** Restart ESP32-C3
  - **Low Power (1):** Finalizes session, enables low-power mode
  - **Normal (2):** Disables low-power mode, resumes readings
- Modal confirmations for destructive commands

#### 3.1.10 Native Android Shell

**Implemented:**
- Kotlin BLE Manager with write queue and retry logic
- GATT service discovery and notification enablement
- RSSI (signal strength) monitoring
- JavaScript bridge to WebView:
  - `Android.startScan()`
  - `Android.stopScan()`
  - `Android.connect(address)`
  - `Android.disconnect()`
  - `Android.sendCommand(cmd)`
  - `Android.openBiDashboard()`
  - `Android.isBluetoothEnabled()`
- WebView callback bridge: `window.FilterTrackBridge.<method>()`
- Queue for pending JS messages during WebView load

**Permissions:**
- BLE scan, connect, and GATT operations
- File storage for downloads
- Bluetooth state monitoring

### 3.2 FastAPI Backend: Data Ingestion and Storage

#### 3.2.1 Session Ingestion

**Implemented:**
- REST endpoint `POST /filtertrack/sessions` accepts session payloads
- Parses and validates schema version
- Deduplicates sessions by normalized key
- Returns ingestion ID (session ID or existing ID if duplicate)

#### 3.2.2 Database Storage

**Implemented:**
- SQLite database on Fly.io volume (`/data/filtertrack.db`)
- Canonical storage of:
  - Session metadata (id, device, filter, timestamps, end reason)
  - Compact distance samples
  - Computed fields stripped (flow, direction, sample count) — recomputed at query time
- Filter metadata preservation (id, name, area, station, location, business unit, custom flag)

#### 3.2.3 Default Filter Catalog

**Implemented:**
- Static default filters in `backend-fastapi/app/data/default_filters.json`
- Includes ID, name, area (m²), station, location, business unit for each filter
- Loaded by BI and Android app

---

### 3.3 BI Dashboard: Analytics and Proposals

#### 3.3.1 Overview (Visão Geral)

**Implemented:**
- KPI cards: operational metrics, flow trends, station summaries
- Recent sessions list with filter, device, flow direction
- Device and station summaries
- Date range and device address filtering
- Flow analytics by station

**Endpoint:**
```
GET /filtertrack/bi/overview [?dateRange, deviceAddress, filterId]
```

#### 3.3.2 Filters View (Filtros)

**Implemented:**
- Unified filter catalog (defaults + approved custom + discovered from sessions)
- Search by name, station, business unit
- Archive/reactivate filters
- Edit filter proposals (update name, area, station, location, business unit)
- Mobile-responsive cards / desktop table
- Area formatted as m²

**Features:**
- Editar (Edit) button opens proposal edit panel
- Arquivar/Reativar (Archive/Reactivate) toggles archive status
- Approval workflow creates `ApprovedFilter` rows that override defaults

#### 3.3.3 Sessions View (Sessões)

**Implemented:**
- Recent sessions list with metadata
- Click Visualizar (View) to see session details
- Session visualization shows:
  - Velocity chart (10-second windows)
  - Average flow and total signed volume
  - Upward vs. downward window counts
  - Closest and farthest sensor distance
  - Sample and velocity-point counts
- Correction proposals:
  - Change filter assignment for whole session
  - Change filter for time slice (splits samples into new session)
  - Time slice requires valid start/end local datetime

**Endpoint:**
```
GET /filtertrack/bi/sessions/{session_ref}
```

#### 3.3.4 Proposals View (Propostas)

**Implemented:**
- Proposal queue with type, status, submission date
- Admin approval/rejection workflow
- Supported proposal types:
  - `session_edit` — filter assignment or time slice re-assignment
  - `custom_filter` — new filter definition
  - `filter_edit` — update filter metadata
  - `filter_archive` — toggle archive state

**Approval Behavior:**
- Whole session edit: updates stored session/device/filter facts
- Time slice edit: splits samples into new session, removes from original

#### 3.3.5 Settings (Ajustes)

**Implemented:**
- Downloads section for CSV/JSON exports
- New filter proposal form:
  - Filter number
  - Area (m²)
  - Station
  - Business unit
  - Submitted by field
  - Auto-generates name as "Filtro <number>"

#### 3.3.6 Authentication and Access Control

**Implemented:**
- Access key stored in browser localStorage: `filtertrack.bi.key`
- Header-based auth: `X-Access-Key`
- Role-based permissions:
  - **user:** view dashboard, filter, sessions, proposals; submit proposals; download exports
  - **admin:** user permissions + approve/reject proposals

#### 3.3.7 Exports

**Implemented:**
- CSV and JSON export formats
- Sessions CSV includes: flow fields, availability, L/min, direction, distance start/end, change
- Samples CSV for detailed analysis
- URL format: `/filtertrack/bi/export/{format}`
- Android WebView native download bridge for DownloadManager integration

#### 3.3.8 Responsive Design

**Implemented:**
- Mobile cards for sessions, filters, recent data
- Desktop tables with wrapping cells
- Single-column KPI layout on mobile
- Full-width filter controls on mobile
- Charts fit phone viewport
- Wrapped titles for mobile display

### 3.4 BLE Hardware Integration

#### 3.4.1 Hardware Sensors

**Integrated:**
- **Ultrasonic Distance Sensor:** measures water level in filter (GPIO 20 trigger, GPIO 21 echo)
- **LSM303 Accelerometer (I2C):**
  - GPIO 8 SDA, GPIO 9 SCL
  - Measures sensor orientation for distance angle correction
- **Power Management:** Sensor power via GPIO 10 (controlled by low-power mode)
- **Status LEDs:**
  - Yellow: low-power mode
  - Green: Bluetooth connected
  - Red: sensor/BLE/I2C error
  - Red blinking: advertising or reading while disconnected

#### 3.4.2 Firmware (ESP32-C3)

**Implemented in:** `firmware/main/FilterTrackv3.c`

**Features:**
- BLE advertisement as `FilterTrackV3`
- Service UUID: `0x00FF`, Characteristic UUID: `0xFF01`
- Notification frequency: ~100ms (distance updates)
- GATT service/characteristic with notify and write support
- Command handling:
  - `0`: Restart ESP32-C3
  - `1`: Enable low-power mode (powers down ultrasonic, keeps BLE)
  - `2`: Disable low-power mode (resumes readings)
- Payload formats:
  - Standard: `DIST=%.2f`
  - With accelerometer: `DIST=%.2f;ACC_RAW=%d,%d,%d`
  - Compact multi-line: `D=%.2f` / `A=%d,%d,%d`
  - Error states: `DIST=ERRO`, `ERRO_TIMEOUT`, `RAW=ERRO`
- Status LED control for operational feedback

#### 3.4.3 BLE Protocol

**Implemented:**
- Service: `000000ff-0000-1000-8000-00805f9b34fb`
- Characteristic: `0000ff01-0000-1000-8000-00805f9b34fb`
- Notifications every ~100ms
- Write support for commands (single character)
- RSSI monitoring for connection quality

---

## 4. Data Flow

```
┌─────────────────────┐
│  ESP32-C3 Sensor    │
│  (Ultrasonic +      │
│   LSM303 accel)     │
└──────────┬──────────┘
           │ BLE notifications
           │ DIST + ACC_RAW
           ▼
┌─────────────────────────────────────┐
│  Android App (Native BLE Manager)   │
│  • BLE scan and connect             │
│  • GATT service discovery           │
│  • Notification parsing             │
└──────────┬──────────────────────────┘
           │ window.FilterTrackBridge
           │ (sample arrays)
           ▼
┌────────────────────────────────────────┐
│  WebView UI (index.html)               │
│  • Sensor processing pipeline          │
│  • Distance → velocity → flow          │
│  • localStorage session persistence    │
│  • Chart rendering                     │
└──────────┬─────────────────────────────┘
           │ POST /filtertrack/sessions
           │ (compact samples + metadata)
           ▼
┌────────────────────────────────────────┐
│  FastAPI Backend                       │
│  • Ingest and canonicalize             │
│  • SQLite storage                      │
│  • Deduplication                       │
└──────────┬─────────────────────────────┘
           │ BI API queries
           ▼
┌────────────────────────────────────────┐
│  BI Dashboard (bi.html)                │
│  • Sessions, filters, proposals        │
│  • Export, analytics                   │
│  • Admin approval workflow             │
└────────────────────────────────────────┘
```

---

## 5. Technical Requirements

### 5.1 Android App

**Platform:** Android 9+ (minSdk 28, targetSdk 36)  
**Language:** Kotlin (native), JavaScript/React (WebView)  
**BLE:** Native Android BLE GATT APIs  
**Storage:** localStorage in WebView (no permission required)  
**Build:** Gradle (`:app` module)

### 5.2 Backend

**Framework:** FastAPI (Python)  
**Database:** SQLite on Fly.io volume  
**Hosting:** Fly.io (`filtertrack-api` app in `gru`)  
**ORM:** SQLAlchemy  

### 5.3 BI Dashboard

**Framework:** React (CDN-loaded inside static HTML)  
**Hosting:** FastAPI static file server at `/bi`  
**Auth:** Access key header (`X-Access-Key`)  
**Storage:** browser localStorage for user state

### 5.4 Hardware

**MCU:** ESP32-C3  
**Sensors:**
- Ultrasonic distance sensor (HC-SR04 compatible)
- LSM303 accelerometer (I2C)
- 3× status LEDs

**Firmware:** ESP-IDF (C)  
**Connectivity:** BLE (Bluetooth Low Energy)

---

## 6. Data Contracts

### 6.1 Sample Compact Format

```json
{
  "format": "distance_cm_x100_v1",
  "t0": 1770000000000,
  "dtUnit": "ms",
  "distanceUnit": "cm",
  "scale": 100,
  "t": [0, 100, 200, ...],
  "d": [2428, 2429, 2430, ...]
}
```

### 6.2 Session Payload

```json
{
  "schemaVersion": 1,
  "app": "FilterTrack",
  "uploadedAt": "2026-05-06T20:00:00.000Z",
  "session": {
    "id": "session-...",
    "startedAt": "2026-05-06T19:50:00.000Z",
    "endedAt": "2026-05-06T20:00:00.000Z",
    "endReason": "manual_stop",
    "device": {
      "name": "FilterTrackV3",
      "address": "AA:BB:CC:DD:EE:FF"
    },
    "filter": {
      "id": "filter-id",
      "name": "...",
      "areaM2": 7.07,
      "station": "...",
      "location": "...",
      "businessUnit": "..."
    },
    "samples": { /* compact format */ }
  }
}
```

### 6.3 Filter Definition

```json
{
  "id": "filter-id",
  "name": "FIL-01 Asc",
  "areaM2": 7.07,
  "station": "ETA North",
  "location": "Hall A",
  "businessUnit": "UN-001",
  "custom": false,
  "createdAt": "2026-05-01T10:00:00.000Z"
}
```

---

## 7. Operational Features

### 7.1 Offline Capability

- App works offline for scanning, connection, and session collection
- Sessions queued locally and synced when network available
- No internet required for field monitoring

### 7.2 Multi-Device Support

- Device address persistence across sessions
- Auto-reconnect to previously known devices
- Manual selection when multiple devices visible

### 7.3 Connection Recovery

- Continuous background scanning on disconnect
- Auto-reconnect when device becomes available
- Session continuity rules based on latest velocity
- User warnings and recovery options for unexpected losses

### 7.4 Low-Power Mode

- Technician can enable low-power mode via Settings
- Finalizes active session
- Powers down ultrasonic sensor
- Keeps BLE active for remote commands
- Yellow LED indicates low-power state
- Resumes full readings on disable command

---

## 8. Deployment

### 8.1 Production Endpoints

**Android App:**
- Default API: `https://filtertrack-api.fly.dev/filtertrack/sessions`
- BI Dashboard: `https://filtertrack-api.fly.dev/bi`

**Configuration:**
- Configurable in app Settings tab
- Persisted in localStorage

### 8.2 Database

- Single SQLite file on Fly.io volume at `/data/filtertrack.db`
- Handles all session, filter, and proposal storage
- No distributed architecture (single instance)

---

## 9. Security

### 9.1 Mobile Authentication

- No per-user login required for technician app
- BLE device connection is local only
- Session uploads to backend (no credentials in app)

### 9.2 BI Dashboard Authentication

- Access key–based auth (header: `X-Access-Key`)
- Role-based access control (user / admin)
- Key injected by native Android app for in-app BI access

### 9.3 Data Validation

- Schema version validation on ingest
- Field-level validation (timestamps, area, distance)
- Deduplication key prevents accidental duplicates

---

## 10. Known Limitations and Caveats

1. **Single Flask Instance:** Backend runs on one Fly instance with SQLite (no distributed support).
2. **Direct HTML Editing:** `index.html` is edited directly in the repository (no templating pipeline in current version).
3. **No Distributed Session Storage:** Sessions must complete before Bluetooth disconnect, or reconnect is required to finalize.
4. **BLE Range:** Typical BLE range is 10–100m depending on antenna and environment.
5. **Ultrasonic Limitations:** May be affected by water turbulence, foam, or extreme backwash pressure.

---

## 11. Success Metrics

- **Adoption:** Number of active technician devices per facility
- **Data Quality:** Percentage of sessions successfully synced to backend
- **Uptime:** Availability of backend and BI dashboard (target: 99.5%)
- **User Experience:** Session load time and chart responsiveness on typical Android device
- **Flow Accuracy:** Correlation between app-computed flow and facility flow meter (target: ±5%)

---

## 12. Future Roadmap (Not Yet Implemented)

- Multi-user authentication for technician app
- Real-time session synchronization (cloud-first storage)
- Mobile BI dashboard (currently desktop-focused)
- Advanced predictive analytics (filter degradation)
- Integration with facility SCADA systems
- Historical session comparison and trending
- Batch operations for technicians (e.g., multiple sessions per shift)

---

## Appendices

### A. Team Roles

- **Hardware Engineering:** ESP32-C3 firmware, sensor integration
- **Mobile Engineering:** Android app, BLE integration, WebView
- **Backend Engineering:** FastAPI, SQLite schema, BI APIs
- **Product Management:** Requirements, user research, prioritization
- **Operations:** Deployment, monitoring, incident response

### B. Related Documentation

- [Architecture](architecture.md) — end-to-end system design
- [Android App Context](android-app.md) — implementation details
- [BI Dashboard Context](bi-dashboard.md) — BI features and API
- [BLE and Firmware Contract](ble-firmware-contract.md) — protocol and commands
- [Sensor Processing Flow](sensor-processing-flow.md) — data processing pipeline
- [Sessions Data Contract](sessions-data-contract.md) — upload format and schema
