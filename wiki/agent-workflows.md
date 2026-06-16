# Agent Workflows

Use this page to decide what to read and validate for common tasks.

## Android UI Or BLE Task

Read:

- [android-app.md](android-app.md)
- [ble-firmware-contract.md](ble-firmware-contract.md)
- [sensor-processing-flow.md](sensor-processing-flow.md)

Likely files:

- `app/src/main/assets/index.html`
- `app/src/main/java/com/example/filtertrack/MainActivity.kt`
- `app/src/main/java/com/example/filtertrack/BLEManager.kt`
- `app/src/main/java/com/example/filtertrack/WebAppInterface.kt`

Validation:

```powershell
.\gradlew.bat :app:assembleDebug
```

## Backend API Task

Read:

- [backend-fastapi.md](backend-fastapi.md)
- [sessions-data-contract.md](sessions-data-contract.md)
- [deployment-operations.md](deployment-operations.md)

Likely files:

- `backend-fastapi/app/main.py`
- `backend-fastapi/app/models.py`
- `backend-fastapi/app/schemas.py`
- `backend-fastapi/app/utils.py`

Validation:

```powershell
cd backend-fastapi
python -m compileall app
```

## BI Dashboard Task

Read:

- [bi-dashboard.md](bi-dashboard.md)
- [backend-fastapi.md](backend-fastapi.md)
- [sessions-data-contract.md](sessions-data-contract.md)

Likely files:

- `backend-fastapi/app/static/bi.html`
- `backend-fastapi/app/bi.py`
- `app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt`

For Android BI download behavior, also read [android-app.md](android-app.md).

## Flow Or Sample Math Task

Read:

- [sensor-processing-flow.md](sensor-processing-flow.md)
- [sessions-data-contract.md](sessions-data-contract.md)
- [ble-firmware-contract.md](ble-firmware-contract.md)

Check both Android and BI/backend code if changing sign convention, filter area,
or aggregation windows.

## Deploy Task

Read:

- [deployment-operations.md](deployment-operations.md)
- [risks-open-items.md](risks-open-items.md)

Commands:

```powershell
cd backend-fastapi
fly deploy --app filtertrack-api
fly status --app filtertrack-api
```

Live checks:

```powershell
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/health" -UseBasicParsing
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/bi" -UseBasicParsing
```

Do not expose secrets in logs, docs, or final answers.

## Wiki Maintenance

Read:

- [schema.md](schema.md)
- [sources.md](sources.md)

After substantial project changes, update the relevant topic page, then
[index.md](index.md) if navigation changes, and finally append [log.md](log.md).

