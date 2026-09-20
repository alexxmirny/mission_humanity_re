"""done_when clause: `/healthz` is 200."""

from __future__ import annotations


def test_healthz_ok(client):
    resp = client.get("/healthz")
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ok"
    assert body["reports_total"] == 0
    assert body["duplicates_total"] == 0


def test_healthz_counts_reflect_accepted_reports(client):
    from .conftest import TOKEN, auth_headers, default_meta, encode_multipart, make_zip_bytes

    body, content_type = encode_multipart(
        description="crash on load", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )
    assert resp.status_code == 201

    health = client.get("/healthz").json()
    assert health["reports_total"] == 1
    assert health["duplicates_total"] == 0
