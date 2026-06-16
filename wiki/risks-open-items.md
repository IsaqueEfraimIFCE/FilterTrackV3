# Risks And Open Items

## Operational Risks

- Production uses a single SQLite database on a single Fly volume. The volume is
  not replicated and is not suitable for multi-writer scale-out.
- Backups are manual. Data loss risk exists if `/data/filtertrack.db` is not
  backed up.
- `min_machines_running=0` lowers cost but can cause cold starts.
- Enabling `FILTERTRACK_API_KEY` requires Android sync changes to send
  `X-API-Key`.
- Real BI/admin/user keys must not be copied into docs.
- The Android release build intentionally embeds the BI admin key for internal
  possession validation. This is extractable from the APK/AAB and must not be
  treated as strong server-side security.

## Source And Workflow Risks

- `app/src/main/assets/index.html` is currently edited directly. If the old
  desktop template workflow is restored, current asset changes must be ported
  back into the template before rebuilding.
- `context.txt` is useful but older than `info/`; check code and `info/` before
  trusting it.
- Local worktree changes existed when this wiki was created. Always inspect
  `git status --short` before editing.
- `release.properties` and `keystore.properties` are intentionally ignored local
  secret files. Do not delete them without first backing up the BI admin key and
  Play upload keystore credentials.

## Known Source Conflict

The latest Fly image differs between raw sources:

- `context.txt`: `filtertrack-api:deployment-01KQYV989M1G11MPHF1NPS82AR`
- `info/`: `filtertrack-api:deployment-01KQZEK46NVRSX5K4456Q9EK23`

Treat the `info/` value as newer but verify live state before deploy or incident
work.

## Open Items

- Three old pending custom-filter proposals named `Teste2` were recorded in the
  old queue; admin/user still needs to decide.
- If the Android filter catalog grows, update
  `backend-fastapi/app/data/default_filters.json` and redeploy. No DB migration
  should be needed.
- Device connection status from prior notes: phone was last visible during app
  install, then not detected by `adb`; USB reconnection may be needed.
- Firmware source is now in `firmware/`. BLE protocol changes should verify
  `firmware/main/FilterTrackv3.c` before updating Android assumptions.
