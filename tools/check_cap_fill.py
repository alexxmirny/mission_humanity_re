#!/usr/bin/env python3
"""
tools/check_cap_fill.py -- patching:PT-CAP8: the ON smoke of the v0.2.2 cap raise (spec S1 8.2 item 8).

The stimulus is the harness cap-fill fixture (`cap_*` keys, capf_tick in mh_harness/harness.cpp): with libmh.dll absent and
`[caps] raise=1` (tools/uiscripts/ini/caps_raise.ini) one player places buildings through the game's OWN debug-construction order
(0xf2 -> llm_bldg_queue_construction -> footprint check -> llm_bldg_construct_finalize) until the roster is past every vanilla cap,
then the match runs on. The raised allocators and the 51 limit bytes are what is under test.

Clauses (all must hold):
  1  mh_net.log: `[caps] relocation APPLIED` and `[caps] limits ON (`; no `[caps] ... REFUSED`.
  2  mh_harness.log: a `; CAP VERDICT ok=1` line, no `; CAP FAIL`, and the fixture's geometry line reports the WIDE rows
     (buildings 500, the five sub-rosters 127) -- i.e. the run was really on the relocated layout.
  3  the counts the verdict reached are past the vanilla caps: buildings > 100, storages > 25, turrets > 32, mines > 32, labs > 25,
     productions > 8 (and the highest index in use of each array is past its vanilla row, so a record really LIVES in the raised
     part); `--prj N` also asks for N live projectiles at the high-water mark (0 = report only).
  4  the game kept stepping after the verdict (a later SAMPLE line) and no desync / crash banner is in either log.
  5  `[caps] mirror_check ARMED` is in the net log and it produced no writer report (`mirror_check: array ... changed`) -- nothing wrote a STOCK address.

  6  (`--order wide|alias`, O1c) the wide-order-index probe (capord_tick): `; CAPORD VERDICT target_hit=T alias_hit=A`.
     `wide` (shipped): the activate order for building index 300 staffed slot 300 and NOT slot 44 (300 & 0xff) -- T=1 A=0, and the
     net log shows the four `order wide index ON ... PATCHED` sites (a site that is DISPLACED is accepted: libmh's twin carries it).
     `alias` (the order_wide_index=0 reproduction arm): the reverse, T=0 A=1 -- the vanilla 8-bit lane applied the order to slot 44.

  7  (`--ovf`, PT-CAP8 S2) the overflow records are HASHED and stay identical: the net log has `[caps] overflow hash ARMED: 21
     slices, 5356988 bytes`, the last `[desync] STATUS` line shows per_step=1, v2_compared > 0 and v2_mismatching=0, and no
     `*** DESYNC` line exists. (The per-step compare spans the 63 manifest regions AND the 21 overflow slices, so a divergent
     overflow byte on either peer would be a mismatch.)
  8  (`--ovf-desync STEP:IDX`, the negative row) one peer's harness poked overflow record IDX of player 0's buildings at STEP
     (`; CAP POKE`): both peers log `overflow hash ARMED`, a `*** DESYNC step=S` with 0 <= S - STEP <= 2 whose first_region is
     the first overflow slice (63 ovf_buildings_p0_3), and a `LOCALISED ... region=63 ovf_buildings_p0_3 ... = buildings p0
     #IDX` line. With `--blind` (the fix-off twin, `[caps] hash_overflow=0` on both peers) the same poke must be INVISIBLE:
     no `overflow hash ARMED`, no `*** DESYNC` -- which is the proof the row sees what S1 could not.

  python tools/check_cap_fill.py [--prj N] [--order wide|alias] [--ovf] <run-dir> [<run-dir> ...]   (several dirs: --order rows)
  python tools/check_cap_fill.py --ovf-desync STEP:IDX[:TARGET] [--blind] <run-dir> [<run-dir> ...]
  (TARGET, PT-CAP9-U7d: buildings (default) | queue | pathN -- the poked overflow slice; see poke_target())
  python tools/check_cap_fill.py --selftest

OFF (`[caps] raise=0`) the overflow extension does not exist; a row without the raise never reaches clauses 7-8.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import check_storage_purge  # noqa: E402  (harness_log_lines)

VANILLA = {"bldg": 100, "storage": 25, "turret": 32, "mine": 32, "lab": 25, "prod": 8}
WIDE_ROWS = {"bldg": 500, "storage": 127, "turret": 127, "mine": 127, "lab": 127, "prod": 127}
VERDICT_RE = re.compile(
    r"; CAP VERDICT ok=1 step=(?P<step>\d+) bldg=(?P<bldg>\d+)\(hi (?P<bldg_hi>\d+)\) storage=(?P<storage>\d+)\(hi (?P<storage_hi>\d+)\)"
    r" turret=(?P<turret>\d+)\(hi (?P<turret_hi>\d+)\) mine=(?P<mine>\d+)\(hi (?P<mine_hi>\d+)\) lab=(?P<lab>\d+)\(hi (?P<lab_hi>\d+)\)"
    r" prod=(?P<prod>\d+)\(hi (?P<prod_hi>\d+)\) placed=(?P<placed>\d+) refused=(?P<refused>\d+) projectiles_hw=(?P<prj>\d+)"
)
GEOM_RE = re.compile(
    r"; CAP GEOMETRY \((?P<why>[^)]*)\): row bldg=(?P<bldg>\d+) storage=(?P<storage>\d+) turret=(?P<turret>\d+) mine=(?P<mine>\d+)"
    r" lab=(?P<lab>\d+) prod=(?P<prod>\d+)"
)
SAMPLE_RE = re.compile(r"; CAP step=(?P<step>\d+) SAMPLE ")
STAMP = re.compile(r"^\[[\d:.]+\]\s*")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


ORDER_RE = re.compile(
    r"; CAPORD VERDICT target_hit=(?P<t>\d+) alias_hit=(?P<a>\d+) idx=(?P<idx>\d+) alias=(?P<alias>\d+)"
)
WIDE_ON_RE = re.compile(
    r"; order wide index ON \(\[net\] order_wide_index=1\): encode @00466242 (?P<e>\w+)[^;]*; check @004661A0 (?P<v>\w+)[^;]*;"
    r" local @0046643A (?P<l>\w+)[^;]*; remote @004667B4 (?P<r>\w+)"
)


def order_problems(net, harness, expect):
    """Clause 6: the CAPORD verdict matches `expect` ('wide' | 'alias' | 'sp'); the carrier banner matches the knob."""
    net, harness = strip(net), strip(harness)
    bad = []
    if any(ln.startswith("; CAPORD FAIL") for ln in harness):
        bad += ["clause 6: " + ln for ln in harness if ln.startswith("; CAPORD FAIL")]
    pre = [ln for ln in harness if ln.startswith("; CAPORD") and " PRE " in ln]
    post = [ln for ln in harness if ln.startswith("; CAPORD") and " POST " in ln]
    if not pre or not post:
        bad.append(
            "clause 6: no `; CAPORD ... PRE` / `POST` pair (the probe never ran: slot not built, or cap_order_at not reached)"
        )
    v = None
    for ln in harness:
        m = ORDER_RE.match(ln)
        if m:
            v = {k: int(x) for k, x in m.groupdict().items()}
    if v is None:
        bad.append("clause 6: no `; CAPORD VERDICT` line")
        return bad
    if expect == "sp":
        # single player: no wire, so the index may be any raised one (> 127); the alias is the truncated reading
        # (signed byte for 127 < idx <= 255, the low byte above that)
        alias_ok = v["alias"] == (v["idx"] & 0xFF if v["idx"] > 255 else v["idx"] & 0x7F)
        if v["idx"] <= 127 or not alias_ok:
            bad.append(
                "clause 6: the probe index %d / alias %d is not a raised (>127) index and its truncated slot"
                % (v["idx"], v["alias"])
            )
    elif v["idx"] <= 255 or v["alias"] != v["idx"] & 0xFF:
        bad.append(
            "clause 6: the probe index %d / alias %d is not a >255 index and its low byte"
            % (v["idx"], v["alias"])
        )
    want = (0, 1) if expect == "alias" else (1, 0)
    if (v["t"], v["a"]) != want:
        bad.append(
            "clause 6: %s lane expected target_hit=%d alias_hit=%d, got target_hit=%d alias_hit=%d"
            % (expect, want[0], want[1], v["t"], v["a"])
        )
    banners = [m for m in (WIDE_ON_RE.search(ln) for ln in net) if m]
    off = any("order wide index OFF" in ln for ln in net)
    if expect == "sp":
        pass  # no carrier banner: the single-player order queue is not the MP wire
    elif expect == "wide":
        if not banners:
            bad.append("clause 6: no `order wide index ON` banner in mh_net.log")
        for m in banners:
            for k in ("e", "v", "l", "r"):
                if m.group(k) not in ("PATCHED", "DISPLACED"):
                    bad.append("clause 6: wide-index site %s is %s" % (k, m.group(k)))
        if not any(m.group("e") == "PATCHED" and m.group("v") == "PATCHED" for m in banners):
            bad.append(
                "clause 6: the encode/check sites are not PATCHED (they are not promotable, so DISPLACED is impossible)"
            )
    elif not off:
        bad.append(
            "clause 6: the alias arm needs the `order wide index OFF` banner (order_wide_index=0 did not reach the process)"
        )
    return bad


ARMED_RE = re.compile(r"; \[caps\] overflow hash ARMED: (?P<n>\d+) slices, (?P<b>\d+) bytes, fp ")
STATUS_RE = re.compile(
    r"; \[desync\] STATUS: .* per_step=(?P<ps>\d+) v2_compared=(?P<cmp>\d+) v2_mismatching=(?P<mis>\d+) "
)
DESYNC_RE = re.compile(
    r"; \[desync\] \*\*\* DESYNC step=(?P<step>\d+) peer=-?\d+ .*?first_region=(?P<fr>-?\d+) (?P<name>\S+)"
)
LOCALISED_RE = re.compile(
    r"; \[desync\] LOCALISED step=(?P<step>\d+) peer=-?\d+ region=(?P<r>\d+) (?P<name>\S+) \+0x[0-9A-Fa-f]+(?: = (?P<desc>.*?))?: \d+ byte"
)
POKE_RE = re.compile(
    r"; CAP POKE step=(?P<step>\d+) arr=(?P<arr>\d+) p=(?P<p>\d+) idx=(?P<idx>\d+)"
)
# PT-CAP9-U7d: 11 roster slices (2,690,052 B) + 10 path / order slices (2,666,936 B: tails 1,924,800 + flags 4,008 + windows 480,000 + queue 258,128).
# Pending / staging are NOT hashed (peer-local; see mh/caps/ovf.h). Region ids are N (63 manifest regions) + the slice index.
OVF_SLICES, OVF_BYTES, OVF_REGION, OVF_NAME = 21, 5356988, 63, "ovf_buildings_p0_3"
N_MANIFEST = 63


def poke_target(target, idx):
    """(region id, slice name, describe prefix) of the overflow slice a poke of `target` at record/slot `idx` lands in.
    target: buildings (player 0, the S2 row) | queue | pathN (player N, slot idx; N defaults to 0)."""
    if target == "buildings":
        return OVF_REGION, OVF_NAME, "buildings p0 #%d" % idx
    if target == "queue":
        return N_MANIFEST + 20, "ovf_order_queue", "order queue #%d" % idx
    if target.startswith("path"):
        pl = int(target[4:] or 0)
        lo, hi = 2 * (pl // 2), 2 * (pl // 2) + 1
        if idx < 100:
            return (
                N_MANIFEST + 16 + pl // 2,
                "ovf_path_window_p%d_%d" % (lo, hi),
                "path pool p%d slot %d" % (pl, idx),
            )
        return (
            N_MANIFEST + 11 + pl // 2,
            "ovf_path_tail_p%d_%d" % (lo, hi),
            "path pool p%d slot %d" % (pl, idx),
        )
    raise ValueError(target)


def ovf_problems(net):
    """Clause 7: the overflow records are hashed and the pair stayed identical."""
    net = strip(net)
    bad = []
    armed = [m for m in (ARMED_RE.search(ln) for ln in net) if m]
    if not armed:
        bad.append(
            "clause 7: no `[caps] overflow hash ARMED` in mh_net.log (the overflow extension never armed)"
        )
    elif (int(armed[-1].group("n")), int(armed[-1].group("b"))) != (OVF_SLICES, OVF_BYTES):
        bad.append(
            "clause 7: overflow hash armed with %s slices / %s bytes, expected %d / %d"
            % (armed[-1].group("n"), armed[-1].group("b"), OVF_SLICES, OVF_BYTES)
        )
    st = [m for m in (STATUS_RE.search(ln) for ln in net) if m]
    if not st:
        bad.append("clause 7: no `[desync] STATUS` line (no proof the per-step compare ran)")
    else:
        last = st[-1]
        if int(last.group("ps")) != 1 or int(last.group("cmp")) <= 0:
            bad.append(
                "clause 7: the per-step compare did not run (per_step=%s v2_compared=%s)"
                % (last.group("ps"), last.group("cmp"))
            )
        if int(last.group("mis")) != 0:
            bad.append(
                "clause 7: v2_mismatching=%s over the overflow-extended vector" % last.group("mis")
            )
    bad += ["clause 7: " + ln for ln in net if "*** DESYNC" in ln]
    return bad


def ovf_desync_problems(net, harness, poke_step, idx, blind, target="buildings"):
    """Clause 8: a poked overflow record is seen (or, `blind`, NOT seen) by the pair."""
    net, harness = strip(net), strip(harness)
    bad = []
    armed = any(ARMED_RE.search(ln) for ln in net)
    desyncs = [m for m in (DESYNC_RE.search(ln) for ln in net) if m]
    if blind:
        if armed:
            bad.append(
                "clause 8: `overflow hash ARMED` under [caps] hash_overflow=0 (the twin did not turn the hash off)"
            )
        if desyncs:
            bad.append(
                "clause 8: the fix-off twin saw a DESYNC (%s) -- the poke was visible without the overflow hash"
                % desyncs[0].group(0)
            )
        return bad
    if not armed:
        bad.append("clause 8: no `[caps] overflow hash ARMED` in mh_net.log")
    if not desyncs:
        bad.append("clause 8: no `*** DESYNC` line (the poked overflow byte went unnoticed)")
        return bad
    first = desyncs[0]
    d = int(first.group("step")) - poke_step
    if not 0 <= d <= 2:
        bad.append(
            "clause 8: the first DESYNC is at step %s, %d step(s) from the poke at %d (want 0..2)"
            % (first.group("step"), d, poke_step)
        )
    region, rname, want = poke_target(target, idx)
    if int(first.group("fr")) != region or first.group("name") != rname:
        bad.append(
            "clause 8: first_region=%s %s, expected %d %s"
            % (first.group("fr"), first.group("name"), region, rname)
        )
    loc = [m for m in (LOCALISED_RE.search(ln) for ln in net) if m]
    if not any(
        int(m.group("r")) == region
        and m.group("name") == rname
        and (m.group("desc") or "").startswith(want)
        for m in loc
    ):
        bad.append(
            "clause 8: no `LOCALISED ... region=%d %s ... = %s` line" % (region, rname, want)
        )
    return bad


def problems(net, harness, prj_min=0, allow_desync=False):
    net, harness = strip(net), strip(harness)
    bad = []
    if not any("[caps] relocation APPLIED" in ln for ln in net):
        bad.append(
            "clause 1: no `[caps] relocation APPLIED` in mh_net.log (the run was not on the relocated layout)"
        )
    if not any("[caps] limits ON (" in ln for ln in net):
        bad.append("clause 1: no `[caps] limits ON (` in mh_net.log (the raise never turned on)")
    for ln in net:
        if "[caps]" in ln and "REFUSED" in ln:
            bad.append("clause 1: " + ln)
    if any(ln.startswith("; CAP FAIL") for ln in harness):
        bad += ["clause 2: " + ln for ln in harness if ln.startswith("; CAP FAIL")]
    v = None
    for ln in harness:
        m = VERDICT_RE.match(ln)
        if m:
            v = {k: int(x) for k, x in m.groupdict().items()}
    if v is None:
        bad.append(
            "clause 2: no `; CAP VERDICT ok=1` line (the fixture never reached its targets, or the process died first)"
        )
        return bad
    geoms = [m for m in (GEOM_RE.match(ln) for ln in harness) if m]
    if not geoms:
        bad.append("clause 2: no `; CAP GEOMETRY` line")
    else:
        g = {k: int(x) for k, x in geoms[-1].groupdict().items() if k != "why"}
        for k, want in WIDE_ROWS.items():
            if g[k] != want:
                bad.append("clause 2: geometry row %s=%d, expected the wide %d" % (k, g[k], want))
    for k, cap in VANILLA.items():
        if v[k] <= cap:
            bad.append("clause 3: %s reached %d, not past the vanilla cap %d" % (k, v[k], cap))
        if v[k + "_hi"] <= cap:
            bad.append(
                "clause 3: %s highest index in use %d is inside the vanilla window (%d)"
                % (k, v[k + "_hi"], cap)
            )
    if prj_min and v["prj"] < prj_min:
        bad.append("clause 3: projectile high-water %d < the asked %d" % (v["prj"], prj_min))
    later = [int(m.group("step")) for m in (SAMPLE_RE.match(ln) for ln in harness) if m]
    if not later or max(later) <= v["step"]:
        bad.append(
            "clause 4: no SAMPLE line after the verdict step %d (the game stopped stepping)"
            % v["step"]
        )
    for ln in net + harness:
        if "*** DESYNC" in ln and not allow_desync:
            bad.append("clause 4: " + ln)
    if not any("[caps] mirror_check ARMED" in ln for ln in net):
        bad.append(
            "clause 5: no `[caps] mirror_check ARMED` banner (a silent watch proves nothing)"
        )
    for ln in net:  # the writer report is the only line the key can emit; its absence IS the clause
        if "[caps] mirror_check:" in ln and "changed" in ln:
            bad.append("clause 5: " + ln)
    return bad


def run(run_dir, prj_min, order=None, ovf=False, ovf_desync=None, blind=False, target="buildings"):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, prj_min, allow_desync=ovf_desync is not None)
    if ovf:
        bad += ovf_problems(net)
    if ovf_desync is not None:
        bad += ovf_desync_problems(net, harness, ovf_desync[0], ovf_desync[1], blind, target)
        for ln in strip(harness):
            if ln.startswith("; CAP POKE"):
                print("  " + ln)
        for ln in strip(net):
            if "*** DESYNC" in ln or "LOCALISED" in ln or "overflow hash ARMED" in ln:
                print("  " + ln[:200])
    if (order or ovf_desync is not None) and not any(
        "[caps] mirror_check ARMED" in ln for ln in net
    ):
        # The CLIENT of a 2-peer row: the host-only ini (mirror_check=1) never reached it and the raise arrives by adoption at START, which
        # its mh_net.log words differently -- the banner clauses (1 limits ON, 5 mirror ARMED) are the host's. Every count/geometry clause stays.
        bad = [
            b
            for b in bad
            if not (b.startswith("clause 1: no `[caps] limits ON") or b.startswith("clause 5: no"))
        ]
    if order:
        bad += order_problems(net, harness, order)
        for ln in strip(harness):
            if ln.startswith("; CAPORD"):
                print("  " + ln)
    v = [ln for ln in strip(harness) if ln.startswith("; CAP VERDICT")]
    if v:
        print("  " + v[-1])
    for b in bad:
        print("  check_cap_fill: " + b)
    print("check_cap_fill: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] mirror_check ARMED: a writer that touches a stock (canonical-mirror) address is reported once per array",
    "; [caps] relocation APPLIED (3945 sites, 2 sections, base .mhcapd=0x040C0000) in 10.1 ms; limits OFF",
    "; [caps] limits ON (gameplay settings): buildings 500, storage 127, turrets 127, mines 127, labs 127, productions 127, projectiles 4000",
]
GOOD_HARNESS = [
    "; CAP GEOMETRY (start): row bldg=500 storage=127 turret=127 mine=127 lab=127 prod=127",
    "; CAP step=900 SAMPLE bldg=60(hi 60) storage=10 turret=12 mine=9 lab=8 prod=3 placed=60 refused=4 projectiles=0(hw 0) met=0",
    "; CAP VERDICT ok=1 step=4000 bldg=140(hi 140) storage=30(hi 30) turret=40(hi 41) mine=40(hi 40) lab=30(hi 30) prod=12(hi 12) placed=140 refused=9 projectiles_hw=7",
    "; CAP step=4500 SAMPLE bldg=140(hi 140) storage=30 turret=40 mine=40 lab=30 prod=12 placed=140 refused=9 projectiles=3(hw 7) met=1",
]


def selftest():
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    def has(bad, tag):
        return any(tag in b for b in bad)

    expect("a good run is green", problems(GOOD_NET, GOOD_HARNESS) == [])
    expect(
        "projectile floor is judged",
        has(problems(GOOD_NET, GOOD_HARNESS, prj_min=1500), "projectile"),
    )
    expect("no raise banner is RED", has(problems(GOOD_NET[:2], GOOD_HARNESS), "limits ON"))
    expect(
        "no relocation banner is RED",
        has(problems([GOOD_NET[0], GOOD_NET[2]], GOOD_HARNESS), "relocation APPLIED"),
    )
    expect(
        "a silent (unarmed) mirror watch is RED", has(problems(GOOD_NET[1:], GOOD_HARNESS), "ARMED")
    )
    expect(
        "a refusal is RED",
        has(problems(GOOD_NET + ["; [caps] raise REFUSED (x): nope"], GOOD_HARNESS), "REFUSED"),
    )
    expect("no verdict is RED", has(problems(GOOD_NET, GOOD_HARNESS[:2]), "VERDICT"))
    expect(
        "a FAIL line is RED",
        has(problems(GOOD_NET, GOOD_HARNESS + ["; CAP FAIL step=1 phase=0: x"]), "CAP FAIL"),
    )
    low = [
        GOOD_HARNESS[0],
        GOOD_HARNESS[1],
        GOOD_HARNESS[2].replace("turret=40(hi 41)", "turret=32(hi 32)"),
        GOOD_HARNESS[3],
    ]
    expect("a count at the vanilla cap is RED", has(problems(GOOD_NET, low), "turret reached 32"))
    inside = [
        GOOD_HARNESS[0],
        GOOD_HARNESS[1],
        GOOD_HARNESS[2].replace("lab=30(hi 30)", "lab=30(hi 20)"),
        GOOD_HARNESS[3],
    ]
    expect(
        "a high index inside the vanilla window is RED",
        has(problems(GOOD_NET, inside), "lab highest index"),
    )
    narrow = [GOOD_HARNESS[0].replace("bldg=500", "bldg=100")] + GOOD_HARNESS[1:]
    expect("vanilla geometry is RED", has(problems(GOOD_NET, narrow), "geometry row bldg"))
    expect("a stalled game is RED", has(problems(GOOD_NET, GOOD_HARNESS[:3]), "stopped stepping"))
    expect(
        "a mirror writer report is RED",
        has(
            problems(
                GOOD_NET
                + ["; [caps] mirror_check: array 2 stock byte +9 changed between two gathers -- x"],
                GOOD_HARNESS,
            ),
            "clause 5",
        ),
    )
    expect(
        "a desync is RED", has(problems(GOOD_NET + ["*** DESYNC step=5"], GOOD_HARNESS), "DESYNC")
    )
    on = "; order wide index ON ([net] order_wide_index=1): encode @00466242 PATCHED; check @004661A0 PATCHED; local @0046643A PATCHED; remote @004667B4 PATCHED"
    hit = [
        "; CAPORD step=1000 PRE b300=1 b44=1 (built_flags; bit 2 = staffed)",
        "; CAPORD step=1200 POST b300=3 b44=1",
        "; CAPORD VERDICT target_hit=1 alias_hit=0 idx=300 alias=44",
    ]
    alias = hit[:2] + ["; CAPORD VERDICT target_hit=0 alias_hit=1 idx=300 alias=44"]
    expect("order: a wide hit is green", order_problems([on], hit, "wide") == [])
    expect(
        "order: the wide expectation rejects the alias hit",
        has(order_problems([on], alias, "wide"), "expected target_hit=1"),
    )
    off = "; order wide index OFF ([net] order_wide_index=0): retail -- x"
    expect(
        "order: an alias hit is green for the reproduction arm",
        order_problems([off], alias, "alias") == [],
    )
    expect(
        "order: the alias arm rejects a wide hit",
        has(order_problems([off], hit, "alias"), "expected target_hit=0"),
    )
    expect(
        "order: the alias arm needs the OFF banner",
        has(order_problems([on], alias, "alias"), "OFF"),
    )
    sp_hit = [h.replace("idx=300 alias=44", "idx=200 alias=72") for h in hit]
    expect(
        "order sp: a wide hit on idx 200 (alias 72) is green",
        order_problems([], sp_hit, "sp") == [],
    )
    expect(
        "order sp: the alias landing is RED",
        has(
            order_problems(
                [],
                [h.replace("target_hit=1 alias_hit=0", "target_hit=0 alias_hit=1") for h in sp_hit],
                "sp",
            ),
            "expected target_hit=1",
        ),
    )
    expect(
        "order sp: an idx inside the vanilla signed window is RED",
        has(
            order_problems(
                [], [h.replace("idx=200 alias=72", "idx=100 alias=100") for h in sp_hit], "sp"
            ),
            "raised (>127)",
        ),
    )
    expect("order: no verdict is RED", has(order_problems([on], hit[:2], "wide"), "CAPORD VERDICT"))
    expect(
        "order: a MISMATCHED site is RED",
        has(
            order_problems(
                [on.replace("check @004661A0 PATCHED", "check @004661A0 MISMATCHED")], hit, "wide"
            ),
            "site v",
        ),
    )
    expect(
        "order: a missing banner is RED",
        has(order_problems([], hit, "wide"), "no `order wide index ON`"),
    )
    expect(
        "order: a probe index inside 8 bits is RED",
        has(
            order_problems(
                [on], [h.replace("idx=300 alias=44", "idx=200 alias=200") for h in hit], "wide"
            ),
            "not a >255",
        ),
    )
    # ---- clauses 7-8 (PT-CAP8 S2) ----
    armed = "; [caps] overflow hash ARMED: 21 slices, 5356988 bytes, fp 0123456789ABCDEF"
    status = "; [desync] STATUS: samples=900 (own 0 / reused 0) compared=0 mismatching=0 too_old=0 bad_frames=0 order_mismatching=0 order_absent=0 step=900 per_step=1 v2_compared=900 v2_mismatching=0 v2_incidents=0 localised=0 fallback=0 | COST per-step 300 us mean | x"
    expect("ovf: a clean ON run is green", ovf_problems([armed, status]) == [])
    expect("ovf: no ARMED banner is RED", has(ovf_problems([status]), "overflow hash ARMED"))
    expect(
        "ovf: a wrong slice/byte count is RED",
        has(ovf_problems([armed.replace("21 slices", "10 slices"), status]), "10 slices"),
    )
    expect("ovf: no STATUS line is RED", has(ovf_problems([armed]), "no `[desync] STATUS`"))
    expect(
        "ovf: mismatching steps are RED",
        has(
            ovf_problems([armed, status.replace("v2_mismatching=0", "v2_mismatching=3")]),
            "v2_mismatching=3",
        ),
    )
    expect(
        "ovf: a run whose per-step compare never ran is RED",
        has(
            ovf_problems([armed, status.replace("v2_compared=900", "v2_compared=0")]), "did not run"
        ),
    )
    expect(
        "ovf: a DESYNC line is RED",
        has(ovf_problems([armed, status, "; [desync] *** DESYNC step=9 peer=1"]), "DESYNC"),
    )
    dline = "; [desync] *** DESYNC step=901 peer=1 mine=0000000000000001 theirs=0000000000000002 first_region=63 ovf_buildings_p0_3 (mismatch #1, per-step: 1 region(s) differ at this step)"
    lline = "; [desync] LOCALISED step=901 peer=1 region=63 ovf_buildings_p0_3 +0x6B85 = buildings p0 #300 +0x10B: 1 byte(s) differ in its 64-byte block 429, first mine=00 theirs=5A"
    expect(
        "ovf-desync: a seen poke is green",
        ovf_desync_problems([armed, dline, lline], [], 900, 300, False) == [],
    )
    expect(
        "ovf-desync: a missed poke is RED",
        has(ovf_desync_problems([armed], [], 900, 300, False), "went unnoticed"),
    )
    expect(
        "ovf-desync: a late DESYNC is RED",
        has(
            ovf_desync_problems(
                [armed, dline.replace("step=901", "step=950"), lline], [], 900, 300, False
            ),
            "from the poke",
        ),
    )
    expect(
        "ovf-desync: a DESYNC before the poke is RED",
        has(
            ovf_desync_problems(
                [armed, dline.replace("step=901", "step=899"), lline], [], 900, 300, False
            ),
            "from the poke",
        ),
    )
    expect(
        "ovf-desync: the wrong first region is RED",
        has(
            ovf_desync_problems(
                [
                    armed,
                    dline.replace("first_region=63 ovf_buildings_p0_3", "first_region=2 units"),
                    lline,
                ],
                [],
                900,
                300,
                False,
            ),
            "first_region=2",
        ),
    )
    expect(
        "ovf-desync: a missing / wrong LOCALISED record is RED",
        has(ovf_desync_problems([armed, dline], [], 900, 300, False), "LOCALISED")
        and has(
            ovf_desync_problems([armed, dline, lline.replace("#300", "#301")], [], 900, 300, False),
            "LOCALISED",
        ),
    )
    for tgt, ix, (reg, nm, pre) in (
        ("queue", 350, (83, "ovf_order_queue", "order queue #350")),
        ("path", 50, (79, "ovf_path_window_p0_1", "path pool p0 slot 50")),
        ("path5", 300, (76, "ovf_path_tail_p4_5", "path pool p5 slot 300")),
    ):
        d2 = (
            "; [desync] *** DESYNC step=901 peer=1 mine=1 theirs=2 first_region=%d %s (mismatch #1)"
            % (reg, nm)
        )
        l2 = (
            "; [desync] LOCALISED step=901 peer=1 region=%d %s +0x6B85 = %s +0x5: 1 byte(s) differ in its 64-byte block 9"
            % (reg, nm, pre)
        )
        expect(
            "ovf-desync %s: the right region + describe is green, another region is RED" % tgt,
            ovf_desync_problems([armed, d2, l2], [], 900, ix, False, tgt) == []
            and has(
                ovf_desync_problems([armed, dline, lline], [], 900, ix, False, tgt),
                "expected %d" % reg,
            ),
        )
    expect(
        "ovf-desync: no ARMED banner is RED",
        has(ovf_desync_problems([dline, lline], [], 900, 300, False), "ARMED"),
    )
    expect("ovf-desync blind: silence is green", ovf_desync_problems([], [], 900, 300, True) == [])
    expect(
        "ovf-desync blind: a DESYNC is RED",
        has(ovf_desync_problems([dline], [], 900, 300, True), "fix-off twin saw"),
    )
    expect(
        "ovf-desync blind: an ARMED hash is RED",
        has(ovf_desync_problems([armed], [], 900, 300, True), "did not turn"),
    )
    expect(
        "ovf: problems() tolerates a DESYNC only when asked",
        has(problems(GOOD_NET + [dline], GOOD_HARNESS), "clause 4")
        and not has(problems(GOOD_NET + [dline], GOOD_HARNESS, allow_desync=True), "clause 4"),
    )
    print("check_cap_fill --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    prj = 0
    if "--prj" in argv:
        i = argv.index("--prj")
        prj = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2 :]
    order = None
    if "--order" in argv:
        i = argv.index("--order")
        order = argv[i + 1]
        if order not in ("wide", "alias", "sp"):
            print(__doc__)
            return 2
        argv = argv[:i] + argv[i + 2 :]
    ovf = "--ovf" in argv
    blind = "--blind" in argv
    ovf_desync = None
    target = "buildings"
    if "--ovf-desync" in argv:
        i = argv.index("--ovf-desync")
        try:
            parts = argv[i + 1].split(":")  # STEP:IDX[:TARGET]  (TARGET: buildings | queue | pathN)
            if len(parts) > 2:
                target = parts[2]
                poke_target(target, int(parts[1]))
            ovf_desync = (int(parts[0]), int(parts[1]))
        except (IndexError, ValueError):
            print(__doc__)
            return 2
        argv = argv[:i] + argv[i + 2 :]
    args = [a for a in argv if not a.startswith("--")]
    if not args or (len(args) > 1 and not order and ovf_desync is None):
        print(__doc__)
        return 2
    # A cross-peer row (post_check_peers) hands EVERY peer's run dir: each one must pass on its own.
    rc = max(run(d, prj, order, ovf, ovf_desync, blind, target) for d in args)
    if ovf_desync is not None:  # the stimulus ran on SOME peer (only the poked one logs it)
        poked = any(
            POKE_RE.search(ln)
            for d in args
            for ln in strip(check_storage_purge.harness_log_lines(d))
        )
        if not poked:
            print(
                "  check_cap_fill: clause 8: no peer logged `; CAP POKE` (the stimulus never ran)"
            )
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
