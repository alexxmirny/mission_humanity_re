"""`collector prune --days N` -- the retention job (plan D14: ~180 days)."""

from __future__ import annotations

import json
from datetime import datetime, timedelta, timezone

from collector.cli import prune
from collector.config import Settings
from collector.storage import atomic_write, report_dir
from collector.ulid import new_ulid

from collector import db


def _write_fake_report(settings: Settings, *, received_at: datetime, sha256: str) -> str:
    ulid = new_ulid()
    target = report_dir(settings.reports_dir, ulid, received_at)
    atomic_write(target / "report.zip", b"fake zip bytes")
    atomic_write(target / "description.txt", b"fake description")
    atomic_write(target / "meta.json", json.dumps({"match_id": "m"}).encode())

    conn = db.connect(settings.db_path)
    try:
        db.insert_report(
            conn,
            ulid=ulid,
            received_at=received_at.isoformat(),
            sha256=sha256,
            size=14,
            match_id="m",
            version="1.0",
            build="b",
            exit_code="0",
            os_name="win",
            client_ip="127.0.0.1",
            dup_of=None,
        )
        conn.commit()
    finally:
        conn.close()
    return ulid


def test_prune_removes_only_older_than_cutoff(tmp_path, monkeypatch):
    reports_dir = tmp_path / "reports"
    settings = Settings(
        reports_dir=reports_dir,
        token="t",
        max_body_mb=64,
        rate_per_min=10,
        timestamp_skew_seconds=600,
    )
    monkeypatch.setenv("REPORTS_DIR", str(reports_dir))
    monkeypatch.setenv("REPORT_TOKEN", "t")

    now = datetime.now(timezone.utc)
    old_ulid = _write_fake_report(settings, received_at=now - timedelta(days=200), sha256="aaa")
    recent_ulid = _write_fake_report(settings, received_at=now - timedelta(days=1), sha256="bbb")

    removed = prune(days=180)
    assert removed == 1

    old_dir = report_dir(reports_dir, old_ulid, now - timedelta(days=200))
    recent_dir = report_dir(reports_dir, recent_ulid, now - timedelta(days=1))
    assert not old_dir.exists()
    assert recent_dir.exists()

    conn = db.connect(settings.db_path)
    try:
        rows = conn.execute("SELECT ulid FROM reports").fetchall()
    finally:
        conn.close()
    assert [r[0] for r in rows] == [recent_ulid]
