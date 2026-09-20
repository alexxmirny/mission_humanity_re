"""`python -m collector.cli prune --days 180` -- the retention job (plan D14:
~180 days). Deletes report directories (and their index rows) whose
`received_at` is older than the cutoff. Duplicate rows have no directory of
their own, so they are removed from the index only.
"""

from __future__ import annotations

import argparse
import shutil
from datetime import datetime, timedelta, timezone

from . import db
from .config import get_settings


def prune(days: int) -> int:
    settings = get_settings()
    cutoff = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()
    conn = db.connect(settings.db_path)
    try:
        rows = db.rows_older_than(conn, cutoff)
        for ulid, received_at, dup_of in rows:
            if dup_of is None:
                when = datetime.fromisoformat(received_at)
                target = settings.reports_dir / when.strftime("%Y-%m") / ulid
                if target.exists():
                    shutil.rmtree(target)
        db.delete_older_than(conn, cutoff)
        conn.commit()
    finally:
        conn.close()
    return len(rows)


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="collector", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    prune_parser = sub.add_parser("prune", help="delete reports older than N days")
    prune_parser.add_argument("--days", type=int, default=180)
    args = parser.parse_args(argv)

    if args.command == "prune":
        removed = prune(args.days)
        print(f"pruned {removed} report row(s) older than {args.days} days")


if __name__ == "__main__":
    main()
