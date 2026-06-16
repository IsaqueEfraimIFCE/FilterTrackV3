# BI Dashboard

The BI dashboard is a static React-in-HTML page served by FastAPI.

## Primary Files

- `backend-fastapi/app/static/bi.html`
- `backend-fastapi/app/bi.py`
- `app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt`

Production URL:

```text
https://filtertrack-api.fly.dev/bi
```

## Authentication

The BI page stores its access key in:

```text
localStorage("filtertrack.bi.key")
```

Requests use:

```text
X-Access-Key: <key>
```

Roles:

- `user` - view dashboard, filter data, download exports, submit proposals.
- `admin` - user abilities plus approve/reject proposals.

Android auto-injects a user key through `BiDashboardActivity`. Do not expose that
key in docs.

## Views

Navigation includes:

- Overview / Visao geral
- Filters / Filtros
- Sessions / Sessoes
- Proposals / Propostas
- Settings / Ajustes

## Overview

Uses:

```text
GET /filtertrack/bi/overview
```

It shows KPIs, trend charts, flow analytics, station/filter summaries, and recent
sessions. Filters include date range, device address, and filter id.

## Filters

Registered filters come from the union catalog:

1. Default catalog from `default_filters.json`
2. Approved custom/session rows from `approved_filters`
3. Session-ingested filter ids missing from the first two sources

Behavior:

- Search by filter name, station, or business unit.
- Optional archived-filter inclusion.
- Mobile card layout and desktop table layout.
- Edit/archive actions create proposals.
- Approval creates or updates an `ApprovedFilter`, including overrides for
  default catalog filters.

## Sessions

The sessions view supports correction proposals and selected-session
visualization.

Clicking `Visualizar` loads:

```text
GET /filtertrack/bi/sessions/{session_ref}
```

It renders:

- Velocity chart from 10-second windows.
- Average flow and total signed volume.
- Up/down velocity-window counts.
- Closest and farthest sensor distance.
- Sample and velocity-point counts.

Session edits can apply to a whole session or a selected time slice. Time-slice
approval splits samples into a new session and removes them from the original.

## Proposals

Proposal types:

- `session_edit`
- `custom_filter`
- `filter_edit`
- `filter_archive`

Admins approve or reject pending proposals.

## Settings And New Filters

The new filter proposal form asks for:

- Filter number
- Area in `m2`
- Station
- Business unit
- Submitter

The submitted filter name is generated as:

```text
Filtro <number>
```

## Exports

Export routes:

- `GET /filtertrack/bi/export/sessions.csv`
- `GET /filtertrack/bi/export/samples.csv`
- `GET /filtertrack/bi/export/sessions.json`

Android export downloads use `window.FilterTrackAndroid.downloadBiFile()` when
available. The native bridge accepts only production FilterTrack BI export URLs
and saves files through Android `DownloadManager`.

## Responsive Notes

Recent mobile fixes include:

- Mobile cards for recent sessions, registered filters, and sessions.
- Single-column KPI cards on mobile.
- Charts constrained to phone viewport width.
- Full-row mobile filter controls.
- Wrapping page titles and table cells where tables remain.

