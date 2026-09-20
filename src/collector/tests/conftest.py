from __future__ import annotations

import hashlib
import io
import json
import time
import uuid
import zipfile

import pytest
from collector.app import create_app
from collector.config import Settings
from collector.signing import sign
from fastapi.testclient import TestClient

from .multipart_helper import encode_multipart

TOKEN = "test-shared-token-please-rotate"


def make_zip_bytes(size: int = 0, payload: bytes = b"pretend-log-data") -> bytes:
    """A syntactically real zip. `size` pads an extra STORED (uncompressed)
    member so the resulting zip is at least roughly `size` bytes -- used for
    the done_when's 40 MB / 70 MB cases."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mh_temporal.log", payload)
        zf.writestr("report.json", json.dumps({"exit_code": 0}))
        if size:
            zf.writestr("padding.bin", b"\0" * size, zipfile.ZIP_STORED)
    return buf.getvalue()


def default_meta() -> dict:
    return {
        "match_id": str(uuid.uuid4()),
        "version": "1.2.3",
        "build": "deadbeef",
        "exit_code": 3221225477,
        "os": "windows-11",
    }


def auth_headers(token: str, body: bytes, timestamp: int | None = None) -> dict:
    ts = timestamp if timestamp is not None else int(time.time())
    return {
        "X-Report-Token": token,
        "X-Report-Timestamp": str(ts),
        "X-Report-Signature": sign(token, ts, body),
    }


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@pytest.fixture
def settings(tmp_path) -> Settings:
    return Settings(
        reports_dir=tmp_path / "reports",
        token=TOKEN,
        max_body_mb=64,
        rate_per_min=100_000,  # keep the app-level rate limit out of the way of ordinary tests
        timestamp_skew_seconds=600,
    )


@pytest.fixture
def client(settings) -> TestClient:
    app = create_app(settings)
    with TestClient(app) as test_client:
        yield test_client


__all__ = [
    "TOKEN",
    "auth_headers",
    "default_meta",
    "encode_multipart",
    "make_zip_bytes",
    "sha256_hex",
]
