"""The per-run log directory NAMES mh.dll writes -- parse, classify, sort (SES1 + SES8).

mh.dll writes one PROCESS directory at boot and one SESSION directory per match under
`<exedir>\\logs\\` (or MH_LOG_ROOT). The C++ that names them is
src/mh_dll/mh_common/include/mh_session_dir.h; this module is the one Python reading of that
contract for the runners (ui_test / test_ui / mp_run / mp_analyze / ...). Three generations of names
coexist in a lane's `logs/`, because a lane keeps its history across builds:

    SES8 (2026-09-29, current)
        process  2026-09-29T08-15-02Z_menu_<role>
        session  2026-09-29T08-15-02Z_<mid8>_<map>_<mode>
                 mode = host | client (a lobby) or campaign | tutorial | skirmish | tactical
    SES1 (2026-09-17 .. 2026-09-28)
        process  20260917T164346Z_menu_<role>
        session  20260917T164346Z_<mid8>_<slot>_<role>
    pre-SES1
        process  20260911_061348_<role>

All stamps are UTC. THE TRAP THIS MODULE EXISTS FOR: a plain string sort does NOT order a mix of
SES8 and SES1 names by time -- `-` (0x2D) sorts before `0` (0x30), so every 2026-09-17 SES1 folder
sorts AFTER a 2026-09-29 SES8 one and "the newest directory" silently becomes an old one. Order by
`sort_key()` (the stamp normalised to digits), never by the raw name.

The single-purpose checkers (`check_*.py`) deliberately carry their own copies of SESSION_DIR_RE and
canon() instead of importing this -- that tree has no import chain between checkers, so one fix
cannot silently reach the others. Keep those copies textually identical to the two below.
"""

import json
import os
import re

# The literal every checker copies. Old branch: <stamp>_<mid8>_<slot>_<role>; new branch:
# <dirstamp>_<mid8>_<map>_<mode> (map token is [A-Za-z0-9.-], never `_`).
SESSION_DIR_RE = re.compile(
    r"^(?:\d{8}T\d{6}Z_[0-9a-f]{8}_\d+|\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z_[0-9a-f]{8}_[A-Za-z0-9.-]+)_[A-Za-z0-9]+$"
)
_NEW_STAMP = re.compile(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2})-(\d{2})-(\d{2})Z")


def canon(name):
    """The name with an SES8 stamp rewritten to the SES1 compact form, so the two sort together.

    `2026-09-29T08-15-02Z_ab12cd34_x_host` -> `20260929T081502Z_ab12cd34_x_host`. Any other name
    is returned unchanged. (Checkers carry this one-liner verbatim.)"""
    return _NEW_STAMP.sub(r"\1\2\3T\4\5\6Z", name)


_PARSE = [
    (
        "session",
        re.compile(
            r"^(?P<stamp>\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z)_(?P<id>[0-9a-f]{8})_(?P<map>[A-Za-z0-9.-]+)_(?P<mode>[a-z]+)$"
        ),
    ),
    (
        "process",
        re.compile(
            r"^(?P<stamp>\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z)_menu_(?P<role>[A-Za-z0-9]+)$"
        ),
    ),
    (
        "session",
        re.compile(
            r"^(?P<stamp>\d{8}T\d{6}Z)_(?P<id>[0-9a-f]{8})_(?P<slot>\d+)_(?P<role>[A-Za-z0-9]+)$"
        ),
    ),
    ("process", re.compile(r"^(?P<stamp>\d{8}T\d{6}Z)_menu_(?P<role>[A-Za-z0-9]+)$")),
    ("process", re.compile(r"^(?P<stamp>\d{8}_\d{6})_(?P<role>[A-Za-z0-9]+)$")),  # pre-SES1
]


def parse(name):
    """{kind: 'process'|'session', stamp, key, id, map, mode, role, slot} or None (not a run dir).

    `mode` is the SES8 match mode; for an SES1 session it falls back to the role (what that
    generation put in the last field). `map` / `slot` are None where the generation had none."""
    name = os.path.basename(os.path.normpath(name))
    for kind, rx in _PARSE:
        m = rx.match(name)
        if not m:
            continue
        d = m.groupdict()
        out = {
            "kind": kind,
            "stamp": d["stamp"],
            "key": re.sub(r"\D", "", d["stamp"]),
            "id": d.get("id"),
            "map": d.get("map"),
            "role": d.get("role"),
            "slot": int(d["slot"]) if d.get("slot") is not None else None,
        }
        out["mode"] = d.get("mode") or (d.get("role") if kind == "session" else None)
        return out
    return None


def is_process_dir(name):
    p = parse(name)
    return bool(p) and p["kind"] == "process"


def is_session_dir(name):
    p = parse(name)
    return bool(p) and p["kind"] == "session"


def sort_key(path):
    """Time order across all three generations; unparsable names sort first, by name."""
    base = os.path.basename(os.path.normpath(path))
    p = parse(base)
    return ((p["key"] if p else ""), canon(base))


def run_dirs(logs_dir):
    """Every run directory under `logs_dir`, oldest first (time order, not name order)."""
    try:
        names = os.listdir(logs_dir)
    except OSError:
        return []
    out = [
        os.path.join(logs_dir, n)
        for n in names
        if parse(n) and os.path.isdir(os.path.join(logs_dir, n))
    ]
    return sorted(out, key=sort_key)


def newest_process_dir(logs_dir, role=None):
    """The newest PROCESS ("menu") directory, optionally of one boot role; None if there is none."""
    c = [
        d
        for d in run_dirs(logs_dir)
        if is_process_dir(d) and (role is None or parse(d)["role"] == role)
    ]
    return c[-1] if c else None


def read_session_json(folder):
    try:
        with open(os.path.join(folder, "session.json"), encoding="utf-8", errors="replace") as fh:
            v = json.load(fh)
        return v if isinstance(v, dict) else None
    except (OSError, ValueError):
        return None


def sessions_of(process_dir):
    """The SESSION directories hanging off one process directory, oldest first.

    Membership is session.json's `process_dir` naming the process leaf -- a lane's `logs/` also holds
    other processes' sessions, and time order alone cannot tell whose they are."""
    process_dir = os.path.normpath(process_dir)
    leaf = os.path.basename(process_dir)
    return [
        d
        for d in run_dirs(os.path.dirname(process_dir))
        if is_session_dir(d) and (read_session_json(d) or {}).get("process_dir") == leaf
    ]


def _selftest():
    fails = []

    def check(what, ok):
        if not ok:
            fails.append(what)

    new_p = "2026-09-29T08-15-02Z_menu_solo"
    new_s = "2026-09-29T08-15-09Z_ab12cd34_blue-monday_host"
    old_p = "20260917T164300Z_menu_solo"
    old_s = "20260917T164346Z_dedd707c_1_client"
    leg = "20260911_061348_solo"
    check(
        "SES8 process parses", parse(new_p)["kind"] == "process" and parse(new_p)["role"] == "solo"
    )
    s = parse(new_s)
    check(
        "SES8 session parses",
        s["kind"] == "session"
        and s["id"] == "ab12cd34"
        and s["map"] == "blue-monday"
        and s["mode"] == "host",
    )
    o = parse(old_s)
    check(
        "SES1 session parses", o["kind"] == "session" and o["slot"] == 1 and o["mode"] == "client"
    )
    check("pre-SES1 parses as a process dir", parse(leg)["kind"] == "process")
    check("junk is not a run dir", parse("logs") is None and parse("2026-09-29_x") is None)
    check(
        "SESSION_DIR_RE: both session shapes",
        bool(SESSION_DIR_RE.match(new_s)) and bool(SESSION_DIR_RE.match(old_s)),
    )
    check(
        "SESSION_DIR_RE: never a process dir",
        not SESSION_DIR_RE.match(new_p) and not SESSION_DIR_RE.match(old_p),
    )
    check(
        "canon() folds the SES8 stamp", canon(new_s) == "20260929T081509Z_ab12cd34_blue-monday_host"
    )
    check("canon() leaves SES1 alone", canon(old_s) == old_s)
    mixed = [new_s, old_s, new_p, leg, old_p]
    check("raw sort gets the mix WRONG (the trap)", sorted(mixed)[-1] != new_s)
    check(
        "sort_key orders the mix by time",
        sorted(mixed, key=sort_key) == [leg, old_p, old_s, new_p, new_s],
    )
    return fails


if __name__ == "__main__":
    import sys

    if "--selftest" in sys.argv:
        f = _selftest()
        for x in f:
            print("FAIL:", x)
        print("_rundir selftest: %s" % ("PASS" if not f else "FAIL (%d)" % len(f)))
        sys.exit(1 if f else 0)
    for a in sys.argv[1:]:
        print(a, parse(a))
