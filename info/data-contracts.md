# Data Contracts

## Android Session Upload

The Android app posts to:

```text
POST /filtertrack/sessions
```

Default production endpoint in the app:

```text
https://filtertrack-api.fly.dev/filtertrack/sessions
```

Payload shape:

```json
{
  "schemaVersion": 1,
  "app": "FilterTrack",
  "uploadedAt": "2026-05-06T20:00:00.000Z",
  "session": {
    "id": "session-...",
    "startedAt": "2026-05-06T19:50:00.000Z",
    "endedAt": "2026-05-06T20:00:00.000Z",
    "endReason": "manual_stop",
    "device": {
      "name": "FilterTrackV3",
      "address": "AA:BB:CC:DD:EE:FF"
    },
    "filter": {
      "id": "filter-id",
      "name": "FIL-01 Asc",
      "areaM2": 7.07,
      "station": "ETA ...",
      "location": "...",
      "businessUnit": "UN-..."
    },
    "samples": {}
  }
}
```

The backend accepts both legacy object-list samples and compact samples.

## Compact Sample Format

Canonical storage format:

```json
{
  "format": "distance_cm_x100_v1",
  "t0": 1770000000000,
  "dtUnit": "ms",
  "distanceUnit": "cm",
  "scale": 100,
  "t": [0, 100, 200],
  "d": [2428, 2429, 2430]
}
```

Meaning:

- `t0` is first sample timestamp in Unix milliseconds.
- `t` stores offsets in milliseconds from `t0`.
- `d` stores distance in centimeters multiplied by `scale`.
- With `scale = 100`, `d = 2428` means `24.28 cm`.

The backend computes `sample_count` from stored samples instead of trusting uploaded `sampleCount`.

## Fields Removed Before Storage

The backend stores canonical source data and strips values that can be recomputed:

- `flow10sLpm`
- `flow60sLpm`
- `flowSessionLpm`
- `direction`
- `isoTime` when `ts` is present
- `sampleCount`

Filter metadata preserved:

- `id`
- `name`
- `areaM2`
- `station`
- `location`
- `businessUnit`
- `custom`
- `createdAt` when present

## Deduplication

Backend deduplicates uploaded sessions by a generated `dedup_key`, based on normalized payload facts. If a duplicate is received, the backend returns the existing ingestion id instead of creating a new row.

## Flow Analytics

Flow is computed from distance change/slope and filter area.

Sign convention:

- Distance down means water level up.
- Water level up means positive velocity and positive flow.
- Positive flow is `Subindo`.
- Negative flow is `Descendo`.

Android and BI should match this convention.

Android local formula:

```text
flowLpm = areaM2 * velocityMPerMin * 1000
```

BI computes flow at query time from stored samples and filter area.

## Filter Catalog

The backend BI filter catalog is a union of:

1. `backend-fastapi/app/data/default_filters.json`
2. Approved filters from the `approved_filters` table
3. Filter ids found in `session_records` that are not otherwise registered

Approved filter rows can override default catalog filters with the same id.

Archived filters are hidden when `includeArchived=false`.

## Proposal Payloads

Common proposal envelope:

```json
{
  "type": "session_edit",
  "targetSessionRef": "session-id-or-ingestion-id",
  "submittedBy": "operator",
  "payload": {}
}
```

Session edit payload:

```json
{
  "filter": {
    "id": "filter-id",
    "name": "FIL-01 Asc",
    "areaM2": 7.07,
    "station": "ETA ...",
    "location": "...",
    "businessUnit": "UN-..."
  },
  "timeSlice": {
    "from": "2026-05-06T19:55:00.000Z",
    "to": "2026-05-06T19:57:00.000Z"
  }
}
```

If `timeSlice` is omitted, the edit applies to the whole session.

Custom filter payload:

```json
{
  "filter": {
    "number": "01",
    "name": "Filtro 01",
    "areaM2": 7.07,
    "station": "ETA ...",
    "location": "",
    "businessUnit": "UN-...",
    "custom": true
  }
}
```
