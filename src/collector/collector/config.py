"""Environment-driven configuration for the collector service.

Plan D14's env surface: `REPORTS_DIR`, `TOKEN_FILE`, `MAX_BODY_MB`, `RATE_PER_MIN`.
`TIMESTAMP_SKEW_SECONDS` (the HMAC replay window, plan D13/D14: 10 minutes) is
also environment-tunable but not called out by name in the scope, so it gets a
sane default instead of being required.

`REPORT_TOKEN` (inline, not a file) exists ONLY so tests and a bare local
`uvicorn` run can supply the shared secret without creating a file on disk.
The compose deployment always uses `TOKEN_FILE` pointed at a Docker secret --
see docker-compose.yml `secrets:` -- and should never set `REPORT_TOKEN`.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


class ConfigError(RuntimeError):
    """Raised when the environment does not describe a runnable configuration."""


@dataclass(frozen=True)
class Settings:
    reports_dir: Path
    token: str
    max_body_mb: int
    rate_per_min: int
    timestamp_skew_seconds: int

    @property
    def max_body_bytes(self) -> int:
        return self.max_body_mb * 1024 * 1024

    @property
    def db_path(self) -> Path:
        return self.reports_dir / "index.db"


def _read_token(token_file: Path, inline: str | None) -> str:
    if inline:
        return inline.strip()
    if not token_file.exists():
        raise ConfigError(
            f"TOKEN_FILE {token_file} does not exist -- create the shared report token "
            "(a Docker secret in production, a plain file for local runs; see README.md) "
            "or set REPORT_TOKEN for a quick local/test override."
        )
    token = token_file.read_text(encoding="utf-8").strip()
    if not token:
        raise ConfigError(f"TOKEN_FILE {token_file} is empty")
    return token


def get_settings() -> Settings:
    reports_dir = Path(os.environ.get("REPORTS_DIR", "./data/reports")).resolve()
    token_file = Path(os.environ.get("TOKEN_FILE", "/run/secrets/report_token"))
    inline_token = os.environ.get("REPORT_TOKEN")
    token = _read_token(token_file, inline_token)
    max_body_mb = int(os.environ.get("MAX_BODY_MB", "64"))
    rate_per_min = int(os.environ.get("RATE_PER_MIN", "10"))
    skew = int(os.environ.get("TIMESTAMP_SKEW_SECONDS", "600"))
    return Settings(
        reports_dir=reports_dir,
        token=token,
        max_body_mb=max_body_mb,
        rate_per_min=rate_per_min,
        timestamp_skew_seconds=skew,
    )
