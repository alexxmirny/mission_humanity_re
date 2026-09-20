"""FastAPI report collector service (tracker dist:RP2).

  POST /v1/reports  -- multipart upload of a session-report zip; HMAC-authenticated.
  GET  /healthz     -- liveness + basic counts.

Auth (plan D13/D14): `X-Report-Token` (the shared secret, loaded from a Docker
secret file -- see config.py) + `X-Report-Timestamp` (unix seconds) +
`X-Report-Signature` (hex HMAC-SHA256 over `f"{timestamp}\\n{sha256(body)}"`,
keyed by that same shared token -- see signing.py/auth.py). Any auth failure
returns 401 BEFORE anything is written to disk or the index: the handler reads
the raw body (needed for the signature check regardless), verifies it, and
only then parses the multipart form and touches storage.

Body-size enforcement happens at two layers, per RP2's done_when: Caddy's
`request_body { max_size 64MB }` in front (../Caddyfile) rejects an oversized
body before it reaches this process in the compose deployment;
`MaxBodySizeMiddleware` (middleware.py) enforces the same limit in-process so
the app is also safe run directly behind uvicorn with no Caddy -- e.g. in
tests, or a bare `uvicorn collector.app:create_app --factory` on a machine
without Docker.
"""

from __future__ import annotations

import hashlib
import json
import logging
from contextlib import asynccontextmanager
from datetime import datetime, timezone

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from starlette.datastructures import UploadFile
from starlette.formparsers import MultiPartException

from . import auth, db
from .config import Settings, get_settings
from .middleware import MaxBodySizeMiddleware
from .ratelimit import TokenBucket
from .storage import atomic_write, report_dir
from .ulid import new_ulid

logger = logging.getLogger("collector")


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or get_settings()

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        settings.reports_dir.mkdir(parents=True, exist_ok=True)
        conn = db.connect(settings.db_path)
        conn.close()
        yield

    app = FastAPI(title="mh report collector", version="1.0.0", lifespan=lifespan)
    app.state.settings = settings
    app.state.ip_bucket = TokenBucket(settings.rate_per_min)
    app.state.token_bucket = TokenBucket(settings.rate_per_min)

    app.add_middleware(MaxBodySizeMiddleware, max_bytes=settings.max_body_bytes)

    @app.get("/healthz")
    def healthz() -> JSONResponse:
        conn = db.connect(settings.db_path)
        try:
            total, dups = db.counts(conn)
        finally:
            conn.close()
        return _json(200, {"status": "ok", "reports_total": total, "duplicates_total": dups})

    @app.post("/v1/reports")
    async def post_report(request: Request) -> JSONResponse:
        client_ip = _client_ip(request)
        token_header = request.headers.get("x-report-token", "")

        if not app.state.ip_bucket.allow(client_ip):
            return _json(429, {"error": "rate limited (ip)"})
        if not app.state.token_bucket.allow(token_header or client_ip):
            return _json(429, {"error": "rate limited (token)"})

        body = await request.body()

        result = auth.verify_request(
            token=settings.token,
            header_token=token_header,
            timestamp_header=request.headers.get("x-report-timestamp", ""),
            signature_header=request.headers.get("x-report-signature", ""),
            body=body,
            skew_seconds=settings.timestamp_skew_seconds,
        )
        if not result.ok:
            logger.warning("auth rejected from %s: %s", client_ip, result.reason)
            return _json(401, {"error": "unauthorized"})

        content_type = request.headers.get("content-type", "")
        if "multipart/form-data" not in content_type:
            return _json(400, {"error": "expected multipart/form-data"})

        try:
            form = await request.form()
        except MultiPartException as exc:
            logger.warning("malformed multipart from %s: %s", client_ip, exc)
            return _json(400, {"error": "malformed multipart body"})
        except Exception as exc:  # noqa: BLE001 - last-resort guard: a bad body must never 500
            logger.warning("multipart parse failure from %s: %s", client_ip, exc)
            return _json(400, {"error": "malformed request body"})

        try:
            report_file = form.get("report")
            description = form.get("description")
            meta_raw = form.get("meta")

            if not isinstance(report_file, UploadFile):
                return _json(422, {"error": "missing 'report' file field"})
            if not isinstance(description, str) or not description.strip():
                return _json(422, {"error": "'description' is required and must be non-empty"})
            if not isinstance(meta_raw, str):
                return _json(422, {"error": "missing 'meta' field"})
            try:
                meta = json.loads(meta_raw)
            except json.JSONDecodeError:
                return _json(422, {"error": "'meta' is not valid JSON"})
            if not isinstance(meta, dict):
                return _json(422, {"error": "'meta' must be a JSON object"})

            zip_bytes = await report_file.read()
        finally:
            await form.close()

        return _store_report(
            settings=settings,
            zip_bytes=zip_bytes,
            description=description,
            meta=meta,
            client_ip=client_ip,
        )

    return app


def _store_report(
    *, settings: Settings, zip_bytes: bytes, description: str, meta: dict, client_ip: str
) -> JSONResponse:
    sha256 = hashlib.sha256(zip_bytes).hexdigest()
    received_at = datetime.now(timezone.utc)

    conn = db.connect(settings.db_path)
    try:
        existing = db.find_original_by_sha256(conn, sha256)
        if existing is not None:
            original_ulid = existing[0]
            dup_ulid = new_ulid()
            db.insert_report(
                conn,
                ulid=dup_ulid,
                received_at=received_at.isoformat(),
                sha256=sha256,
                size=len(zip_bytes),
                match_id=meta.get("match_id"),
                version=meta.get("version"),
                build=meta.get("build"),
                exit_code=_exit_code_str(meta),
                os_name=meta.get("os"),
                client_ip=client_ip,
                dup_of=original_ulid,
            )
            conn.commit()
            return _json(
                200,
                {
                    "status": "duplicate",
                    "ulid": dup_ulid,
                    "dup_of": original_ulid,
                    "sha256": sha256,
                },
            )

        ulid = new_ulid()
        target_dir = report_dir(settings.reports_dir, ulid, received_at)
        atomic_write(target_dir / "report.zip", zip_bytes)
        atomic_write(target_dir / "description.txt", description.encode("utf-8"))

        meta_out = dict(meta)
        meta_out["_received_at"] = received_at.isoformat()
        meta_out["_client_ip"] = client_ip
        meta_out["_sha256"] = sha256
        atomic_write(
            target_dir / "meta.json",
            json.dumps(meta_out, indent=2, sort_keys=True).encode("utf-8"),
        )

        db.insert_report(
            conn,
            ulid=ulid,
            received_at=received_at.isoformat(),
            sha256=sha256,
            size=len(zip_bytes),
            match_id=meta.get("match_id"),
            version=meta.get("version"),
            build=meta.get("build"),
            exit_code=_exit_code_str(meta),
            os_name=meta.get("os"),
            client_ip=client_ip,
            dup_of=None,
        )
        conn.commit()
    finally:
        conn.close()

    return _json(201, {"status": "ok", "ulid": ulid, "sha256": sha256})


def _exit_code_str(meta: dict) -> str | None:
    if "exit_code" not in meta:
        return None
    return str(meta["exit_code"])


def _client_ip(request: Request) -> str:
    forwarded = request.headers.get("x-forwarded-for")
    if forwarded:
        return forwarded.split(",")[0].strip()
    if request.client:
        return request.client.host
    return "unknown"


def _json(status_code: int, payload: dict) -> JSONResponse:
    return JSONResponse(payload, status_code=status_code)


# No eager module-level `app = create_app()` here on purpose: create_app() calls
# get_settings(), which reads TOKEN_FILE/REPORT_TOKEN from the environment and
# raises if neither is set -- eager construction would make merely *importing*
# this module (e.g. `from collector.app import create_app` in tests) fail in an
# unconfigured environment. Run it with uvicorn's factory mode instead, which
# calls create_app() only once the process is actually starting:
#   uvicorn collector.app:create_app --factory --host 0.0.0.0 --port 8000
# (see ../Dockerfile). Tests call create_app(settings) directly with their own
# Settings, never touching the environment at all.
