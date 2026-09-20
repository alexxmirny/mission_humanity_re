"""done_when clause: a re-sent identical zip is recorded as a duplicate, not a
second file."""

from __future__ import annotations

from .conftest import TOKEN, auth_headers, default_meta, encode_multipart, make_zip_bytes


def test_resending_identical_zip_is_recorded_as_duplicate(client, settings):
    zip_bytes = make_zip_bytes(payload=b"identical crash log content")

    body1, content_type1 = encode_multipart(
        description="first send", meta=default_meta(), zip_bytes=zip_bytes
    )
    resp1 = client.post(
        "/v1/reports",
        content=body1,
        headers={"Content-Type": content_type1, **auth_headers(TOKEN, body1)},
    )
    assert resp1.status_code == 201, resp1.text
    original_ulid = resp1.json()["ulid"]

    # Re-send the SAME zip bytes (a fresh description/meta is realistic -- the
    # launcher may resend after a retry with a slightly different wrapper --
    # but the report content is byte-identical, which is what dedupe keys on).
    body2, content_type2 = encode_multipart(
        description="retry after timeout", meta=default_meta(), zip_bytes=zip_bytes
    )
    resp2 = client.post(
        "/v1/reports",
        content=body2,
        headers={"Content-Type": content_type2, **auth_headers(TOKEN, body2)},
    )
    assert resp2.status_code == 200, resp2.text
    payload2 = resp2.json()
    assert payload2["status"] == "duplicate"
    assert payload2["dup_of"] == original_ulid
    assert payload2["ulid"] != original_ulid  # its own row, no file of its own

    # Only ONE report.zip on disk, and /healthz's dedupe counters agree.
    all_zips = list(settings.reports_dir.glob("**/report.zip"))
    assert len(all_zips) == 1
    assert all_zips[0].read_bytes() == zip_bytes

    health = client.get("/healthz").json()
    assert health["reports_total"] == 1
    assert health["duplicates_total"] == 1


def test_a_third_resend_still_points_at_the_original(client):
    zip_bytes = make_zip_bytes(payload=b"same content, three times")
    original_ulid = None
    for i in range(3):
        body, content_type = encode_multipart(
            description=f"send #{i}", meta=default_meta(), zip_bytes=zip_bytes
        )
        resp = client.post(
            "/v1/reports",
            content=body,
            headers={"Content-Type": content_type, **auth_headers(TOKEN, body)},
        )
        if i == 0:
            assert resp.status_code == 201
            original_ulid = resp.json()["ulid"]
        else:
            assert resp.status_code == 200
            assert resp.json()["status"] == "duplicate"
            assert resp.json()["dup_of"] == original_ulid

    health = client.get("/healthz").json()
    assert health["reports_total"] == 1
    assert health["duplicates_total"] == 2
