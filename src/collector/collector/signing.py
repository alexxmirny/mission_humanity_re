"""The HMAC scheme shared by the server (auth.py) and any client that talks to
it (scripts/send_report.py, tests). Kept in one place so the two sides cannot
silently drift.

Signature = hex HMAC-SHA256, keyed by the shared report token, over the
ASCII string `f"{timestamp}\\n{sha256_hex(body)}"`, where `body` is the raw
request body (the whole multipart payload) and `timestamp` is the same string
sent in `X-Report-Timestamp` (unix seconds, as text).
"""

from __future__ import annotations

import hashlib
import hmac


def sign(token: str, timestamp: str | int, body: bytes) -> str:
    body_hash = hashlib.sha256(body).hexdigest()
    message = f"{timestamp}\n{body_hash}".encode("ascii")
    return hmac.new(token.encode("utf-8"), message, hashlib.sha256).hexdigest()
