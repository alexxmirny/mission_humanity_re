"""done_when clause: a request with a bad HMAC gets 401 and writes nothing.

Also covers the adjacent auth failure modes (wrong token, stale timestamp)
since they share the same "401, nothing written" contract, and the ordering
requirement: auth must be checked -- and fail closed -- before any multipart
parsing or storage write happens.
"""

from __future__ import annotations

from .conftest import TOKEN, auth_headers, default_meta, encode_multipart, make_zip_bytes


def _assert_nothing_written(client, settings):
    assert client.get("/healthz").json()["reports_total"] == 0
    assert list(settings.reports_dir.glob("**/report.zip")) == []


def test_bad_signature_is_401_and_writes_nothing(client, settings):
    body, content_type = encode_multipart(
        description="should never be stored", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    headers = auth_headers(TOKEN, body)
    headers["X-Report-Signature"] = "0" * 64  # syntactically valid hex, wrong value

    resp = client.post(
        "/v1/reports", content=body, headers={"Content-Type": content_type, **headers}
    )

    assert resp.status_code == 401
    _assert_nothing_written(client, settings)


def test_wrong_token_is_401_and_writes_nothing(client, settings):
    body, content_type = encode_multipart(
        description="wrong token", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    # Signed correctly, but with a DIFFERENT token than the server is configured
    # with -- both the token header and the signature it produced are "valid"
    # from the client's point of view, just not for this server.
    headers = auth_headers("some-other-token", body)

    resp = client.post(
        "/v1/reports", content=body, headers={"Content-Type": content_type, **headers}
    )

    assert resp.status_code == 401
    _assert_nothing_written(client, settings)


def test_missing_token_header_is_401(client, settings):
    body, content_type = encode_multipart(
        description="no token at all", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    headers = auth_headers(TOKEN, body)
    del headers["X-Report-Token"]

    resp = client.post(
        "/v1/reports", content=body, headers={"Content-Type": content_type, **headers}
    )

    assert resp.status_code == 401
    _assert_nothing_written(client, settings)


def test_stale_timestamp_is_401_and_writes_nothing(client, settings):
    body, content_type = encode_multipart(
        description="too old", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    stale_timestamp = 1_000_000  # 1970, way outside any sane skew window
    headers = auth_headers(TOKEN, body, timestamp=stale_timestamp)

    resp = client.post(
        "/v1/reports", content=body, headers={"Content-Type": content_type, **headers}
    )

    assert resp.status_code == 401
    _assert_nothing_written(client, settings)


def test_signature_over_a_different_body_than_what_was_sent_is_401(client, settings):
    real_body, content_type = encode_multipart(
        description="tampered in transit", meta=default_meta(), zip_bytes=make_zip_bytes()
    )
    other_body, _ = encode_multipart(
        description="a different payload entirely",
        meta=default_meta(),
        zip_bytes=make_zip_bytes(payload=b"different"),
    )
    # Sign the OTHER body, then send the real one -- simulates a body tampered
    # with in flight (or a signature computed over the wrong bytes).
    headers = auth_headers(TOKEN, other_body)

    resp = client.post(
        "/v1/reports", content=real_body, headers={"Content-Type": content_type, **headers}
    )

    assert resp.status_code == 401
    _assert_nothing_written(client, settings)
