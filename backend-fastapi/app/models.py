from sqlalchemy import Boolean, DateTime, Float, Integer, String, func
from sqlalchemy.orm import Mapped, mapped_column
from sqlalchemy.types import JSON

from .db import Base


class SessionRecord(Base):
    __tablename__ = "session_records"

    ingestion_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    dedup_key: Mapped[str] = mapped_column(String(512), unique=True, index=True, nullable=False)

    schema_version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    app: Mapped[str] = mapped_column(String(80), nullable=False, default="FilterTrack")
    uploaded_at: Mapped[DateTime] = mapped_column(DateTime(timezone=True), nullable=False)
    received_at: Mapped[DateTime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=func.now(),
        index=True,
    )

    session_id: Mapped[str] = mapped_column(String(200), nullable=False, index=True)
    started_at: Mapped[DateTime | None] = mapped_column(DateTime(timezone=True), index=True)
    ended_at: Mapped[DateTime | None] = mapped_column(DateTime(timezone=True), index=True)
    end_reason: Mapped[str | None] = mapped_column(String(200))
    sample_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)

    device_name: Mapped[str] = mapped_column(String(160), nullable=False, default="")
    device_address: Mapped[str] = mapped_column(String(160), nullable=False, default="", index=True)

    filter_id: Mapped[str] = mapped_column(String(160), nullable=False, default="", index=True)
    filter_name: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    filter_area_m2: Mapped[float | None] = mapped_column(Float)
    filter_station: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    filter_location: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    filter_business_unit: Mapped[str] = mapped_column(String(200), nullable=False, default="")

    payload: Mapped[dict] = mapped_column(JSON, nullable=False)


class ChangeProposal(Base):
    __tablename__ = "change_proposals"

    proposal_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    proposal_type: Mapped[str] = mapped_column(String(40), nullable=False, index=True)
    status: Mapped[str] = mapped_column(String(20), nullable=False, default="pending", index=True)

    submitted_by: Mapped[str] = mapped_column(String(120), nullable=False, default="")
    target_session_ref: Mapped[str] = mapped_column(String(200), nullable=False, default="", index=True)
    payload: Mapped[dict] = mapped_column(JSON, nullable=False)

    created_at: Mapped[DateTime] = mapped_column(DateTime(timezone=True), nullable=False, server_default=func.now(), index=True)
    decided_at: Mapped[DateTime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    decided_by: Mapped[str] = mapped_column(String(120), nullable=False, default="")
    decision_note: Mapped[str] = mapped_column(String(500), nullable=False, default="")


class ApprovedFilter(Base):
    __tablename__ = "approved_filters"

    filter_id: Mapped[str] = mapped_column(String(160), primary_key=True)
    name: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    area_m2: Mapped[float | None] = mapped_column(Float)
    station: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    location: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    business_unit: Mapped[str] = mapped_column(String(200), nullable=False, default="")
    payload: Mapped[dict] = mapped_column(JSON, nullable=False)

    source: Mapped[str] = mapped_column(String(40), nullable=False, default="proposal")
    archived: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)

    source_proposal_id: Mapped[str] = mapped_column(String(64), nullable=False, default="", index=True)
    created_at: Mapped[DateTime] = mapped_column(DateTime(timezone=True), nullable=False, server_default=func.now(), index=True)
    approved_at: Mapped[DateTime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    updated_at: Mapped[DateTime | None] = mapped_column(DateTime(timezone=True), nullable=True)
