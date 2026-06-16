
This page lists the raw sources used to create the wiki and how to treat them.

## Primary Raw Sources

- [../info/README.md](../info/README.md) - compact index for AI handoff files.
- [../info/project-overview.md](../info/project-overview.md) - product overview,
  repo map, production path, caveats.
- [../info/android-app.md](../info/android-app.md) - Android shell, BLE flow,
  WebView behavior, local state, sync, debug mode.
- [../info/backend-fastapi.md](../info/backend-fastapi.md) - backend files,
  stack, config, models, routes.
- [../info/bi-dashboard.md](../info/bi-dashboard.md) - BI dashboard behavior,
  roles, proposals, exports, mobile fixes.
- [../info/data-contracts.md](../info/data-contracts.md) - session payload,
  compact samples, deduplication, flow analytics, proposal payloads.
- [../info/deployment-and-ops.md](../info/deployment-and-ops.md) - Fly.io setup,
  deploy and validation commands, risks.
- [../info/firmware-ble-contract.md](../info/firmware-ble-contract.md) - BLE
  service/characteristic, payloads, commands, filtering expectations.
- [../info/monitor-chart-changes.md](../info/monitor-chart-changes.md) - chart
  accumulation and flow inversion changes from 2026-05-07.
- [../info/ai-working-notes.md](../info/ai-working-notes.md) - future-agent
  operating notes and common pitfalls.

## Secondary Raw Sources

- [../context.txt](../context.txt) - older consolidated context with useful
  historical deployment and behavior notes.
- [../backend-fastapi/README.md](../backend-fastapi/README.md) - Portuguese
  backend README covering endpoints, local run, env vars, BI, and Fly deploy.
- [../llm-wiki.md](../llm-wiki.md) - general wiki pattern used for this directory.
- [../firmware/main/FilterTrackv3.c](../firmware/main/FilterTrackv3.c) -
  current in-repo firmware implementation for BLE advertising, notifications,
  commands, ultrasonic reads, LSM303 raw reads, and status LEDs.
- [../firmware/main/idf_component.yml](../firmware/main/idf_component.yml),
  [../firmware/dependencies.lock](../firmware/dependencies.lock), and
  [../firmware/sdkconfig](../firmware/sdkconfig) - firmware build/dependency
  context.

## Implementation Sources To Check Before Editing

- `app/src/main/assets/index.html` - Android WebView UI and local app logic.
- `app/src/main/java/com/example/filtertrack/MainActivity.kt` - WebView setup.
- `app/src/main/java/com/example/filtertrack/BLEManager.kt` - BLE scan/GATT logic.
- `app/src/main/java/com/example/filtertrack/WebAppInterface.kt` - JS bridge.
- `app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt` - BI WebView
  launcher, login injection, export download bridge.
- `backend-fastapi/app/main.py` - FastAPI app, public API, startup migrations.
- `backend-fastapi/app/bi.py` - BI API and proposal approval logic.
- `backend-fastapi/app/static/bi.html` - static BI dashboard UI.
- `backend-fastapi/app/models.py` - SQLAlchemy models.
- `backend-fastapi/app/data/default_filters.json` - backend default filter catalog.
- `firmware/main/FilterTrackv3.c` - firmware BLE protocol and hardware behavior.

## Firmware Source Scope

Use `firmware/main/FilterTrackv3.c` as the current authority for firmware
behavior. Treat `firmware/build/` as generated ESP-IDF output and
`firmware/managed_components/` as dependency/vendor code unless a task explicitly
targets build artifacts or dependency internals.

## Known Source Conflict

`context.txt` records a latest deployed image of
`filtertrack-api:deployment-01KQYV989M1G11MPHF1NPS82AR` on 2026-05-06.

`info/project-overview.md` and `info/deployment-and-ops.md` record a newer latest
observed image:

```text
filtertrack-api:deployment-01KQZEK46NVRSX5K4456Q9EK23
```

This wiki treats the `info/` value as the newer local observation, but future
deployment work should verify live state with:

```powershell
fly status --app filtertrack-api
```
