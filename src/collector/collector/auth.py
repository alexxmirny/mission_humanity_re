"""Request authentication: shared token + timestamped HMAC (plan D13/D14).

Every check here must run, and must fail closed to 401, BEFORE the handler
writes anything to disk or the SQLite index -- that ordering is the point:
`POST /v1/reports` calls `verify_request` right after reading the raw body,
and only proceeds to parse the multipart form (let alone touch storage) once
this returns ok=True.
"""

from __future__ import annotations

import hmac
import time
from dataclasses import dataclass

from .signing import sign


@dataclass(frozen=True)
class AuthResult:
    ok: bool
    reason: str


def verify_request(
    *,
    token: str,
    header_token: str | None,
    timestamp_header: str | None,
    signature_header: str | None,
    body: bytes,
    skew_seconds: int,
    now: int | None = None,
) -> AuthResult:
    if not header_token or not hmac.compare_digest(header_token, token):
        return AuthResult(False, "bad token")

    if not timestamp_header or not _looks_like_unix_seconds(timestamp_header):
        return AuthResult(False, "bad or missing timestamp")

    ts = int(timestamp_header)
    current = now if now is not None else int(time.time())
    if abs(current - ts) > skew_seconds:
        return AuthResult(False, "timestamp outside the allowed skew")

    if not signature_header:
        return AuthResult(False, "missing signature")

    expected = sign(token, timestamp_header, body)
    if not hmac.compare_digest(signature_header.lower().strip(), expected):
        return AuthResult(False, "signature mismatch")

    return AuthResult(True, "ok")


def _looks_like_unix_seconds(value: str) -> bool:
    value = value.strip()
    if not value or (value[0] == "-" and not value[1:].isdigit()):
        return False
    return value.lstrip("-").isdigit()
