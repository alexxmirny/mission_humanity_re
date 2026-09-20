"""SQLite report index: `REPORTS_DIR/index.db`.

One row per accepted POST, including duplicates (a duplicate row carries
`dup_of` = the ulid of the original and never has its own files on disk --
`sha256` is still recorded so a *third* re-send of the same content also
resolves as a duplicate of the original, not of the second row).
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS reports (
    ulid TEXT PRIMARY KEY,
    received_at TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL,
    match_id TEXT,
    version TEXT,
    build TEXT,
    exit_code TEXT,
    os TEXT,
    client_ip TEXT,
    dup_of TEXT
);
CREATE INDEX IF NOT EXISTS idx_reports_sha256 ON reports(sha256);
CREATE INDEX IF NOT EXISTS idx_reports_received_at ON reports(received_at);
"""


def connect(db_path: Path) -> sqlite3.Connection:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(db_path), timeout=30, isolation_level=None)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=FULL")
    conn.executescript(SCHEMA)
    return conn


def find_original_by_sha256(conn: sqlite3.Connection, sha256: str) -> tuple[str] | None:
    """The ulid of the first (non-duplicate) row with this sha256, if any."""
    row = conn.execute(
        "SELECT ulid FROM reports WHERE sha256 = ? AND dup_of IS NULL ORDER BY received_at LIMIT 1",
        (sha256,),
    ).fetchone()
    return row


def insert_report(
    conn: sqlite3.Connection,
    *,
    ulid: str,
    received_at: str,
    sha256: str,
    size: int,
    match_id: str | None,
    version: str | None,
    build: str | None,
    exit_code: str | None,
    os_name: str | None,
    client_ip: str | None,
    dup_of: str | None,
) -> None:
    conn.execute(
        "INSERT INTO reports "
        "(ulid, received_at, sha256, size, match_id, version, build, exit_code, os, client_ip, dup_of) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            ulid,
            received_at,
            sha256,
            size,
            match_id,
            version,
            build,
            exit_code,
            os_name,
            client_ip,
            dup_of,
        ),
    )


def counts(conn: sqlite3.Connection) -> tuple[int, int]:
    total = conn.execute("SELECT COUNT(*) FROM reports WHERE dup_of IS NULL").fetchone()[0]
    dups = conn.execute("SELECT COUNT(*) FROM reports WHERE dup_of IS NOT NULL").fetchone()[0]
    return total, dups


def rows_older_than(conn: sqlite3.Connection, cutoff_iso: str) -> list[sqlite3.Row]:
    return conn.execute(
        "SELECT ulid, received_at, dup_of FROM reports WHERE received_at < ?", (cutoff_iso,)
    ).fetchall()


def delete_older_than(conn: sqlite3.Connection, cutoff_iso: str) -> None:
    conn.execute("DELETE FROM reports WHERE received_at < ?", (cutoff_iso,))
