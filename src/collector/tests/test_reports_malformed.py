"""done_when clause: the container survives a malformed multipart (no worker
death).

Proven here as: the handler returns a clean 4xx for garbage that passes auth
but fails to parse as multipart, AND the same TestClient (same app instance,
same process) keeps answering requests correctly afterward -- i.e. nothing in
the request handling knocked the app over.
"""

from __future__ import annotations

from .conftest import TOKEN, auth_headers, default_meta, encode_multipart, make_zip_bytes


def test_truncated_multipart_body_is_4xx_not_500(client):
    good_body, content_type = encode_multipart(
        description="valid shape", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    truncated = good_body[: len(good_body) // 2]  # cut mid-part, no closing boundary

    resp = client.post(
        "/v1/reports",
        content=truncated,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, truncated)},
    )

    assert 400 <= resp.status_code < 500, resp.text


def test_garbage_body_with_multipart_content_type_is_4xx_not_500(client):
    garbage = b"this is not multipart data at all, just some bytes\x00\x01\x02" * 100
    content_type = "multipart/form-data; boundary=not-actually-present"

    resp = client.post(
        "/v1/reports",
        content=garbage,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, garbage)},
    )

    assert 400 <= resp.status_code < 500, resp.text


def test_wrong_content_type_for_multipart_endpoint_is_4xx(client):
    body, _content_type = encode_multipart(
        description="looks right, labeled wrong", meta=default_meta(), zip_bytes=make_zip_bytes()
    )

    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": "application/json", **auth_headers(TOKEN, body)},
    )

    assert 400 <= resp.status_code < 500, resp.text


def test_app_stays_alive_after_a_string_of_malformed_requests(client):
    """No worker death: hammer the endpoint with several different malformed
    bodies, then confirm the very same client/app can still complete a normal
    request end to end."""
    garbage_bodies = [
        b"",
        b"not multipart",
        b"--boundary\r\nincomplete" * 50,
        b"\xff\xfe\x00" * 1000,
    ]
    for garbage in garbage_bodies:
        headers = auth_headers(TOKEN, garbage)
        resp = client.post(
            "/v1/reports",
            content=garbage,
            headers={"Content-Type": "multipart/form-data; boundary=x", **headers},
        )
        assert resp.status_code < 500, (garbage, resp.status_code, resp.text)

    # The app is still fully functional: healthz responds, and a real, valid
    # report upload still succeeds.
    assert client.get("/healthz").status_code == 200

    body, content_type = encode_multipart(
        description="proves the app survived", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    resp = client.post(
        "/v1/reports",
        content=body,
        headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
    )
    assert resp.status_code == 201, resp.text
