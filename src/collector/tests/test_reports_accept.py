"""done_when clause: a 40 MB zip is accepted and indexed."""

from __future__ import annotations

from .conftest import (
    TOKEN,
    auth_headers,
    default_meta,
    encode_multipart,
    make_zip_bytes,
    sha256_hex,
)

FORTY_MB = 40 * 1024 * 1024


def test_forty_mb_zip_is_accepted_and_indexed(client, settings):
    zip_bytes = make_zip_bytes(size=FORTY_MB)
    assert len(zip_bytes) > FORTY_MB  # sanity: the padding actually landed in the zip

    meta = default_meta()
    body, content_type = encode_multipart(
        description="40 MB crash zip", meta=meta, zip_bytes=zip_bytes
    )
    assert len(body) < 64 * 1024 * 1024  # under the 64 MB cap, so this must NOT trip the 413 path

    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )

    assert resp.status_code == 201, resp.text
    payload = resp.json()
    assert payload["status"] == "ok"
    assert payload["sha256"] == sha256_hex(zip_bytes)
    ulid = payload["ulid"]

    # Indexed: /healthz counts it, AND the file is really on disk under the
    # ulid path with the exact bytes that were sent (storage.py's atomic write).
    health = client.get("/healthz").json()
    assert health["reports_total"] == 1

    month_dirs = [p for p in settings.reports_dir.glob("*") if p.is_dir()]
    assert len(month_dirs) == 1
    report_dir = month_dirs[0] / ulid
    assert report_dir.is_dir()
    stored_zip = report_dir / "report.zip"
    assert stored_zip.exists()
    assert stored_zip.read_bytes() == zip_bytes
    assert (report_dir / "description.txt").read_text(encoding="utf-8") == "40 MB crash zip"

    meta_on_disk = (report_dir / "meta.json").read_text(encoding="utf-8")
    assert meta["match_id"] in meta_on_disk


def test_small_zip_round_trips_meta_fields(client):
    meta = default_meta()
    body, content_type = encode_multipart(
        description="minor UI glitch", meta=meta, zip_bytes=make_zip_bytes()
    )
    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )
    assert resp.status_code == 201, resp.text


def test_missing_report_field_is_rejected(client):
    body, content_type = encode_multipart(
        description="no file attached", meta=default_meta(), zip_bytes=b"", omit_report=True
    )
    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )
    assert resp.status_code == 422


def test_empty_description_is_rejected(client):
    body, content_type = encode_multipart(
        description="", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )
    assert resp.status_code == 422
