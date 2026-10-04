#!/usr/bin/env python3
"""check_u64_failure.py -- mp:U64 (HM-M6): the verdict of a 4-peer `det_arms.py --determinism --u64 <arm>` run.

Reads the four pulled peer dirs (<det_dir>/{host,client1,client2,client3}); the arms and their incident knobs are
described in tools/u64_arms.py (ARMS). Pass conditions, per arm kind:

  PLAYS ON (elect4, newhub, partition, heal): every expected survivor
    * logs `failover SUSPECT` and `failover CORROBORATED`, and the SAME `hub elected <id> epoch <e>` line (id and epoch
      agree across survivors; the id is the arm's `expect_hub` when it names one; the elected peer says it became the hub,
      the others say they re-dialled it);
    * stays ALL PAIRS IDENTICAL with every other survivor for >= MIN_POST steps past the LAST incident (mp_analyze);
    * never stalls the game clock longer than STALL_MAX_MS around an incident;
    * is never dropped by a silence timer (`GS2: peer <survivor>` / `U17 fast-drop ... side=<survivor>`);
    * removes every dead / cut-off peer at the SAME step as every other survivor.
    newhub: TWO `hub elected` lines on each survivor (the second names a survivor, not the dead first hub).
    partition / heal: the cut-off host (the minority, 1 of 4) ends: it reaches `on_gameover` or logs a minority/lost
    line, and does NOT keep stepping alone. heal: after the shim passes again the survivors log NO second failover.
  MINORITY (double): the survivors (2 of 4, an even split) log `failover MINORITY (ends)` (or the equivalent budget end)
    within MINORITY_MAX_S of the loss, none of them steps on past the loss + MINORITY_STEPS, and all of them agree.
  base4: all four peers IDENTICAL over the run and every peer holds the same epoch / hub / ranking.

Absence of a log is a REFUSAL (exit 2), never a pass.

  python tools/check_u64_failure.py <det_dir> <arm>
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_u53_elim as u53  # noqa: E402
from check_u62_handover import _wall, stall_window  # noqa: E402

PEERS = ("host", "client1", "client2", "client3")
ID_OF = {"host": 0, "client1": 1, "client2": 2, "client3": 3}
MIN_POST = 1000
STALL_MAX_MS = 6000
MINORITY_MAX_S = 40.0
MINORITY_PEER_MAX_S = (
    30.0  # a cut-off hub: the 10 s link timeout + the removals + the end, well inside U55's 58 s
)
MINORITY_GCLK_S = (
    5.0  # a minority leaves within this much GAME clock of the loss (the suspicion alone is 2 s)
)
ELECTED_RE = re.compile(r"hub elected (\d+) epoch (\d+)")
RANK_RE = re.compile(r"failover SUSPECT .*? rank=([0-9>]+)")
EPOCH_RE = re.compile(
    r"net: mesh epoch=(\d+) hub=(\d+) tier=(\d+) rank=([0-9>]+) digest=([0-9a-f]{8}) cov=(\d+)/(\d+)"
)


def _read_all():
    return None


def _first_wall(net, needle):
    for ln in net.splitlines():
        if needle in ln:
            w = _wall(ln)
            if w is not None:
                return w
    return None


def _last_step(har):
    st = u53.harness_steps(har)
    return st[-1] if st else (0, 0.0)


def _identical(det_dir, peers, min_common):
    """(ok, text-lines) for the all-pairs compare of `peers`' hashed steps from step 1 to the shorter log's end."""
    out, ok = [], True
    for i, a in enumerate(peers):
        for b in peers[i + 1 :]:
            txt = u53.analyze(os.path.join(det_dir, a), os.path.join(det_dir, b), min_common)
            mm = re.search(r"combined-hash=(\d+)", txt)
            common = int(mm.group(1)) if mm else 0
            ident = "ALL PAIRS IDENTICAL" in txt
            out.append(
                "    %s vs %s: %s, %d common hashed steps"
                % (a, b, "IDENTICAL" if ident else "NOT IDENTICAL", common)
            )
            if not ident:
                ok = False
                out += ["      " + ln for ln in txt.strip().splitlines()[-6:]]
    return ok, out


def _removals(net):
    """{dead id: step} of the removal lines a survivor logged (llm_net_player_remove / pinned removal)."""
    out = {}
    for ln in net.splitlines():
        m = re.search(r"llm_net_player_remove.*?(?:player|slot)\D{0,3}(\d+).*?step\D{0,3}(\d+)", ln)
        if m:
            out.setdefault(int(m.group(1)), int(m.group(2)))
    return out


def check(det_dir, arm, spec, crash=None, relayed=False):
    crash = spec["crash"] if crash is None else crash
    out, bad = [], []
    logs = {}
    for p in PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (
            u53._read(os.path.join(d, "mh_net.log")),
            u53._read(os.path.join(d, "mh_harness.log")),
        )
    surv = list(spec["survivors"])
    last = {p: _last_step(logs[p][1]) for p in PEERS}
    for p in PEERS:
        out.append("%-8s last hashed step=%d (gclk %.1fs)" % (p, last[p][0], last[p][1]))

    spec_p = spec.get("spectator")
    if spec_p:
        # mp:U71: the defeated peer must have become a SPECTATOR on every peer, and (unless the arm withholds the bit)
        # each survivor must have exported it to its transport (`U71 spectator mask 00 -> 0x`).
        bit = 1 << ID_OF[spec_p]
        withheld = "spectate_mask=0" in spec.get("net_extra", "")
        if not re.search(r"; U54 spectate: defeat dialog opened by a SPECTATOR", logs[spec_p][0]):
            bad.append("%s was never made a spectator (no U54 spectate line)" % spec_p)
        for sv in surv:
            ms = re.findall(r"; U71 spectator mask ([0-9a-f]{2}) -> ([0-9a-f]{2})", logs[sv][0])
            out.append("%s spectator mask transitions: %s" % (sv, ms[:3]))
            got = any(int(n, 16) & bit for _o, n in ms)
            if withheld and ms:
                bad.append("%s exported a spectator mask although spectate_mask=0: %r" % (sv, ms))
            if not withheld and not got:
                bad.append("%s never exported %s's spectator bit" % (sv, spec_p))
    if arm == "base4":
        epochs = {}
        for p in PEERS:
            ms = EPOCH_RE.findall(logs[p][0])
            epochs[p] = (
                ms[-1] if ms else None
            )  # the epoch it HOLDS at the end (client3 is admitted after epoch 1)
            out.append("%-8s final epoch line: %s" % (p, epochs[p]))
        if None in epochs.values():
            bad.append("a peer never logged a mesh epoch: %r" % (epochs,))
        elif len({e[:5] for e in epochs.values()}) != 1:
            bad.append("the peers' final epochs differ: %r" % (epochs,))
        ok, lines = _identical(det_dir, list(PEERS), 400)
        out += lines
        if not ok:
            bad.append("the four peers are not ALL PAIRS IDENTICAL")
        return (not bad), out + ["FAIL: " + b for b in bad]

    first_incident = min(crash.values()) if crash else spec.get("partition_at", 0)
    last_incident = (
        max(crash.values()) if crash else spec.get("heal_at") or spec.get("partition_at", 0)
    )
    # a game-clock stamp for the first incident, from a survivor's own hashed rows
    sv0 = surv[0]
    st = u53.harness_steps(logs[sv0][1])
    first_clock = next((c for s, c in st if s >= first_incident), 0.0)
    out.append(
        "first incident at step %d (gclk %.1fs); last at step %d"
        % (first_incident, first_clock, last_incident)
    )

    # ---- who elected whom -------------------------------------------------------------------------------
    elected = {}
    for sv in surv:
        net = logs[sv][0]
        els = ELECTED_RE.findall(net)
        elected[sv] = els
        for key in ("failover SUSPECT", "failover CORROBORATED"):
            ln = next((x for x in net.splitlines() if key in x), None)
            if ln is None and not spec.get("minority"):
                bad.append("%s never logged `%s`" % (sv, key))
            elif ln:
                out.append("    %s: %s" % (sv, ln.strip()[:170]))
        for x in [x for x in net.splitlines() if "hub elected" in x][:2]:
            out.append("    %s: %s" % (sv, x.strip()[:190]))

    if spec.get("minority"):
        for sv in surv:
            net = logs[sv][0]
            mi = [x for x in net.splitlines() if "failover MINORITY" in x or "failover FAILED" in x]
            if not mi and spec.get("neg_control") and "on_gameover" in net:
                # the negative control, relayed: the non-hub survivor follows the elected hub's minority verdict (its link
                # to the hub ends, outcome 8) without a verdict line of its own
                out.append(
                    "    %s: ended by the elected hub's minority verdict (no own MINORITY line)"
                    % sv
                )
            elif not mi:
                bad.append(
                    "%s never ended its side of the 2-of-4 minority (no failover MINORITY/FAILED line)"
                    % sv
                )
            else:
                out.append("    %s: %s" % (sv, mi[0].strip()[:200]))
                if "FAILED" in mi[0] and "MINORITY" not in mi[0]:
                    bad.append(
                        "%s ended through the failover budget (FAILED), not as a MINORITY" % sv
                    )
            if elected[sv] and not spec.get("neg_control"):
                bad.append(
                    "%s announced a hub election in a match it could not keep: %r"
                    % (sv, elected[sv])
                )
            tl = u53.timeline(net, sv)
            out += tl
            over = next((x for x in net.splitlines() if "on_gameover" in x), None)
            if over is None:
                bad.append("%s never reached on_gameover (the match did not end on it)" % sv)
            mg = re.search(
                r"on_gameover ENTER sess=\d+ outcome=(\d+) gclk=(\d+)|on_gameover outcome=(\d+) gclk=(\d+)",
                net,
            )
            if mg:
                oc = int(mg.group(1) or mg.group(3))
                gclk = int(mg.group(2) or mg.group(4)) / 1000.0
                out.append(
                    "    %s: on_gameover outcome=%d at gclk %.1fs (the loss was at gclk %.1fs)"
                    % (sv, oc, gclk, first_clock)
                )
                if oc != 8:
                    bad.append("%s ended with outcome %d, expected 8 (connection lost)" % (sv, oc))
                if gclk > first_clock + MINORITY_GCLK_S:
                    bad.append(
                        "%s played %.1f s of game clock past the loss (> %.0f): a minority must not play on"
                        % (sv, gclk - first_clock, MINORITY_GCLK_S)
                    )
            t_loss = _first_wall(net, "failover SUSPECT")
            t_end = _first_wall(net, "failover MINORITY") or _first_wall(net, "failover FAILED")
            if t_loss is not None and t_end is not None:
                out.append(
                    "    %s: minority verdict %.1f s after the suspicion" % (sv, t_end - t_loss)
                )
                if t_end - t_loss > MINORITY_MAX_S:
                    bad.append(
                        "%s took %.1f s to end (> %.0f)" % (sv, t_end - t_loss, MINORITY_MAX_S)
                    )
        # the match ENDS early on purpose, so mp_analyze reads it as "environmental"; what matters is that no desync was seen
        _ok, lines = _identical(det_dir, surv, 1)
        out += lines
        if not spec.get("neg_control") and any(
            "IN-BAND: no peer reported a desync" not in ln
            and "IN-BAND" in ln
            and "desync" in ln
            and "no peer" not in ln
            for ln in lines
        ):
            bad.append("an in-band desync was reported before the minority ended")
        return (not bad), out + ["FAIL: " + b_ for b_ in bad]

    # ---- plays on ---------------------------------------------------------------------------------------
    need = 2 if spec.get("second_failover") else 1
    for sv in surv:
        if len(elected[sv]) < need:
            bad.append(
                "%s logged %d `hub elected` line(s), expected %d" % (sv, len(elected[sv]), need)
            )
    firsts = {sv: elected[sv][0] for sv in surv if elected[sv]}
    if len(set(firsts.values())) > 1:
        bad.append("the survivors elected DIFFERENT first hubs: %r" % (firsts,))
    elif firsts:
        hid, ep = next(iter(firsts.values()))
        out.append("all survivors logged `hub elected %s epoch %s`" % (hid, ep))
        if spec.get("expect_hub") is not None and not relayed and int(hid) != spec["expect_hub"]:
            bad.append("expected player %d to be elected, got %s" % (spec["expect_hub"], hid))
        # the election must realise the held ranking: the first candidate that is not already dead
        ranks = RANK_RE.findall(logs[surv[0]][0])
        if ranks:
            exp = next((int(c) for c in ranks[0].split(">") if int(c) != 0), None)
            if exp is not None and exp != int(hid):
                bad.append("elected %s but the held ranking %s says %d" % (hid, ranks[0], exp))
            out.append(
                "held ranking before the loss: %s -> first live candidate %s" % (ranks[0], exp)
            )
        if int(hid) not in [ID_OF[s] for s in surv] + [ID_OF[s] for s in crash if s != "host"]:
            bad.append("the elected hub %s is not a survivor (of that failover)" % hid)
        else:
            hp = PEERS[int(hid)]
            if hp in PEERS:
                if "this player is the hub now" not in logs[hp][0]:
                    bad.append("%s was elected but never logged that it became the hub" % hp)
            for sv in surv:
                if sv != hp and "re-dialled player" not in logs[sv][0]:
                    bad.append("%s never logged re-dialling the elected hub" % sv)
    if spec.get("second_failover"):
        seconds = {sv: elected[sv][1] for sv in surv if len(elected[sv]) > 1}
        if len(set(seconds.values())) > 1:
            bad.append("the survivors elected DIFFERENT second hubs: %r" % (seconds,))
        elif seconds:
            hid2, ep2 = next(iter(seconds.values()))
            out.append("second election on every survivor: hub %s epoch %s" % (hid2, ep2))
            ranks = RANK_RE.findall(logs[surv[0]][0])
            if len(ranks) > 1:
                dead2 = {0} | {ID_OF[p] for p in crash if p != "host"}
                exp2 = next((int(c) for c in ranks[1].split(">") if int(c) not in dead2), None)
                out.append(
                    "held ranking before the second loss: %s -> first live candidate %s"
                    % (ranks[1], exp2)
                )
                if exp2 is not None and exp2 != int(hid2):
                    bad.append(
                        "second election chose %s but the held ranking %s says %d"
                        % (hid2, ranks[1], exp2)
                    )
            if int(hid2) == int(next(iter(firsts.values()))[0]) or int(hid2) not in [
                ID_OF[s] for s in surv
            ]:
                bad.append("the second hub %s is the dead first hub or not a survivor" % hid2)
    # dropped by a silence timer?
    ids = [ID_OF[s] for s in surv]
    for sv in surv:
        net = logs[sv][0]
        for x in net.splitlines():
            mm = re.search(r"GS2: peer (\d+) ", x) or re.search(r"U17 fast-drop: .*side=(\d+)\b", x)
            if mm and int(mm.group(1)) in ids:
                bad.append(
                    "%s: a survivor was dropped by a silence timer: %s" % (sv, x.strip()[-120:])
                )
                break
    # identical
    ok, lines = _identical(det_dir, surv, last_incident + MIN_POST if arm != "base4" else 1)
    out += lines
    if not ok:
        bad.append("survivors are not ALL PAIRS IDENTICAL")
    for sv in surv:
        if last[sv][0] - last_incident < MIN_POST:
            bad.append(
                "%s stepped only %d past the last incident (< %d)"
                % (sv, last[sv][0] - last_incident, MIN_POST)
            )
    # stall around each incident
    for step in sorted(
        set(crash.values()) | ({spec["partition_at"]} if spec.get("partition_at") else set())
    ):
        clk = next((c for s, c in u53.harness_steps(logs[sv0][1]) if s >= step), None)
        if clk is None:
            continue
        lo, hi = int(max(0.0, clk - 1.0) * 1000), int((clk + 20.0) * 1000)
        for sv in surv:
            gap, n = stall_window(os.path.join(det_dir, sv, "mh_lockstep.log"), lo, hi)
            out.append(
                "%s longest game-clock stall in [%.1fs, %.1fs]: %d ms over %d rows"
                % (sv, lo / 1000.0, hi / 1000.0, gap, n)
            )
            if n == 0:
                bad.append("%s: no lockstep rows near step %d" % (sv, step))
            elif gap > STALL_MAX_MS:
                bad.append("%s stalled %d ms around step %d (> %d)" % (sv, gap, step, STALL_MAX_MS))
    # removals: the NEW hub's U17 fast-drop removes a dead peer at the PARKED step and broadcasts it, so every survivor
    # applies it at that one step -- a different step on one survivor would change its roster flags and break the hash
    # identity checked above. What is checked here is that the removal HAPPENED, once per dead peer, by a survivor.
    dead_ids = {ID_OF[p] for p in crash} | {ID_OF[p] for p in spec.get("minority_peers", ())}
    drops = {}
    for sv in PEERS:  # every peer's log: the FIRST new hub may itself be a later casualty
        for x in logs[sv][0].splitlines():
            mm = re.search(
                r"U17 fast-drop: transport-dead peer side=(\d+) -> broadcast removal \((\w+-?\w*) after (\d+) frames\)",
                x,
            )
            if mm:
                drops.setdefault(int(mm.group(1)), []).append((sv, mm.group(2), int(mm.group(3))))
    for d in sorted(dead_ids):
        got = drops.get(d, [])
        if not got and spec_p and d == ID_OF[spec_p]:
            # a SPECTATOR was already out of the roster (mp:U54): its transport closing is "already removed, no second removal"
            pre = [
                sv
                for sv in surv
                if re.search(
                    r"U19b: transport of already-removed peer side=%d closed" % d, logs[sv][0]
                )
            ]
            out.append("    peer %d is the spectator: already out of the roster (%s)" % (d, pre))
            if not pre:
                bad.append(
                    "the dead spectator %d was neither removed nor logged as already removed" % d
                )
            continue
        out.append("    removal of peer %d: %s" % (d, got or "NOT LOGGED"))
        if len(got) != 1:
            bad.append(
                "peer %d must be removed exactly once (by the hub of that failover), saw %r"
                % (d, got)
            )
        elif got[0][1] != "PARKED":
            bad.append(
                "peer %d was removed by the safety valve (%s), not at the PARKED step"
                % (d, got[0][1])
            )
    # the cut-off minority
    for mp in spec.get("minority_peers", ()):
        net, har = logs[mp]
        over = next((x for x in net.splitlines() if "on_gameover" in x), None)
        out.append(
            "    minority %s: %s" % (mp, over.strip()[:160] if over else "NO on_gameover line")
        )
        out += u53.timeline(net, mp)
        if over is None:
            bad.append("the minority %s never ended its match (no on_gameover)" % mp)
        else:
            t_end = _wall(over)
            t_cut = _first_wall(logs[surv[0]][0], "failover SUSPECT")
            if t_end is not None and t_cut is not None:
                out.append(
                    "    minority %s ended %.1f s after the survivors' suspicion began"
                    % (mp, t_end - t_cut)
                )
                if t_end - t_cut > MINORITY_PEER_MAX_S:
                    bad.append(
                        "the minority %s took %.1f s to end (> %.0f s)"
                        % (mp, t_end - t_cut, MINORITY_PEER_MAX_S)
                    )
    if spec.get("heal_at"):
        wc = {}
        for sv in surv:
            m = re.findall(r"wrong-conn (\d+)", logs[sv][0])
            wc[sv] = int(m[-1]) if m else 0
        out.append(
            "    datagrams with a foreign connection id (the old host's, after the heal): %r"
            % (wc,)
        )
        if not any(wc.values()):
            bad.append(
                "no survivor counted a wrong-conn datagram: the old host's traffic never reached them (the heal proved nothing)"
            )
        for sv in surv:
            n = len(ELECTED_RE.findall(logs[sv][0]))
            if n != 1:
                bad.append(
                    "%s logged %d hub elections across the heal (a returning old host must not cause one more)"
                    % (sv, n)
                )
    return (not bad), out + ["FAIL: " + b for b in bad]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("det_dir")
    ap.add_argument("arm")
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import u64_arms

    try:
        ok, lines = check(a.det_dir, a.arm, u64_arms.ARMS[a.arm])
    except u53.Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
