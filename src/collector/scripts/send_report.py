#!/usr/bin/env python3
"""Manual test client for the collector's `POST /v1/reports` (tracker dist:RP2).

This is NOT the launcher's uploader -- RP1 (tracker `dist:RP1`) implements
that, in Rust, with a retry and keep-and-resend-on-failure policy. This script
exists so a human (or this task's own verification step) can exercise a
running collector without the launcher: send a zip, and see acceptance /
duplicate / 401 / 413 first-hand.

Depends only on the standard library, so it never needs a `pip install` to
run -- deliberately independent of both `tools/` (this repo's RE tooling) and
the `collector` package (so it also works as an outside client, the way the
real launcher will hit the HTTP API rather than importing server internals).

Examples:
  # Normal send against a local `docker compose up` stack (self-signed cert):
  python send_report.py --url https://localhost/v1/reports \\
      --token-file ../secrets/report_token.txt --insecure \\
      --zip /path/to/report.zip --description "crashed loading a custom map"

  # Against a bare `uvicorn` run (no Caddy, no TLS):
  python send_report.py --url http://127.0.0.1:8000/v1/reports \\
      --token-file ../secrets/report_token.txt \\
      --zip /path/to/report.zip --description "..."

  # Smoke-test the 401 path (deliberately wrong signature):
  python send_report.py --url ... --token-file ... --zip ... \\
      --description "..." --bad-signature
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import mimetypes
import ssl
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path


def read_token(path: Path) -> str:
    return path.read_text(encoding="utf-8").strip()


def sign(token: str, timestamp: int, body: bytes) -> str:
    """Mirrors collector/signing.py::sign -- kept duplicated on purpose (this
    script has zero third-party or repo-internal imports by design)."""
    body_hash = hashlib.sha256(body).hexdigest()
    message = f"{timestamp}\n{body_hash}".encode("ascii")
    return hmac.new(token.encode("utf-8"), message, hashlib.sha256).hexdigest()


def build_multipart(boundary: str, zip_path: Path, description: str, meta: dict) -> bytes:
    crlf = b"\r\n"
    parts: list[bytes] = []

    def field(name: str, value: str) -> None:
        parts.append(f"--{boundary}".encode())
        parts.append(f'Content-Disposition: form-data; name="{name}"'.encode())
        parts.append(b"")
        parts.append(value.encode("utf-8"))

    field("description", description)
    field("meta", json.dumps(meta))

    parts.append(f"--{boundary}".encode())
    parts.append(
        f'Content-Disposition: form-data; name="report"; filename="{zip_path.name}"'.encode()
    )
    content_type = mimetypes.guess_type(zip_path.name)[0] or "application/zip"
    parts.append(f"Content-Type: {content_type}".encode())
    parts.append(b"")
    parts.append(zip_path.read_bytes())
    parts.append(f"--{boundary}--".encode())
    parts.append(b"")

    return crlf.join(parts)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--url", required=True, help="collector endpoint, e.g. https://localhost/v1/reports"
    )
    ap.add_argument("--token-file", required=True, type=Path)
    ap.add_argument("--zip", required=True, type=Path, dest="zip_path")
    ap.add_argument("--description", required=True)
    ap.add_argument("--match-id", default=None, help="default: a fresh random UUID")
    ap.add_argument("--version", default="0.0.0-dev")
    ap.add_argument("--build", default="dev")
    ap.add_argument("--exit-code", default="0")
    ap.add_argument("--os", default=sys.platform, dest="os_name")
    ap.add_argument(
        "--insecure", action="store_true", help="skip TLS verification (self-signed cert, dev only)"
    )
    ap.add_argument(
        "--bad-signature",
        action="store_true",
        help="send a deliberately wrong HMAC signature, to smoke-test the collector's 401 path",
    )
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args(argv)

    token = read_token(args.token_file)
    meta = {
        "match_id": args.match_id or str(uuid.uuid4()),
        "version": args.version,
        "build": args.build,
        "exit_code": args.exit_code,
        "os": args.os_name,
    }

    boundary = uuid.uuid4().hex
    body = build_multipart(boundary, args.zip_path, args.description, meta)

    timestamp = int(time.time())
    signature = sign(token, timestamp, body)
    if args.bad_signature:
        signature = "0" * len(signature)

    headers = {
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "X-Report-Token": token,
        "X-Report-Timestamp": str(timestamp),
        "X-Report-Signature": signature,
    }

    ctx = ssl.create_default_context()
    if args.insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    request = urllib.request.Request(args.url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, context=ctx, timeout=args.timeout) as resp:
            print(resp.status, resp.read().decode("utf-8", "replace"))
            return 0
    except urllib.error.HTTPError as exc:
        print(exc.code, exc.read().decode("utf-8", "replace"))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
