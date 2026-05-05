import hashlib
import json
import math
from datetime import UTC, datetime
from typing import Any
from uuid import uuid4


DERIVED_SAMPLE_KEYS = {"flow10sLpm", "flow60sLpm", "flowSessionLpm", "direction"}
DERIVED_SESSION_KEYS = {"sampleCount"}
SAMPLE_FORMAT = "distance_cm_x100_v1"
SAMPLE_DISTANCE_SCALE = 100


def now_utc() -> datetime:
    return datetime.now(UTC)


def to_iso_z(value: datetime | None) -> str | None:
    if value is None:
        return None
    if value.tzinfo is None:
        value = value.replace(tzinfo=UTC)
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def parse_iso_or_none(value: str | None) -> datetime | None:
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    if text.endswith("Z"):
        text = f"{text[:-1]}+00:00"
    try:
        dt = datetime.fromisoformat(text)
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=UTC)
    return dt.astimezone(UTC)


def safe_text(value: object, max_len: int = 256) -> str:
    if value is None:
        return ""
    text = str(value).strip()
    if len(text) <= max_len:
        return text
    return text[:max_len]


def _safe_number(value: object) -> float | int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value):
        return value
    if isinstance(value, str):
        try:
            parsed = float(value.replace(",", "."))
        except ValueError:
            return None
        if math.isfinite(parsed):
            return parsed
    return None


def _compact_samples() -> dict[str, Any]:
    return {
        "format": SAMPLE_FORMAT,
        "t0": None,
        "dtUnit": "ms",
        "distanceUnit": "cm",
        "scale": SAMPLE_DISTANCE_SCALE,
        "t": [],
        "d": [],
    }


def _sample_timestamp_ms(sample: dict[str, Any]) -> int | None:
    ts = _safe_number(sample.get("ts"))
    if ts is not None:
        return int(round(float(ts)))

    iso_time = parse_iso_or_none(sample.get("isoTime"))
    if iso_time is not None:
        return int(round(iso_time.timestamp() * 1000))

    legacy_time = parse_iso_or_none(sample.get("t"))
    if legacy_time is not None:
        return int(round(legacy_time.timestamp() * 1000))

    return None


def _append_compact_sample(series: dict[str, Any], ts_ms: int, distance_cm: float) -> None:
    if series["t0"] is None:
        series["t0"] = ts_ms
    offset_ms = max(0, ts_ms - int(series["t0"]))
    series["t"].append(offset_ms)
    series["d"].append(int(round(distance_cm * SAMPLE_DISTANCE_SCALE)))


def _normalize_legacy_sample(sample: Any) -> tuple[int, float] | None:
    if not isinstance(sample, dict):
        return None

    ts_ms = _sample_timestamp_ms(sample)
    distance_cm = _safe_number(sample.get("distanceCm"))
    if ts_ms is None or distance_cm is None:
        return None
    return ts_ms, float(distance_cm)


def _normalize_compact_samples(samples: dict[str, Any]) -> dict[str, Any]:
    series = _compact_samples()
    t0 = _safe_number(samples.get("t0"))
    raw_t = samples.get("t")
    raw_d = samples.get("d")
    if t0 is None or not isinstance(raw_t, list) or not isinstance(raw_d, list):
        return series

    series["t0"] = int(round(float(t0)))
    for raw_offset, raw_distance in zip(raw_t, raw_d, strict=False):
        offset = _safe_number(raw_offset)
        distance = _safe_number(raw_distance)
        if offset is None or distance is None:
            continue
        series["t"].append(max(0, int(round(float(offset)))))
        series["d"].append(int(round(float(distance))))
    return series


def _normalize_samples(samples: Any) -> dict[str, Any]:
    if isinstance(samples, dict) and samples.get("format") == SAMPLE_FORMAT:
        return _normalize_compact_samples(samples)

    series = _compact_samples()
    if not isinstance(samples, list):
        return series

    for sample in samples:
        item = _normalize_legacy_sample(sample)
        if item is not None:
            ts_ms, distance_cm = item
            _append_compact_sample(series, ts_ms, distance_cm)
    return series


def count_samples(samples: Any) -> int:
    if isinstance(samples, dict):
        offsets = samples.get("t")
        distances = samples.get("d")
        if isinstance(offsets, list) and isinstance(distances, list):
            return min(len(offsets), len(distances))
    if isinstance(samples, list):
        return len(samples)
    return 0


def normalize_payload(raw: dict, max_samples_per_session: int) -> dict:
    if not isinstance(raw, dict):
        raise ValueError("Payload deve ser objeto JSON.")

    session = raw.get("session")
    if not isinstance(session, dict):
        raise ValueError("Campo 'session' e obrigatorio.")

    samples = session.get("samples")
    if not isinstance(samples, (dict, list)):
        samples = []

    normalized_samples = _normalize_samples(samples)

    if count_samples(normalized_samples) > max_samples_per_session:
        raise OverflowError(f"Sessao excede limite de {max_samples_per_session} amostras.")

    started_at = parse_iso_or_none(session.get("startedAt"))
    ended_at = parse_iso_or_none(session.get("endedAt"))
    uploaded_at = parse_iso_or_none(raw.get("uploadedAt")) or now_utc()

    session_id = safe_text(session.get("id"), 200) or str(uuid4())
    device = session.get("device") if isinstance(session.get("device"), dict) else {}
    raw_filter = session.get("filter") if isinstance(session.get("filter"), dict) else None

    parsed_filter = None
    if raw_filter:
        area_raw = raw_filter.get("areaM2")
        try:
            area_value = float(area_raw) if area_raw is not None else None
        except (TypeError, ValueError):
            area_value = None
        parsed_filter = {
            "id": safe_text(raw_filter.get("id"), 160),
            "name": safe_text(raw_filter.get("name"), 200),
            "areaM2": area_value,
            "station": safe_text(raw_filter.get("station"), 200),
            "location": safe_text(raw_filter.get("location"), 200),
            "businessUnit": safe_text(raw_filter.get("businessUnit"), 200),
        }
        if "custom" in raw_filter:
            parsed_filter["custom"] = bool(raw_filter.get("custom"))
        created_at = parse_iso_or_none(raw_filter.get("createdAt"))
        if created_at is not None:
            parsed_filter["createdAt"] = to_iso_z(created_at)

    normalized_session = {
        key: value
        for key, value in session.items()
        if key not in DERIVED_SESSION_KEYS
    }
    normalized_session.update(
        {
            "id": session_id,
            "startedAt": to_iso_z(started_at),
            "endedAt": to_iso_z(ended_at),
            "endReason": safe_text(session.get("endReason"), 200),
            "device": {
                "name": safe_text(device.get("name"), 160),
                "address": safe_text(device.get("address"), 160),
            },
            "filter": parsed_filter,
            "samples": normalized_samples,
        }
    )

    normalized = {
        "schemaVersion": int(raw.get("schemaVersion") or 1),
        "app": safe_text(raw.get("app"), 80) or "FilterTrack",
        "uploadedAt": to_iso_z(uploaded_at),
        "session": normalized_session,
    }
    return normalized


def build_dedup_key(normalized_payload: dict) -> str:
    app_name = safe_text(normalized_payload.get("app"), 80) or "FilterTrack"
    session = normalized_payload.get("session", {}) if isinstance(normalized_payload, dict) else {}
    session_id = safe_text(session.get("id"), 200)
    if session_id:
        return f"{app_name}:{session_id}"

    fingerprint_data = {
        "app": app_name,
        "startedAt": session.get("startedAt"),
        "endedAt": session.get("endedAt"),
        "deviceAddress": (session.get("device") or {}).get("address"),
        "sampleCount": count_samples(session.get("samples")),
    }
    digest = hashlib.sha256(
        json.dumps(fingerprint_data, ensure_ascii=True, sort_keys=True).encode("utf-8")
    ).hexdigest()
    return f"{app_name}:hash:{digest}"
