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
- Version code: `2`
- Version name: `1.0`
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

Major sections in `index.html`:

- Connect screen and device list.
- Filter picker and filter catalog.
- Monitor tab with velocity and flow metrics.
- Sessions tab with local archive and manual sync.
- Filters tab for selecting filter definitions.
- Settings tab with server URL, BLE commands, and debug mode.
- Debug panel with raw and processed metrics.

The selected-filter card shows derived filter number, direction, location,
business unit, and area. It does not show the raw filter name/id.

## Local Storage

Known keys include:

- `filtertrack.filters.v1`
- `filtertrack.selectedFilter.v1`
- `filtertrack.knownDevices.v1`
- `filtertrack.activeSession.v1`
- `filtertrack.sessionArchive.v1`
- `filtertrack.pendingSessions.v1`
- `filtertrack.serverUrl.v1`
- `filtertrack.lastSyncAt.v1`
- `filtertrack.debugMode.v1`

## Sync

Default app endpoint:

```text
https://filtertrack-api.fly.dev/filtertrack/sessions
```

Pending sessions remain local until the backend confirms successful sync.

## BI Integration

Before a device is connected, a native floating BI/data button is visible. After
connection, it is hidden and the WebView `Dados` tab opens `BiDashboardActivity`.

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
