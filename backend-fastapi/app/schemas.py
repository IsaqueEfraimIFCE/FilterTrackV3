from datetime import datetime
from typing import Any

from pydantic import BaseModel, ConfigDict


class IngestResponse(BaseModel):
    ok: bool
    duplicate: bool
    ingestionId: str
    sessionId: str
    receivedAt: str


class SessionSummary(BaseModel):
    ingestionId: str
    sessionId: str
    app: str
    schemaVersion: int
    receivedAt: str
    uploadedAt: str
    startedAt: str | None
    endedAt: str | None
    endReason: str | None
    sampleCount: int
    device: dict[str, Any]
    filter: dict[str, Any] | None


class SessionListResponse(BaseModel):
    ok: bool
    total: int
    limit: int
    offset: int
    items: list[SessionSummary]


class SessionDetailResponse(BaseModel):
    ok: bool
    item: dict[str, Any]


class StatsResponse(BaseModel):
    ok: bool
    stats: dict[str, Any]


class HealthResponse(BaseModel):
    ok: bool
    status: str
    time: datetime
    sessionCount: int

    model_config = ConfigDict(json_schema_extra={"example": {"ok": True}})
