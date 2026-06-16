from collections import Counter
from datetime import datetime
from typing import Any
from uuid import uuid4

from fastapi import Depends, FastAPI, Header, HTTPException, Query, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from sqlalchemy import and_, func, inspect, or_, select, text
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from .bi import router as bi_router
from .config import settings
from .db import Base, engine, get_db
from .models import ApprovedFilter, SessionRecord
from .schemas import (
    HealthResponse,
    IngestResponse,
    SessionDetailResponse,
    SessionListResponse,
    StatsResponse,
)
from .utils import build_dedup_key, count_samples, normalize_payload, now_utc, parse_iso_or_none, to_iso_z


app = FastAPI(title=settings.app_name, version="2.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.cors_allow_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(bi_router)


@app.get("/privacy", include_in_schema=False)
def privacy_policy() -> FileResponse:
    return FileResponse("app/static/privacy.html", media_type="text/html; charset=utf-8")


@app.on_event("startup")
def on_startup() -> None:
    Base.metadata.create_all(bind=engine)
    _ensure_database_schema()


def _ensure_database_schema() -> None:
    columns = {
        item["name"]
        for item in inspect(engine).get_columns(SessionRecord.__tablename__)
    }
    if "filter_station" not in columns:
        with engine.begin() as conn:
            conn.execute(
                text(
                    "ALTER TABLE session_records "
                    "ADD COLUMN filter_station VARCHAR(200) NOT NULL DEFAULT ''"
                )
            )

    af_columns = {
        item["name"]
        for item in inspect(engine).get_columns(ApprovedFilter.__tablename__)
    }
    with engine.begin() as conn:
        if "source" not in af_columns:
            conn.execute(
                text(
                    "ALTER TABLE approved_filters "
                    "ADD COLUMN source VARCHAR(40) NOT NULL DEFAULT 'proposal'"
                )
            )
        if "archived" not in af_columns:
            conn.execute(
                text(
                    "ALTER TABLE approved_filters "
                    "ADD COLUMN archived INTEGER NOT NULL DEFAULT 0"
                )
            )
        if "updated_at" not in af_columns:
            conn.execute(text("ALTER TABLE approved_filters ADD COLUMN updated_at TIMESTAMP"))


def _register_filter_from_session(db: Session, filter_data: dict[str, Any] | None) -> None:
    if not isinstance(filter_data, dict):
        return
    fid = str(filter_data.get("id") or "").strip()
    if not fid:
        return
    if db.get(ApprovedFilter, fid) is not None:
        return

    cleaned = {
        "id": fid,
        "name": str(filter_data.get("name") or ""),
        "areaM2": filter_data.get("areaM2"),
        "station": str(filter_data.get("station") or ""),
        "location": str(filter_data.get("location") or ""),
        "businessUnit": str(filter_data.get("businessUnit") or ""),
    }
    if "custom" in filter_data:
        cleaned["custom"] = bool(filter_data.get("custom"))

    record = ApprovedFilter(
        filter_id=fid,
        name=cleaned["name"],
        area_m2=cleaned["areaM2"],
        station=cleaned["station"],
        location=cleaned["location"],
        business_unit=cleaned["businessUnit"],
        payload=cleaned,
        source="session",
        source_proposal_id="",
        approved_at=now_utc(),
    )
    db.add(record)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()


def _sample_count(session_data: dict[str, Any]) -> int:
    return count_samples(session_data.get("samples"))


def require_api_key(x_api_key: str | None = Header(default=None, alias="X-API-Key")) -> None:
    if settings.api_key and x_api_key != settings.api_key:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Nao autorizado. Header X-API-Key ausente ou invalido.",
        )


def _summary_from_record(record: SessionRecord) -> dict[str, Any]:
    payload = record.payload if isinstance(record.payload, dict) else {}
    session = payload.get("session", {}) if isinstance(payload, dict) else {}
    return {
        "ingestionId": record.ingestion_id,
        "sessionId": record.session_id,
        "app": record.app,
        "schemaVersion": record.schema_version,
        "receivedAt": to_iso_z(record.received_at),
        "uploadedAt": to_iso_z(record.uploaded_at),
        "startedAt": to_iso_z(record.started_at),
        "endedAt": to_iso_z(record.ended_at),
        "endReason": record.end_reason,
        "sampleCount": record.sample_count,
        "device": session.get("device") or {"name": "", "address": ""},
        "filter": session.get("filter"),
    }


def _base_filters(
    from_dt: datetime | None,
    to_dt: datetime | None,
    device_address: str | None,
    filter_id: str | None,
) -> list[Any]:
    clauses = []
    if from_dt is not None:
        clauses.append(func.coalesce(SessionRecord.started_at, SessionRecord.ended_at, SessionRecord.received_at) >= from_dt)
    if to_dt is not None:
        clauses.append(func.coalesce(SessionRecord.started_at, SessionRecord.ended_at, SessionRecord.received_at) <= to_dt)
    if device_address:
        clauses.append(func.lower(SessionRecord.device_address) == device_address.lower().strip())
    if filter_id:
        clauses.append(func.lower(SessionRecord.filter_id) == filter_id.lower().strip())
    return clauses


@app.get("/health", response_model=HealthResponse)
def health(db: Session = Depends(get_db)) -> dict[str, Any]:
    db.execute(text("SELECT 1"))
    total = db.scalar(select(func.count()).select_from(SessionRecord)) or 0
    return {
        "ok": True,
        "status": "healthy",
        "time": now_utc(),
        "sessionCount": int(total),
    }


@app.post(
    "/filtertrack/sessions",
    response_model=IngestResponse,
    dependencies=[Depends(require_api_key)],
)
def ingest_session(
    raw_payload: dict[str, Any],
    db: Session = Depends(get_db),
) -> dict[str, Any]:
    try:
        normalized = normalize_payload(raw_payload, settings.max_samples_per_session)
    except OverflowError as exc:
        raise HTTPException(status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    dedup_key = build_dedup_key(normalized)
    existing = db.execute(
        select(SessionRecord).where(SessionRecord.dedup_key == dedup_key)
    ).scalar_one_or_none()

    if existing:
        return {
            "ok": True,
            "duplicate": True,
            "ingestionId": existing.ingestion_id,
            "sessionId": existing.session_id,
            "receivedAt": to_iso_z(existing.received_at),
        }

    session_data = normalized["session"]
    started_at = parse_iso_or_none(session_data.get("startedAt"))
    ended_at = parse_iso_or_none(session_data.get("endedAt"))
    uploaded_at = parse_iso_or_none(normalized.get("uploadedAt")) or now_utc()

    device = session_data.get("device") if isinstance(session_data.get("device"), dict) else {}
    filter_data = session_data.get("filter") if isinstance(session_data.get("filter"), dict) else {}

    record = SessionRecord(
        ingestion_id=str(uuid4()),
        dedup_key=dedup_key,
        schema_version=int(normalized["schemaVersion"]),
        app=normalized["app"],
        uploaded_at=uploaded_at,
        session_id=session_data["id"],
        started_at=started_at,
        ended_at=ended_at,
        end_reason=session_data.get("endReason"),
        sample_count=_sample_count(session_data),
        device_name=str(device.get("name") or ""),
        device_address=str(device.get("address") or ""),
        filter_id=str(filter_data.get("id") or ""),
        filter_name=str(filter_data.get("name") or ""),
        filter_area_m2=filter_data.get("areaM2"),
        filter_station=str(filter_data.get("station") or ""),
        filter_location=str(filter_data.get("location") or ""),
        filter_business_unit=str(filter_data.get("businessUnit") or ""),
        payload=normalized,
    )

    db.add(record)
    try:
        db.commit()
        db.refresh(record)
    except IntegrityError:
        db.rollback()
        existing = db.execute(
            select(SessionRecord).where(SessionRecord.dedup_key == dedup_key)
        ).scalar_one()
        return {
            "ok": True,
            "duplicate": True,
            "ingestionId": existing.ingestion_id,
            "sessionId": existing.session_id,
            "receivedAt": to_iso_z(existing.received_at),
        }

    _register_filter_from_session(db, filter_data)

    return {
        "ok": True,
        "duplicate": False,
        "ingestionId": record.ingestion_id,
        "sessionId": record.session_id,
        "receivedAt": to_iso_z(record.received_at),
    }


@app.get(
    "/filtertrack/filters",
    dependencies=[Depends(require_api_key)],
)
def list_registered_filters(
    db: Session = Depends(get_db),
    include_archived: bool = Query(default=False, alias="includeArchived"),
) -> dict[str, Any]:
    stmt = select(ApprovedFilter).order_by(ApprovedFilter.name)
    if not include_archived:
        stmt = stmt.where(ApprovedFilter.archived == False)  # noqa: E712
    rows = db.execute(stmt).scalars().all()
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
                "source": row.source,
                "archived": bool(row.archived),
                "createdAt": to_iso_z(row.created_at),
                "approvedAt": to_iso_z(row.approved_at),
                "updatedAt": to_iso_z(row.updated_at) or to_iso_z(row.approved_at) or to_iso_z(row.created_at),
            }
            for row in rows
        ],
    }


@app.get(
    "/filtertrack/sessions",
    response_model=SessionListResponse,
    dependencies=[Depends(require_api_key)],
)
def list_sessions(
    db: Session = Depends(get_db),
    limit: int = Query(default=50, ge=1, le=500),
    offset: int = Query(default=0, ge=0, le=1_000_000),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
) -> dict[str, Any]:
    if from_date and to_date and from_date > to_date:
        raise HTTPException(status_code=400, detail="Parametro 'from' deve ser menor ou igual a 'to'.")

    clauses = _base_filters(from_date, to_date, deviceAddress, filterId)

    count_stmt = select(func.count()).select_from(SessionRecord)
    if clauses:
        count_stmt = count_stmt.where(and_(*clauses))
    total = int(db.scalar(count_stmt) or 0)

    query = select(SessionRecord).order_by(SessionRecord.received_at.desc())
    if clauses:
        query = query.where(and_(*clauses))
    rows = db.execute(query.offset(offset).limit(limit)).scalars().all()

    return {
        "ok": True,
        "total": total,
        "limit": limit,
        "offset": offset,
        "items": [_summary_from_record(item) for item in rows],
    }


@app.get(
    "/filtertrack/sessions/{session_ref}",
    response_model=SessionDetailResponse,
    dependencies=[Depends(require_api_key)],
)
def get_session_detail(
    session_ref: str,
    db: Session = Depends(get_db),
) -> dict[str, Any]:
    ref = (session_ref or "").strip()
    if not ref:
        raise HTTPException(status_code=400, detail="ID de sessao invalido.")

    record = db.execute(
        select(SessionRecord).where(
            or_(SessionRecord.ingestion_id == ref, SessionRecord.session_id == ref)
        ).order_by(SessionRecord.received_at.desc())
    ).scalar_one_or_none()

    if record is None:
        raise HTTPException(status_code=404, detail="Sessao nao encontrada.")

    return {
        "ok": True,
        "item": {
            "ingestionId": record.ingestion_id,
            "dedupKey": record.dedup_key,
            "receivedAt": to_iso_z(record.received_at),
            "schemaVersion": record.schema_version,
            "app": record.app,
            "uploadedAt": to_iso_z(record.uploaded_at),
            "session": (record.payload or {}).get("session", {}),
        },
    }


@app.get(
    "/filtertrack/stats",
    response_model=StatsResponse,
    dependencies=[Depends(require_api_key)],
)
def get_stats(
    db: Session = Depends(get_db),
    from_date: datetime | None = Query(default=None, alias="from"),
    to_date: datetime | None = Query(default=None, alias="to"),
    deviceAddress: str | None = Query(default=None),
    filterId: str | None = Query(default=None),
) -> dict[str, Any]:
    if from_date and to_date and from_date > to_date:
        raise HTTPException(status_code=400, detail="Parametro 'from' deve ser menor ou igual a 'to'.")

    clauses = _base_filters(from_date, to_date, deviceAddress, filterId)
    query = select(SessionRecord)
    if clauses:
        query = query.where(and_(*clauses))

    records = db.execute(query).scalars().all()
    if not records:
        return {
            "ok": True,
            "stats": {
                "totalSessions": 0,
                "totalSamples": 0,
                "firstSessionAt": None,
                "lastSessionAt": None,
                "topDevices": [],
                "topFilters": [],
            },
        }

    total_samples = sum(max(0, int(item.sample_count or 0)) for item in records)
    points = [
        item.started_at or item.ended_at or item.received_at
        for item in records
        if item.started_at or item.ended_at or item.received_at
    ]
    first_dt = min(points) if points else None
    last_dt = max(points) if points else None

    device_counts = Counter((item.device_address or "desconhecido") for item in records)
    filter_counts = Counter((item.filter_id or "sem-filtro") for item in records)

    return {
        "ok": True,
        "stats": {
            "totalSessions": len(records),
            "totalSamples": total_samples,
            "firstSessionAt": to_iso_z(first_dt),
            "lastSessionAt": to_iso_z(last_dt),
            "topDevices": [
                {"address": address, "count": count}
                for address, count in device_counts.most_common(10)
            ],
            "topFilters": [
                {"id": filter_id, "count": count}
                for filter_id, count in filter_counts.most_common(10)
            ],
        },
    }
