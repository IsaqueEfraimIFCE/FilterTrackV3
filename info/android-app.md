# Android App Context

## Primary Files

- `app/src/main/java/com/example/filtertrack/MainActivity.kt`
- `app/src/main/java/com/example/filtertrack/BLEManager.kt`
- `app/src/main/java/com/example/filtertrack/WebAppInterface.kt`
- `app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt`
- `app/src/main/assets/index.html`
- `app/src/main/res/layout/activity_main.xml`

## Build

The Android module is `:app`.

Useful validation command:

```powershell
.\gradlew.bat :app:assembleDebug
```

Current app config:

- Application id: `com.example.filtertrack`
- minSdk: `28`
- targetSdk: `36`
- compileSdk: Android 36.1
- Java compatibility: 11

## Native Shell

`MainActivity.kt` creates the WebView, enables JavaScript and DOM storage, registers the JavaScript interface as `window.Android`, and loads:

```text
file:///android_asset/index.html
```

Native pushes BLE events into the WebView through:

```javascript
window.FilterTrackBridge.<method>(...)
```

If the page is not ready, native JS messages are queued and flushed after `onPageFinished`.

## JavaScript Bridge

`WebAppInterface.kt` exposes these methods to `index.html`:

- `Android.startScan()`
- `Android.stopScan()`
- `Android.connect(address)`
- `Android.disconnect()`
- `Android.sendCommand(cmd)`
- `Android.openBiDashboard()`
- `Android.isBluetoothEnabled()`

The WebView wraps these calls through its local bridge object. The bottom tab `Dados` calls `openBiDashboard`.

## BLE Flow

`BLEManager.kt` handles BLE scanning, connection, notifications, RSSI, and serialized GATT writes.

Key behavior:

- Scans with `SCAN_MODE_LOW_LATENCY`.
- Only reports scan results with a resolvable advertised or device name.
- On connect, discovers services, enables notifications, reads RSSI, and reports connection state.
- Uses a write queue with retry/timeout logic because Android allows only one pending GATT write at a time.
- Prefer the known FilterTrack service/characteristic and fall back to first writable/notifiable characteristic if needed.

## Connection Recovery

The connect screen keeps scanning continuously and restarts scanning automatically until the user taps `Parar`. Tapping `Retomar busca` resumes the scan loop.

If more than one `FilterTrackV3` sensor is visible, auto-connect is paused, the `Escolha esse` hint is hidden, and the user must choose the device from the list manually.

When the advertised name is `FilterTrackV3`, the Android UI hides the MAC address in the device list and connected-device bar.

If Bluetooth stops or the device connection is lost unexpectedly, the app shows a warning modal with options to try a sensor restart, wait while scanning/reconnecting continues, or restart the app.

On unexpected connection loss, the active session is kept open for automatic reconnect only when the latest displayed velocity is above `0.1 m/min`. If velocity is unavailable or at/below that threshold, the old session is finalized and the next connection starts a new session.

If phone Bluetooth is disabled while connected, native Android forces the BLE state to disconnected and pushes a Bluetooth-off error so the WebView does not remain frozen on the monitor screen.

Manual `Desconectar` does not show the lost-connection warning, but the connect screen still resumes scanning unless the user presses `Parar`.

When the sensor is restarted from Settings, the app finalizes the active session, sends command `0`, keeps scanning, and does not reload the WebView. If the device reconnects automatically after restart, the app keeps the current selected filter and does not open the filter picker.

## WebView UI

The main UI lives in `app/src/main/assets/index.html`.

Important UI sections:

- Connect screen and auto-scan/auto-connect.
- Filter picker and filter catalog.
- Monitor tab with flow/velocity metrics.
- Sessions tab with local archive and manual sync.
- Filters tab for selecting filter definitions.
- Settings tab with server URL, BLE sensor commands, and debug mode.
- Debug panel showing raw and processed metrics.

## Storage Keys

The WebView stores local state in `localStorage` under keys including:

- `filtertrack.filters.v1`
- `filtertrack.selectedFilter.v1`
- `filtertrack.knownDevices.v1`
- `filtertrack.activeSession.v1`
- `filtertrack.sessionArchive.v1`
- `filtertrack.pendingSessions.v1`
- `filtertrack.serverUrl.v1`
- `filtertrack.lastSyncAt.v1`
- `filtertrack.debugMode.v1`

## Sync Endpoint

Default server URL in the app:

```text
https://filtertrack-api.fly.dev/filtertrack/sessions
```

Pending sessions are stored locally, posted to the backend, and removed from the pending queue only after successful sync.

## Sensor Processing

Current rules in the WebView pipeline:

- Payload parsing supports `DIST=...`, JSON-like payloads, and numeric extraction.
- Distance readings below `25 cm` are ignored.
- First distance reading after connect/command reset is discarded.
- A robust 2-second median/MAD filter is used before flow calculation.
- Flow/velocity uses a 10-second window.
- The velocity metric and chart refresh every 10 seconds.
- During the first 10 seconds, the velocity and flow tiles show a collecting-data countdown and spinner.
- When velocity is unavailable, flow is also unavailable and both tiles show the current processing reason.
- The flow tile toggles between `L/min` and `m3/h`.
- Movement below `0.1 cm` over 10 seconds is ignored.
- Velocity above `10 m/min` is treated as invalid.
- Flow uses `areaM2 * velocityMPerMin * 1000` to produce L/min.

## Chart Accumulation and Flow Inversion (Updated 2026-05-07)

The velocidade de subida (velocity) and vazão (flow) charts now accumulate data from session start through the current point.

**Chart behavior:**
- Accumulates all data points until flow inverts
- Does not limit history to recent points (previously limited to 3600 points)
- On flow inversion, history resets and a 10-second countdown begins
- After 10 seconds, resumes accumulating new data

**Flow inversion detection:**
- Compares velocity sign (`Math.sign(velocityMpm)`) between consecutive 10-second windows
- Triggered when velocity sign flips from positive to negative or vice versa
- Examples:
  * Window 1: distance 30cm→40cm (negative velocity) → Window 2: distance 40cm→30cm (positive velocity) = inversion
  * Window with ascending flow + next window with descending flow = inversion
- Implementation uses `lastWindowVelocitySignRef` to track previous window's velocity sign
- See `info/monitor-chart-changes.md` for detailed documentation

Sign convention:

- Distance decreasing means water level rising, positive velocity/flow.
- `Subindo` is success/green.
- `Descendo` is danger/red.

## Debug Mode

Settings has a persisted debug toggle. The debug panel currently shows:

- Distance from ESP.
- Post-processed distance.
- Processed velocity and 10-second flow.
- Raw "sem filtros" velocity, flow, and movement.
- Movement in processed 10-second window.
- Recent sample counts and buffer count.
- Distance chart and raw ESP payload log.
- Processing rules used by the app.

The raw "sem filtros" values still need a valid selected filter area to compute raw flow; raw velocity/movement do not need filter area.

## BI Button And BI Activity

Before connection, the native floating BI/data button is visible. After connection, it is hidden and the WebView bottom tab `Dados` opens `BiDashboardActivity`.

`BiDashboardActivity.kt` loads:

```text
https://filtertrack-api.fly.dev/bi
```

It auto-injects a user access key into BI localStorage and clicks the login button. Do not copy the key into docs.

If BI fails to load, the activity routes back to the initial app screen with an error toast.
