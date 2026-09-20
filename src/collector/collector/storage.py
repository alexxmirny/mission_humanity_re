"""On-disk report storage: `REPORTS_DIR/<YYYY-MM>/<ulid>/{report.zip,meta.json,description.txt}`.

Durability recipe per file: write to a temp file in the SAME directory (so the
final `os.replace` is an atomic rename on the same filesystem, never a cross-
device copy), fsync the file, replace, then fsync the containing directory so
the rename itself survives a crash. The directory fsync is a POSIX-only step;
on Windows (this repo's normal dev machine) there is no equivalent syscall
exposed the same way, so it is skipped there -- the deployed target is the
Linux container (Dockerfile), where it is active.
"""

from __future__ import annotations

import os
import tempfile
from datetime import datetime
from pathlib import Path


def month_dir(reports_dir: Path, when: datetime) -> Path:
    return reports_dir / when.strftime("%Y-%m")


def report_dir(reports_dir: Path, ulid: str, when: datetime) -> Path:
    return month_dir(reports_dir, when) / ulid


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(dir=str(path.parent), prefix=".tmp-", suffix=".part")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise
    _fsync_dir(path.parent)


def _fsync_dir(dir_path: Path) -> None:
    if os.name == "nt":
        # No directory-fsync equivalent via os.open()/os.fsync() on Windows; the
        # rename is still atomic here, just not additionally durability-synced.
        return
    dir_fd = os.open(str(dir_path), os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)
