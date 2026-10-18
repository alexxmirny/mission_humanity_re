#!/usr/bin/env python3
"""
tools/check_d47_poke.py -- mp:D47 positive control: a poke on the peer that PLAYED SINGLE-PLAYER FIRST goes red
at the MATCH step it names, on the region it poked.

The oracle used to key peers by the process step, so a loaded peer was "red" from step 1 whatever it did
(a negative control on that peer proved nothing). With the match-step keying the pair is IDENTICAL until a
deliberate `region_poke_at=K`, and then the first mismatch must be match step K naming the poked region:

  * the loaded peer's harness segment was REBASED (step_base > 0) -- otherwise the row did not exercise D47;
  * its `; REGION POKE step=<K> ... <name>` line, rebased, reads K;
  * the pair's first state mismatch is step K, and the per-region R row at K (region_hash_step divides K)
    names the poked region among the diverging ones.

  python tools/check_d47_poke.py --step K --region NAME <host-run-dir> <client-run-dir>
  python tools/check_d47_poke.py --selftest
"""

import argparse
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_analyze  # noqa: E402


def check(host_dir, client_dir, step, region):
    fails = []
    a, b = mp_analyze.load_peer(host_dir), mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        return ["a peer has no harness log"], "no harness"
    ha, hb = a["harness"], b["harness"]
    poked = None
    for h in (ha, hb):
        if h.get("poke"):
            poked = h
    if poked is None:
        return ["no `; REGION POKE` line on either peer -- the poke never fired"], "no poke"
    if not (poked.get("step_base") or 0):
        fails.append(
            "the poked peer's step_base is 0 -- it did not play single-player first, so this row "
            "does not exercise the process-step keying"
        )
    pst, _idx, pname = poked["poke"]
    if pst != step:
        fails.append("the poke fired at MATCH step %d, wanted %d" % (pst, step))
    if pname != region:
        fails.append("the poke hit region %r, wanted %r" % (pname, region))
    d = mp_analyze.diff_peers(ha, hb, "host", "client")
    fm = d.get("first_mismatch") or {}
    if not fm:
        fails.append("the pair is IDENTICAL -- the poke was not seen by the oracle")
    else:
        if fm.get("step") != step:
            fails.append(
                "first state mismatch at match step %s, wanted %d" % (fm.get("step"), step)
            )
        regs = fm.get("regions")
        if regs is None:
            fails.append("no per-region R row at step %d (region_hash_step must divide it)" % step)
        elif region not in regs:
            fails.append(
                "region %r not among the diverging regions: %s" % (region, ", ".join(regs))
            )
    return fails, "poke=%s@%s first_mismatch=%s" % (pname, pst, fm.get("step"))


def _selftest():
    import shutil

    root = tempfile.mkdtemp(prefix="d47poke_")
    bad = 0
    try:
        k, sp = 100, 4
        names = mp_analyze.REGION_NAMES
        for tag, poke_step, ok_want in (("good", k, True), ("late", k + 1, False)):
            dirs = []
            for role in ("host", "client1"):
                p = os.path.join(root, tag, role)
                os.makedirs(p)
                base = sp if role != "host" else 0
                rows = ["; ==== mh replay harness armed: region_hash_step=50 ===="]
                for i in range(1, base + 1):
                    rows.append("%d %016X %016X %016X" % (i, 1, 2, 3 + i))
                for s in range(1, 151):
                    poked = role != "host" and s >= poke_step
                    st = 0xAAAA0000 + s + (0xF0F0 if poked else 0)
                    rows.append("%d %016X %016X %016X" % (s + base, s, st, st))
                    if s % 50 == 0:
                        cols = ["%016X" % (s + i) for i in range(len(names))]
                        if poked:
                            cols[55] = "%016X" % 0xDEAD
                        rows.append("R %d %s" % (s + base, " ".join(cols)))
                if role != "host":
                    rows.insert(
                        1,
                        "; REGION POKE step=%d idx=55 %s @00000000+16 -> 00"
                        % (poke_step + base, names[55]),
                    )
                    with open(os.path.join(p, "mh_match_harness.log"), "w") as f:
                        f.write(
                            "; [match] segment OPEN x at process step %d: match step k = process step "
                            "%d+k (step_base=%d).\n" % (base + 1, base, base)
                        )
                with open(os.path.join(p, "mh_harness.log"), "w") as f:
                    f.write("\n".join(rows) + "\n")
                dirs.append(p)
            fails, _ = check(dirs[0], dirs[1], k, names[55])
            ok = (not fails) is ok_want
            print("   %-60s %s" % ("D47poke %s" % tag, "ok" if ok else "FAIL %s" % fails))
            bad += 0 if ok else 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("check_d47_poke --selftest: %s" % ("PASS" if not bad else "FAIL"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--step", type=int)
    ap.add_argument("--region")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return _selftest()
    if len(args.dirs) != 2 or args.step is None or not args.region:
        ap.error("need --step, --region and <host-dir> <client-dir>")
    fails, summary = check(args.dirs[0], args.dirs[1], args.step, args.region)
    print("check_d47_poke: %s" % summary)
    for f in fails:
        print("  FAIL: %s" % f)
    print("check_d47_poke: %s" % ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
