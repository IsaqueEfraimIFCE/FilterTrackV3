# Project Overview

FilterTrack is an Android field collection app for monitoring filter water-level
behavior using a BLE ultrasonic sensor. Operators collect distance samples in the
Android app, sync sessions to a cloud backend, and review analytics or propose
corrections through a BI dashboard.

## Active Production Path

1. A BLE sensor advertises as `FilterTrackV3` and sends distance notifications.
2. The Android app connects through native BLE code and pushes events into a
   WebView UI.
3. The WebView UI processes distance into velocity and flow, stores local
   sessions, and queues unsynced records.
4. Sessions are uploaded to `https://filtertrack-api.fly.dev/filtertrack/sessions`.
5. FastAPI stores canonical session payloads in SQLite on a Fly volume.
6. BI at `https://filtertrack-api.fly.dev/bi` reads from the backend for charts,
   exports, filter management, and proposal approval.

See [architecture.md](architecture.md) for the end-to-end data flow.

## Repository Map

- `app/` - Android app module.
- `app/src/main/assets/index.html` - primary WebView UI.
- `app/src/main/java/com/example/filtertrack/` - native Android shell, BLE, and
  WebView bridge code.
- `backend-fastapi/` - production FastAPI backend and static BI dashboard.
- `backend-fastapi/app/static/bi.html` - BI dashboard served at `/bi`.
- `backend-fastapi/app/data/default_filters.json` - backend default filter catalog.
- `firmware/` - ESP-IDF firmware project for the BLE ultrasonic sensor.
- `firmware/main/FilterTrackv3.c` - primary firmware source for BLE advertising,
  notifications, commands, ultrasonic reads, LSM303 raw reads, and status LEDs.
- `info/` - modular AI handoff context.
- `wiki/` - generated, persistent project wiki.

## Current Stack

- Android: Kotlin, Android WebView, BLE GATT APIs.
- WebView UI: standalone React loaded from CDN inside `index.html`.
- Backend: FastAPI, SQLAlchemy, SQLite.
- Firmware: ESP-IDF for ESP32-C3 BLE sensor firmware.
- Hosting: Fly.io app `filtertrack-api` in `gru`.
- Database: one SQLite file mounted at `/data/filtertrack.db`.

## Current Caveats

- `app/src/main/assets/index.html` is currently edited directly. Older notes
  mention a desktop template workflow, but the template was not present when
  recent changes were made.
- The legacy Node/NDJSON backend and old root browser mock dashboard were removed.
- The worktree had local uncommitted changes when this wiki was created.
- Do not copy embedded BI access keys from Android source into docs or prompts.
