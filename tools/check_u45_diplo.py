#!/usr/bin/env python3
"""
tools/check_u45_diplo.py -- mp:U45: a diplomacy Apply that changes relation AND control mode.

One Apply issues set_player_relation (order 0xf4) and set_player_control_mode (0xf5) in the same frame,
both with unit 0 and the issuer as owner. Retail's order-release dedup (llm_strat_order_release_due,
0x0046652e) treats those as revisions of one order and lets the higher code win, so 0xf4 was dropped on
EVERY peer and the relation only applied on a second Apply. `[net] diplo_order_dedup_fix=1` (shipped
default) exempts the pair.

Both arms read each peer's recorded order stream (`[harness] order_mode=1` -> mh_orders.bin, one record
per order sitting in the queue per step; decoded by tools/mp_order_diff.py). Every peer applies every
order, so EVERY peer's stream must show the outcome:

  --expect fixed   (the ship arm)   some owner has BOTH a 0xf4 and a 0xf5 with the same other-player
                                    (args[2]) on every peer;
  --expect retail  (negative arm, `diplo_order_dedup_fix=0`)  0xf5 is present and that owner's 0xf4 is
                                    ABSENT on every peer -- proves the ship arm is green for the right
                                    reason (the walk did reach the dialog and did issue both).

mp:U46 reuses it with `--only rel --other 2` (u46_diplo_rows: slots [host, AI, human], the host ticks the
HUMAN's allied box; no control change, so no 0xf5): fixed = a 0xf4 for player 2 on every peer; retail
(`[net] diplo_row_fix=0`) = NO 0xf4 at all, because Apply read the survivor's settings from the quitter's row.

  python tools/check_u45_diplo.py [--expect fixed|retail] [--only rel] [--other N] <host-run-dir> <client-run-dir> [...]
  python tools/check_u45_diplo.py --selftest
"""

import argparse
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_orders_agree  # noqa: E402
import mp_order_diff as mod  # noqa: E402

REL, CTL = 0xF4, 0xF5
ARG_OTHER, ARG_VALUE = 0x18 + 0, 0x1C  # args[2] / args[3] of the packed record (triage decode)


def admin_orders(path):
    """{owner: {code: set(other_player)}} over the 0xf4/0xf5 records of one mh_orders.bin."""
    out = {}
    for o in mod.load(path):
        if o.order_code not in (REL, CTL):
            continue
        (other,) = struct.unpack_from("<i", o.raw, ARG_OTHER)
        out.setdefault(o.owner_and_kind & 0xF, {}).setdefault(o.order_code, set()).add(other)
    return out


def peer_path(run_dir):
    d = check_orders_agree._process_dir(run_dir) if os.path.isdir(run_dir) else run_dir
    return mod.resolve(d)


def _has_file(d, name):
    base = check_orders_agree._process_dir(d) if os.path.isdir(d) else d
    return os.path.isdir(base) and os.path.isfile(os.path.join(base, name))


def verdict(peer_dirs, expect, only=None, other=None):
    ok, lines = True, []
    for d in peer_dirs:
        try:
            adm = admin_orders(peer_path(d))
        except SystemExit as exc:
            # The harness creates mh_orders.bin lazily, on the first recorded order. In the u46 retail arm
            # NO order is issued at all (that is the bug), so an absent file is the expected outcome -- but
            # only from a run that provably walked to the click (the host's script logged its marker) and
            # armed the harness (mh_harness.log present). Anything else stays a FAIL.
            host_walked = (
                _has_file(peer_dirs[0], "mh_uidrive.log")
                and "ticked allied for the"
                in open(
                    os.path.join(check_orders_agree._process_dir(peer_dirs[0]), "mh_uidrive.log"),
                    encoding="utf-8",
                    errors="replace",
                ).read()
            )
            if (
                only == "rel"
                and expect == "retail"
                and host_walked
                and _has_file(d, "mh_harness.log")
            ):
                lines.append(
                    "      %s: no mh_orders.bin -- the walk clicked, the harness was armed, no order recorded"
                    % d
                )
                continue
            return False, ["      FAIL: %s: %s" % (d, exc)]
        if only == "rel":
            owners = [ow for ow, c in adm.items() if REL in c]
            if expect == "retail":
                if owners:
                    lines.append(
                        "      FAIL: %s: a 0xf4 was issued with the gate OFF (%s)"
                        % (d, sorted(adm))
                    )
                    ok = False
                else:
                    lines.append("      %s: no 0xf4 (retail row walk read the wrong row)" % d)
                continue
            good = [ow for ow in owners if other is None or other in adm[ow][REL]]
            wrong = [ow for ow in owners if other is not None and adm[ow][REL] - {other}]
            lines.append(
                "      %s: 0xf4 owners %s, targets %s"
                % (d, sorted(owners), [sorted(adm[o][REL]) for o in owners])
            )
            if not good or wrong:
                lines.append(
                    "      FAIL: %s: no 0xf4 for player %s, or one for another player" % (d, other)
                )
                ok = False
            continue
        ctl_owners = [ow for ow, c in adm.items() if CTL in c]
        if not ctl_owners:
            lines.append(
                "      FAIL: %s recorded no 0xf5 control order (the walk never applied)" % d
            )
            ok = False
            continue
        for ow in ctl_owners:
            c = adm[ow]
            has_rel = bool(c.get(REL, set()) & c[CTL])
            lines.append(
                "      %s owner %d: 0xf5 for %s, 0xf4 for %s"
                % (d, ow, sorted(c[CTL]), sorted(c.get(REL, set())) or "NONE")
            )
            if expect == "fixed" and not has_rel:
                lines.append(
                    "      FAIL: %s owner %d: 0xf4 was deduped away (U45 regressed)" % (d, ow)
                )
                ok = False
            if expect == "retail" and has_rel:
                lines.append(
                    "      FAIL: %s owner %d: 0xf4 survived with the gate OFF (negative arm is not "
                    "reproducing the bug)" % (d, ow)
                )
                ok = False
    return ok, lines


def _write(path, recs):
    with open(path, "wb") as f:
        f.write(b"MHOR" + struct.pack("<I", 1))
        for step, code, owner, other in recs:
            raw = bytearray(mod.ORDER_LEN)
            struct.pack_into("<H", raw, 0x0A, owner)
            struct.pack_into("<H", raw, 0x0E, code)
            struct.pack_into("<i", raw, ARG_OTHER, other)
            f.write(struct.pack("<i", step) + bytes(raw))


def selftest():
    fails = 0

    def check(what, got, want):
        nonlocal fails
        if got != want:
            fails += 1
            print("  FAIL %s (got %r want %r)" % (what, got, want))

    d = tempfile.mkdtemp(prefix="u45_selftest_")
    try:

        def peer(name, recs):
            p = os.path.join(d, name)
            os.makedirs(p, exist_ok=True)
            _write(os.path.join(p, "mh_orders.bin"), recs)
            return p

        both = peer("both", [(5, REL, 0, 1), (5, CTL, 0, 1), (6, REL, 0, 1)])
        only_ctl = peer("ctl", [(5, CTL, 0, 1), (6, CTL, 0, 1)])
        none = peer("none", [(5, 0x03, 0, 1)])
        other_j = peer("otherj", [(5, REL, 0, 2), (5, CTL, 0, 1)])
        check("fixed: both orders on both peers -> ok", verdict([both, both], "fixed")[0], True)
        check(
            "fixed: 0xf4 missing on one peer -> RED", verdict([both, only_ctl], "fixed")[0], False
        )
        check(
            "fixed: 0xf4 for a different player -> RED", verdict([both, other_j], "fixed")[0], False
        )
        check("fixed: no admin order at all -> RED", verdict([both, none], "fixed")[0], False)
        check("retail: only 0xf5 -> ok", verdict([only_ctl, only_ctl], "retail")[0], True)
        check("retail: 0xf4 survived -> RED", verdict([only_ctl, both], "retail")[0], False)
        check("retail: no 0xf5 -> RED", verdict([only_ctl, none], "retail")[0], False)
        rel2 = peer("rel2", [(5, REL, 0, 2), (6, REL, 0, 2)])
        rel1 = peer("rel1", [(5, REL, 0, 1)])
        check(
            "U46 fixed: 0xf4 for player 2 -> ok", verdict([rel2, rel2], "fixed", "rel", 2)[0], True
        )
        check(
            "U46 fixed: 0xf4 for the AI (1) -> RED",
            verdict([rel2, rel1], "fixed", "rel", 2)[0],
            False,
        )
        check("U46 fixed: no 0xf4 -> RED", verdict([rel2, none], "fixed", "rel", 2)[0], False)
        check("U46 retail: no 0xf4 -> ok", verdict([none, none], "retail", "rel", 2)[0], True)
        check("U46 retail: a 0xf4 -> RED", verdict([none, rel2], "retail", "rel", 2)[0], False)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print("check_u45_diplo selftest: %s" % ("PASS" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--expect", choices=("fixed", "retail"), default="fixed")
    ap.add_argument("--only", choices=("rel",), default=None)
    ap.add_argument("--other", type=int, default=None)
    ap.add_argument(
        "--skip",
        type=int,
        action="append",
        default=[],
        help="0-based index of a peer dir to ignore (a peer that LEFT the match: u46's quitter)",
    )
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if len(a.dirs) < 2:
        ap.error("need <host-run-dir> <client-run-dir>")
    dirs = [d for i, d in enumerate(a.dirs) if i not in a.skip]
    ok, lines = verdict(dirs, a.expect, a.only, a.other)
    print("\n".join(lines))
    print("U45 %s arm: %s" % (a.expect, "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
