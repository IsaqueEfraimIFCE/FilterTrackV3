# Architecture

FilterTrack has three active runtime surfaces:

- Android app for field collection.
- FastAPI backend for ingestion, storage, and BI APIs.
- Static BI dashboard served by the backend.

## System Flow

```text
BLE sensor
  -> Android BLEManager.kt
  -> MainActivity.kt WebView bridge
  -> app/src/main/assets/index.html
  -> localStorage session archive and pending sync
  -> POST /filtertrack/sessions
  -> FastAPI + SQLite
  -> /filtertrack/bi/* APIs
  -> backend-fastapi/app/static/bi.html
```

## Android Boundary

Native Kotlin owns Bluetooth, WebView setup, Android permissions, BI WebView
launch, and native downloads. The WebView owns the app UI, filter selection,
local sessions, sample processing, and sync logic.

The JavaScript interface is registered as:

```text
window.Android
```

Native calls back into the WebView through:

```text
window.FilterTrackBridge.<method>(...)
```

See [android-app.md](android-app.md).

## Backend Boundary

FastAPI owns session ingest, canonicalization, deduplication, database schema,
BI APIs, proposal approval, exports, and static BI serving.

The backend intentionally uses SQLite on one Fly volume. Postgres was removed
from the active architecture. See [backend-fastapi.md](backend-fastapi.md) and
[deployment-operations.md](deployment-operations.md).

## BI Boundary

The BI page is a static React-in-HTML application at
`backend-fastapi/app/static/bi.html`. It calls `/filtertrack/bi/*` endpoints with
`X-Access-Key`.

Android opens the BI in `BiDashboardActivity`, injects a user key into local
storage, and provides a native download bridge for export URLs. See
[bi-dashboard.md](bi-dashboard.md).

## Shared Contracts

- BLE commands and notification payloads are documented in
  [ble-firmware-contract.md](ble-firmware-contract.md).
- Sample upload and compact storage are documented in
  [sessions-data-contract.md](sessions-data-contract.md).
- Flow and velocity sign convention are documented in
  [sensor-processing-flow.md](sensor-processing-flow.md).

