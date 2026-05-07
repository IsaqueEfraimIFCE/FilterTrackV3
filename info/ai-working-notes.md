# AI Working Notes

## General Rules For Future Agents

- Read only the needed `info/` files first, then inspect source files.
- Prefer existing patterns in the repo. The UI is mostly inline React styles in single HTML files.
- Use `rg` or `rg --files` for searches.
- The repo may have local uncommitted changes; do not revert them without explicit instruction.
- `backend-fastapi/sessions.txt` was intentionally deleted after being merged into context. Do not restore it unless asked.
- The legacy Node backend, root browser mock dashboard, root duplicate assets, template tests, and local smoke/user instruction files were removed. Do not recreate them unless asked.

## Files Most Often Edited

Android WebView UI:

```text
app/src/main/assets/index.html
```

Native Android bridge/BLE:

```text
app/src/main/java/com/example/filtertrack/MainActivity.kt
app/src/main/java/com/example/filtertrack/BLEManager.kt
app/src/main/java/com/example/filtertrack/WebAppInterface.kt
app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt
```

Backend and BI:

```text
backend-fastapi/app/main.py
backend-fastapi/app/bi.py
backend-fastapi/app/static/bi.html
backend-fastapi/app/models.py
backend-fastapi/app/utils.py
```

## Validation Checklist

For Android asset/UI changes:

```powershell
.\gradlew.bat :app:assembleDebug
```

For backend Python changes:

```powershell
cd backend-fastapi
python -m compileall app
```

For deploy:

```powershell
cd backend-fastapi
fly deploy
fly status --app filtertrack-api
```

For live checks:

```powershell
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/health" -UseBasicParsing
Invoke-WebRequest -Uri "https://filtertrack-api.fly.dev/bi" -UseBasicParsing
```

## Common Pitfalls

- `index.html` in the Android asset is not currently regenerated from a template in this repo. Edit it directly unless the template workflow is restored.
- BI dashboard source is `backend-fastapi/app/static/bi.html`, not a separate frontend project.
- The Android app's cloud endpoint is hardcoded as `CLOUD_SERVER_URL` but can also be changed in Settings and persisted in localStorage.
- BI auth and app ingest auth are separate concepts: BI uses `X-Access-Key`; ingest/list/stats can use `X-API-Key` only if configured.
- Do not paste real access keys into docs or final answers.
- If changing filter area behavior, update both Android display and backend/BI formatting if needed.
- If changing flow sign convention, update Android, backend BI analytics, CSV export wording, and docs together.

## Recent User Requests Captured

- Improve filter list appearance: show area in m² and align Edit/Archive actions on the right.
- Session edit should scroll all the way to the edit panel.
- Debug should show flow and velocity without filters.
- Deploy backend to Fly after BI/backend changes.
- Create this `info/` folder for modular AI context.
- Clean unused leftovers from the project.
- Keep scan recovery behavior intact: the connect screen scans continuously until `Parar`, restart keeps the current filter on automatic reconnect, and unexpected Bluetooth/device loss opens the recovery modal.
- Bluetooth-off while connected must force a disconnected WebView state; multiple visible `FilterTrackV3` devices require manual selection; flow display toggles to `m3/h`; velocity display/chart update every 10 seconds and show a reason when unavailable.
- Metric loading now shows a 10-second collecting countdown/spinner on velocity and flow; unexpected reconnect keeps a session only when latest displayed velocity is above `0.1 m/min`; Android hides the MAC address for `FilterTrackV3`; BI Sessions has a selected-session visualization with velocity chart, volume, up/down counts, and distance extremes.
- Velocidade de subida and vazão charts should accumulate all data from 0 to current point (not limited to recent history). When flow inverts (distance comparison between consecutive 10-second windows changes direction), reset the chart and wait 10 seconds before accumulating again (2026-05-07).
