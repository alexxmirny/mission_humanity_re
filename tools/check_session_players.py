#!/usr/bin/env python3
"""check_session_players.py -- dist RL15: DO THE TWO PEERS' session.json AGREE ON WHO PLAYED?

session.json's `players` (human names in slot order) and `ai_count` feed the launcher's match list.
The host learns every name from the JOIN frames; a client did not, and wrote its OWN name for both
slots (`[client, client]`, later `[client]` once the lobby was torn down). mh.dll now reads the names
from the lobby slot snapshot the host broadcasts, so both peers must record the SAME list.

Usage:  check_session_players.py <host process run dir>     (test_ui: the row's post_check hand-over)
        check_session_players.py --selftest

The client lane is the host lane's `_c1` sibling (test_ui's lane naming). The match is the host's
newest session directory; the client's session is the one with the same `match_id`.
Requires: >= 2 players, all names non-empty and distinct, the two `players` lists equal, `ai_count`
equal. ABSENCE IS A FAILURE: a missing lane, session or field is RED, never a pass.
Exit 0 = PASS, 1 = the peers disagree, 2 = REFUSED (the evidence is not there).
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _rundir  # noqa: E402


class Refusal(Exception):
    pass


def _lane_of(run_dir):
    logs = os.path.dirname(os.path.abspath(run_dir))
    if os.path.basename(logs).lower() != "logs":
        raise Refusal("%s is not <lane>/logs/<run>" % run_dir)
    return os.path.dirname(logs)


def _client_lane(host_lane):
    base = os.path.basename(host_lane)
    if not base.endswith("_host"):
        raise Refusal("host lane %r does not end in '_host'" % base)
    cand = os.path.join(os.path.dirname(host_lane), base[: -len("_host")] + "_c1")
    if not os.path.isdir(cand):
        raise Refusal("client lane %s not found" % cand)
    return cand


def _sessions(lane):
    """[(dir, session.json dict)], oldest first, of the lane's SESSION directories."""
    logs = os.path.join(lane, "logs")
    out = []
    for d in _rundir.run_dirs(logs):
        if _rundir.is_session_dir(os.path.basename(d)):
            doc = _rundir.read_session_json(d)
            if doc:
                out.append((d, doc))
    return out


def check(host_run_dir):
    host_lane = _lane_of(host_run_dir)
    client_lane = _client_lane(host_lane)
    hs = _sessions(host_lane)
    if not hs:
        raise Refusal("no session.json in %s" % host_lane)
    hdoc = hs[-1][1]
    mid = hdoc.get("match_id")
    if not mid:
        raise Refusal("the host's newest session.json has no match_id")
    cs = [doc for _, doc in _sessions(client_lane) if doc.get("match_id") == mid]
    if not cs:
        raise Refusal("the client lane has no session for match %s" % mid)
    return compare(hdoc, cs[-1])


def compare(h, c):
    probs = []
    for tag, doc in (("host", h), ("client", c)):
        pl = doc.get("players")
        if not isinstance(pl, list) or not isinstance(doc.get("ai_count"), int):
            probs.append("%s: players/ai_count missing or malformed" % tag)
            continue
        if len(pl) < 2:
            probs.append("%s: players %r, want >= 2 humans" % (tag, pl))
        if any((not isinstance(n, str)) or not n.strip() for n in pl):
            probs.append("%s: an empty player name in %r" % (tag, pl))
        if len(set(pl)) != len(pl):
            probs.append("%s: duplicate names in %r (the client-twice shape)" % (tag, pl))
    if not probs:
        if h["players"] != c["players"]:
            probs.append("players differ: host %r, client %r" % (h["players"], c["players"]))
        if h["ai_count"] != c["ai_count"]:
            probs.append("ai_count differs: host %r, client %r" % (h["ai_count"], c["ai_count"]))
    return probs


def selftest():
    fails = []

    def ok(what, cond):
        if not cond:
            fails.append(what)

    g = {"players": ["host", "client"], "ai_count": 0}
    ok("agree -> green", compare(g, dict(g)) == [])
    ok("client twice -> red", bool(compare(g, {"players": ["client", "client"], "ai_count": 0})))
    ok("client only -> red", bool(compare(g, {"players": ["client"], "ai_count": 0})))
    ok("ai differs -> red", bool(compare(g, {"players": ["host", "client"], "ai_count": 1})))
    ok("order differs -> red", bool(compare(g, {"players": ["client", "host"], "ai_count": 0})))
    ok("missing field -> red", bool(compare(g, {"ai_count": 0})))
    ok("empty name -> red", bool(compare({"players": ["host", ""], "ai_count": 0}, g)))
    root = tempfile.mkdtemp()
    try:
        for lane, pl in (("ui_t_host", ["host", "client"]), ("ui_t_c1", ["host", "client"])):
            d = os.path.join(root, lane, "logs", "2026-10-12T10-00-00Z_ab12cd34_map_host")
            os.makedirs(d)
            with open(os.path.join(d, "session.json"), "w", encoding="utf-8") as f:
                json.dump({"match_id": "m1", "players": pl, "ai_count": 0, "process_dir": "x"}, f)
        menu = os.path.join(root, "ui_t_host", "logs", "2026-10-12T09-59-00Z_menu_solo")
        os.makedirs(menu)
        try:
            ok("planted pair green", check(menu) == [])
        except Refusal as e:
            fails.append("planted pair refused: %s" % e)
        os.remove(
            os.path.join(
                root, "ui_t_c1", "logs", "2026-10-12T10-00-00Z_ab12cd34_map_host", "session.json"
            )
        )
        try:
            check(menu)
            fails.append("missing client session did not refuse")
        except Refusal:
            pass
    finally:
        import shutil

        shutil.rmtree(root, ignore_errors=True)
    print("check_session_players selftest: %s" % ("FAIL " + "; ".join(fails) if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    if len(argv) != 1:
        print("usage: check_session_players.py <host process run dir> | --selftest")
        return 2
    try:
        probs = check(argv[0])
    except Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print("PASS: both peers recorded the same players and ai_count")
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
