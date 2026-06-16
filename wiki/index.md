# Wiki Index

Use this file as the first stop for project navigation. It is content-oriented,
not chronological. For recent wiki maintenance, see [log.md](log.md).

## Operating Files

- [README.md](README.md) - entry point for this generated wiki.
- [schema.md](schema.md) - rules for ingesting new sources and maintaining pages.
- [sources.md](sources.md) - raw source register, authority levels, and conflicts.
- [log.md](log.md) - append-only maintenance timeline.

## Product And Architecture

- [project-overview.md](project-overview.md) - what FilterTrack is, who it serves,
  and the current production path.
- [architecture.md](architecture.md) - Android, BLE sensor, FastAPI, SQLite, and BI
  interactions.

## Android And Sensor

- [android-app.md](android-app.md) - native Kotlin shell, WebView app, bridge methods,
  scan/reconnect behavior, local storage, BI launcher.
- [android-play-internal-testing.md](android-play-internal-testing.md) - Play internal
  testing package, signing, build, and console checklist.
- [ble-firmware-contract.md](ble-firmware-contract.md) - advertised BLE name, UUIDs,
  notification payloads, commands, and firmware assumptions.
- [sensor-processing-flow.md](sensor-processing-flow.md) - raw distance parsing,
  filtering, 10-second windows, chart reset behavior, sign convention.

## Backend, Data, And BI

- [sessions-data-contract.md](sessions-data-contract.md) - Android upload payload,
  compact sample format, canonical storage, deduplication, proposals.
- [backend-fastapi.md](backend-fastapi.md) - FastAPI routes, configuration,
  database models, startup migrations, SQLite policy.
- Public privacy policy URL: `https://filtertrack-api.fly.dev/privacy`.
- [bi-dashboard.md](bi-dashboard.md) - BI views, auth roles, session/filter edits,
  proposal approval, Android downloads.

## Operations

- [deployment-operations.md](deployment-operations.md) - Fly.io app state, deploy
  commands, validation, backup commands, operational limits.
- [risks-open-items.md](risks-open-items.md) - unresolved work, safety constraints,
  and known source conflicts.
- [agent-workflows.md](agent-workflows.md) - focused instructions for common future
  tasks.

## High-Value Facts

- Active Android UI source: `app/src/main/assets/index.html`.
- Active BI source: `backend-fastapi/app/static/bi.html`.
- Backend app path: `backend-fastapi/`.
- Active firmware source: `firmware/main/FilterTrackv3.c`.
- Production backend: `https://filtertrack-api.fly.dev/`.
- Production BI: `https://filtertrack-api.fly.dev/bi`.
- Play Console package: `com.filtertrack`; current uploaded-test build uses
  `versionCode=2`, `versionName=1.0`.
- Signed Play AAB path: `app/build/outputs/bundle/release/app-release.aab`.
- Privacy policy URL: `https://filtertrack-api.fly.dev/privacy`.
- Fly app: `filtertrack-api`, region `gru`, single SQLite DB on a Fly volume.
- Android BLE interface is exposed to WebView as `window.Android`.
- BI access and ingest API auth are separate: BI uses `X-Access-Key`; optional
  app API auth uses `X-API-Key`.
- Android release builds prefer `FILTERTRACK_BI_ADMIN_KEY` from ignored
  `release.properties`, falling back to `FILTERTRACK_BI_USER_KEY`.
- Do not copy real BI/admin/user keys into this wiki.
