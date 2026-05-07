# Project Overview

## Purpose

FilterTrack is an Android-based field collection app for monitoring filter water-level behavior through a BLE ultrasonic sensor. The app stores local collection sessions, syncs them to a cloud backend, and exposes a BI dashboard for operational review, flow analytics, filter catalog management, and data correction proposals.

## Repository Map

- `app/` - Android application module.
- `app/src/main/assets/index.html` - primary WebView UI used by the Android app.
- `app/src/main/java/com/example/filtertrack/` - native Android BLE/WebView shell.
- `backend-fastapi/` - production FastAPI backend and BI static page.
- `backend-fastapi/app/static/bi.html` - BI dashboard served by the backend at `/bi`.
- `backend-fastapi/app/data/default_filters.json` - default filter catalog extracted from the Android catalog.
- `app/src/main/assets/` - WebView asset bundle and Cagece images used by the Android app.
- `context.txt` - older consolidated working context. Useful, but not authoritative.
- `info/` - AI handoff context files.

## Active Systems

The active production path is:

1. Android WebView app collects BLE distance samples.
2. The app stores sessions locally and queues pending sync.
3. Sessions are posted to `https://filtertrack-api.fly.dev/filtertrack/sessions`.
4. FastAPI normalizes and stores sessions in SQLite on a Fly volume.
5. BI at `https://filtertrack-api.fly.dev/bi` reads from the same backend and supports analytics, exports, and approval workflows.

## Current Tech Stack

- Android native shell: Kotlin, Android WebView, BLE GATT APIs.
- WebView app UI: standalone React loaded from CDN inside `index.html`.
- Backend: FastAPI, SQLAlchemy, SQLite.
- Deployment: Fly.io app `filtertrack-api`, region `gru`, one machine, one mounted volume.

## Current Production State

Last observed deployment in this workspace:

- Fly app: `filtertrack-api`
- Public host: `filtertrack-api.fly.dev`
- Machine id: `2863064c759e38`
- Latest deployed image observed after the last deploy: `filtertrack-api:deployment-01KQZEK46NVRSX5K4456Q9EK23`
- Health check observed: `/health` returned `HTTP 200` with `status: healthy`.
- BI check observed: `/bi` returned `HTTP 200` and included the latest `Área em m²` UI text.

## Current Caveats

- `app/src/main/assets/index.html` is edited directly. Older notes say it used to be generated from a desktop template, but that template was not present in the workspace when the current changes were made.
- The legacy Node/NDJSON backend, old browser mock dashboard, root duplicate assets, sample smoke payload, and template Android tests were removed during cleanup.
- The backend source has local uncommitted changes. Check `git status --short` before assuming a clean baseline.
- `backend-fastapi/sessions.txt` is intentionally deleted after its useful context was merged into `context.txt`.
- The BI user key is embedded in `BiDashboardActivity.kt` for auto-login. Do not duplicate that value into public notes.
