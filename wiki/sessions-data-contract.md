# Sessions And Data Contract

This page describes the contract between the Android app and FastAPI backend.

## Android Upload

The Android app posts to:

```text
POST /filtertrack/sessions
```

Default production endpoint:

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

The backend accepts legacy object-list samples and compact samples.

## Compact Sample Format

Canonical sample storage:

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

- `t0` is the first sample timestamp in Unix milliseconds.
- `t` stores offsets from `t0` in milliseconds.
- `d` stores distance in centimeters multiplied by `scale`.
- With `scale = 100`, `d = 2428` means `24.28 cm`.

## Canonical Storage

The backend stores source facts and strips values that can be recomputed:

- `flow10sLpm`
- `flow60sLpm`
- `flowSessionLpm`
- `direction`
- `isoTime` when `ts` is present
- `sampleCount`

The DB column `sample_count` is computed from stored samples, not trusted from
the upload payload.

Preserved filter metadata:

- `id`
- `name`
- `areaM2`
- `station`
- `location`
- `businessUnit`
- `custom`
- `createdAt` when present

## Deduplication

The backend generates a normalized `dedup_key`. If a duplicate session is
uploaded, the backend returns the existing ingestion id instead of inserting a
new record.

## Flow Analytics

Android computes live flow in memory. Stored sessions keep distance source data.
BI computes flow at query time from compact samples and filter area.

Sign convention is shared with [sensor-processing-flow.md](sensor-processing-flow.md):
distance down means water up, which means positive `Subindo` flow.

## Filter Catalog

The BI registered filter catalog is a union of:

1. `backend-fastapi/app/data/default_filters.json`
2. Approved filters from `approved_filters`
3. Filter ids found in `session_records` and missing from the first two sources

Approved rows can override default catalog filters by id. Archived filters are
hidden when `includeArchived=false`.

## Proposals

Common proposal envelope:

```json
{
  "type": "session_edit",
  "targetSessionRef": "session-id-or-ingestion-id",
  "submittedBy": "operator",
  "payload": {}
}
```

Session edits may apply to an entire session or to a time slice. A time-slice
approval splits selected samples into a new session record with the chosen filter
and removes those samples from the original record.

Custom filter proposals create a filter named:

```text
Filtro <number>
```

