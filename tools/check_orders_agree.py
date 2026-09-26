#!/usr/bin/env python3
"""
tools/check_orders_agree.py -- tooling:TL-SUITE-FOLD-DETC1 / dist:V022: did every peer RECORD its
order stream (`[harness] order_mode=1`), and do the streams agree on every common step?

WHERE THIS CAME FROM. Until this fold, the ONLY gate row that proved order_mode=1 actually produces
a recording in configuration (1) (no libmh.dll -- the -net-debug.zip shape) was run_gate.py's
dedicated `det_c1` unit, which booted its OWN throwaway match just to record + compare. `match_launch_net`
(the registry's own configuration-(1) row) already boots the same shape and already hash-compares by
default (TL-SUITE-HASHDEF) -- a second boot proving the same shape was a duplicate, not a second fact.
This checker is that row's post_check: it decodes each peer's mh_orders.bin (harness.cpp's
order_record()) and fails unless every peer recorded a non-empty stream and every stream agrees with
the first (the host's) on every step they share.

IT DOES NOT DECODE ANYTHING ITSELF -- tools/mp_order_diff.py already owns the mh_orders.bin format
(header + flat `{int32 step; uint8 order[0x44]}` records) and the file/dir resolution; this module is
purely the PASS/FAIL wrapper around it, in the (ok, [line, ...]) shape test_ui.py's other verdict
helpers use. `det_orders_verdict()` is that exact function, formerly inline in test_ui.py (moved here
so run_det_config1's det_dir/{peer} layout and this checker's one-dir-per-peer layout share one
decoder instead of two).

mh_orders.bin lives in the PROCESS run folder (opened once at DllMain time, same decision as
mh_net.log -- harness.cpp copy_arm_paths/order_record), never a session subfolder. match_launch_net
sets `post_check_session: true` for its OTHER checkers (match-time-only log lines), so this checker
is handed the SESSION dir too; `_process_dir()` follows its session.json back to the process dir,
same trick check_cancel_task.net_log_lines() uses.

  python tools/check_orders_agree.py <host-run-dir> <client-run-dir> [<more-peer-dir> ...]
  python tools/check_orders_agree.py --selftest      planted mh_orders.bin files, every negative RED
"""

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_order_diff as mod


def _process_dir(run_dir):
    """mh_orders.bin is opened once at MH_Harness_Init (DllMain-time, alongside g_log_path) and lives
    in the PROCESS run folder -- never a session subfolder (harness.cpp copy_arm_paths/order_record).
    A row like match_launch_net that sets `post_check_session: true` (for OTHER checkers that need
    match-time-only log lines) hands this checker the SESSION dir instead; follow its session.json
    back to the process dir, the same trick check_cancel_task.net_log_lines() uses. A dir with no
    session.json (run_det_config1's flat det_dir/{peer} layout, which predates sessions entirely) is
    returned unchanged."""
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except (OSError, ValueError):
            pd = None
        if pd:
            parent = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
            cand = os.path.join(parent, pd)
            if os.path.isdir(cand):
                return cand
    return run_dir


def peer_orders(run_dir):
    """(ok, line, steps_or_None) for ONE peer's mh_orders.bin. `run_dir` is a directory (searched
    recursively via mp_order_diff.resolve, same as its own CLI, after resolving to the PROCESS dir --
    see _process_dir) or the .bin path itself. `steps_or_None` is mp_order_diff.by_step()'s dict, or
    None when the peer contributed nothing to compare."""
    run_dir = _process_dir(run_dir) if os.path.isdir(run_dir) else run_dir
    try:
        path = mod.resolve(run_dir)
    except SystemExit as exc:
        return False, "      FAIL: %s" % exc, None
    if not os.path.isfile(path):
        return (
            False,
            "      FAIL: %s has no mh_orders.bin (order_mode=1 recorded nothing)" % run_dir,
            None,
        )
    try:
        recs = mod.load(path)
    except SystemExit as exc:
        return False, "      FAIL: %s: %s" % (run_dir, exc), None
    steps = mod.by_step(recs)
    line = "      orders %s: %d records over %d steps (%d bytes)" % (
        run_dir,
        len(recs),
        len(steps),
        os.path.getsize(path),
    )
    if not recs:
        return False, line + "\n      FAIL: %s recorded an EMPTY order stream" % run_dir, None
    return True, line, steps


def orders_verdict(peer_dirs):
    """(ok, [line, ...]) -- every peer directory in `peer_dirs` must have a non-empty mh_orders.bin,
    and peer_dirs[0] (the host)'s stream must agree with every other peer's on every common step.
    This is the shared verdict: run_det_config1 calls it via det_orders_verdict() below (its
    det_dir/{peer} layout), and this module's own main() calls it directly for a post_check (one
    directory per peer, as test_ui.py's post_check_peers hands them over)."""
    ok = True
    lines = []
    streams = {}
    for d in peer_dirs:
        pok, line, steps = peer_orders(d)
        lines.append(line)
        ok = ok and pok
        if pok:
            streams[d] = steps
    if ok and len(streams) == len(peer_dirs) >= 2:
        base = peer_dirs[0]
        a = streams[base]
        for d in peer_dirs[1:]:
            b = streams[d]
            common = sorted(set(a) & set(b))
            diffs = [s for s in common if [o.raw for o in a[s]] != [o.raw for o in b[s]]]
            lines.append(
                "      orders %s vs %s: %d common steps, %d differ"
                % (base, d, len(common), len(diffs))
            )
            if not common:
                lines.append("      FAIL: %s and %s share no step" % (base, d))
                ok = False
            elif diffs:
                lines.append(
                    "      FAIL: order streams differ between %s and %s, first at step %d"
                    % (base, d, diffs[0])
                )
                ok = False
    return ok, lines


def det_orders_verdict(det_dir, peers=("host", "client1")):
    """dist:V022 back-compat entry point -- run_det_config1's det_dir/{peer}/mh_orders.bin layout
    (a shared parent folder with one named subfolder per peer), unchanged from before the fold."""
    return orders_verdict([os.path.join(det_dir, p) for p in peers])


# ---- selftest --------------------------------------------------------------------------------

ORDER_LEN = mod.ORDER_LEN


def _order_bytes(step, salt=0):
    # Content is opaque to the comparison (raw-byte equality only); deterministic per (step, salt)
    # so two peers built with the same salt produce byte-identical streams.
    return bytes((step * 7 + salt + i) % 256 for i in range(ORDER_LEN))


def _write_orders(path, records):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(b"\x00" * mod.HEADER_LEN)
        fh.writelines(struct.pack("<i", step) + raw for step, raw in records)


def _plant_peer(root, name, steps, present=True, empty=False, flip_step=None):
    run_dir = os.path.join(root, name)
    os.makedirs(run_dir, exist_ok=True)
    if not present:
        return run_dir  # dir exists (a real lane always does); the .bin simply never got written
    path = os.path.join(run_dir, "mh_orders.bin")
    if empty:
        _write_orders(path, [])  # exactly the HEADER_LEN-byte file -- V022's "cut to its header"
        return run_dir
    records = []
    for s in range(1, steps + 1):
        raw = _order_bytes(s)
        if flip_step is not None and s == flip_step:
            raw = bytes([raw[0] ^ 0xFF]) + raw[1:]  # one flipped byte, dist:V022's mutant #3
        records.append((s, raw))
    _write_orders(path, records)
    return run_dir


def _plant_peer_session(root, name, steps):
    """A PROCESS dir (with mh_orders.bin) plus a sibling SESSION dir whose session.json points back
    to it -- the shape post_check_session:true rows (match_launch_net) hand this checker."""
    _plant_peer(root, name + "_proc", steps)
    sess = os.path.join(root, name + "_sess")
    os.makedirs(sess, exist_ok=True)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"process_dir": name + "_proc"}, fh)
    return sess


def selftest():
    cases = [
        ("clean: both peers agree", dict(), True),
        ("mutant: host mh_orders.bin missing", dict(host_present=False), False),
        ("mutant: host mh_orders.bin cut to its header (EMPTY)", dict(host_empty=True), False),
        ("mutant: one flipped order byte -> differ", dict(flip=500), False),
    ]
    bad = 0
    for label, kw, want in cases:
        root = tempfile.mkdtemp(prefix="ordchk_")
        try:
            host = _plant_peer(
                root,
                "host",
                1000,
                present=kw.get("host_present", True),
                empty=kw.get("host_empty", False),
            )
            client = _plant_peer(root, "client1", 1000, flip_step=None)
            if kw.get("flip"):
                # re-plant the CLIENT with the flip, so the host stays the clean reference stream --
                # either side flipping proves the same thing, this just keeps one plant() call path.
                client = _plant_peer(root, "client1", 1000, flip_step=kw["flip"])
            ok, lines = orders_verdict([host, client])
            hit = ok == want
            print(
                "  %s  %s%s"
                % ("ok " if hit else "BAD", label, "" if hit else "\n" + "\n".join(lines))
            )
            bad += 0 if hit else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    # match_launch_net sets post_check_session:true (for check_resync_storm/check_lobby_ping, which
    # need match-time log lines), so THIS checker gets handed the SESSION dir too -- prove
    # _process_dir's session.json indirection actually finds the PROCESS dir's mh_orders.bin.
    label = "post_check_session layout: session dirs resolve to their process dirs"
    root = tempfile.mkdtemp(prefix="ordchk_sess_")
    try:
        host_sess = _plant_peer_session(root, "host", 1000)
        client_sess = _plant_peer_session(root, "client1", 1000)
        ok, lines = orders_verdict([host_sess, client_sess])
        hit = ok is True
        print(
            "  %s  %s%s" % ("ok " if hit else "BAD", label, "" if hit else "\n" + "\n".join(lines))
        )
        bad += 0 if hit else 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
    # run_det_config1's flat det_dir/{peer} layout: pull_peer_logs (ui_test.py) copies the newest
    # session's session.json alongside mh_orders.bin into that SAME flat folder, so a stray
    # session.json whose process_dir does not exist as a sibling must not shadow the mh_orders.bin
    # already sitting right there -- _process_dir's isdir(cand) guard is what this proves.
    label = "flat det_dir/{peer} layout: a copied session.json pointing nowhere is ignored"
    root = tempfile.mkdtemp(prefix="ordchk_flat_")
    try:
        host = _plant_peer(root, "host", 1000)
        client = _plant_peer(root, "client1", 1000)
        for d in (host, client):
            with open(os.path.join(d, "session.json"), "w", encoding="utf-8") as fh:
                json.dump({"process_dir": "20260101T000000Z_menu_solo"}, fh)  # no such sibling
        ok, lines = orders_verdict([host, client])
        hit = ok is True
        print(
            "  %s  %s%s" % ("ok " if hit else "BAD", label, "" if hit else "\n" + "\n".join(lines))
        )
        bad += 0 if hit else 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
    total = len(cases) + 2
    print("check_orders_agree selftest: %d/%d" % (total - bad, total))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) < 2:
        print("usage: check_orders_agree.py <host-run-dir> <client-run-dir> [<more-peer-dir> ...]")
        return 2
    ok, lines = orders_verdict(args.dirs)
    print("\n".join(lines))
    print("check_orders_agree: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
