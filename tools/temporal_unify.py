#!/usr/bin/env python3
# tools/temporal_unify.py -- PERSISTENT engine (promoted from tools/oneoff/ on 2026-07-20 after the
# third MP timing run used it; per the script policy (docs/conventions.md#script-policy),
# a recurring shape belongs in tools/).
# Pure CPython, no Ghidra -> NO shim; run it directly:  python tools/temporal_unify.py ...
#
# Transform the per-EVENT temporal traces (mh_temporal.log, [trace] temporal=1) into a
# human-readable, UNIFIED-TIME form:
#   * ts=0 is the HOST's game start (its first lockstep event);
#   * the CLIENT's clock is mapped onto the HOST's by removing the constant QPC epoch offset
#     (both peers share the one physical TSC on this box -- Hyper-V reference time -- so a single
#     constant aligns them; residual = the +-15 ms lockstep skew, not drift);
#   * the numeric event id becomes a text name;
#   * a legend header documents every column + the event vocabulary.
#
# Emits three files into --out:  *.host.log, *.client.log, *.merged.log (both peers, sorted by ts).
#
# Usage:
#   python tools/temporal_unify.py --host <host mh_temporal.log> --client <client mh_temporal.log> [--out <dir>]
#   python tools/temporal_unify.py --run <host log DIR> --client <client mh_temporal.log>
#     --run <dir>  = a run directory (…/logs/<stamp>_host); --host/--out are derived from it.
#     --out        defaults to the host log's directory.
# There are deliberately NO hard-coded run paths: the previous defaults pointed at whichever run the
# author last looked at, which silently unified the WRONG run when re-invoked bare.
#
# TRUNCATION CHECK: the capture buffer drains to disk every present and recycles (net_seams.cpp), so a
# complete run has no gaps. If the DLL ever fills it between drains it writes a
# "# WARNING: temporal buffer hit TEV_MAX" line -- this script surfaces that as a loud banner, because
# a silently truncated trace previously looked exactly like a complete one (2026-07-20: a 548 fps run
# stopped at sim_step 225 of 800 with no indication).

import argparse, os, statistics

EVENT = {
    0: "frame",
    1: "pump",
    2: "commit",
    3: "time_tick",
    4: "sim_tick",
    5: "sim_step",
    6: "send_ext",
    7: "present",
}

EVENT_DOC = [
    ("frame", "llm_strat_frame", "top of the strategic frame"),
    ("pump", "llm_net_lockstep_pump", "drain received packets -> recompute committed"),
    ("commit", "llm_net_lockstep_commit_horizon", "committed = min(local horizon, active peers)"),
    ("time_tick", "llm_strat_time_tick", "TOTAL += real dt; clamp TOTAL=committed; advertise"),
    ("sim_tick", "llm_strat_sim_tick", "catch-up loop (emitted by the harness detour)"),
    ("sim_step", "llm_strat_sim_step", "one deterministic 0.1 s step (GAME_CLOCK += 100)"),
    ("send_ext", "llm_net_send_lockstep_extend", "advertise horizon = GAME_CLOCK + step"),
    ("present", "llm_gfx_present_flip", "frame end / DirectDraw flip"),
]


def load(path):
    ev = []
    with open(path, errors="replace") as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            p = ln.split()
            if len(p) < 6:
                continue
            try:
                ev.append([int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])])
            except ValueError:
                continue
    return ev


def step_qpc(ev):
    m = {}
    for e in ev:
        if e[1] == 5 and e[2] not in m:
            m[e[2]] = e[0]
    return m


def legend(offset_us, host_t0, merged, changes=False):
    days = abs(offset_us) / 1e6 / 86400.0
    peer_col = (
        "#   peer             which machine emitted the event: host | client\n" if merged else ""
    )
    changes_note = (
        (
            "# CHANGES-ONLY view: a row is kept only when clk/tot/com/loc changes from this peer's previous\n"
            "# row -- the per-frame render spam (repeated identical state) is collapsed away.\n#\n"
        )
        if changes
        else ""
    )
    return changes_note + (
        "# Unified MP lockstep temporal trace -- host + client on ONE time axis.\n"
        "# Source: per-EVENT QPC trace (mh_temporal.log, [trace] temporal=1).\n"
        "#\n"
        "# ts=0 is the HOST's game start (its first captured lockstep event). The client's QPC is mapped\n"
        "# onto the host's clock by removing a single constant epoch offset (%d us ~= %.2f days) -- both peers\n"
        "# share the same physical TSC on this machine, so one constant aligns them. Client timestamps are\n"
        "# therefore accurate to ~the sub-ms wire latency plus the +-15 ms per-step lockstep skew.\n"
        "#\n"
        "# Columns:\n"
        "#   ts_ms            unified time in milliseconds since host game start (us precision)\n"
        "%s"
        "#   event            which instrumented game function fired (vocabulary below)\n"
        "#   game_clk_ms      GAME_CLOCK -- the deterministic sim clock (advances 100 ms per sim_step)\n"
        "#   total_ms         TOTAL_GAME_TIME -- the real-dt accumulator, clamped to committed in MP\n"
        "#   committed_ms     committed horizon = min(local, peers) -- the lockstep ceiling the sim can't pass\n"
        "#   local_ms         this peer's own advertised look-ahead horizon (GAME_CLOCK + step)\n"
        "#\n"
        "# event vocabulary (call order within one strategic frame):\n"
        + "".join("#   %-9s %-31s %s\n" % (n, sym, desc) for n, sym, desc in EVENT_DOC)
        + "#\n"
    ) % (offset_us, days, peer_col)


def fmt_row(ts, peer, ev, merged):
    name = EVENT.get(ev[1], "id%d" % ev[1])
    if merged:
        return "%12.3f  %-6s %-9s %9d %9d %10d %9d\n" % (ts, peer, name, ev[2], ev[3], ev[4], ev[5])
    return "%12.3f  %-9s %9d %9d %10d %9d\n" % (ts, name, ev[2], ev[3], ev[4], ev[5])


def header(merged):
    if merged:
        return "%12s  %-6s %-9s %9s %9s %10s %9s\n" % (
            "ts_ms",
            "peer",
            "event",
            "game_clk",
            "total",
            "committed",
            "local",
        )
    return "%12s  %-9s %9s %9s %10s %9s\n" % (
        "ts_ms",
        "event",
        "game_clk",
        "total",
        "committed",
        "local",
    )


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--run", help="host run directory (…/logs/<stamp>_host); derives --host and --out"
    )
    ap.add_argument("--host", help="host mh_temporal.log (required unless --run)")
    ap.add_argument("--client", required=True, help="client mh_temporal.log")
    ap.add_argument("--out", help="output dir (default: the host log's directory)")
    ap.add_argument("--prefix", default="mh_temporal_unified")
    args = ap.parse_args()

    if args.run and not args.host:
        args.host = os.path.join(args.run, "mh_temporal.log")
    if not args.host:
        raise SystemExit("need --host <mh_temporal.log> or --run <host run dir>")
    if not args.out:
        args.out = args.run or os.path.dirname(os.path.abspath(args.host))

    for label, p in (("host", args.host), ("client", args.client)):
        if not os.path.isfile(p):
            raise SystemExit("%s log not found: %s" % (label, p))

    H = load(args.host)
    C = load(args.client)
    for label, p in (("host", args.host), ("client", args.client)):
        with open(p, errors="replace") as f:
            if any("hit TEV_MAX" in ln for ln in f):
                print("!" * 78)
                print(
                    "!! %s trace is TRUNCATED: the DLL reported a temporal-buffer overflow."
                    % label.upper()
                )
                print("!! Events were DROPPED -- do not draw timing conclusions from this run.")
                print("!" * 78)
    if not H or not C:
        raise SystemExit("empty host/client log (host=%d client=%d)" % (len(H), len(C)))

    # constant clock offset: median(client_qpc - host_qpc) over sim_steps sharing a game-clock value
    hs, cs = step_qpc(H), step_qpc(C)
    common = [c for c in (set(hs) & set(cs)) if c > 2000]  # skip startup transient
    if not common:
        common = list(set(hs) & set(cs))
    offset = statistics.median([cs[c] - hs[c] for c in common])
    host_t0 = H[0][0]  # first host event = game start -> ts 0

    def ts_h(q):
        return (q - host_t0) / 1000.0

    def ts_c(q):
        return (q - offset - host_t0) / 1000.0

    # "changes only" filter: keep a row only when (clk,tot,com,loc) differs from the peer's last kept row.
    # Collapses the per-frame render spam (frame/present/pump re-reading identical state), keeping every
    # sim_step / commit / send_ext / quantum tick -- the state-change skeleton of the lockstep dance.
    def changes(ev):
        out, last = [], None
        for e in ev:
            key = (e[2], e[3], e[4], e[5])
            if key != last:
                out.append(e)
                last = key
        return out

    os.makedirs(args.out, exist_ok=True)
    Hc, Cc = changes(H), changes(C)
    # per-peer files (full + changes-only)
    for peer, ev, tsf in (("host", H, ts_h), ("client", C, ts_c)):
        for suffix, data in (("", ev), (".changes", changes(ev))):
            path = os.path.join(args.out, "%s.%s%s.log" % (args.prefix, peer, suffix))
            with open(path, "w", newline="\n") as f:
                f.write(legend(offset, host_t0, merged=False, changes=bool(suffix)))
                f.write(header(merged=False))
                for e in data:
                    f.write(fmt_row(tsf(e[0]), peer, e, merged=False))
            print("wrote %s (%d rows)" % (path, len(data)))
    # merged (full + changes-only), sorted by unified ts
    for suffix, hh, cc in (("", H, C), (".changes", Hc, Cc)):
        rows = [(ts_h(e[0]), "host", e) for e in hh] + [(ts_c(e[0]), "client", e) for e in cc]
        rows.sort(key=lambda r: r[0])
        mpath = os.path.join(args.out, "%s.merged%s.log" % (args.prefix, suffix))
        with open(mpath, "w", newline="\n") as f:
            f.write(legend(offset, host_t0, merged=True, changes=bool(suffix)))
            f.write(header(merged=True))
            for ts, peer, e in rows:
                f.write(fmt_row(ts, peer, e, merged=True))
        print("wrote %s (%d rows)" % (mpath, len(rows)))
    print(
        "offset=%d us (%.2f days) ; host_t0(qpc)=%d ; common steps=%d"
        % (offset, abs(offset) / 1e6 / 86400.0, host_t0, len(common))
    )


if __name__ == "__main__":
    main()
