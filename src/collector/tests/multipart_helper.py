"""Hand-built multipart/form-data encoding for the test suite.

We need the *exact* raw request body bytes to sign (the collector's HMAC
covers the whole body -- see collector/signing.py), so tests cannot use
httpx's `files=`/`data=` convenience kwargs (they build the body internally,
after we'd need to have already signed it). This mirrors the encoder in
../scripts/send_report.py on purpose -- both sides of the wire format agree
because both are this small and both read the same way.
"""

from __future__ import annotations

import json
import uuid


def encode_multipart(
    *,
    description: str,
    meta: dict,
    zip_bytes: bytes,
    filename: str = "report.zip",
    boundary: str | None = None,
    extra_fields: dict[str, str] | None = None,
    omit_report: bool = False,
) -> tuple[bytes, str]:
    boundary = boundary or uuid.uuid4().hex
    crlf = b"\r\n"
    parts: list[bytes] = []

    def field(name: str, value: str) -> None:
        parts.append(f"--{boundary}".encode())
        parts.append(f'Content-Disposition: form-data; name="{name}"'.encode())
        parts.append(b"")
        parts.append(value.encode("utf-8"))

    field("description", description)
    field("meta", json.dumps(meta))
    for key, value in (extra_fields or {}).items():
        field(key, value)

    if not omit_report:
        parts.append(f"--{boundary}".encode())
        parts.append(
            f'Content-Disposition: form-data; name="report"; filename="{filename}"'.encode()
        )
        parts.append(b"Content-Type: application/zip")
        parts.append(b"")
        parts.append(zip_bytes)

    parts.append(f"--{boundary}--".encode())
    parts.append(b"")

    body = crlf.join(parts)
    content_type = f"multipart/form-data; boundary={boundary}"
    return body, content_type
