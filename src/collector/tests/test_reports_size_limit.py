"""done_when clause: a 70 MB body gets 413 before the app sees it.

Proven at the app layer here (no Caddy in this test process): the 70 MB body
must be rejected by `MaxBodySizeMiddleware` (middleware.py) before the
request handler -- and therefore before any auth check or storage write --
ever runs. We assert both the status code AND that nothing was written.
"""

from __future__ import annotations

from .conftest import TOKEN, auth_headers, default_meta, encode_multipart, make_zip_bytes

SEVENTY_MB = 70 * 1024 * 1024


def test_seventy_mb_body_gets_413_before_the_app_sees_it(client, settings):
    zip_bytes = make_zip_bytes(size=SEVENTY_MB)
    body, content_type = encode_multipart(
        description="oversized report", meta=default_meta(), zip_bytes=zip_bytes
    )
    assert len(body) > 64 * 1024 * 1024  # sanity: this really does exceed the 64 MB cap

    # A deliberately WRONG signature: if the middleware runs first (as required),
    # the request never reaches auth, so this must still be 413, not 401.
    bad_headers = auth_headers(TOKEN, body)
    bad_headers["X-Report-Signature"] = "0" * 64

    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **bad_headers},
    )

    assert resp.status_code == 413, resp.text
    assert client.get("/healthz").json()["reports_total"] == 0
    # No report.zip written anywhere -- only the index.db (+ its WAL sidecars,
    # created by the healthz check above) may legitimately exist under REPORTS_DIR.
    assert list(settings.reports_dir.glob("**/report.zip")) == []


def test_body_just_under_the_cap_is_not_rejected_by_size(client):
    # 63 MB zip -> total multipart body still comfortably under the 64 MB cap.
    zip_bytes = make_zip_bytes(size=63 * 1024 * 1024)
    body, content_type = encode_multipart(
        description="right at the edge", meta=default_meta(), zip_bytes=zip_bytes
    )
    assert len(body) < 64 * 1024 * 1024

    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )
    assert resp.status_code == 201, resp.text
