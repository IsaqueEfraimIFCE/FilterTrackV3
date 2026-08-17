# Wiki Log

Append entries here when sources are ingested, wiki pages change, or substantial
answers are filed back into the wiki.

## [2026-07-15] update | Rebuild Play bundle with version code 5

- Play Console reported `versionCode=4` as already used.
- Incremented the Android release to `versionCode=5`, retaining
  `versionName=1.2`, and rebuilt the signed Play bundle at
  `app/build/outputs/bundle/release/app-release.aab`.

## [2026-07-15] Android | Complete app sensorless consultation

- Added `Entrar sem sensor` to the complete-app BLE connection screen.
- Kept sessions, filters, Ajustes, CSV, and BI available without BLE.
- Added a persistent disconnected banner and a `Conectar` action.
- Marked and intercepted sensor-only session, command, calibration, wash
  detection, and firmware controls with a contextual connection warning.
## [2026-07-15] Android | Interactive tutorial coach marks

- Replaced text-slide tutorial cards with spotlights anchored to the real app
  controls and real touch-driven mode/navigation transitions.
- Added multi-step coach marks inside the CSV analyzer and a highlighted BI
  navigation view.
- Added `Abrir tutorial` and `Mostrar ao abrir o app` controls in Ajustes.
- Added finish/exit choices for showing the tutorial on the next launch.
## [2026-07-15] Android | First-launch full app guide

- Added a first-launch-only walkthrough using simulated BLE and flow data.
- The guide demonstrates simplified mode, full mode, the Settings return path,
  the offline CSV analyzer, and the online BI dashboard.
- Guide completion is stored in native SharedPreferences; simulated steps do
  not save sessions or send sensor commands.
- Validated the React/JSX transform and rebuilt Kotlin successfully.
## [2026-07-15] Android | Analyzer data, larger UI, and Play assets

- Added automatic newest-session loading, endpoint preselection, and bundled
  Primavera CSV fallback to the offline two-point flow analyzer.
- Simplified analyzer copy, enlarged typography/touch targets, and renamed it
  to `Analisador de vazão`.
- Replaced launcher resources with the blue/green FilterTrack-Cagece mark and
  added the 512 px Play listing icon.
- Built and USB-tested `versionCode=4` / `versionName=1.2`; signed Play bundle:
  `app/build/outputs/bundle/release/app-release.aab`.

## [2026-07-09] revert | Session/wash button + chart refinements rolled back

The user reported problems with the build containing the session-control
button and follow-up changes. `index.html` was reverted to the state right
before the button work. **Removed**: "Encerrar sessão"/"Fim de lavagem"
buttons, "Velocidade" label rename (back to "Velocidade de subida"),
continuous-chart/negatives-as-zero drawing, threshold-filtered "Média",
`session.washEvents` upload, `occurredAt` reconstruction from `WL=CNT`,
and the `online`-event/3-min sync retries. **Kept**: the earlier
minimal-mode display rules (tiles never dash while data flows, 0.00 below
threshold, wash chart from `WL=WSTART` with frozen tiles) and
`versionCode=3`/`versionName=1.1` (AAB rebuilt from the reverted code —
re-upload this one to Play, not the earlier build). Firmware was never
touched in this cycle (still `PROJECT_VER 1.8.0` from the wash-state-machine
work); no OTA was performed, so nothing to revert on the sensor. Entries
below describing the removed features are historical.

## [2026-07-09] update | Wiki catch-up + minimal-mode display rules

- Audited the wiki against the working tree (firmware 1.8.0). Fixed the stale
  "1.875 MB" OTA slot size in [ble-firmware-contract.md](ble-firmware-contract.md)
  (slots are 1344 KB since the `storage` partition was added), documented
  commands `5`/`6` (sand mode), the `SAND=` notification, and the `LOG:`
  wash-log transfer protocol (`LOG:COUNT/READ/ACK/TEST`, `WL=CNT/R/END/CLR/ADD/ERR`).
- Rewrote the WebView UI section of [android-app.md](android-app.md): minimal
  mode (default single-screen sand-filter UI), its firmware coupling
  (command 5 enforcement, silent wash-log sync), session lifecycle around
  washes, and the new local-storage keys.
- Linked [firmware-build-and-flash.md](firmware-build-and-flash.md) from
  index.md and README.md (it was orphaned).
- Changed minimal-mode display behavior in `index.html` and documented it:
  dashes only when there is no valid sensor data; below-threshold readings
  show as 0.00 with the chart dropping to 0.00; during a wash the chart
  restarts at WL=WSTART and accumulates every 2 s (no inversion resets,
  no threshold cut) while the velocity/vazão tiles hold the latest
  above-threshold reading.

- Added a manual "Encerrar sessão" button to the minimal screen: saves the
  session and sends command `1` (low power) to the ESP32; a "Iniciar nova
  medição" button sends `2` and opens the next session. Hidden during washes.
- Replaced "Encerrar sessão" with **"Fim de lavagem"**: records a manual wash
  entry (elapsed time as duration), force-saves the session (`wash_manual`),
  and starts the next one; no BLE command (sensor keeps reading).
- Offline-first hardening: pending sessions now also retry on the `online`
  event and on a 3-min interval; wash-log entries not yet uploaded are
  attached to the next finalized session as `session.washEvents` (backend
  stores the full payload, so no server change); wash records from the
  sensor's current boot get an absolute `occurredAt` reconstructed from
  `WL=CNT` boot/uptime. Verified the firmware side already covers week-long
  offline capture (washlog in flash, ~68k records, restored on boot; sand
  mode in NVS; detection runs disconnected at 2 s sampling).
- Bumped app to `versionCode=3` / `versionName=1.1` and built the Play
  bundle: `app/build/outputs/bundle/release/app-release.aab`.
- Minimal chart refinements: labels renamed to "Velocidade"; chart is now
  fully continuous (no inversion resets in minimal mode, negatives plotted
  as 0, line breaks only on >45 s sensor-data gaps); the "Média" stat uses
  absolute values and counts only points at/above the display threshold.

Missing-history note: the OTA update path, storage partition + self-test,
wash state machine, tilt calibration, and minimal mode were built between
late June and early July 2026 but never logged here; their documentation
lives in the topic pages above.

## [2026-06-11] update | Play internal testing, privacy policy, signing, admin key

- Prepared Android for Google Play internal/development testing with package
  `com.filtertrack`.
- Current Play test release uses `versionCode=2` and `versionName=1.0`; Play had
  already consumed `versionCode=1`.
- Generated a signed AAB at `app/build/outputs/bundle/release/app-release.aab`.
  Signature verification should show `META-INF/FILTERTR.RSA`.
- Generated a local upload keystore at `release/filtertrack-upload.jks` with
  credentials in ignored `keystore.properties`.
- Added a public privacy policy page served by FastAPI at
  `https://filtertrack-api.fly.dev/privacy`; deployed and validated HTTP 200.
- Added `FILTERTRACK_BI_ADMIN_KEY` support to Android release builds. The app
  now prefers the admin key and falls back to `FILTERTRACK_BI_USER_KEY`.
- Rotated `FILTERTRACK_ADMIN_KEY` on Fly because the previous value was not
  recoverable from Fly or the wiki. The current value is stored only in ignored
  local `release.properties` and must not be copied into documentation.
- Validated production BI auth with the local admin key: `/filtertrack/bi/auth`
  returned HTTP 200 and `role=admin`.

## [2026-05-19] fix | BLE chunk reassembly + angle formula clarification

- Fixed `BLEManager.kt`: added `handleRawBleData()` to buffer 20-byte BLE chunks
  before dispatching to JS. Without this, each chunk was parsed independently:
  continuation chunks (bare integers like `"20"`) were treated as new distance
  readings, inflating "Amostras 2s" count and corrupting "Distancia ESP" display.
- Confirmed `acos(|gz|)` is the **correct** angle formula. The LSM303 Z axis is
  perpendicular to the ultrasonic beam: gz ≈ 0 when sensor is vertical (pointing
  down), so acos(0) = 90° → sin(90°) = 1 → no correction. `asin` would be wrong.
- Documented both findings:
  - [sensor-processing-flow.md](sensor-processing-flow.md): "Angle Correction — Sensor Mounting" section
  - [ble-firmware-contract.md](ble-firmware-contract.md): chunk reassembly explanation

## [2026-05-19] update | Firmware build and flash — IDF v5.5.1 + magnetometer removal

- Removed all magnetometer code from firmware, app, Python serial tool, and wiki.
- Firmware rebuilt with IDF v5.5.1 (stale v4.3.1 build cache cleared first).
- Flashed successfully to ESP32-C3 on COM13 at 460800 baud via `python -m esptool`.
- Rewrote [firmware-build-and-flash.md](firmware-build-and-flash.md) with verified
  IDF v5.5.1 paths, PowerShell env setup, flash address map, and troubleshooting
  notes (port busy, baud fallback, esptool v5 flag changes).

## [2026-05-15] ingest | ADB path and device install flow

- Discovered `adb` is not in the system PATH on the dev machine.
- Full path: `$env:USERPROFILE\AppData\Local\Android\Sdk\platform-tools\adb.exe`.
- Added "Install On Device" section to [deployment-operations.md](deployment-operations.md)
  with the `$adb` variable pattern and the build→install command sequence.

## [2026-05-12] ingest | Add in-repo firmware context

- Added `firmware/` as the current ESP-IDF firmware source.
- Updated BLE firmware contract pages to use `firmware/main/FilterTrackv3.c`
  instead of older external desktop paths.
- Marked `firmware/build/` as generated output and `firmware/managed_components/`
  as dependency/vendor code for context scoping.

## [2026-05-11] ingest | Initial project wiki

Created the first `wiki/` layer from `llm-wiki.md`, `context.txt`, `info/`, and
`backend-fastapi/README.md`.

Pages created:

- `README.md`
- `index.md`
- `schema.md`
- `sources.md`
- `project-overview.md`
- `architecture.md`
- `android-app.md`
- `ble-firmware-contract.md`
- `sensor-processing-flow.md`
- `sessions-data-contract.md`
- `backend-fastapi.md`
- `bi-dashboard.md`
- `deployment-operations.md`
- `risks-open-items.md`
- `agent-workflows.md`

Noted one source conflict around the latest observed Fly image and preserved it
in [sources.md](sources.md) and [risks-open-items.md](risks-open-items.md).
