# FilterTrack

Android app for field technicians to measure sand-filter backwash flow via BLE, using the ESP32-C6 sensor unit, and sync sessions to the cloud BI dashboard.

- [filtertrack-firmware](https://github.com/IsaqueEfraimIFCE/filtertrack-firmware) — ESP32-C6 firmware + enclosure/PCB hardware files
- [filtertrack-backend](https://github.com/IsaqueEfraimIFCE/filtertrack-backend) — FastAPI backend + BI dashboard

## Requirements

- Android Studio (or Gradle 9.3.1 + JDK 21 standalone)
- compileSdk/targetSdk 36, minSdk 28
- A paired [FilterTrack ESP32-C6 sensor](https://github.com/IsaqueEfraimIFCE/filtertrack-firmware) for live use (the app also works offline/without a sensor for session review and the BI dashboard)

## Build & install

```powershell
.\gradlew.bat assembleDebug
.\gradlew.bat installDebug   # with a device/emulator connected
```

Release builds need a signing config — copy `keystore.properties.example` → `keystore.properties` and `release.properties.example` → `release.properties`, fill in your keystore and BI access keys (both files are gitignored).

## App structure

- Native Kotlin shell (`app/src/main/java/...`): BLE (`BLEManager.kt`), lifecycle (`MainActivity.kt`), JS bridge (`WebAppInterface.kt`), BI WebView (`BiDashboardActivity.kt`)
- WebView UI (`app/src/main/assets/index.html`): minimal single-screen field mode by default; full tabbed app behind "Abrir app completo"
- BLE device name: `FilterTrackV3` — see the firmware repo for the full command/notification protocol

## Backend URL

The app posts sessions to a FastAPI backend (`serverUrl` in-app setting), default `https://filtertrack-api.fly.dev/filtertrack/sessions`. Run your own backend from [filtertrack-backend](https://github.com/IsaqueEfraimIFCE/filtertrack-backend) and point the app at it if needed.
