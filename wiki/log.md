# Wiki Log

Append entries here when sources are ingested, wiki pages change, or substantial
answers are filed back into the wiki.

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
