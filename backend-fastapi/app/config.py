import os
from pathlib import Path


def _read_positive_int(name: str, fallback: int) -> int:
    raw = os.getenv(name, "").strip()
    try:
        value = int(raw)
        return value if value > 0 else fallback
    except (TypeError, ValueError):
        return fallback


def _default_database_path() -> str:
    if os.getenv("FLY_APP_NAME"):
        return "/data/filtertrack.db"
    return "./filtertrack.db"


def _sqlite_url_from_path(raw_path: str) -> str:
    path = (raw_path or "").strip() or _default_database_path()
    if path == ":memory:":
        return "sqlite:///:memory:"

    db_path = Path(path).expanduser()
    return f"sqlite:///{db_path.as_posix()}"


def _resolve_database_url() -> str:
    raw_url = os.getenv("DATABASE_URL", "").strip()
    if raw_url:
        if raw_url.startswith("sqlite"):
            return raw_url
        raise ValueError(
            "Only SQLite is supported in this minimal setup. "
            "Unset DATABASE_URL or set it to sqlite:////data/filtertrack.db."
        )
    return _sqlite_url_from_path(os.getenv("DATABASE_PATH", ""))


class Settings:
    def __init__(self) -> None:
        self.app_name = "FilterTrack API (FastAPI)"
        self.host = os.getenv("HOST", "0.0.0.0")
        self.port = _read_positive_int("PORT", 8080)
        self.database_url = _resolve_database_url()
        self.max_samples_per_session = _read_positive_int("MAX_SAMPLES_PER_SESSION", 120000)
        self.api_key = os.getenv("FILTERTRACK_API_KEY", "").strip()
        self.user_access_key = os.getenv("FILTERTRACK_USER_KEY", "").strip()
        self.admin_access_key = os.getenv("FILTERTRACK_ADMIN_KEY", "").strip()
        self.allow_local_bi_without_keys = (
            not os.getenv("FLY_APP_NAME")
            and not self.user_access_key
            and not self.admin_access_key
        )
        self.cors_allow_origins = [
            item.strip()
            for item in os.getenv("CORS_ALLOW_ORIGINS", "*").split(",")
            if item.strip()
        ] or ["*"]


settings = Settings()
