# Deployment And Operations

Production backend runs on Fly.io.

```text
App: filtertrack-api
Region: gru
Public URL: https://filtertrack-api.fly.dev/
BI URL: https://filtertrack-api.fly.dev/bi
```

## Fly Configuration

Config file:

```text
backend-fastapi/fly.toml
```

Recorded settings:

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

## Latest Observed State

Latest observed state from June 11, 2026 app-store prep:

```text
Health: /health HTTP 200
Privacy policy: /privacy HTTP 200
BI admin auth: /filtertrack/bi/auth HTTP 200 with role=admin
Machine: 2863064c759e38
```

Earlier observed deployment in `info/`:

```text
Image: filtertrack-api:deployment-01KQZEK46NVRSX5K4456Q9EK23
Machine: 2863064c759e38
Health: /health HTTP 200
BI: /bi HTTP 200
```

`context.txt` contains an older image value. See [sources.md](sources.md).

## Deploy

Run from `backend-fastapi/`:

```powershell
fly deploy --app filtertrack-api
```

Useful checks:

```powershell
fly status --app filtertrack-api
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/health" -UseBasicParsing
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/bi" -UseBasicParsing
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/privacy" -UseBasicParsing
```

Expected:

- `/health` returns HTTP 200 and healthy JSON.
- `/bi` returns HTTP 200 and BI HTML.
- `/privacy` returns HTTP 200 and the public privacy policy HTML.

## Local Backend

From `backend-fastapi/`:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8080 --reload
```

Health:

```powershell
Invoke-WebRequest -Uri "http://127.0.0.1:8080/health" -UseBasicParsing
```

## Install On Device

`adb` is **not** in the system PATH. Use the full SDK path:

```powershell
$adb = "$env:USERPROFILE\AppData\Local\Android\Sdk\platform-tools\adb.exe"
```

Check connected devices:

```powershell
& $adb devices
```

Build and install debug APK in one flow:

```powershell
.\gradlew.bat :app:assembleDebug
& $adb install -r "app\build\outputs\apk\debug\app-debug.apk"
```

## Validation

Android:

```powershell
.\gradlew.bat :app:assembleDebug
```

Backend syntax:

```powershell
cd backend-fastapi
python -m compileall app
```

## Secrets

Expected production secrets:

- `FILTERTRACK_USER_KEY`
- `FILTERTRACK_ADMIN_KEY`
- Optional `FILTERTRACK_API_KEY`

Never write secret values into docs, prompts, or wiki pages.

On June 11, 2026, `FILTERTRACK_ADMIN_KEY` was rotated on Fly because the previous
value was not recoverable from the wiki or Fly secrets. The new value is stored
locally in ignored `release.properties` as `FILTERTRACK_BI_ADMIN_KEY` and is
embedded into Android release builds. Fly can show secret digests with
`fly secrets list --app filtertrack-api`, but it cannot reveal secret values.

Validate the local admin key against production without printing it:

```powershell
$props = @{}
Get-Content release.properties | ForEach-Object {
    if ($_ -match '^([^#][^=]+)=(.*)$') { $props[$matches[1].Trim()] = $matches[2].Trim() }
}
$response = Invoke-WebRequest -Uri 'https://filtertrack-api.fly.dev/filtertrack/bi/auth' `
    -Headers @{ 'X-Access-Key' = $props['FILTERTRACK_BI_ADMIN_KEY'] } `
    -UseBasicParsing
($response.Content | ConvertFrom-Json).role
```

Expected result:

```text
admin
```

## Backups

The Fly volume is not replicated and app-level backups are not automatic. If data
matters, back up `/data/filtertrack.db`.

Previously recorded backups:

- `backups\fly-volume-filtertrack-api-20260426-224418Z\`
- `backups\fly-volume-filtertrack-api-20260426-224540Z-raw\`
- `backups\fly-volume-filtertrack-api-20260426-224540Z-raw.zip`

## Recreate Backend If Destroyed

```powershell
flyctl apps create filtertrack-api --org personal
flyctl volumes create filtertrack_data --region gru --size 1 -a filtertrack-api
flyctl secrets unset DATABASE_URL -a filtertrack-api
flyctl secrets set FILTERTRACK_USER_KEY=... FILTERTRACK_ADMIN_KEY=... -a filtertrack-api
flyctl deploy -a filtertrack-api
```

Keep `DATABASE_URL` unset unless it is intentionally a SQLite URL.
