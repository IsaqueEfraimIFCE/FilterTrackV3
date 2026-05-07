# FastAPI Backend Context

## Primary Files

- `backend-fastapi/app/main.py`
- `backend-fastapi/app/bi.py`
- `backend-fastapi/app/config.py`
- `backend-fastapi/app/db.py`
- `backend-fastapi/app/models.py`
- `backend-fastapi/app/utils.py`
- `backend-fastapi/app/schemas.py`
- `backend-fastapi/app/static/bi.html`
- `backend-fastapi/app/data/default_filters.json`
- `backend-fastapi/fly.toml`
- `backend-fastapi/Dockerfile`

## Stack

- FastAPI
- SQLAlchemy 2.x
- SQLite
- Uvicorn
- Docker image based on `python:3.12-slim`

Requirements:

```text
fastapi
uvicorn[standard]
SQLAlchemy
```

## App Startup

`app.main` creates the FastAPI app, installs CORS, includes the BI router, creates database tables, and runs lightweight ALTER TABLE migrations for older SQLite schemas.

Startup migrations currently ensure:

- `session_records.filter_station`
- `approved_filters.source`
- `approved_filters.archived`
- `approved_filters.updated_at`

## Configuration

`config.py` reads:

- `HOST`, default `0.0.0.0`
- `PORT`, default `8080`
- `DATABASE_PATH`, default local `./filtertrack.db`, Fly `/data/filtertrack.db`
- `DATABASE_URL`, allowed only when it is SQLite
- `MAX_SAMPLES_PER_SESSION`, default `120000`
- `CORS_ALLOW_ORIGINS`, default `*`
- `FILTERTRACK_API_KEY`, optional for ingestion/list/stats endpoints
- `FILTERTRACK_USER_KEY`, BI user key
- `FILTERTRACK_ADMIN_KEY`, BI admin key

Important: Managed Postgres was intentionally removed. Keep `DATABASE_URL` unset in production unless it is a SQLite URL.

## Database

SQLite is configured with:

- `foreign_keys=ON`
- `busy_timeout=5000`
- WAL mode when not in-memory
- `synchronous=NORMAL`
- `check_same_thread=False`

Production DB path:

```text
/data/filtertrack.db
```

Local default DB path:

```text
backend-fastapi/filtertrack.db
```

## Models

`SessionRecord`

- Primary key: `ingestion_id`
- Dedup key: `dedup_key`, unique
- Session metadata: `session_id`, `started_at`, `ended_at`, `end_reason`, `sample_count`
- Device metadata: `device_name`, `device_address`
- Filter metadata: `filter_id`, `filter_name`, `filter_area_m2`, `filter_station`, `filter_location`, `filter_business_unit`
- Full canonical payload: `payload` JSON

`ChangeProposal`

- Proposal queue for BI data changes.
- Tracks proposal type, status, submitter, target session/filter ref, payload, decision metadata.

`ApprovedFilter`

- Approved custom filters and overrides for default/session-ingested filters.
- Includes archive status and source metadata.

## Public API Routes

Routes in `main.py`:

- `GET /health`
- `POST /filtertrack/sessions`
- `GET /filtertrack/filters`
- `GET /filtertrack/sessions`
- `GET /filtertrack/sessions/{session_ref}`
- `GET /filtertrack/stats`

If `FILTERTRACK_API_KEY` is set, `/filtertrack/*` API routes require:

```text
X-API-Key: <key>
```

The Android app currently posts to `/filtertrack/sessions` without an API key unless the app is changed.

## BI Routes

Routes in `bi.py`:

- `GET /` and `GET /bi` serve the static BI dashboard.
- `GET /filtertrack/bi/auth`
- `GET /filtertrack/bi/overview`
- `GET /filtertrack/bi/sessions`
- `GET /filtertrack/bi/sessions/{session_ref}`
- `GET /filtertrack/bi/export/sessions.csv`
- `GET /filtertrack/bi/export/samples.csv`
- `GET /filtertrack/bi/export/sessions.json`
- `GET /filtertrack/bi/proposals`
- `POST /filtertrack/bi/proposals`
- `POST /filtertrack/bi/proposals/{proposal_id}/approve`
- `POST /filtertrack/bi/proposals/{proposal_id}/reject`
- `GET /filtertrack/bi/filters`
- `GET /filtertrack/bi/custom-filters`

BI auth uses:

```text
X-Access-Key: <FILTERTRACK_USER_KEY or FILTERTRACK_ADMIN_KEY>
```

Local BI may allow access without keys if no Fly app name and no BI keys are set.

## Production Backend

Fly app:

```text
filtertrack-api
```

Public URL:

```text
https://filtertrack-api.fly.dev/
```

BI:

```text
https://filtertrack-api.fly.dev/bi
```
