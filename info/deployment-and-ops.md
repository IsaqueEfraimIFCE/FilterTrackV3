# Deployment And Operations

## Production Hosting

Production backend runs on Fly.io.

```text
App: filtertrack-api
Region: gru
Public URL: https://filtertrack-api.fly.dev/
BI URL: https://filtertrack-api.fly.dev/bi
```

Fly config file:

```text
backend-fastapi/fly.toml
```

Current Fly settings from `fly.toml`:

- Internal port: `8080`
- HTTPS forced
- Volume source: `filtertrack_data`
- Volume destination: `/data`
- `DATABASE_PATH=/data/filtertrack.db`
- `MAX_SAMPLES_PER_SESSION=120000`
- `auto_stop_machines="stop"`
- `auto_start_machines=true`
- `min_machines_running=0`
- VM: 512 MB shared CPU, 1 CPU

## Deploy Command

Run from `backend-fastapi/`:

```powershell
fly deploy
```

Useful explicit variant:

```powershell
fly deploy --app filtertrack-api
```

## Validation Commands

```powershell
fly status --app filtertrack-api
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/health" -UseBasicParsing
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/bi" -UseBasicParsing
```

Expected:

- `/health` returns HTTP 200 and JSON with `ok: true`, `status: healthy`.
- `/bi` returns HTTP 200 and BI HTML.

Latest observed deployment from this workspace:

- Image: `filtertrack-api:deployment-01KQZEK46NVRSX5K4456Q9EK23`
- Machine: `2863064c759e38`
- Machine state after validation: `started`
- `/health`: HTTP 200
- `/bi`: HTTP 200

## Local Backend Run

From `backend-fastapi/`:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8080 --reload
```

Local health:

```powershell
Invoke-WebRequest -Uri "http://127.0.0.1:8080/health" -UseBasicParsing
```

Without config, local SQLite DB is `backend-fastapi/filtertrack.db`.

## Android Build Validation

From repo root:

```powershell
.\gradlew.bat :app:assembleDebug
```

This validates Android/Kotlin compile and packages WebView assets.

## Backend Code Validation

Basic Python syntax check:

```powershell
cd backend-fastapi
python -m compileall app
```

Optional JS syntax check for the BI page depends on Node availability and the page structure. The static BI page uses JSX-like React script in-browser, so use source-aware checks carefully.

## Operational Risks

- SQLite volume is local to a single Fly machine. It is not multi-writer shared storage.
- There are no automatic app-level backups in this repo. Back up `/data/filtertrack.db` if data matters.
- `min_machines_running=0` reduces cost but can cause cold starts.
- Do not set production `DATABASE_URL` to Postgres; the current backend intentionally supports SQLite only.
- If `FILTERTRACK_API_KEY` is enabled, Android sync must be updated to send `X-API-Key`.

## Secrets

Expected production secrets:

- `FILTERTRACK_USER_KEY`
- `FILTERTRACK_ADMIN_KEY`
- Optional: `FILTERTRACK_API_KEY`

Never write secret values into docs or prompts. The Android BI activity currently embeds a user key for auto-login; avoid reproducing it.
