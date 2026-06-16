import csv
import io
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from uuid import uuid4

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Response, status
from fastapi.responses import HTMLResponse, RedirectResponse
from sqlalchemy import and_, func, or_, select
from sqlalchemy.orm import Session
from sqlalchemy.orm.attributes import flag_modified

from .config import settings
from .db import get_db
from .models import ApprovedFilter, ChangeProposal, SessionRecord
from .utils import count_samples, now_utc, parse_iso_or_none, safe_text, to_iso_z


router = APIRouter()
BI_HTML_PATH = Path(__file__).with_name("static") / "bi.html"
DEFAULT_FILTERS_PATH = Path(__file__).with_name("data") / "default_filters.json"
FLOW_EPSILON_LPM = 0.05
SAMPLE_FORMAT = "distance_cm_x100_v1"

_DEFAULT_FILTERS_CACHE: list[dict[str, Any]] | None = None


def _load_default_filters() -> list[dict[str, Any]]:
    global _DEFAULT_FILTERS_CACHE
    if _DEFAULT_FILTERS_CACHE is None:
        try:
            with DEFAULT_FILTERS_PATH.open("r", encoding="utf-8") as fh:
                raw = json.load(fh)
        except FileNotFoundError:
            raw = []
        cleaned: list[dict[str, Any]] = []
        for item in raw if isinstance(raw, list) else []:
            if not isinstance(item, dict):
                continue
            fid = safe_text(item.get("id"), 160)
            name = safe_text(item.get("name"), 200)
            if not fid or not name:
                continue
            try:
                area = float(item.get("areaM2")) if item.get("areaM2") is not None else None
            except (TypeError, ValueError):
                area = None
            cleaned.append(
                {
                    "id": fid,
                    "name": name,
                    "areaM2": area,
                    "station": safe_text(item.get("station"), 200),
                    "location": safe_text(item.get("location"), 200),
                    "businessUnit": safe_text(item.get("businessUnit"), 200),
                }
            )
        _DEFAULT_FILTERS_CACHE = cleaned
    return _DEFAULT_FILTERS_CACHE


@dataclass(frozen=True)
class AccessContext:
    role: str
    label: str


def require_bi_access(
    x_access_key: str | None = Header(default=None, alias="X-Access-Key"),
    access_key: str | None = Query(default=None, alias="accessKey"),
) -> AccessContext:
    key = (x_access_key or access_key or "").strip()

    if settings.admin_access_key and key == settings.admin_access_key:
        return AccessContext(role="admin", label="admin")
    if settings.user_access_key and key == settings.user_access_key:
        return AccessContext(role="user", label="user")
    if settings.allow_local_bi_without_keys:
        return AccessContext(role="admin" if key.lower() == "admin" else "user", label="local")

    raise HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail="Chave de acesso invalida.",
    )


def require_admin(access: AccessContext = Depends(require_bi_access)) -> AccessContext:
    if access.role != "admin":
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Acesso de administrador necessario.",
        )
    return access


def _query_clauses(
    from_date: datetime | None,
    to_date: datetime | None,
    device_address: str | None,
    filter_id: str | None,
    session_ref: str | None = None,
) -> list[Any]:
    clauses = []
    point = func.coalesce(SessionRecord.started_at, SessionRecord.ended_at, SessionRecord.received_at)
    if from_date is not None:
        clauses.append(point >= from_date)
    if to_date is not None:
        clauses.append(point <= to_date)
    if device_address:
        clauses.append(func.lower(SessionRecord.device_address) == device_address.lower().strip())
    if filter_id:
        clauses.append(func.lower(SessionRecord.filter_id) == filter_id.lower().strip())
    if session_ref:
        ref = session_ref.lower().strip()
        clauses.append(
            or_(
                func.lower(SessionRecord.session_id) == ref,
                func.lower(SessionRecord.ingestion_id) == ref,
            )
        )
    return clauses


def _records_for_filters(
    db: Session,
    from_date: datetime | None,
    to_date: datetime | None,
    device_address: str | None,
    filter_id: str | None,
    session_ref: str | None = None,
) -> list[SessionRecord]:
    stmt = select(SessionRecord).order_by(SessionRecord.received_at.desc())
    clauses = _query_clauses(from_date, to_date, device_address, filter_id, session_ref)
    if clauses:
        stmt = stmt.where(and_(*clauses))
    return list(db.execute(stmt).scalars().all())


def _record_point(record: SessionRecord) -> datetime | None:
    return record.started_at or record.ended_at or record.received_at


def _duration_seconds(record: SessionRecord) -> float | None:
    if not record.started_at or not record.ended_at:
        return None
    return max(0.0, (record.ended_at - record.started_at).total_seconds())


def _payload_session(record: SessionRecord) -> dict[str, Any]:
    payload = record.payload if isinstance(record.payload, dict) else {}
    session = payload.get("session") if isinstance(payload, dict) else {}
    return session if isinstance(session, dict) else {}


def _finite_float(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        parsed = float(value)
    elif isinstance(value, str):
        try:
            parsed = float(value.replace(",", "."))
        except ValueError:
            return None
    else:
        return None
    return parsed if math.isfinite(parsed) else None


def _sample_timestamp_ms(sample: dict[str, Any]) -> int | None:
    ts = _finite_float(sample.get("ts"))
    if ts is not None:
        return int(round(ts))

    iso_time = parse_iso_or_none(sample.get("isoTime"))
    if iso_time is not None:
        return int(round(iso_time.timestamp() * 1000))

    legacy_time = parse_iso_or_none(sample.get("t"))
    if legacy_time is not None:
        return int(round(legacy_time.timestamp() * 1000))

    return None


def _sample_endpoints(samples: Any) -> tuple[tuple[int, float], tuple[int, float]] | None:
    first: tuple[int, float] | None = None
    last: tuple[int, float] | None = None

    if isinstance(samples, dict):
        t0 = _finite_float(samples.get("t0"))
        offsets = samples.get("t")
        distances = samples.get("d")
        scale = _finite_float(samples.get("scale")) or 100.0
        if t0 is None or scale <= 0 or not isinstance(offsets, list) or not isinstance(distances, list):
            return None
        for raw_offset, raw_distance in zip(offsets, distances, strict=False):
            offset = _finite_float(raw_offset)
            distance = _finite_float(raw_distance)
            if offset is None or distance is None:
                continue
            point = (int(round(t0 + offset)), distance / scale)
            if first is None:
                first = point
            last = point

    elif isinstance(samples, list):
        for sample in samples:
            if not isinstance(sample, dict):
                continue
            ts_ms = _sample_timestamp_ms(sample)
            distance = _finite_float(sample.get("distanceCm"))
            if ts_ms is None or distance is None:
                continue
            point = (ts_ms, distance)
            if first is None:
                first = point
            last = point

    if first is None or last is None or first == last:
        return None
    return first, last


def _sample_points(samples: Any) -> tuple[list[tuple[int, int]], float]:
    points: list[tuple[int, int]] = []
    scale = 100.0

    if isinstance(samples, dict):
        t0 = _finite_float(samples.get("t0"))
        offsets = samples.get("t")
        distances = samples.get("d")
        scale = _finite_float(samples.get("scale")) or 100.0
        if t0 is None or scale <= 0 or not isinstance(offsets, list) or not isinstance(distances, list):
            return [], scale
        for raw_offset, raw_distance in zip(offsets, distances, strict=False):
            offset = _finite_float(raw_offset)
            distance = _finite_float(raw_distance)
            if offset is None or distance is None:
                continue
            points.append((int(round(t0 + offset)), int(round(distance))))

    elif isinstance(samples, list):
        for sample in samples:
            if not isinstance(sample, dict):
                continue
            ts_ms = _sample_timestamp_ms(sample)
            distance = _finite_float(sample.get("distanceCm"))
            if distance is None:
                distance = _finite_float(sample.get("distance"))
            if ts_ms is None or distance is None:
                continue
            points.append((ts_ms, int(round(distance * scale))))

    points.sort(key=lambda item: item[0])
    return points, scale


def _compact_from_points(points: list[tuple[int, int]], scale: float) -> dict[str, Any]:
    if not points:
        return {
            "format": SAMPLE_FORMAT,
            "t0": None,
            "dtUnit": "ms",
            "distanceUnit": "cm",
            "scale": int(scale) if float(scale).is_integer() else scale,
            "t": [],
            "d": [],
        }
    t0 = points[0][0]
    return {
        "format": SAMPLE_FORMAT,
        "t0": t0,
        "dtUnit": "ms",
        "distanceUnit": "cm",
        "scale": int(scale) if float(scale).is_integer() else scale,
        "t": [max(0, ts - t0) for ts, _ in points],
        "d": [distance for _, distance in points],
    }


def _point_bounds(points: list[tuple[int, int]]) -> tuple[datetime | None, datetime | None]:
    if not points:
        return None, None
    return (
        datetime.fromtimestamp(points[0][0] / 1000, UTC),
        datetime.fromtimestamp(points[-1][0] / 1000, UTC),
    )


def _clean_time_slice(raw: Any) -> dict[str, str] | None:
    if not isinstance(raw, dict):
        return None
    start = parse_iso_or_none(raw.get("from") or raw.get("start"))
    end = parse_iso_or_none(raw.get("to") or raw.get("end"))
    if start is None and end is None:
        return None
    if start is None or end is None or end <= start:
        raise HTTPException(status_code=400, detail="Recorte de tempo invalido.")
    return {"from": to_iso_z(start) or "", "to": to_iso_z(end) or ""}


def _clean_manual_measurement(raw: Any) -> dict[str, Any] | None:
    if not isinstance(raw, dict):
        return None
    cleaned: dict[str, Any] = {}
    for key in (
        "velocityMpm",
        "flowLpm",
        "sensorVelocityMpm",
        "sensorFlowLpm",
        "velocityErrorPercent",
        "flowErrorPercent",
        "confidencePercent",
    ):
        if key in raw:
            cleaned[key] = _finite_float(raw.get(key))
    scope = safe_text(raw.get("scope"), 40)
    if scope:
        cleaned["scope"] = scope
    if not any(value is not None for key, value in cleaned.items() if key != "scope"):
        return None
    return cleaned


def _flow_direction(flow_lpm: float | None) -> str:
    if flow_lpm is None:
        return "unknown"
    if flow_lpm > FLOW_EPSILON_LPM:
        return "positive"
    if flow_lpm < -FLOW_EPSILON_LPM:
        return "negative"
    return "stable"


def _record_flow_summary(record: SessionRecord) -> dict[str, Any]:
    session = _payload_session(record)
    endpoints = _sample_endpoints(session.get("samples"))
    area_m2 = _finite_float(record.filter_area_m2)

    if endpoints is None:
        return {
            "available": False,
            "reason": "samples",
            "direction": "unknown",
        }
    if area_m2 is None or area_m2 <= 0:
        return {
            "available": False,
            "reason": "area",
            "direction": "unknown",
        }

    first, last = endpoints
    dt_seconds = (last[0] - first[0]) / 1000
    if dt_seconds <= 0:
        return {
            "available": False,
            "reason": "duration",
            "direction": "unknown",
        }

    distance_change_cm = last[1] - first[1]
    level_change_m = -distance_change_cm / 100
    flow_lpm = area_m2 * level_change_m * 60000 / dt_seconds
    return {
        "available": True,
        "flowLpm": round(flow_lpm, 4),
        "absFlowLpm": round(abs(flow_lpm), 4),
        "direction": _flow_direction(flow_lpm),
        "distanceStartCm": round(first[1], 4),
        "distanceEndCm": round(last[1], 4),
        "distanceChangeCm": round(distance_change_cm, 4),
        "durationSeconds": round(dt_seconds, 3),
        "areaM2": area_m2,
    }


def _empty_flow_stats() -> dict[str, Any]:
    return {
        "flowSessions": 0,
        "flowTotal": 0.0,
        "absFlowTotal": 0.0,
        "distanceChangeTotal": 0.0,
        "positiveSessions": 0,
        "negativeSessions": 0,
        "stableSessions": 0,
        "unknownSessions": 0,
        "minFlowLpm": None,
        "maxFlowLpm": None,
    }


def _add_flow_stats(row: dict[str, Any], flow: dict[str, Any]) -> None:
    if not flow.get("available"):
        row["unknownSessions"] += 1
        return

    flow_lpm = float(flow["flowLpm"])
    row["flowSessions"] += 1
    row["flowTotal"] += flow_lpm
    row["absFlowTotal"] += float(flow["absFlowLpm"])
    row["distanceChangeTotal"] += float(flow["distanceChangeCm"])
    row["minFlowLpm"] = flow_lpm if row["minFlowLpm"] is None else min(row["minFlowLpm"], flow_lpm)
    row["maxFlowLpm"] = flow_lpm if row["maxFlowLpm"] is None else max(row["maxFlowLpm"], flow_lpm)

    direction = flow.get("direction")
    if direction == "positive":
        row["positiveSessions"] += 1
    elif direction == "negative":
        row["negativeSessions"] += 1
    elif direction == "stable":
        row["stableSessions"] += 1
    else:
        row["unknownSessions"] += 1


def _finalize_flow_stats(row: dict[str, Any]) -> dict[str, Any]:
    flow_sessions = int(row.get("flowSessions") or 0)
    result = dict(row)
    result["avgFlowLpm"] = round(float(row.get("flowTotal") or 0) / flow_sessions, 4) if flow_sessions else None
    result["avgAbsFlowLpm"] = round(float(row.get("absFlowTotal") or 0) / flow_sessions, 4) if flow_sessions else None
    result["avgDistanceChangeCm"] = (
        round(float(row.get("distanceChangeTotal") or 0) / flow_sessions, 4) if flow_sessions else None
    )
    for key in ("flowTotal", "absFlowTotal", "distanceChangeTotal"):
        result.pop(key, None)
    return result


def _record_summary(record: SessionRecord) -> dict[str, Any]:
    return {
        "ingestionId": record.ingestion_id,
        "sessionId": record.session_id,
        "receivedAt": to_iso_z(record.received_at),
        "uploadedAt": to_iso_z(record.uploaded_at),
        "startedAt": to_iso_z(record.started_at),
        "endedAt": to_iso_z(record.ended_at),
        "durationSeconds": _duration_seconds(record),
        "endReason": record.end_reason,
        "sampleCount": int(record.sample_count or 0),
        "device": {
            "name": record.device_name,
            "address": record.device_address,
        },
        "filter": {
            "id": record.filter_id,
            "name": record.filter_name,
            "areaM2": record.filter_area_m2,
            "station": record.filter_station,
            "location": record.filter_location,
            "businessUnit": record.filter_business_unit,
        },
        "flow": _record_flow_summary(record),
    }


def _date_key(value: datetime | None) -> str:
    if value is None:
        return "sem-data"
    if value.tzinfo is None:
        value = value.replace(tzinfo=UTC)
    return value.astimezone(UTC).date().isoformat()


def _series_count(samples: Any) -> int:
    return count_samples(samples)


def _iter_sample_rows(record: SessionRecord):
    session = _payload_session(record)
    samples = session.get("samples")

    if isinstance(samples, dict):
        t0 = samples.get("t0")
        offsets = samples.get("t")
        distances = samples.get("d")
        scale = samples.get("scale") or 100
        if not isinstance(t0, (int, float)) or not isinstance(offsets, list) or not isinstance(distances, list):
            return
        for idx, (offset, distance) in enumerate(zip(offsets, distances, strict=False)):
            if not isinstance(offset, (int, float)) or not isinstance(distance, (int, float)):
                continue
            ts_ms = int(round(float(t0) + float(offset)))
            timestamp = datetime.fromtimestamp(ts_ms / 1000, UTC)
            yield {
                "sessionId": record.session_id,
                "deviceAddress": record.device_address,
                "filterId": record.filter_id,
                "index": idx,
                "timestamp": to_iso_z(timestamp),
                "offsetMs": int(round(float(offset))),
                "distanceCm": float(distance) / float(scale),
            }
        return

    if isinstance(samples, list):
        for idx, sample in enumerate(samples):
            if not isinstance(sample, dict):
                continue
            yield {
                "sessionId": record.session_id,
                "deviceAddress": record.device_address,
                "filterId": record.filter_id,
                "index": idx,
                "timestamp": sample.get("isoTime") or "",
                "offsetMs": "",
                "distanceCm": sample.get("distanceCm"),
            }


def _clean_filter(raw: Any, generate_id: bool = False) -> dict[str, Any]:
    raw = raw if isinstance(raw, dict) else {}
    area_raw = raw.get("areaM2")
    try:
        area_m2 = float(area_raw) if area_raw not in (None, "") else None
    except (TypeError, ValueError):
        area_m2 = None

    filter_id = safe_text(raw.get("id"), 160)
    if not filter_id and generate_id:
        filter_id = f"custom-{uuid4().hex[:12]}"

    name = safe_text(raw.get("name"), 200)
    number = safe_text(raw.get("number"), 40)
    if not name and number:
        name = f"Filtro {number}"

    cleaned = {
        "id": filter_id,
        "name": name,
        "areaM2": area_m2,
        "station": safe_text(raw.get("station"), 200),
        "location": safe_text(raw.get("location"), 200),
        "businessUnit": safe_text(raw.get("businessUnit"), 200),
    }
    if number:
        cleaned["number"] = number
    if "custom" in raw or generate_id:
        cleaned["custom"] = bool(raw.get("custom", True))
    return cleaned


def _registered_filter_payload(db: Session, filter_id: str) -> dict[str, Any] | None:
    filter_id = safe_text(filter_id, 160)
    if not filter_id:
        return None

    approved = db.get(ApprovedFilter, filter_id)
    if approved is not None:
        return {
            "id": approved.filter_id,
            "name": approved.name,
            "areaM2": approved.area_m2,
            "station": approved.station,
            "location": approved.location,
            "businessUnit": approved.business_unit,
        }

    for item in _load_default_filters():
        if item.get("id") == filter_id:
            return {
                "id": item.get("id") or "",
                "name": item.get("name") or "",
                "areaM2": item.get("areaM2"),
                "station": item.get("station") or "",
                "location": item.get("location") or "",
                "businessUnit": item.get("businessUnit") or "",
            }

    record = db.execute(
        select(SessionRecord).where(SessionRecord.filter_id == filter_id).limit(1)
    ).scalar_one_or_none()
    if record is None:
        return None
    return {
        "id": record.filter_id,
        "name": record.filter_name,
        "areaM2": record.filter_area_m2,
        "station": record.filter_station,
        "location": record.filter_location,
        "businessUnit": record.filter_business_unit,
    }


def _clean_device(raw: Any) -> dict[str, str]:
    raw = raw if isinstance(raw, dict) else {}
    return {
        "name": safe_text(raw.get("name"), 160),
        "address": safe_text(raw.get("address"), 160),
    }


def _proposal_dict(proposal: ChangeProposal) -> dict[str, Any]:
    return {
        "proposalId": proposal.proposal_id,
        "type": proposal.proposal_type,
        "status": proposal.status,
        "submittedBy": proposal.submitted_by,
        "targetSessionRef": proposal.target_session_ref,
        "payload": proposal.payload,
        "createdAt": to_iso_z(proposal.created_at),
        "decidedAt": to_iso_z(proposal.decided_at),
        "decidedBy": proposal.decided_by,
        "decisionNote": proposal.decision_note,
    }


def _find_session(db: Session, session_ref: str) -> SessionRecord:
    record = db.execute(
        select(SessionRecord).where(
            or_(
                SessionRecord.ingestion_id == session_ref,
                SessionRecord.session_id == session_ref,
            )
        )
    ).scalar_one_or_none()
    if record is None:
        raise HTTPException(status_code=404, detail="Sessao nao encontrada.")
    return record


def _apply_session_edit(db: Session, proposal: ChangeProposal) -> None:
    record = _find_session(db, proposal.target_session_ref)
    changes = proposal.payload if isinstance(proposal.payload, dict) else {}
    session = dict(_payload_session(record))

    if "device" in changes:
        device = _clean_device(changes.get("device"))
        session["device"] = device
        record.device_name = device["name"]
        record.device_address = device["address"]

    if "filter" in changes and changes.get("timeSlice"):
        filter_data = _clean_filter(changes.get("filter"))
        time_slice = _clean_time_slice(changes.get("timeSlice"))
        if time_slice is None:
            raise HTTPException(status_code=400, detail="Recorte de tempo invalido.")
        _apply_session_time_slice(db, record, filter_data, time_slice, changes.get("manualMeasurement"))
        return

    manual_measurement = _clean_manual_measurement(changes.get("manualMeasurement"))
    if manual_measurement is not None:
        session["manualMeasurement"] = manual_measurement

    if "filter" in changes:
        filter_data = _clean_filter(changes.get("filter"))
        session["filter"] = filter_data
        record.filter_id = filter_data["id"]
        record.filter_name = filter_data["name"]
        record.filter_area_m2 = filter_data["areaM2"]
        record.filter_station = filter_data["station"]
        record.filter_location = filter_data["location"]
        record.filter_business_unit = filter_data["businessUnit"]

    payload = dict(record.payload) if isinstance(record.payload, dict) else {}
    payload["session"] = session
    record.payload = payload
    flag_modified(record, "payload")


def _apply_session_time_slice(
    db: Session,
    record: SessionRecord,
    filter_data: dict[str, Any],
    time_slice: dict[str, str],
    manual_measurement: Any = None,
) -> None:
    start = parse_iso_or_none(time_slice.get("from"))
    end = parse_iso_or_none(time_slice.get("to"))
    if start is None or end is None or end <= start:
        raise HTTPException(status_code=400, detail="Recorte de tempo invalido.")

    start_ms = int(round(start.timestamp() * 1000))
    end_ms = int(round(end.timestamp() * 1000))
    payload = dict(record.payload) if isinstance(record.payload, dict) else {}
    session = dict(_payload_session(record))
    points, scale = _sample_points(session.get("samples"))
    selected = [point for point in points if start_ms <= point[0] <= end_ms]
    remaining = [point for point in points if point[0] < start_ms or point[0] > end_ms]

    if not selected:
        raise HTTPException(status_code=400, detail="Recorte sem amostras.")

    cleaned_measurement = _clean_manual_measurement(manual_measurement)

    if not remaining:
        session["filter"] = filter_data
        session["samples"] = _compact_from_points(selected, scale)
        if cleaned_measurement is not None:
            session["manualMeasurement"] = cleaned_measurement
        selected_start, selected_end = _point_bounds(selected)
        session["startedAt"] = to_iso_z(selected_start)
        session["endedAt"] = to_iso_z(selected_end)
        payload["session"] = session
        record.payload = payload
        record.started_at = selected_start
        record.ended_at = selected_end
        record.sample_count = len(selected)
        record.filter_id = filter_data["id"]
        record.filter_name = filter_data["name"]
        record.filter_area_m2 = filter_data["areaM2"]
        record.filter_station = filter_data["station"]
        record.filter_location = filter_data["location"]
        record.filter_business_unit = filter_data["businessUnit"]
        flag_modified(record, "payload")
        return

    token = uuid4().hex[:10]
    original_session_id = safe_text(session.get("id") or record.session_id, 170)
    selected_start, selected_end = _point_bounds(selected)
    remaining_start, remaining_end = _point_bounds(remaining)

    base_ingestion_id = safe_text(record.ingestion_id, 44) or "session"
    ingestion_id = f"{base_ingestion_id}-slice-{token}"
    while db.get(SessionRecord, ingestion_id) is not None:
        token = uuid4().hex[:10]
        ingestion_id = f"{base_ingestion_id}-slice-{token}"

    slice_session_id = f"{original_session_id}-slice-{token}"
    slice_session = dict(session)
    slice_session["id"] = slice_session_id
    slice_session["startedAt"] = to_iso_z(selected_start)
    slice_session["endedAt"] = to_iso_z(selected_end)
    slice_session["endReason"] = "bi_time_slice"
    slice_session["filter"] = filter_data
    slice_session["samples"] = _compact_from_points(selected, scale)
    slice_session["splitFromSessionId"] = record.session_id
    slice_session["timeSlice"] = time_slice
    if cleaned_measurement is not None:
        slice_session["manualMeasurement"] = cleaned_measurement

    slice_payload = dict(payload)
    slice_payload["session"] = slice_session

    dedup_key = f"{safe_text(record.dedup_key, 490)}|slice|{token}"
    new_record = SessionRecord(
        ingestion_id=ingestion_id,
        dedup_key=dedup_key,
        schema_version=record.schema_version,
        app=record.app,
        uploaded_at=record.uploaded_at,
        received_at=now_utc(),
        session_id=slice_session_id,
        started_at=selected_start,
        ended_at=selected_end,
        end_reason="bi_time_slice",
        sample_count=len(selected),
        device_name=record.device_name,
        device_address=record.device_address,
        filter_id=filter_data["id"],
        filter_name=filter_data["name"],
        filter_area_m2=filter_data["areaM2"],
        filter_station=filter_data["station"],
        filter_location=filter_data["location"],
        filter_business_unit=filter_data["businessUnit"],
        payload=slice_payload,
    )
    db.add(new_record)

    session["samples"] = _compact_from_points(remaining, scale)
    session["startedAt"] = to_iso_z(remaining_start)
    session["endedAt"] = to_iso_z(remaining_end)
    session["timeSliceRemoved"] = time_slice
    payload["session"] = session
    record.payload = payload
    record.started_at = remaining_start
    record.ended_at = remaining_end
    record.sample_count = len(remaining)
    flag_modified(record, "payload")


def _apply_custom_filter(db: Session, proposal: ChangeProposal) -> None:
    payload = proposal.payload if isinstance(proposal.payload, dict) else {}
    filter_data = _clean_filter(payload.get("filter") or payload, generate_id=True)
    if not filter_data["name"]:
        raise HTTPException(status_code=400, detail="Filtro sem nome.")

    approved = ApprovedFilter(
        filter_id=filter_data["id"],
        name=filter_data["name"],
        area_m2=filter_data["areaM2"],
        station=filter_data["station"],
        location=filter_data["location"],
        business_unit=filter_data["businessUnit"],
        payload=filter_data,
        source="proposal",
        archived=False,
        source_proposal_id=proposal.proposal_id,
        approved_at=now_utc(),
        updated_at=now_utc(),
    )
    db.merge(approved)


def _apply_filter_edit(db: Session, proposal: ChangeProposal) -> None:
    target_id = (proposal.target_session_ref or "").strip()
    record = db.get(ApprovedFilter, target_id) if target_id else None
    if record is None:
        base = _registered_filter_payload(db, target_id)
        if base is None:
            raise HTTPException(status_code=404, detail="Filtro nao encontrado.")
        cleaned_base = _clean_filter(base)
        record = ApprovedFilter(
            filter_id=cleaned_base["id"],
            name=cleaned_base["name"],
            area_m2=cleaned_base["areaM2"],
            station=cleaned_base["station"],
            location=cleaned_base["location"],
            business_unit=cleaned_base["businessUnit"],
            payload=cleaned_base,
            source="proposal",
            archived=False,
            source_proposal_id=proposal.proposal_id,
            approved_at=now_utc(),
            updated_at=now_utc(),
        )
        db.add(record)

    payload = proposal.payload if isinstance(proposal.payload, dict) else {}
    changes = payload.get("changes") if isinstance(payload.get("changes"), dict) else {}
    retroactive = bool(payload.get("retroactive", True))

    merged = {
        "id": record.filter_id,
        "name": changes.get("name", record.name),
        "areaM2": changes.get("areaM2", record.area_m2),
        "station": changes.get("station", record.station),
        "location": changes.get("location", record.location),
        "businessUnit": changes.get("businessUnit", record.business_unit),
    }
    cleaned = _clean_filter(merged)

    record.name = cleaned["name"]
    record.area_m2 = cleaned["areaM2"]
    record.station = cleaned["station"]
    record.location = cleaned["location"]
    record.business_unit = cleaned["businessUnit"]
    record.payload = cleaned
    record.updated_at = now_utc()
    flag_modified(record, "payload")

    if retroactive:
        sessions = db.execute(
            select(SessionRecord).where(SessionRecord.filter_id == record.filter_id)
        ).scalars().all()
        for sr in sessions:
            sr.filter_name = cleaned["name"]
            sr.filter_area_m2 = cleaned["areaM2"]
            sr.filter_station = cleaned["station"]
            sr.filter_location = cleaned["location"]
            sr.filter_business_unit = cleaned["businessUnit"]
            payload_copy = dict(sr.payload) if isinstance(sr.payload, dict) else {}
            session_copy = dict(payload_copy.get("session") or {})
            session_filter = dict(session_copy.get("filter") or {})
            session_filter.update({
                "id": cleaned["id"],
                "name": cleaned["name"],
                "areaM2": cleaned["areaM2"],
                "station": cleaned["station"],
                "location": cleaned["location"],
                "businessUnit": cleaned["businessUnit"],
            })
            session_copy["filter"] = session_filter
            payload_copy["session"] = session_copy
            sr.payload = payload_copy
            flag_modified(sr, "payload")


def _apply_filter_archive(db: Session, proposal: ChangeProposal) -> None:
    target_id = (proposal.target_session_ref or "").strip()
    record = db.get(ApprovedFilter, target_id) if target_id else None
    if record is None:
        base = _registered_filter_payload(db, target_id)
        if base is None:
            raise HTTPException(status_code=404, detail="Filtro nao encontrado.")
        cleaned_base = _clean_filter(base)
        record = ApprovedFilter(
            filter_id=cleaned_base["id"],
            name=cleaned_base["name"],
            area_m2=cleaned_base["areaM2"],
            station=cleaned_base["station"],
            location=cleaned_base["location"],
            business_unit=cleaned_base["businessUnit"],
            payload=cleaned_base,
            source="proposal",
            archived=False,
            source_proposal_id=proposal.proposal_id,
            approved_at=now_utc(),
            updated_at=now_utc(),
        )
        db.add(record)
    payload = proposal.payload if isinstance(proposal.payload, dict) else {}
    record.archived = bool(payload.get("archived", True))
    record.updated_at = now_utc()


@router.get("/", include_in_schema=False)
def root() -> RedirectResponse:
    return RedirectResponse(url="/bi")


@router.get("/bi", include_in_schema=False)
def bi_page() -> HTMLResponse:
    return HTMLResponse(BI_HTML_PATH.read_text(encoding="utf-8"))


@router.get("/filtertrack/bi/auth")
def auth_check(access: AccessContext = Depends(require_bi_access)) -> dict[str, Any]:
    return {"ok": True, "role": access.role}


@router.get("/filtertrack/bi/overview")
def bi_overview(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
) -> dict[str, Any]:
    records = _records_for_filters(db, from_date, to_date, deviceAddress, filterId)

    total_samples = sum(max(0, int(record.sample_count or 0)) for record in records)
    unique_devices = {record.device_address for record in records if record.device_address}
    unique_filters = {record.filter_id for record in records if record.filter_id}
    durations = [item for item in (_duration_seconds(record) for record in records) if item is not None]
    points = [_record_point(record) for record in records if _record_point(record) is not None]

    by_device: dict[str, dict[str, Any]] = {}
    by_filter: dict[str, dict[str, Any]] = {}
    by_station: dict[str, dict[str, Any]] = {}
    by_day: dict[str, dict[str, Any]] = defaultdict(lambda: {"date": "", "sessions": 0, "samples": 0})
    overall_flow = _empty_flow_stats()
    quality = {
        "sessionsWithoutSamples": 0,
        "sessionsWithoutFilterArea": 0,
        "sessionsWithoutFlow": 0,
    }

    for record in records:
        flow = _record_flow_summary(record)
        _add_flow_stats(overall_flow, flow)
        if not int(record.sample_count or 0):
            quality["sessionsWithoutSamples"] += 1
        if not _finite_float(record.filter_area_m2):
            quality["sessionsWithoutFilterArea"] += 1
        if not flow.get("available"):
            quality["sessionsWithoutFlow"] += 1

        device_key = record.device_address or "desconhecido"
        device_row = by_device.setdefault(
            device_key,
            {
                "address": device_key,
                "name": record.device_name or "Dispositivo",
                "sessions": 0,
                "samples": 0,
                "lastSeenAt": None,
                **_empty_flow_stats(),
            },
        )
        device_row["sessions"] += 1
        device_row["samples"] += int(record.sample_count or 0)
        _add_flow_stats(device_row, flow)
        point = _record_point(record)
        if point and (device_row["lastSeenAt"] is None or point > device_row["lastSeenAt"]):
            device_row["lastSeenAt"] = point

        filter_key = record.filter_id or "sem-filtro"
        filter_row = by_filter.setdefault(
            filter_key,
            {
                "id": filter_key,
                "name": record.filter_name or "Sem filtro",
                "station": record.filter_station,
                "location": record.filter_location,
                "businessUnit": record.filter_business_unit,
                "sessions": 0,
                "samples": 0,
                "areaM2": record.filter_area_m2,
                **_empty_flow_stats(),
            },
        )
        filter_row["sessions"] += 1
        filter_row["samples"] += int(record.sample_count or 0)
        _add_flow_stats(filter_row, flow)

        station_key = record.filter_station or "sem-estacao"
        station_row = by_station.setdefault(
            station_key,
            {
                "station": station_key,
                "location": record.filter_location,
                "businessUnit": record.filter_business_unit,
                "sessions": 0,
                "samples": 0,
                **_empty_flow_stats(),
            },
        )
        station_row["sessions"] += 1
        station_row["samples"] += int(record.sample_count or 0)
        _add_flow_stats(station_row, flow)

        day = _date_key(point)
        by_day[day]["date"] = day
        by_day[day]["sessions"] += 1
        by_day[day]["samples"] += int(record.sample_count or 0)
        if "flowSessions" not in by_day[day]:
            by_day[day].update(_empty_flow_stats())
        _add_flow_stats(by_day[day], flow)

    return {
        "ok": True,
        "role": access.role,
        "summary": {
            "totalSessions": len(records),
            "totalSamples": total_samples,
            "uniqueDevices": len(unique_devices),
            "uniqueFilters": len(unique_filters),
            "firstSessionAt": to_iso_z(min(points)) if points else None,
            "lastSessionAt": to_iso_z(max(points)) if points else None,
            "avgSamplesPerSession": round(total_samples / len(records), 2) if records else 0,
            "avgDurationMinutes": round((sum(durations) / len(durations)) / 60, 2) if durations else 0,
            "flow": _finalize_flow_stats(overall_flow),
            "quality": quality,
        },
        "devices": [
            {**_finalize_flow_stats(item), "lastSeenAt": to_iso_z(item["lastSeenAt"])}
            for item in sorted(by_device.values(), key=lambda row: row["samples"], reverse=True)
        ],
        "filters": [
            _finalize_flow_stats(item)
            for item in sorted(by_filter.values(), key=lambda row: row["samples"], reverse=True)
        ],
        "stations": [
            _finalize_flow_stats(item)
            for item in sorted(by_station.values(), key=lambda row: row["samples"], reverse=True)
        ],
        "series": [_finalize_flow_stats(item) for item in sorted(by_day.values(), key=lambda row: row["date"])],
        "latestSessions": [_record_summary(record) for record in records[:25]],
    }


@router.get("/filtertrack/bi/sessions")
def bi_sessions(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    limit: int = Query(default=100, ge=1, le=10000),
    offset: int = Query(default=0, ge=0),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
    sessionRef: str | None = Query(default=None),
) -> dict[str, Any]:
    del access
    clauses = _query_clauses(from_date, to_date, deviceAddress, filterId, sessionRef)
    count_stmt = select(func.count()).select_from(SessionRecord)
    stmt = select(SessionRecord).order_by(SessionRecord.received_at.desc())
    if clauses:
        count_stmt = count_stmt.where(and_(*clauses))
        stmt = stmt.where(and_(*clauses))
    total = int(db.scalar(count_stmt) or 0)
    records = db.execute(stmt.offset(offset).limit(limit)).scalars().all()
    return {"ok": True, "total": total, "items": [_record_summary(record) for record in records]}


@router.get("/filtertrack/bi/sessions/{session_ref}")
def bi_session_detail(
    session_ref: str,
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
) -> dict[str, Any]:
    del access
    record = _find_session(db, session_ref.strip())
    session = _payload_session(record)
    return {
        "ok": True,
        "item": {
            **_record_summary(record),
            "session": session,
            "sampleCount": _series_count(session.get("samples")),
        },
    }


@router.get("/filtertrack/bi/export/sessions.csv")
def export_sessions_csv(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
    sessionRef: str | None = Query(default=None),
) -> Response:
    del access
    records = _records_for_filters(db, from_date, to_date, deviceAddress, filterId, sessionRef)
    output = io.StringIO()
    writer = csv.DictWriter(
        output,
        fieldnames=[
            "sessionId",
            "ingestionId",
            "receivedAt",
            "startedAt",
            "endedAt",
            "durationSeconds",
            "sampleCount",
            "deviceName",
            "deviceAddress",
            "filterId",
            "filterName",
            "filterStation",
            "filterLocation",
            "filterBusinessUnit",
            "filterAreaM2",
            "flowAvailable",
            "flowLpm",
            "flowDirection",
            "distanceStartCm",
            "distanceEndCm",
            "distanceChangeCm",
        ],
    )
    writer.writeheader()
    for record in records:
        flow = _record_flow_summary(record)
        writer.writerow(
            {
                "sessionId": record.session_id,
                "ingestionId": record.ingestion_id,
                "receivedAt": to_iso_z(record.received_at),
                "startedAt": to_iso_z(record.started_at),
                "endedAt": to_iso_z(record.ended_at),
                "durationSeconds": _duration_seconds(record),
                "sampleCount": record.sample_count,
                "deviceName": record.device_name,
                "deviceAddress": record.device_address,
                "filterId": record.filter_id,
                "filterName": record.filter_name,
                "filterStation": record.filter_station,
                "filterLocation": record.filter_location,
                "filterBusinessUnit": record.filter_business_unit,
                "filterAreaM2": record.filter_area_m2,
                "flowAvailable": flow.get("available"),
                "flowLpm": flow.get("flowLpm"),
                "flowDirection": flow.get("direction"),
                "distanceStartCm": flow.get("distanceStartCm"),
                "distanceEndCm": flow.get("distanceEndCm"),
                "distanceChangeCm": flow.get("distanceChangeCm"),
            }
        )
    return Response(
        content=output.getvalue(),
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": "attachment; filename=filtertrack-sessions.csv"},
    )


@router.get("/filtertrack/bi/export/samples.csv")
def export_samples_csv(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
    sessionRef: str | None = Query(default=None),
) -> Response:
    del access
    records = _records_for_filters(db, from_date, to_date, deviceAddress, filterId, sessionRef)
    output = io.StringIO()
    writer = csv.DictWriter(
        output,
        fieldnames=["sessionId", "deviceAddress", "filterId", "index", "timestamp", "offsetMs", "distanceCm"],
    )
    writer.writeheader()
    for record in records:
        for row in _iter_sample_rows(record) or []:
            writer.writerow(row)
    return Response(
        content=output.getvalue(),
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": "attachment; filename=filtertrack-samples.csv"},
    )


@router.get("/filtertrack/bi/export/sessions.json")
def export_sessions_json(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
    sessionRef: str | None = Query(default=None),
) -> dict[str, Any]:
    del access
    records = _records_for_filters(db, from_date, to_date, deviceAddress, filterId, sessionRef)
    return {
        "ok": True,
        "items": [
            {
                **_record_summary(record),
                "session": _payload_session(record),
            }
            for record in records
        ],
    }


@router.get("/filtertrack/bi/proposals")
def list_proposals(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    status_filter: str | None = Query(default=None, alias="status"),
) -> dict[str, Any]:
    del access
    stmt = select(ChangeProposal).order_by(ChangeProposal.created_at.desc())
    if status_filter:
        stmt = stmt.where(ChangeProposal.status == status_filter)
    proposals = db.execute(stmt.limit(500)).scalars().all()
    return {"ok": True, "items": [_proposal_dict(item) for item in proposals]}


@router.post("/filtertrack/bi/proposals")
def create_proposal(
    raw_payload: dict[str, Any],
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
) -> dict[str, Any]:
    proposal_type = safe_text(raw_payload.get("type"), 40)
    if proposal_type not in {"session_edit", "custom_filter", "filter_edit", "filter_archive"}:
        raise HTTPException(status_code=400, detail="Tipo de proposta invalido.")

    target_session_ref = safe_text(
        raw_payload.get("targetSessionRef") or raw_payload.get("targetFilterId"),
        200,
    )
    payload = raw_payload.get("payload") if isinstance(raw_payload.get("payload"), dict) else {}
    if proposal_type == "session_edit" and not target_session_ref:
        raise HTTPException(status_code=400, detail="Sessao alvo obrigatoria.")
    if proposal_type in {"filter_edit", "filter_archive"} and not target_session_ref:
        raise HTTPException(status_code=400, detail="Filtro alvo obrigatorio.")
    if proposal_type == "custom_filter":
        raw_filter = payload.get("filter") if isinstance(payload.get("filter"), dict) else payload
        payload = {"filter": _clean_filter(raw_filter, generate_id=True)}
    if proposal_type == "session_edit":
        cleaned: dict[str, Any] = {}
        if "device" in payload:
            cleaned["device"] = _clean_device(payload.get("device"))
        if "filter" in payload:
            cleaned["filter"] = _clean_filter(payload.get("filter"))
        if "timeSlice" in payload:
            time_slice = _clean_time_slice(payload.get("timeSlice"))
            if time_slice is not None:
                cleaned["timeSlice"] = time_slice
        manual_measurement = _clean_manual_measurement(payload.get("manualMeasurement"))
        if manual_measurement is not None:
            cleaned["manualMeasurement"] = manual_measurement
        if "timeSlice" in cleaned and "filter" not in cleaned:
            raise HTTPException(status_code=400, detail="Recorte de tempo exige filtro alvo.")
        payload = cleaned
        if not payload:
            raise HTTPException(status_code=400, detail="Nenhuma alteracao enviada.")
    if proposal_type == "filter_edit":
        target = _registered_filter_payload(db, target_session_ref)
        if target is None:
            raise HTTPException(status_code=404, detail="Filtro nao encontrado.")
        raw_changes = payload.get("changes") if isinstance(payload.get("changes"), dict) else payload
        cleaned_changes: dict[str, Any] = {}
        for key in ("name", "station", "location", "businessUnit"):
            if key in raw_changes:
                cleaned_changes[key] = safe_text(raw_changes.get(key), 200)
        if "areaM2" in raw_changes:
            area_value = raw_changes["areaM2"]
            try:
                cleaned_changes["areaM2"] = (
                    float(area_value) if area_value not in (None, "") else None
                )
            except (TypeError, ValueError):
                cleaned_changes["areaM2"] = None
        if not cleaned_changes:
            raise HTTPException(status_code=400, detail="Nenhuma alteracao enviada.")
        payload = {
            "changes": cleaned_changes,
            "retroactive": bool(payload.get("retroactive", True)),
        }
    if proposal_type == "filter_archive":
        target = _registered_filter_payload(db, target_session_ref)
        if target is None:
            raise HTTPException(status_code=404, detail="Filtro nao encontrado.")
        payload = {"archived": bool(payload.get("archived", True))}

    proposal = ChangeProposal(
        proposal_id=str(uuid4()),
        proposal_type=proposal_type,
        status="pending",
        submitted_by=safe_text(raw_payload.get("submittedBy"), 120) or access.role,
        target_session_ref=target_session_ref,
        payload=payload,
    )
    db.add(proposal)
    db.commit()
    db.refresh(proposal)
    return {"ok": True, "item": _proposal_dict(proposal)}


@router.post("/filtertrack/bi/proposals/{proposal_id}/approve")
def approve_proposal(
    proposal_id: str,
    raw_payload: dict[str, Any] | None = None,
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_admin),
) -> dict[str, Any]:
    proposal = db.get(ChangeProposal, proposal_id)
    if proposal is None:
        raise HTTPException(status_code=404, detail="Proposta nao encontrada.")
    if proposal.status != "pending":
        raise HTTPException(status_code=409, detail="Proposta ja decidida.")

    if proposal.proposal_type == "session_edit":
        _apply_session_edit(db, proposal)
    elif proposal.proposal_type == "custom_filter":
        _apply_custom_filter(db, proposal)
    elif proposal.proposal_type == "filter_edit":
        _apply_filter_edit(db, proposal)
    elif proposal.proposal_type == "filter_archive":
        _apply_filter_archive(db, proposal)
    else:
        raise HTTPException(status_code=400, detail="Tipo de proposta invalido.")

    proposal.status = "approved"
    proposal.decided_at = now_utc()
    proposal.decided_by = access.label
    proposal.decision_note = safe_text((raw_payload or {}).get("note"), 500)
    db.commit()
    db.refresh(proposal)
    return {"ok": True, "item": _proposal_dict(proposal)}


@router.post("/filtertrack/bi/proposals/{proposal_id}/reject")
def reject_proposal(
    proposal_id: str,
    raw_payload: dict[str, Any] | None = None,
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_admin),
) -> dict[str, Any]:
    proposal = db.get(ChangeProposal, proposal_id)
    if proposal is None:
        raise HTTPException(status_code=404, detail="Proposta nao encontrada.")
    if proposal.status != "pending":
        raise HTTPException(status_code=409, detail="Proposta ja decidida.")

    proposal.status = "rejected"
    proposal.decided_at = now_utc()
    proposal.decided_by = access.label
    proposal.decision_note = safe_text((raw_payload or {}).get("note"), 500)
    db.commit()
    db.refresh(proposal)
    return {"ok": True, "item": _proposal_dict(proposal)}


@router.get("/filtertrack/bi/filters")
def bi_filters(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
    include_archived: bool = Query(default=True, alias="includeArchived"),
) -> dict[str, Any]:
    del access
    approved_rows = db.execute(select(ApprovedFilter)).scalars().all()
    approved_map = {row.filter_id: row for row in approved_rows}

    stats_rows = db.execute(
        select(
            SessionRecord.filter_id,
            SessionRecord.filter_name,
            SessionRecord.filter_area_m2,
            SessionRecord.filter_station,
            SessionRecord.filter_location,
            SessionRecord.filter_business_unit,
            func.count().label("sessions"),
            func.coalesce(func.sum(SessionRecord.sample_count), 0).label("samples"),
            func.max(
                func.coalesce(SessionRecord.started_at, SessionRecord.received_at)
            ).label("last_seen"),
        ).group_by(SessionRecord.filter_id)
    ).all()
    stats_map = {row.filter_id: row for row in stats_rows if row.filter_id}

    items: list[dict[str, Any]] = []
    seen_ids: set[str] = set()

    def _stats_payload(stat: Any | None) -> dict[str, Any]:
        return {
            "sessions": int(stat.sessions) if stat else 0,
            "samples": int(stat.samples) if stat else 0,
            "lastSeenAt": to_iso_z(stat.last_seen) if stat else None,
        }

    for f in _load_default_filters():
        fid = f["id"]
        if fid in seen_ids:
            continue
        seen_ids.add(fid)
        approved = approved_map.get(fid)
        stat = stats_map.get(fid)
        items.append(
            {
                "id": fid,
                "name": (approved.name if approved else f["name"]),
                "areaM2": (approved.area_m2 if approved else f["areaM2"]),
                "station": (approved.station if approved else f["station"]),
                "location": (approved.location if approved else f["location"]),
                "businessUnit": (approved.business_unit if approved else f["businessUnit"]),
                "source": (approved.source if approved else "default"),
                "archived": bool(approved.archived) if approved else False,
                "createdAt": to_iso_z(approved.created_at) if approved else None,
                "approvedAt": to_iso_z(approved.approved_at) if approved else None,
                "updatedAt": to_iso_z(approved.updated_at) if approved else None,
                "sourceProposalId": approved.source_proposal_id if approved else None,
                **_stats_payload(stat),
            }
        )

    for row in approved_rows:
        if row.filter_id in seen_ids:
            continue
        seen_ids.add(row.filter_id)
        stat = stats_map.get(row.filter_id)
        items.append(
            {
                "id": row.filter_id,
                "name": row.name,
                "areaM2": row.area_m2,
                "station": row.station,
                "location": row.location,
                "businessUnit": row.business_unit,
                "source": row.source,
                "archived": bool(row.archived),
                "createdAt": to_iso_z(row.created_at),
                "approvedAt": to_iso_z(row.approved_at),
                "updatedAt": to_iso_z(row.updated_at),
                "sourceProposalId": row.source_proposal_id,
                **_stats_payload(stat),
            }
        )

    for fid, stat in stats_map.items():
        if fid in seen_ids:
            continue
        seen_ids.add(fid)
        items.append(
            {
                "id": fid,
                "name": stat.filter_name or fid,
                "areaM2": stat.filter_area_m2,
                "station": stat.filter_station,
                "location": stat.filter_location,
                "businessUnit": stat.filter_business_unit,
                "source": "ingested",
                "archived": False,
                "createdAt": None,
                "approvedAt": None,
                "updatedAt": None,
                "sourceProposalId": None,
                **_stats_payload(stat),
            }
        )

    if not include_archived:
        items = [it for it in items if not it["archived"]]

    items.sort(key=lambda it: ((it.get("station") or "").lower(), (it.get("name") or "").lower()))
    return {"ok": True, "items": items}


@router.get("/filtertrack/bi/custom-filters")
def list_custom_filters(
    db: Session = Depends(get_db),
    access: AccessContext = Depends(require_bi_access),
) -> dict[str, Any]:
    del access
    rows = db.execute(select(ApprovedFilter).order_by(ApprovedFilter.created_at.desc())).scalars().all()
    return {
        "ok": True,
        "items": [
            {
                "id": row.filter_id,
                "name": row.name,
                "areaM2": row.area_m2,
                "station": row.station,
                "location": row.location,
                "businessUnit": row.business_unit,
                "createdAt": to_iso_z(row.created_at),
                "approvedAt": to_iso_z(row.approved_at),
                "sourceProposalId": row.source_proposal_id,
            }
            for row in rows
        ],
    }
