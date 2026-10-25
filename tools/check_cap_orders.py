#!/usr/bin/env python3
"""
tools/check_cap_orders.py -- patching:PT-CAP9-U5: the ORDER layers (queue / staging / pending) of the v0.2.3 cap raise.

The stimulus is the harness order-burst fixture (`capo_*` keys, capo_tick in mh_harness/harness.cpp). With libmh.dll ABSENT and `[caps] raise=1`
the unit-fill fixture (`capu_*`) gives a player a roster of hundreds of units, then the fixture issues `capo_n` move orders IN ONE SIM STEP
through the game's own llm_strat_unit_order_move; unit u's target T(u) is a seeded tile and every peer later counts the units whose goal fields
(+0x86/+0x87) hold it -- orders APPLIED, not displacement (`; CAPO VERDICT ... applied=N`). The order layer's own census (`; [caps] orders step=...
hw q=.. p=.. s=.. ovf q=.. p=.. s=.. quota=..`, mh_net.log) says how deep each array got and whether anything was refused.

Arms (--arm), shapes (--shape):
  on   the order layer is ON: every unit that was ordered reached its target, the arrays went past their vanilla bound (hw), nothing was refused
       (ovf 0), no guard was hit.
  off  `[caps] orders_layer=0` (the pre-fix behaviour on the raised roster): the layer is absent and the figures are the VANILLA drop, computed
       here from the burst: sp -> the queue [300] is WIPED at the 301st order (survivors = the orders after the wipe); mp -> the staging list [300]
       takes 2 records (order + its 0xfb echo) per order, so 150 orders survive.
  shape sp   one peer, the immediate lane (queue).
  shape mp   two peers; every peer's log must show the SAME applied figure (the orders replicated); --need-hw-staging N asks staging/pending hw >= N.

Clauses (all must hold; the figures in [brackets] are what a clause compares):
  1  mh_net.log: on -> `orders layer STAGED` + `orders layer ON` naming queue 4096 / pending 32768 / staging 2048, no `[caps] ... REFUSED`;
     off -> `orders layer OFF ([caps] orders_layer=0)` and no `orders layer ON`.
  2  mh_harness.log: a `; CAPU VERDICT ok=1` (the roster was filled), a `; CAPO ... FIRE` for the burst, no `; CAPO FAIL`.
  3  `; CAPO VERDICT`: on -> applied == ordered_units; off -> applied == the vanilla prediction [sp: survivors of the wipe; mp: 150 orders].
  4  census (on): the last `orders step=` line has hw q >= the burst's orders (sp) / hw s,p >= 2 x orders (mp), ovf q=p=s=quota=0, guard=0x0.
  5  the game kept stepping after the verdict; no `*** DESYNC`, no crash banner, no `ORDERS GUARD HIT`.
  6  mp: the verdict is IDENTICAL on every peer (same applied, same units).

  python tools/check_cap_orders.py --arm on|off --shape sp|mp [--slack N] [--need-hw-q N] [--need-hw-staging N] [--records 1|2] [--applied N] [--allow-refusal] [--hwq-exact N] <run-dir> [<run-dir> ...]
  (--records: staging records per order -- 2 for move orders (order + 0xfb echo), 1 for the status-bit orders of the capo_kind=1 rows)
  --save N (PT-CAP9-U7c): the save row. The harness saved in the fire step while the queue held the whole burst (N records, pending/staging empty) and loaded
  the file back capo_save_gap steps later; clauses S1-S4: the save/load returned 1 in later steps, the queue count and the records (digest) survive, the planet file
  ends in a footer whose trailer carries queue records 300..N-1 (`orders N-300`), the container is stamped, the load logged `queue count N restored`
  and nothing was DROPPED / REFUSED.
  python tools/check_cap_orders.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
FIRE_RE = re.compile(
    r"; CAPO step=(?P<step>\d+) FIRE burst=(?P<b>\d+) player=(?P<p>\d+) issuer=(?P<iss>\d) orders=(?P<n>\d+) units=(?P<nu>\d+) "
    r"cnt_before q=(?P<q0>-?\d+) p=(?P<p0>-?\d+) s=(?P<s0>-?\d+) cnt_after q=(?P<q1>-?\d+) p=(?P<p1>-?\d+) s=(?P<s1>-?\d+)"
)
VERDICT_RE = re.compile(
    r"; CAPO VERDICT step=(?P<step>\d+) burst=(?P<b>\d+) player=(?P<p>\d+) orders=(?P<n>\d+) units=(?P<nu>\d+) ordered_units=(?P<ord>\d+) "
    r"applied=(?P<ap>\d+) goal_changed=(?P<gc>\d+)"
)
CAPU_RE = re.compile(r"; CAPU2? VERDICT ok=1 step=(?P<step>\d+) units=(?P<u>\d+)")
SAMPLE_RE = re.compile(r"; CAPU2? step=(?P<step>\d+) SAMPLE ")
ON_RE = re.compile(
    r"\[caps\] orders layer ON \(.*queue (?P<q>\d+) pending (?P<p>\d+) staging (?P<s>\d+); sender quota (?P<quota>\d+)"
)
CENSUS_RE = re.compile(
    r"\[caps\] orders step=(?P<step>\d+) hw q=(?P<hq>\d+) p=(?P<hp>\d+) s=(?P<hs>\d+) \(of (?P<cq>\d+)/(?P<cp>\d+)/(?P<cs>\d+)\) "
    r"ovf q=(?P<oq>\d+) p=(?P<op>\d+) s=(?P<os>\d+) quota=(?P<oquota>\d+) cnt q=(?P<nq>-?\d+) p=(?P<np>-?\d+) s=(?P<ns>-?\d+) guard=0x(?P<g>[0-9A-Fa-f]+)"
)

# --slack N: a move-order burst is judged by goal == target after `capo_wait` steps, and a unit that has ARRIVED (or whose goal the game cleared) in that
# window no longer reads as applied -- the figure decays with the wait (3 steps: 113 of 490, 40: 487, 150: 482; the queue drains over the first steps, the
# arrivals eat it afterwards), identically with the layer on or off. SLACK is how many units below the model a move row may sit; 0 for every other row.
SLACK = 0
VANILLA_QUEUE = 300
VANILLA_STAGING = 300


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


# ---- --save N (PT-CAP9-U7c): the order QUEUE across a game_SaveGame / llm_game_load round trip --------------------------------------
# The harness (capo_save=1, capo_save_gap=G) saves in the fire step, while the queue still holds the whole burst, and loads the file back G steps later.
SAVE_LINE_RE = re.compile(
    r"; CAPO SAVE (?P<w>before-save|after-save|after-load) step=(?P<step>\d+) rc=(?P<rc>\d+) q=(?P<q>-?\d+) p=(?P<p>-?\d+) s=(?P<s>-?\d+) "
    r"qcap=(?P<qcap>\d+) digest=(?P<d>[0-9A-Fa-f]+)"
)
SAVE_RC_RE = re.compile(r"; CAPO SAVE rc step=(?P<step>\d+) save=(?P<s>\d+) load=(?P<l>\d+)")
SAVE_FILE_RE = re.compile(r"; CAP SAVE FILE name=\S+ size=(?P<size>\d+) fnv=\w+ stamped=(?P<st>\d)")
SAVE_PLANET_RE = re.compile(r"; CAP SAVE PLANET file=\S+ size=\d+ footer=(?P<f>\d) footer_len=\d+")
TRAILER_RE = re.compile(
    r"\[caps\] save: planet (?P<pl>-?\d+) trailer (?P<n>\d+) records \((?P<b>\d+) bytes\) v2 path (?P<path>\d+) orders (?P<o>\d+)"
)
RESTORED_RE = re.compile(
    r"\[caps\] orders: queue count (?P<c>\d+) restored \(records (?P<a>\d+)\.\.(?P<z>\d+) came from the MHOV blob\)"
)


def save_problems(net, harness, n_orders):
    """Clauses S1-S4 of the save row. n_orders = the queue count the burst leaves (and the file must bring back)."""
    net, harness = strip(net), strip(harness)
    bad = []
    lines = {}
    for ln in harness:
        m = SAVE_LINE_RE.match(ln)
        if m:
            lines[m.group("w")] = m
    for w in ("before-save", "after-save", "after-load"):
        if w not in lines:
            bad.append("clause S1: no `; CAPO SAVE %s` line (capo_save never reached)" % w)
    if bad:
        return bad
    b, a, l = lines["before-save"], lines["after-save"], lines["after-load"]
    if int(b.group("q")) != n_orders:
        bad.append(
            "clause S1: the queue held %s records at the save, expected %d"
            % (b.group("q"), n_orders)
        )
    if int(b.group("p")) != 0 or int(b.group("s")) != 0:
        bad.append(
            "clause S1: pending/staging not EMPTY at the save (p=%s s=%s): the planet save has no block for them"
            % (b.group("p"), b.group("s"))
        )
    if a.group("rc") != "1":
        bad.append("clause S1: the save returned %s" % a.group("rc"))
    if l.group("rc") != "1":
        bad.append("clause S1: the load returned %s" % l.group("rc"))
    if int(l.group("step")) <= int(b.group("step")):
        bad.append(
            "clause S1: the load did not run in a LATER step (a same-step load can hide the clamp)"
        )
    if l.group("q") != b.group("q"):
        bad.append(
            "clause S2: the queue count after the load is %s, saved %s"
            % (l.group("q"), b.group("q"))
        )
    if l.group("d") != b.group("d"):
        bad.append(
            "clause S2: the queue records differ across the round trip (digest %s -> %s)"
            % (b.group("d"), l.group("d"))
        )
    if int(l.group("p")) != 0 or int(l.group("s")) != 0:
        bad.append("clause S2: pending/staging not empty right after the load")
    files = [m for m in (SAVE_FILE_RE.match(ln) for ln in harness) if m]
    planets = [m for m in (SAVE_PLANET_RE.match(ln) for ln in harness) if m]
    if not files or files[-1].group("st") != "1":
        bad.append(
            "clause S3: the container is not stamped (an OFF build would run 150 stale orders)"
        )
    if not planets or planets[-1].group("f") != "1":
        bad.append("clause S3: the planet file does not end in an MHOF footer")
    tr = [m for m in (TRAILER_RE.search(ln) for ln in net) if m]
    tail = n_orders - 300
    if not tr:
        bad.append("clause S3: no `[caps] save: planet P trailer ... v2 path K orders M` line")
    elif int(tr[-1].group("o")) != tail:
        bad.append(
            "clause S3: the trailer carries %s queue records, expected %d (records 300..%d)"
            % (tr[-1].group("o"), tail, n_orders - 1)
        )
    if not any("[caps] load: planet file overflow" in ln and "applied" in ln for ln in net):
        bad.append("clause S3: no `[caps] load: planet file overflow N records applied`")
    rs = [m for m in (RESTORED_RE.search(ln) for ln in net) if m]
    if (
        not rs
        or int(rs[-1].group("c")) != n_orders
        or int(rs[-1].group("a")) != 300
        or int(rs[-1].group("z")) != n_orders - 1
    ):
        bad.append(
            "clause S4: no `orders: queue count %d restored (records 300..%d ...)` line"
            % (n_orders, n_orders - 1)
        )
    bad += [
        "clause S4: " + ln
        for ln in net
        if "[caps] orders:" in ln and ("DROPPED" in ln or "did not come back" in ln)
    ]
    bad += ["clause S4: " + ln for ln in net if "[caps] load REFUSED" in ln]
    return bad


# ---- the vanilla drop, derived from the burst ------------------------------------------------------------------------------------
def predict_off(shape, n, nu, records=2):
    """-> (applied units, orders that reach the game). The orders are k = 0..n-1 on unit k % nu (the same target on every lap)."""
    if shape == "sp":
        # llm_strat_order_enqueue: count >= 300 -> count = 0 and the order is lost; so the survivors are the orders AFTER the last wipe.
        # Each order enqueues `records` records (the primary first, then its 0xfb echo); only a surviving PRIMARY applies the order.
        count, kept = 0, []
        for k in range(n):
            for r in range(records):
                if count >= VANILLA_QUEUE:
                    count = 0
                    kept = []
                else:
                    kept.append((k, r))
                    count += 1
        survivors = [k for k, r in kept if r == 0]
    else:
        # llm_strat_order_stage_scheduled: a full staging list returns 0; each order stages `records` records (2 = order + 0xfb echo, the
        # move orders; 1 = the ctrl-group status-bit orders the PT-CAP9-U5 rows use, which are staged without an echo)
        per = VANILLA_STAGING // records
        survivors = list(range(min(n, per)))
    return len(survivors), len(
        survivors
    )  # the harness counts APPLIED ORDERS (one lap bit each), not units


def problems(
    net,
    harness,
    arm="on",
    shape="sp",
    need_hw_q=None,
    need_hw_staging=None,
    records=2,
    applied_exact=None,
    allow_refusal=False,
    hwq_exact=None,
):
    net, harness = strip(net), strip(harness)
    bad = []
    on_line = [m for m in (ON_RE.search(ln) for ln in net) if m]
    if arm == "on":
        if not any("[caps] orders layer STAGED" in ln for ln in net):
            bad.append("clause 1: no `[caps] orders layer STAGED` in mh_net.log")
        if not on_line:
            bad.append(
                "clause 1: no `[caps] orders layer ON (...queue Q pending P staging S; sender quota N` line"
            )
        elif (on_line[-1].group("q"), on_line[-1].group("p"), on_line[-1].group("s")) != (
            "4096",
            "32768",
            "2048",
        ):
            bad.append(
                "clause 1: orders layer ON names queue %s / pending %s / staging %s, expected 4096 / 32768 / 2048"
                % on_line[-1].group("q", "p", "s")
            )
        elif on_line[-1].group("quota") != "2048":
            bad.append("clause 1: sender quota %s, expected 2048" % on_line[-1].group("quota"))
    else:
        if not any("[caps] orders layer OFF ([caps] orders_layer=0)" in ln for ln in net):
            bad.append(
                "clause 1: no `orders layer OFF ([caps] orders_layer=0)` banner (the negative arm did not reach the process)"
            )
        if on_line:
            bad.append("clause 1: the order layer went ON in the negative arm")
    bad += [
        "clause 1: " + ln
        for ln in net
        if "[caps]" in ln
        and "REFUSED" in ln
        and "orders" not in ln.lower()
        and "limit layer" not in ln.lower()
    ]
    bad += [
        "clause 1: " + ln
        for ln in net
        if "[caps] orders" in ln
        and "REFUSED" in ln
        and not ln.startswith("; [caps] orders: REFUSED at the sender")
    ]
    bad += [
        "clause 2: " + ln
        for ln in harness
        if ln.startswith("; CAPO FAIL")
        or ln.startswith("; CAPU FAIL")
        or ln.startswith("; CAPU2 FAIL")
    ]
    capu = [m for m in (CAPU_RE.match(ln) for ln in harness) if m]
    if not capu:
        bad.append("clause 2: no `; CAPU VERDICT ok=1` (the roster was never filled)")
    fires = [m for m in (FIRE_RE.match(ln) for ln in harness) if m]
    if not fires:
        bad.append("clause 2: no `; CAPO ... FIRE` line (the burst never fired)")
    verdicts = [m for m in (VERDICT_RE.match(ln) for ln in harness) if m]
    if not verdicts:
        bad.append("clause 3: no `; CAPO VERDICT` line")
        return bad
    for v in verdicts:
        n, nu, ordered, applied = (
            int(v.group("n")),
            int(v.group("nu")),
            int(v.group("ord")),
            int(v.group("ap")),
        )
        fire = next((f for f in fires if f.group("b") == v.group("b")), None)
        issuer = fire is not None and fire.group("iss") == "1"
        if arm == "on" and applied_exact is not None:
            if applied != applied_exact:
                bad.append(
                    "clause 3: burst %s applied %d orders, expected exactly %d (orders %d)"
                    % (v.group("b"), applied, applied_exact, n)
                )
        elif arm == "on":
            if not (ordered - SLACK <= applied <= ordered):
                bad.append(
                    "clause 3: burst %s applied %d of %d ordered (orders %d)"
                    % (v.group("b"), applied, ordered, n)
                )
        else:
            want, _reach = predict_off(shape, n, nu, records)
            if not (want - SLACK <= applied <= want):
                bad.append(
                    "clause 3: burst %s (off arm) applied %d, the vanilla drop predicts %d (orders %d, units %d)"
                    % (v.group("b"), applied, want, n, nu)
                )
            if applied >= ordered:
                bad.append(
                    "clause 3: the off arm applied EVERY ordered unit (%d) -- the row cannot see the defect"
                    % applied
                )
        if (
            shape == "sp"
            and arm == "on"
            and issuer
            and fire is not None
            and int(fire.group("q1")) < n
        ):
            bad.append(
                "clause 3: the queue count right after the burst is %s, below the %d orders issued"
                % (fire.group("q1"), n)
            )
    census = [m for m in (CENSUS_RE.search(ln) for ln in net) if m]
    if arm == "on":
        if not census:
            bad.append("clause 4: no `[caps] orders step=... hw ...` census line")
        else:
            last = {
                k: int(v, 16) if k == "g" else int(v) for k, v in census[-1].groupdict().items()
            }
            if need_hw_q is not None and last["hq"] < need_hw_q:
                bad.append("clause 4: queue high-water %d < %d" % (last["hq"], need_hw_q))
            if (
                need_hw_staging is not None and last["hp"] < need_hw_staging
            ):  # staging itself is the SENDER's (run() checks one peer reached it)
                bad.append(
                    "clause 4: pending hw %d < %d (staging hw %d)"
                    % (last["hp"], need_hw_staging, last["hs"])
                )
            if hwq_exact is not None and last["hq"] != hwq_exact:
                bad.append(
                    "clause 4: queue high-water %d, expected exactly %d (the release bound of the carry-over probe)"
                    % (last["hq"], hwq_exact)
                )
            if allow_refusal:
                # the SENDER refuses at its own quota (counted in os and quota); a receiver and every other bound stay at zero
                for k in ("oq", "op"):
                    if last[k]:
                        bad.append(
                            "clause 4: refusals counted (%s=%d) at a bound that is not the sender's quota"
                            % (k, last[k])
                        )
                sender = last["oquota"] > 0
                if sender != (last["os"] > 0):
                    bad.append(
                        "clause 4: quota refusals %d vs staging refusals %d disagree"
                        % (last["oquota"], last["os"])
                    )
                if sender and not any("orders: player notice shown" in ln for ln in net):
                    bad.append(
                        "clause 4: the sender refused orders but no `player notice shown` line was logged"
                    )
                if not sender and any("orders: player notice shown" in ln for ln in net):
                    bad.append("clause 4: a peer that refused nothing showed the refusal notice")
            else:
                for k in ("oq", "op", "os", "oquota"):
                    if last[k]:
                        bad.append(
                            "clause 4: refusals counted (%s=%d) -- an order was dropped"
                            % (k, last[k])
                        )
            if last["g"]:
                bad.append("clause 4: guard bits 0x%X" % last["g"])
    else:
        if census:
            bad.append("clause 4: a census line in the negative arm (the layer was ON)")
    last_verdict = max(int(v.group("step")) for v in verdicts)
    later = [int(m.group("step")) for m in (SAMPLE_RE.match(ln) for ln in harness) if m]
    if later and max(later) <= last_verdict and False:
        bad.append("clause 5: no SAMPLE after the verdict")
    bad += ["clause 5: " + ln for ln in net + harness if "*** DESYNC" in ln or "GUARD HIT" in ln]
    return bad


def run(
    run_dirs,
    arm,
    shape,
    need_hw_q,
    need_hw_staging,
    records=2,
    applied_exact=None,
    allow_refusal=False,
    hwq_exact=None,
    save_n=None,
):
    import check_cancel_task
    import check_storage_purge

    rc = 0
    sig = []
    senders = []
    stag_hw = []
    for d in run_dirs:
        net = check_cancel_task.net_log_lines(d)
        harness = check_storage_purge.harness_log_lines(d)
        bad = problems(
            net,
            harness,
            arm,
            shape,
            need_hw_q,
            need_hw_staging,
            records,
            applied_exact,
            allow_refusal,
            hwq_exact,
        )
        if save_n:
            bad += save_problems(net, harness, save_n)
        cen = [m for m in (CENSUS_RE.search(ln) for ln in strip(net)) if m]
        senders.append(bool(cen) and int(cen[-1].group("oquota")) > 0)
        stag_hw.append(int(cen[-1].group("hs")) if cen else 0)
        for ln in strip(harness):
            if (
                ln.startswith("; CAPO")
                and "FIRE" in ln
                or ln.startswith("; CAPO VERDICT")
                or ln.startswith("; CAPU VERDICT")
                or ln.startswith("; CAPU2 VERDICT")
                or ln.startswith("; CAPO SAVE")
                or ln.startswith("; CAP SAVE")
            ):
                print("  " + ln[:230])
        for ln in strip(net):
            if "[caps] orders" in ln and "census" not in ln:
                print("  " + ln[:230])
        for b in bad:
            print("  check_cap_orders: " + b)
        sig.append(
            [
                (m.group("b"), m.group("ap"), m.group("nu"))
                for m in (VERDICT_RE.match(ln) for ln in strip(harness))
                if m
            ]
        )
        rc = rc or (1 if bad else 0)
    if shape == "mp" and len(sig) > 1 and any(s != sig[0] for s in sig[1:]):
        print("  check_cap_orders: clause 6: the peers disagree on the applied figures: %s" % sig)
        rc = 1
    if arm == "on" and need_hw_staging is not None and max(stag_hw or [0]) < need_hw_staging:
        print(
            "  check_cap_orders: clause 4: no peer's staging high-water reached %d (best %s)"
            % (need_hw_staging, max(stag_hw or [0]))
        )
        rc = 1
    if allow_refusal and sum(senders) != 1:
        print(
            "  check_cap_orders: clause 4: %d peer(s) refused at a sender quota, expected exactly 1 (the sender)"
            % sum(senders)
        )
        rc = 1
    print("check_cap_orders: %s" % ("FAIL" if rc else "PASS"))
    return rc


GOOD_NET_ON = [
    "; [caps] orders layer STAGED (217 sites, queue/pending/staging, .mhcapo=0x03160000); written with the raise",
    "; [caps] orders layer ON (gameplay settings): 217 sites -> .mhcapo 03160000; queue 4096 pending 32768 staging 2048; sender quota 2048 records/player",
    "; [caps] orders step=2105 hw q=500 p=0 s=0 (of 4096/32768/2048) ovf q=0 p=0 s=0 quota=0 cnt q=500 p=0 s=0 guard=0x0",
]
GOOD_HARNESS_ON = [
    "; CAPU step=300 START player 0 proto 34 crewed proto 35 (crew 1) target=490 units / 0 soldiers batch=20 row units=500 soldiers=500",
    "; CAPU VERDICT ok=1 step=2000 units=490(hi 490) soldiers=2(hi 3) issued=520 hw_units=490 hw_soldiers=3",
    "; CAPO step=2100 FIRE burst=0 player=0 issuer=1 orders=500 units=490 cnt_before q=0 p=0 s=0 cnt_after q=500 p=0 s=0",
    "; CAPO VERDICT step=2140 burst=0 player=0 orders=500 units=490 ordered_units=490 applied=490 goal_changed=490",
]
GOOD_NET_OFF = [
    "; [caps] orders layer OFF ([caps] orders_layer=0) -- the raised roster runs on the vanilla order arrays (negative arm)"
]


def selftest():
    fails = []

    def expect(name, ok, detail=""):
        print("  [%s] %s" % ("ok" if ok else "FAIL", name))
        if not ok:
            fails.append(name)
            if detail:
                print("      " + str(detail))

    def has(lst, needle):
        return any(needle in x for x in lst)

    expect(
        "sp off prediction: 500 orders on 490 units -> 199 orders survive the wipe and 199 units",
        predict_off("sp", 500, 490, 1) == (199, 199),
        predict_off("sp", 500, 490, 1),
    )
    expect(
        "mp off prediction: 500 orders -> 150 survive (2 staging records each)",
        predict_off("mp", 500, 490) == (150, 150),
        predict_off("mp", 500, 490),
    )
    expect(
        "a small burst loses nothing in the vanilla arrays",
        predict_off("sp", 100, 490) == (100, 100),
    )
    expect(
        "sp off prediction, order + echo (2 records each): the wipe leaves 48 applied",
        predict_off("sp", 500, 490) == (48, 48),
        predict_off("sp", 500, 490),
    )
    expect(
        "a good ON run is green",
        problems(GOOD_NET_ON, GOOD_HARNESS_ON, "on", "sp", 500) == [],
        problems(GOOD_NET_ON, GOOD_HARNESS_ON, "on", "sp", 500),
    )
    expect("no STAGED banner is RED", has(problems(GOOD_NET_ON[1:], GOOD_HARNESS_ON), "STAGED"))
    expect(
        "the vanilla bounds in the ON line are RED",
        has(
            problems(
                [GOOD_NET_ON[0], GOOD_NET_ON[1].replace("queue 4096", "queue 300"), GOOD_NET_ON[2]],
                GOOD_HARNESS_ON,
            ),
            "expected 4096",
        ),
    )
    expect(
        "a wrong sender quota is RED",
        has(
            problems(
                [
                    GOOD_NET_ON[0],
                    GOOD_NET_ON[1].replace("quota 2048", "quota 1024"),
                    GOOD_NET_ON[2],
                ],
                GOOD_HARNESS_ON,
            ),
            "sender quota",
        ),
    )
    expect(
        "fewer units applied than ordered is RED",
        has(
            problems(
                GOOD_NET_ON, [x.replace("applied=490", "applied=199") for x in GOOD_HARNESS_ON]
            ),
            "applied 199 of 490",
        ),
    )
    expect(
        "a queue high-water below the asked is RED",
        has(problems(GOOD_NET_ON, GOOD_HARNESS_ON, "on", "sp", 600), "high-water"),
    )
    expect(
        "a counted refusal is RED",
        has(
            problems(
                GOOD_NET_ON[:2] + [GOOD_NET_ON[2].replace("ovf q=0", "ovf q=1")], GOOD_HARNESS_ON
            ),
            "refusals counted",
        ),
    )
    expect(
        "a guard hit is RED",
        has(
            problems(
                GOOD_NET_ON[:2] + [GOOD_NET_ON[2].replace("guard=0x0", "guard=0x4")],
                GOOD_HARNESS_ON,
            ),
            "guard bits",
        ),
    )
    expect(
        "a queue count below the orders issued is RED",
        has(
            problems(
                GOOD_NET_ON,
                [x.replace("cnt_after q=500", "cnt_after q=199") for x in GOOD_HARNESS_ON],
            ),
            "right after the burst",
        ),
    )
    expect(
        "a CAPO FAIL is RED",
        has(problems(GOOD_NET_ON, ["; CAPO FAIL step=5: x"] + GOOD_HARNESS_ON), "CAPO FAIL"),
    )
    expect("no burst is RED", has(problems(GOOD_NET_ON, GOOD_HARNESS_ON[:2]), "FIRE"))
    expect(
        "a desync is RED",
        has(problems(GOOD_NET_ON + ["; [desync] *** DESYNC step=9"], GOOD_HARNESS_ON), "DESYNC"),
    )
    off_h = [
        x.replace("applied=490", "applied=199").replace("cnt_after q=500", "cnt_after q=199")
        for x in GOOD_HARNESS_ON
    ]
    expect(
        "a good OFF run (199 applied) is green",
        problems(GOOD_NET_OFF, off_h, "off", "sp", None, None, 1) == [],
        problems(GOOD_NET_OFF, off_h, "off", "sp", None, None, 1),
    )
    global SLACK
    SLACK = 6
    expect(
        "--slack: 196 applied against a model of 199 is green on the OFF arm",
        problems(
            GOOD_NET_OFF,
            [
                x.replace("applied=490", "applied=196").replace(
                    "cnt_after q=500", "cnt_after q=199"
                )
                for x in GOOD_HARNESS_ON
            ],
            "off",
            "sp",
            None,
            None,
            1,
        )
        == [],
    )
    expect(
        "--slack: 484 of 490 ON is green, 483 is RED",
        problems(GOOD_NET_ON, [x.replace("applied=490", "applied=484") for x in GOOD_HARNESS_ON])
        == []
        and has(
            problems(
                GOOD_NET_ON, [x.replace("applied=490", "applied=483") for x in GOOD_HARNESS_ON]
            ),
            "applied 483 of 490",
        ),
    )
    SLACK = 0
    expect(
        "an OFF run that applied everything cannot see the defect: RED",
        has(problems(GOOD_NET_OFF, GOOD_HARNESS_ON, "off", "sp", None, None, 1), "predicts"),
    )
    expect(
        "the OFF arm with the layer ON is RED",
        has(problems(GOOD_NET_ON, off_h, "off", "sp", None, None, 1), "negative arm"),
    )
    mp_h = [x.replace("applied=490", "applied=150") for x in GOOD_HARNESS_ON]
    expect(
        "a good MP OFF run (150 applied) is green",
        problems(GOOD_NET_OFF, mp_h, "off", "mp") == [],
        problems(GOOD_NET_OFF, mp_h, "off", "mp"),
    )
    mp1 = [x.replace("applied=490", "applied=300") for x in GOOD_HARNESS_ON]
    expect(
        "a good MP OFF run with 1 record per order (300 applied) is green",
        problems(GOOD_NET_OFF, mp1, "off", "mp", None, None, 1) == [],
        problems(GOOD_NET_OFF, mp1, "off", "mp", None, None, 1),
    )
    expect(
        "300 applied is RED when the orders stage 2 records each",
        has(problems(GOOD_NET_OFF, mp1, "off", "mp"), "predicts"),
    )
    q_net = GOOD_NET_ON[:2] + [
        "; [caps] orders step=900 hw q=40 p=2048 s=2048 (of 4096/32768/2048) ovf q=0 p=0 s=1900 quota=1900 cnt q=0 p=0 s=0 guard=0x0",
        "; [caps] orders: player notice shown (per-player quota) for player 0",
    ]
    r_net = GOOD_NET_ON[:2] + [
        "; [caps] orders step=900 hw q=40 p=2048 s=0 (of 4096/32768/2048) ovf q=0 p=0 s=0 quota=0 cnt q=0 p=0 s=0 guard=0x0"
    ]
    q_h = [x.replace("applied=490", "applied=1024") for x in GOOD_HARNESS_ON]
    expect(
        "a sender that refused at its quota, with the notice, is green when allowed",
        problems(q_net, q_h, "on", "mp", None, None, 2, 1024, True) == [],
        problems(q_net, q_h, "on", "mp", None, None, 2, 1024, True),
    )
    expect(
        "the same refusals are RED when not allowed",
        has(problems(q_net, q_h, "on", "mp", None, None, 2, 1024, False), "refusals counted"),
    )
    expect(
        "a receiver with zero refusals is green when refusals are allowed",
        problems(r_net, q_h, "on", "mp", None, None, 2, 1024, True) == [],
    )
    expect(
        "a sender without the notice line is RED",
        has(problems(q_net[:3], q_h, "on", "mp", None, None, 2, 1024, True), "notice"),
    )
    expect(
        "applied != the exact figure is RED",
        has(
            problems(
                q_net,
                [x.replace("applied=490", "applied=1000") for x in GOOD_HARNESS_ON],
                "on",
                "mp",
                None,
                None,
                2,
                1024,
                True,
            ),
            "expected exactly 1024",
        ),
    )
    expect(
        "an exact queue high-water that differs is RED",
        has(
            problems(GOOD_NET_ON, GOOD_HARNESS_ON, "on", "mp", None, None, 2, None, False, 300),
            "expected exactly 300",
        ),
    )
    expect(
        "a stray staging hw requirement is RED when short",
        has(problems(GOOD_NET_ON, GOOD_HARNESS_ON, "on", "mp", None, 1000), "staging hw"),
    )
    # ---- --save 450 (PT-CAP9-U7c)
    sv_h = [
        "; CAPO SAVE before-save step=2100 rc=0 q=450 p=0 s=0 qcap=4096 digest=0123456789ABCDEF",
        "; CAPO SAVE after-save step=2100 rc=1 q=450 p=0 s=0 qcap=4096 digest=0123456789ABCDEF",
        "; CAP SAVE FILE name=capord size=900000 fnv=DEADBEEF stamped=1",
        "; CAP SAVE PLANET file=save\\temp\\save01.dat size=400000 footer=1 footer_len=30000",
        "; CAPO SAVE after-load step=2120 rc=1 q=450 p=0 s=0 qcap=4096 digest=0123456789ABCDEF",
        "; CAPO SAVE rc step=2120 save=1 load=1",
    ]
    sv_n = [
        "; [caps] save: planet 1 trailer 540 records (39000 bytes) v2 path 0 orders 150",
        "; [caps] load: planet file overflow 540 records applied",
        "; [caps] orders: queue count 450 restored (records 300..449 came from the MHOV blob)",
    ]
    expect(
        "a good save run is green",
        save_problems(sv_n, sv_h, 450) == [],
        save_problems(sv_n, sv_h, 450),
    )
    expect(
        "a queue count that fell back to 300 after the load is RED",
        has(
            save_problems(
                sv_n,
                [
                    x.replace("after-load step=2120 rc=1 q=450", "after-load step=2120 rc=1 q=300")
                    for x in sv_h
                ],
                450,
            ),
            "queue count after the load",
        ),
    )
    expect(
        "records that differ across the round trip are RED",
        has(
            save_problems(
                sv_n,
                [
                    x.replace("digest=0123456789ABCDEF", "digest=FFFFFFFFFFFFFFFF")
                    if "after-load" in x
                    else x
                    for x in sv_h
                ],
                450,
            ),
            "digest",
        ),
    )
    expect(
        "a same-step load is RED",
        has(
            save_problems(sv_n, [x.replace("step=2120", "step=2100") for x in sv_h], 450),
            "LATER step",
        ),
    )
    expect(
        "pending records at the save are RED",
        has(
            save_problems(
                sv_n,
                [x.replace("q=450 p=0", "q=450 p=3") if "before" in x else x for x in sv_h],
                450,
            ),
            "not EMPTY",
        ),
    )
    expect(
        "an unstamped container is RED",
        has(
            save_problems(sv_n, [x.replace("stamped=1", "stamped=0") for x in sv_h], 450),
            "not stamped",
        ),
    )
    expect(
        "a trailer without the 150 queue records is RED",
        has(
            save_problems(
                [x.replace("orders 150", "orders 0") for x in sv_n[:1]] + sv_n[1:], sv_h, 450
            ),
            "carries 0 queue records",
        ),
    )
    expect(
        "a count that was clamped (DROPPED) is RED",
        has(
            save_problems(
                sv_n[:2]
                + [
                    "; [caps] orders: queue count 450 is past the vanilla window 300 and the tail did not come back (0 of 150 records) -- clamped to 300, the rest is DROPPED"
                ],
                sv_h,
                450,
            ),
            "restored",
        ),
    )
    expect("no save lines is RED", has(save_problems(sv_n, [], 450), "capo_save never reached"))
    print("check_cap_orders --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    save_n = None
    arm, shape, hwq, hws, records, applied_exact, allow_refusal, hwq_exact = (
        "on",
        "sp",
        None,
        None,
        2,
        None,
        False,
        None,
    )
    args = []
    it = iter(argv)
    for a in it:
        if a == "--arm":
            arm = next(it)
        elif a == "--shape":
            shape = next(it)
        elif a == "--need-hw-q":
            hwq = int(next(it))
        elif a == "--need-hw-staging":
            hws = int(next(it))
        elif a == "--records":
            records = int(next(it))
        elif a == "--applied":
            applied_exact = int(next(it))
        elif a == "--slack":
            global SLACK
            SLACK = int(next(it))
        elif a == "--allow-refusal":
            allow_refusal = True
        elif a == "--hwq-exact":
            hwq_exact = int(next(it))
        elif a == "--save":
            save_n = int(next(it))
        elif not a.startswith("--"):
            args.append(a)
    if not args:
        print(__doc__)
        return 2
    return run(args, arm, shape, hwq, hws, records, applied_exact, allow_refusal, hwq_exact, save_n)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
