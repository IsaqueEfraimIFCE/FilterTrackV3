# FilterTrack AI Context Index

This folder contains compact project context for future AI agents and developers.
Read this file first, then open only the system file relevant to the task.

## Files

- [project-overview.md](project-overview.md) - product purpose, repo map, active systems, current caveats.
- [android-app.md](android-app.md) - native Android shell, WebView app, BLE bridge, local collection flow.
- [backend-fastapi.md](backend-fastapi.md) - production backend architecture, routes, models, config.
- [bi-dashboard.md](bi-dashboard.md) - web BI dashboard, roles, proposals, edit/archive flows.
- [data-contracts.md](data-contracts.md) - session payloads, compact samples, flow analytics, filter catalog.
- [deployment-and-ops.md](deployment-and-ops.md) - Fly.io deployment, validation, local commands, operational risks.
- [firmware-ble-contract.md](firmware-ble-contract.md) - in-repo firmware source, BLE service/characteristic, payloads, commands expected by the app.
- [monitor-chart-changes.md](monitor-chart-changes.md) - velocidade de subida and vazão chart accumulation, flow inversion detection (updated 2026-05-07).
- [ai-working-notes.md](ai-working-notes.md) - guidance for future agents working in this workspace.

## Fast Start For AI Agents

1. For Android UI, BLE, local sessions, debug mode, or app build issues, read `android-app.md`.
2. For cloud API, database, Fly deploy, ingest/list/stats, or auth, read `backend-fastapi.md` and `deployment-and-ops.md`.
3. For BI charts, filters, sessions, proposals, edit/archive, or dashboard layout, read `bi-dashboard.md`.
4. For payload shape, samples, flow math, or filter catalog behavior, read `data-contracts.md`.
5. For hardware/BLE assumptions, read `firmware-ble-contract.md` and then check `../firmware/main/FilterTrackv3.c` before changing the protocol.

## Current Production URLs

- Backend base: `https://filtertrack-api.fly.dev/`
- Android ingestion endpoint: `https://filtertrack-api.fly.dev/filtertrack/sessions`
- BI dashboard: `https://filtertrack-api.fly.dev/bi`

## Important Safety Notes

- Do not assume `context.txt` is exhaustive or current; treat this `info/` folder plus source files as the handoff context.
- Do not commit or expose real BI/admin/user keys. The Android BI activity currently contains an embedded user key in source; avoid copying it into docs.
- The production backend uses a single SQLite database on a Fly volume. Backups are not automatic.
- The worktree may contain unrelated local changes. Do not revert files unless explicitly asked.
