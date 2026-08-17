# Android App

The Android app combines a native Kotlin shell with a WebView-hosted React UI.
The main app surface is `app/src/main/assets/index.html`.

The React/ReactDOM/Babel runtimes are vendored in `app/src/main/assets/vendor/`
and referenced with relative `src` paths so the app works **without any
internet connection** (they were previously loaded from the unpkg CDN, which
made a fresh install render a blank WebView offline). Keep it that way: don't
reintroduce CDN `<script src>` tags in `index.html`.

## Primary Files

- `app/src/main/java/com/example/filtertrack/MainActivity.kt`
- `app/src/main/java/com/example/filtertrack/BLEManager.kt`
- `app/src/main/java/com/example/filtertrack/WebAppInterface.kt`
- `app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt`
- `app/src/main/assets/index.html`
- `app/src/main/res/layout/activity_main.xml`

## Build

Validate Android changes from the repo root:

```powershell
.\gradlew.bat :app:assembleDebug
```

For Google Play internal testing preparation, see
[android-play-internal-testing.md](android-play-internal-testing.md).

Current app config from the source notes:

- Application id: `com.filtertrack`
- Version code: `4`
- Version name: `1.2`
- minSdk: `28`
- targetSdk: `36`
- compileSdk: Android 36.1
- Java compatibility: 11

## Native Shell

`MainActivity.kt` creates the WebView, enables JavaScript and DOM storage,
registers `window.Android`, and loads:

```text
file:///android_asset/index.html
```

If native BLE events arrive before the page is ready, JavaScript messages are
queued and flushed after `onPageFinished`.

## JavaScript Bridge

`WebAppInterface.kt` exposes:

- `Android.startScan()`
- `Android.stopScan()`
- `Android.connect(address)`
- `Android.disconnect()`
- `Android.sendCommand(cmd)`
- `Android.openBiDashboard()`
- `Android.isBluetoothEnabled()`

The WebView wraps these calls through its local bridge object. The bottom `Dados`
tab opens the BI dashboard after connection.

## BLE Connection Behavior

`BLEManager.kt` scans, connects, discovers services, enables notifications, reads
RSSI, and serializes GATT writes.

Important behavior:

- Scan mode is low latency.
- Only devices with resolvable advertised/device names are reported.
- Eligible devices have names starting with `filtertrack` or have a previously
  connected address.
- Auto-connect picks the first eligible device unless multiple `FilterTrackV3`
  sensors are visible.
- With multiple visible `FilterTrackV3` sensors, auto-connect pauses and the user
  must manually choose the intended device.
- When the advertised name is `FilterTrackV3`, the UI hides the MAC address.

## Connection Recovery

The connect screen keeps scanning until the user taps `Parar`. `Retomar busca`
resumes the scan loop.

Unexpected Bluetooth/device loss opens a recovery modal with options to try a
sensor restart, wait for reconnection, or restart the app.

On unexpected connection loss:

- If latest displayed velocity is above `0.1 m/min`, the active session stays
  open for automatic reconnect.
- Otherwise the old session is finalized and the next connection starts a new
  session.

Phone Bluetooth turning off while connected forces the WebView into disconnected
state so the UI does not remain frozen.

Sensor restart from Settings finalizes the active session, sends command `0`,
keeps scanning, and preserves the current selected filter on automatic reconnect.

## WebView UI

The app has two UI modes inside `index.html`:

- **Minimal mode (default)** — single-screen sand-filter UI, see next section.
- **Full app** — reached via "Abrir app completo"; "Usar modo simplificado" in
  Settings returns to minimal mode. The choice persists in
  `filtertrack.minimalMode.v1` (default `"1"`).

Major sections of the full app in `index.html`:

- Connect screen and device list.
- Filter picker and filter catalog.
- Monitor tab with velocity and flow metrics.
- Sessions tab with local archive and manual sync.
- Filters tab for selecting filter definitions.
- Settings tab with server URL, BLE commands, display units, minimum display
  velocity, tilt calibration (`CAL:`), wash-detection config (`CFG:`),
  firmware OTA update tile, and debug mode.
- Debug panel with raw and processed metrics.

The selected-filter card shows derived filter number, direction, location,
business unit, and area. It does not show the raw filter name/id.

## Minimal Mode (default)

`MinimalScreen` is a single-screen UI for a fixed sand-filter installation.
It is locked to a fixed filter (`MINIMAL_FILTER_ID`, FIL-01 Asc · ETA
Primavera); the full app keeps whatever filter the user selected.

Firmware coupling (see [ble-firmware-contract.md](ble-firmware-contract.md)):

- On connect it sends command `5` (sand mode on) after 1.5 s and re-sends it
  whenever the firmware notifies `SAND=0` (rate-limited by
  `SAND_MODE_REASSERT_MS`).
- After 4 s it pulls the firmware wash log silently: `LOG:COUNT` →
  `LOG:READ` → stores `WL=R,...` records in `filtertrack.washLog.v1` →
  `LOG:ACK:<n>` erases them on the sensor.
- `WL=WSTART` finalizes the current session (`wash_started`) and shows the
  "Lavagem em andamento" banner with a live `WL=WEVT` event count; `WL=EVT`
  clears the banner, finalizes (`wash_detected`), and starts the next
  session. The banner self-clears after 35 min or on disconnect.

Display rules (velocity/vazão tiles + real-time chart):

- Dashes (`--`) appear **only when there is no valid sensor data** (not
  connected, no recent samples, or no valid distance after robust filtering).
- While valid readings arrive, the tiles always show a number: the current
  10-second-window value when it is at or above the minimum display velocity
  (`filtertrack.minVelocity.v1`, default 0.25 m/min), otherwise `0.00` — and
  the chart records a `0.00` point, so the line drops to zero instead of
  freezing. Crossing zero this way is not treated as a flow inversion.
- While the 10 s window is still refilling (right after connect or after a
  wash), the tiles hold the latest above-threshold reading instead of dashes
  (or `0.00` if there has been none yet).
- **During a wash** (`WL=WSTART` … `WL=EVT`): the chart restarts at the wash
  start and accumulates points every 2 s for the whole wash — no inversion
  resets and no display-threshold cut — so all wash events are visible. The
  velocity and vazão tiles stay frozen at the latest above-threshold reading
  until the wash ends.
- Outside a wash the chart keeps the standard accumulation behavior
  (reset on flow inversion); outside minimal mode the full app keeps the
  original behavior (below-threshold readings are hidden).

The screen also lists recent finished sessions ("Últimas medições"); tapping
one opens `SessionDetailOverlay` with the reconstructed velocity chart,
statistics, and the wash-log entries received in that period.

## First-Launch Guide

A full initial guide starts automatically until it is completed or skipped. It
uses the real app screens but replaces the BLE snapshot and flow metrics with
clearly labelled simulated data; guide interactions do not create sessions or
send firmware commands.

The sequence is:

1. Introduce simplified mode with a simulated connected sensor, velocity,
   flow, history chart, and filter metadata.
2. Open full mode and explain its monitor and navigation tabs.
3. Open `Ajustes`, scroll to `Usar modo simplificado`, and return to the
   simplified screen.
4. Open the offline CSV analyzer with its bundled Primavera example.
5. Open the online BI dashboard and explain synchronized views and exports.

Completion is stored natively in SharedPreferences
`filtertrack_guide/initial_guide_complete_v1`, so WebView local-storage resets
do not unexpectedly replay the guide. The BI step needs internet; CSV analysis
and every preceding simulated step work offline.
## Complete App Without A Sensor

From the complete-app connection screen, `Entrar sem sensor` opens the full
WebView without requiring a nearby BLE device. This consultation mode keeps
sessions, filters, Settings, CSV analysis, and BI navigation available.

A persistent yellow `Modo sem sensor` banner remains visible with a `Conectar`
action. Session controls and the Settings tiles that send BLE commands
(sensor power/restart, tilt calibration, wash detection configuration, and OTA
firmware) are marked as sensor-only. Touching any of them is intercepted and
shows a contextual warning instead of silently doing nothing or issuing a
command. Returning through `Conectar` resumes the normal BLE scan flow.
## Local Storage

Known keys include:

- `filtertrack.filters.v1`
- `filtertrack.selectedFilterId.v1`
- `filtertrack.knownDevices.v1`
- `filtertrack.activeSession.v1`
- `filtertrack.sessionArchive.v1`
- `filtertrack.pendingSessions.v1`
- `filtertrack.serverUrl.v1`
- `filtertrack.lastSyncAt.v1`
- `filtertrack.debugMode.v1`
- `filtertrack.minimalMode.v1` — `"1"` = minimal mode (default)
- `filtertrack.washLog.v1` — wash records pulled from the firmware
- `filtertrack.velocityUnit.v1` / `filtertrack.flowUnit.v1` — display units
- `filtertrack.minVelocity.v1` — minimum display velocity (default 0.25 m/min)

## Sync

Default app endpoint:

```text
https://filtertrack-api.fly.dev/filtertrack/sessions
```

Pending sessions remain local until the backend confirms successful sync.
Sync is attempted at startup, on session end, and manually.

Note on offline capture: the ESP32 itself can stay offline for a week or
more — the wash log lives in the flash `storage` partition (~68k record
capacity, restored on boot) and sand mode persists in NVS, with detection
running while disconnected (2 s sampling). The app pulls the log on the next
BLE connection.

## BI Integration

Before a device is connected, a native floating BI/data button is visible. After
connection, it is hidden and the WebView `Dados` tab opens `BiDashboardActivity`.

A second floating button opens the offline `CsvAnalysisActivity`. The analyzer
loads the newest usable active or archived session from WebView local storage,
preselects the first and last chart points, and falls back to the bundled
`filtertrack-samples.csv` Primavera series when no session data exists. Its file
picker remains available for manual replacement data.

The launcher uses the blue/green FilterTrack-Cagece mark in the adaptive and
legacy icon resources. The 512 px Play listing asset is
`release/filtertrack-play-icon.png`.

`BiDashboardActivity` loads:

```text
https://filtertrack-api.fly.dev/bi
```

It injects a BI access key into local storage and clicks the login button. The
Android build prefers the admin key when `FILTERTRACK_BI_ADMIN_KEY` is provided,
falling back to `FILTERTRACK_BI_USER_KEY`. Do not copy real key values into
documentation.

The current app-store test build intentionally embeds the BI admin key as an
internal possession check for the installed app. This is not a cryptographic
secret: any key embedded in an APK/AAB can be extracted by reverse engineering.
Treat it as lightweight gatekeeping only.

BI downloads in Android WebView use `window.FilterTrackAndroid.downloadBiFile()`
for allowed `/filtertrack/bi/export/` URLs.
