# BI Dashboard Context

## Primary Files

- Source: `backend-fastapi/app/static/bi.html`
- Router/API support: `backend-fastapi/app/bi.py`
- Android WebView launcher: `app/src/main/java/com/example/filtertrack/BiDashboardActivity.kt`

## URL

Production:

```text
https://filtertrack-api.fly.dev/bi
```

The BI page is a single static HTML file with React loaded from CDN. It calls backend routes with `fetch`.

## Authentication And Roles

The BI dashboard stores the access key in:

```text
localStorage("filtertrack.bi.key")
```

Requests use:

```text
X-Access-Key: <key>
```

Roles:

- `user`: view dashboard, filter data, download exports, submit proposals.
- `admin`: user abilities plus approve/reject proposals.

The Android `BiDashboardActivity` auto-injects a user key and attempts login. Do not expose the key in docs.

## Main Views

The BI navigation includes:

- Overview / Visão geral
- Filters / Filtros
- Sessions / Sessões
- Proposals / Propostas
- Settings / Ajustes

## Overview

Shows operational KPIs, trends, flow analytics, station and filter summaries, and recent sessions.

Backend endpoint:

```text
GET /filtertrack/bi/overview
```

Supported filters include date range, device address, and filter id.

## Filters View

Shows registered filters from the union catalog:

1. Default catalog from `default_filters.json`.
2. Approved custom/session rows from `approved_filters`, overriding defaults by id.
3. Filter ids found in session records but missing from both previous sources.

Current UI behavior:

- Search by filter name, station, or business unit.
- Optionally include archived filters.
- Mobile renders filters as cards.
- Desktop renders a table.
- Area is formatted as `m²`.
- `Editar` and `Arquivar`/`Reativar` actions are aligned to the right.
- Editing a filter opens an edit proposal panel and scrolls to the panel.

Filter edit/archive proposals:

- `filter_edit` can update name, area, station, location, business unit.
- `filter_archive` toggles archived state.
- Approval creates or updates an `ApprovedFilter` row, including overrides for default catalog filters.

## Sessions View

Shows recent sessions from the overview payload and allows correction proposals.

The Sessions page also has a selected-session visualization section. Click `Visualizar` on a session to load `/filtertrack/bi/sessions/{session_ref}` and render:

- Velocity chart computed from 10-second windows.
- Average flow and total signed volume.
- Counts of upward and downward velocity windows.
- Closest and farthest sensor distance.
- Sample and velocity-point counts.

Current edit flow:

- Click `Editar` on a session.
- The page scrolls to the proposal edit panel.
- Choose a registered target filter.
- Apply the target filter to either the whole session or a time slice.
- Time slice requires valid start/end local datetime values.

Approval behavior:

- Whole session edit updates the stored session/device/filter facts.
- Time-slice edit splits selected samples into a new session record with the chosen filter and removes those samples from the original session.

## Proposals View

Shows proposal queue. Admins can approve/reject pending proposals.

Proposal types:

- `session_edit`
- `custom_filter`
- `filter_edit`
- `filter_archive`

## Settings View

Includes downloads and new filter proposal form.

Current new filter proposal form asks for:

- Filter number
- Area in m²
- Station
- Business unit
- Submitted by

It generates the submitted filter name as:

```text
Filtro <number>
```

It no longer asks for filter name or location in the UI.

## Exports

Export routes:

- `GET /filtertrack/bi/export/sessions.csv`
- `GET /filtertrack/bi/export/samples.csv`
- `GET /filtertrack/bi/export/sessions.json`

Sessions CSV includes flow fields such as availability, L/min, direction, distance start/end, and distance change.

On Android WebView, `downloadBiFile()` uses `window.FilterTrackAndroid.downloadBiFile()` when available. The native activity validates that the URL is a FilterTrack BI export URL, adds BI access credentials through the native request, and saves the file through Android `DownloadManager`. Browser users keep the normal anchor-download fallback.

## Responsive Design Notes

Recent mobile fixes:

- Mobile tables converted to cards for recent sessions, filters, and sessions.
- KPI cards use one mobile column to avoid clipped numbers.
- Charts fit phone viewport.
- Filter controls use full mobile row width.
- Remaining desktop tables wrap cells on mobile when needed.
- Page titles wrap instead of clipping.
