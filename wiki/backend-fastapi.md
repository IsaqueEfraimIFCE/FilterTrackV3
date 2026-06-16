# FastAPI Backend

The backend lives in `backend-fastapi/` and is the production API, database
owner, BI API provider, and static BI host.

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

## Startup

`app.main` creates the FastAPI app, installs CORS, includes the BI router,
creates tables, and runs lightweight SQLite migrations.

Current startup migrations ensure:

- `session_records.filter_station`
- `approved_filters.source`
- `approved_filters.archived`
- `approved_filters.updated_at`

## Configuration

`config.py` reads:

- `HOST`, default `0.0.0.0`
- `PORT`, default `8080`
- `DATABASE_PATH`, default local `./filtertrack.db`, Fly `/data/filtertrack.db`
- `DATABASE_URL`, allowed only for SQLite URLs
- `MAX_SAMPLES_PER_SESSION`, default `120000`
- `CORS_ALLOW_ORIGINS`, default `*`
- `FILTERTRACK_API_KEY`, optional public API key
- `FILTERTRACK_USER_KEY`, BI user key
- `FILTERTRACK_ADMIN_KEY`, BI admin key

Production should keep `DATABASE_URL` unset unless it is a SQLite URL. Managed
Postgres was intentionally removed from the active design.

## SQLite

SQLite settings:

- `foreign_keys=ON`
- `busy_timeout=5000`
- WAL when not in-memory
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

`SessionRecord` stores metadata, canonical payload JSON, and operational columns
for listing and analytics.

`ChangeProposal` stores BI proposal queue records.

`ApprovedFilter` stores approved custom filters and overrides, including archive
status and source metadata.

## Public API Routes

Routes in `main.py`:

- `GET /health`
- `GET /privacy`
- `POST /filtertrack/sessions`
- `GET /filtertrack/filters`
- `GET /filtertrack/sessions`
- `GET /filtertrack/sessions/{session_ref}`
- `GET /filtertrack/stats`

`GET /privacy` serves `backend-fastapi/app/static/privacy.html` for the Google
Play privacy policy URL:

```text
https://filtertrack-api.fly.dev/privacy
```

If `FILTERTRACK_API_KEY` is set, `/filtertrack/*` API routes require:

```text
X-API-Key: <key>
```

The Android app currently posts without an API key unless changed.

## BI Routes

Routes in `bi.py` include:

- `GET /` and `GET /bi`
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
