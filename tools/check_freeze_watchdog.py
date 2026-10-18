#!/usr/bin/env python3
"""check_freeze_watchdog.py -- mp:P17: read the main-thread frame watchdog's and the focus tap's lines.

WHAT IT READS. The `[freeze]` / `[focus]` / `[video]` lines mh.dll writes to mh_net.log
(src/mh_dll/mh/seams/net_diag.cpp, "mp:P17 frame watchdog + focus tap"). Arguments are the peers' SESSION
run directories, the HOST first (the registry rows set post_check_session + post_check_peers).

  --expect-inject N   every peer ran `[net] frame_watchdog_inject_ms=N`: each must hold
                      * the `INJECT Sleep(N)` line carrying the address of frame_watchdog_inject_sleep,
                      * a `main thread no present for <ms>` capture (sample 1) with ms in [threshold, N+600],
                      * an ebp-chain or stack-ret entry in the SAME module, within 0x60 bytes past that
                        function's start -- i.e. the capture NAMES the Sleep call site,
                      * a `frozen main thread ended: next present after <ms>` line within 25% of N.
  --expect-none       the watchdog was switched off: NO capture line may exist (the negative arm; the row
                      is expect_red because the positive clause above must then fail).
  --alttab            report (and with --max-client-stall-ms assert) what a minimised window did: the
                      host's focus lines, its freeze captures, the client's longest `[netind] stall`.
  --max-client-stall-ms M   (with --alttab) FAIL when the client's longest stall exceeds M.
  --expect-no-freeze  (with --alttab) FAIL when the host's frame loop stopped (a capture line exists) -- the
                      owned-d3d11 result recorded by the rig run.
  --expect-frozen     (with --alttab) FAIL unless the host DID freeze (a capture line exists) -- the
                      documented retail-ish behaviour a rig run recorded.
  --hitch             read the `[hitch]` frame lines and their MID-FRAME stack samples (single-player; [net] hitch_ms) and print them
                      SYMBOLIZED: an exe offset (`mh.exe+0xOFF` / `mh.focus.exe+0xOFF`, image base 0x400000) becomes the EN VA and the
                      function that contains it (tools/data/en_functions.json); `mh.dll+0xOFF` is kept (no map ships).
  --expect-hitch-inject N   (with --hitch) the run set `[net] hitch_inject_ms=N`: there must be the `[hitch] INJECT Sleep(N)` line, a
                      `[hitch] frame` of at least 0.9*N ms, and a sample under that frame whose ebp-chain / stack-ret holds an
                      address in the same module within 0x60 bytes past hitch_inject_sleep -- i.e. the sample NAMES the Sleep site.
  --selftest          planted logs, every negative RED.
"""

import argparse
import glob
import os
import re
import sys
import tempfile

NET_LOG = "mh_net.log"

STAMP = r"(?:\[(?P<t>\d\d:\d\d:\d\d\.\d{3})\] )?"
ARMED = re.compile(STAMP + r"; \[freeze\] frame watchdog armed: threshold=(?P<thr>\d+) ms")
INJECT = re.compile(
    STAMP
    + r"; \[freeze\] INJECT Sleep\((?P<ms>\d+)\) from frame_watchdog_inject_sleep=(?P<mod>[^+ ]+)\+0x(?P<off>[0-9A-Fa-f]+)"
)
CAPTURE = re.compile(
    STAMP
    + r"; \[freeze\] main thread no present for (?P<ms>\d+) ms \(sample (?P<n>\d+), (?P<rest>[^)]*)\)"
)
CHAIN = re.compile(r"; \[freeze\]   (?:ebp-chain|stack-ret):(?P<body>.*)")
ENDED = re.compile(
    STAMP + r"; \[freeze\] frozen main thread ended: next present after (?P<ms>\d+) ms"
)
FOCUS = re.compile(
    STAMP + r"; \[focus\] (?P<what>\w+) hwnd=\S+ (?P<det>.*?) iconic=(?P<ic>-?\d+) fg=(?P<fg>-?\d+)"
)
DDRAW = re.compile(r"; \[video\] loaded ddraw\.dll=(?P<path>.*?) d3d11\.dll=(?P<d3d>\d)")
STALL = re.compile(
    STAMP + r"; \[netind\] stall waiting_for=(?P<who>\S+) peer=(?P<peer>-?\d+) ms=(?P<ms>\d+)"
)


HITCH_INJECT = re.compile(
    STAMP
    + r"; \[hitch\] INJECT Sleep\((?P<ms>\d+)\) from hitch_inject_sleep=(?P<mod>[^+ ]+)\+0x(?P<off>[0-9A-Fa-f]+)"
)
HITCH_FRAME = re.compile(
    STAMP + r"; \[hitch\] frame (?P<ms>[\d.]+) ms \(>= (?P<thr>\d+)\): (?P<rest>.*)"
)
HITCH_EIP = re.compile(
    r"; \[hitch\]   eip=(?P<eip>\S+) .*?\(sample (?P<n>\d+), (?P<ms>\d+) ms into the frame\)"
)
HITCH_CHAIN = re.compile(r"; \[hitch\]   (?P<kind>ebp-chain|stack-ret):(?P<body>.*)")
MODOFF = re.compile(r"([^\s@]+)\+0x([0-9A-Fa-f]+)")
EXE_BASE = 0x400000
FUNCS_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "en_functions.json")
_funcs = None


def load_funcs():
    """Sorted [(entry, end, name)] of the EN program's functions (the committed Ghidra dump)."""
    global _funcs
    if _funcs is None:
        import json

        with open(FUNCS_JSON, encoding="utf-8") as fh:
            d = json.load(fh)
        _funcs = sorted((int(f["entry"], 16), int(f["end"], 16), f["name"]) for f in d["functions"])
    return _funcs


def symbolize(tok):
    """`mh.exe+0xOFF` / `mh.focus.exe+0xOFF` -> `tok[VA function+0xDELTA]`; anything else is returned unchanged."""
    m = MODOFF.fullmatch(tok)
    if not m or not m.group(1).lower().endswith(".exe"):
        return tok
    import bisect

    va = EXE_BASE + int(m.group(2), 16)
    fs = load_funcs()
    i = bisect.bisect_right([f[0] for f in fs], va) - 1
    if i >= 0 and fs[i][0] <= va <= fs[i][1]:
        return "%s[0x%08X %s+0x%X]" % (tok, va, fs[i][2], va - fs[i][0])
    return "%s[0x%08X ?]" % (tok, va)


def parse_hitch(lines):
    """{'inject': {...}|None, 'frames': [{'ms','rest','samples':[{'n','ms','eip','refs','raw'}]}]} from `[hitch]` lines."""
    r = {"inject": None, "frames": []}
    for ln in lines:
        m = HITCH_INJECT.search(ln)
        if m:
            r["inject"] = {
                "ms": int(m.group("ms")),
                "mod": m.group("mod"),
                "off": int(m.group("off"), 16),
            }
            continue
        m = HITCH_FRAME.search(ln)
        if m:
            r["frames"].append({"ms": float(m.group("ms")), "rest": m.group("rest"), "samples": []})
            continue
        m = HITCH_EIP.search(ln)
        if m and r["frames"]:
            r["frames"][-1]["samples"].append(
                {
                    "n": int(m.group("n")),
                    "ms": int(m.group("ms")),
                    "eip": m.group("eip"),
                    "refs": [],
                    "raw": [],
                }
            )
            continue
        m = HITCH_CHAIN.search(ln)
        if m and r["frames"] and r["frames"][-1]["samples"]:
            smp = r["frames"][-1]["samples"][-1]
            smp["refs"].extend((a, int(b, 16)) for a, b in MODOFF.findall(m.group("body")))
            smp["raw"].append((m.group("kind"), m.group("body").split()))
    return r


def check_hitch_inject(h, n):
    if not h["inject"]:
        return [
            "no `[hitch] INJECT Sleep(%d)` line (hitch_inject_ms not set, or no strategic frame ran for 3 s)"
            % n
        ]
    inj = h["inject"]
    fails = [] if inj["ms"] == n else ["INJECT Sleep(%d), expected %d" % (inj["ms"], n)]
    slow = [f for f in h["frames"] if f["ms"] >= 0.9 * n]
    if not slow:
        return fails + ["no `[hitch] frame` of >= %d ms" % int(0.9 * n)]
    for f in slow:
        for smp in f["samples"]:
            for mod, off in smp["refs"]:
                if mod == inj["mod"] and 0 <= off - inj["off"] <= 0x60:
                    return fails
    return fails + [
        "no sample under the slow frame names %s+0x%X..0x%X (the Sleep call site)"
        % (inj["mod"], inj["off"], inj["off"] + 0x60)
    ]


def print_hitch(h):
    for f in h["frames"]:
        print("[hitch] frame %.1f ms | %s" % (f["ms"], f["rest"]))
        for smp in f["samples"]:
            print("    sample %d (%d ms in): eip=%s" % (smp["n"], smp["ms"], symbolize(smp["eip"])))
            for kind, toks in smp["raw"]:
                print("      %s:" % kind)
                for t in toks:
                    print("        %s" % symbolize(t))


class Refusal(Exception):
    pass


def read_log(d):
    p = os.path.join(d, NET_LOG)
    if not os.path.isfile(p):
        cands = sorted(glob.glob(os.path.join(d, "logs", "*", NET_LOG)), key=os.path.getmtime)
        if not cands:
            raise Refusal("no %s under %s" % (NET_LOG, d))
        p = cands[-1]
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def parse(lines):
    r = {
        "armed": None,
        "inject": None,
        "captures": [],
        "ended": [],
        "focus": [],
        "ddraw": None,
        "stalls": [],
    }
    for ln in lines:
        m = ARMED.search(ln)
        if m:
            r["armed"] = int(m.group("thr"))
        m = INJECT.search(ln)
        if m:
            r["inject"] = {
                "ms": int(m.group("ms")),
                "mod": m.group("mod"),
                "off": int(m.group("off"), 16),
            }
        m = CAPTURE.search(ln)
        if m:
            r["captures"].append(
                {
                    "ms": int(m.group("ms")),
                    "n": int(m.group("n")),
                    "rest": m.group("rest"),
                    "refs": [],
                }
            )
            continue
        m = CHAIN.search(ln)
        if m and r["captures"]:
            r["captures"][-1]["refs"].extend(
                re.findall(r"([^\s@]+)\+0x([0-9A-Fa-f]+)", m.group("body"))
            )
            continue
        m = ENDED.search(ln)
        if m:
            r["ended"].append(int(m.group("ms")))
        m = FOCUS.search(ln)
        if m:
            r["focus"].append(
                (m.group("t") or "", m.group("what"), m.group("det"), m.group("ic"), m.group("fg"))
            )
        m = DDRAW.search(ln)
        if m:
            r["ddraw"] = (m.group("path"), m.group("d3d"))
        m = STALL.search(ln)
        if m:
            r["stalls"].append(int(m.group("ms")))
    return r


def check_inject(r, n):
    fails = []
    # (the armed banner is written at process start, into the MENU dir -- not in the match's session dir)
    inj = r["inject"]
    if not inj or inj["ms"] != n:
        fails.append("no `INJECT Sleep(%d)` line" % n)
    caps = [c for c in r["captures"] if c["n"] == 1]
    if not caps:
        fails.append(
            "no `main thread no present for ... (sample 1` capture line -- the freeze left no stack"
        )
        return fails
    c = caps[0]
    thr = r["armed"] or 1000
    if not (thr <= c["ms"] <= n + 600):
        fails.append("capture says %d ms, expected within [%d, %d]" % (c["ms"], thr, n + 600))
    if inj:
        hit = [
            (m, o) for m, o in c["refs"] if m == inj["mod"] and 0 < int(o, 16) - inj["off"] <= 0x60
        ]
        if not hit:
            fails.append(
                "no stack entry names frame_watchdog_inject_sleep (%s+0x%X .. +0x60); entries: %s"
                % (
                    inj["mod"],
                    inj["off"],
                    " ".join("%s+0x%s" % t for t in c["refs"][:12]) or "(none)",
                )
            )
    if not r["ended"]:
        fails.append("no `frozen main thread ended` line")
    elif abs(r["ended"][0] - n) > max(300, n // 4):
        fails.append("ended after %d ms, expected ~%d" % (r["ended"][0], n))
    return fails


def report(r, tag):
    print(
        "%s: armed=%s inject=%s captures=%d ended=%s ddraw=%s"
        % (tag, r["armed"], r["inject"], len(r["captures"]), r["ended"], r["ddraw"])
    )
    for f in r["focus"]:
        print("%s:   focus %s %s %s iconic=%s fg=%s" % (tag, f[0], f[1], f[2], f[3], f[4]))
    for c in r["captures"]:
        print(
            "%s:   freeze %d ms sample %d (%s) refs=%s"
            % (tag, c["ms"], c["n"], c["rest"], " ".join("%s+0x%s" % t for t in c["refs"][:6]))
        )
    if r["stalls"]:
        print(
            "%s:   netind stalls ms: max=%d n=%d %s"
            % (tag, max(r["stalls"]), len(r["stalls"]), r["stalls"][:12])
        )


def run_hitch(args):
    if not args.dirs:
        raise Refusal("no run directories given")
    h = parse_hitch(read_log(args.dirs[0]))
    print_hitch(h)
    return check_hitch_inject(h, args.expect_hitch_inject) if args.expect_hitch_inject else []


def run(args):
    if getattr(args, "hitch", False):
        return run_hitch(args)
    if not args.dirs:
        raise Refusal("no run directories given")
    peers = [parse(read_log(d)) for d in args.dirs]
    for i, p in enumerate(peers):
        report(p, "host" if i == 0 else "client%d" % i)
    host = peers[0]
    fails = []
    if args.expect_inject:
        # every peer carries the arm (the registry has no host-only [net] channel): each must catch its own
        for i, p in enumerate(peers):
            fails += [
                "%s: %s" % ("host" if i == 0 else "client%d" % i, f)
                for f in check_inject(p, args.expect_inject)
            ]
    if args.expect_none:
        if host["captures"]:
            fails.append("watchdog was off but %d capture line(s) exist" % len(host["captures"]))
    if args.alttab:
        if not host["focus"]:
            fails.append(
                "no `[focus]` line on the host -- the focus tap saw nothing (hook not installed, or no focus event)"
            )
        if not any("MINIMIZED" in f[2] for f in host["focus"]):
            fails.append("the host's focus tap never saw WM_SIZE MINIMIZED")
        if args.expect_no_freeze and host["captures"]:
            fails.append(
                "--expect-no-freeze: the minimised host's frame loop STOPPED (%d capture line(s))"
                % len(host["captures"])
            )
        if args.expect_frozen and not host["captures"]:
            fails.append("--expect-frozen: the host's frame loop did NOT stop (no capture line)")
        if args.max_client_stall_ms is not None:
            worst = max([s for p in peers[1:] for s in p["stalls"]] or [0])
            if worst > args.max_client_stall_ms:
                fails.append(
                    "client stalled %d ms waiting on the minimised host (limit %d)"
                    % (worst, args.max_client_stall_ms)
                )
    return fails


def selftest():
    good = [
        "[10:00:00.000] ; [freeze] frame watchdog armed: threshold=1000 ms (MP matches only), focus tap=on, INJECT arm set",
        "[10:00:03.000] ; [freeze] INJECT Sleep(1500) from frame_watchdog_inject_sleep=mh.dll+0x1A2B0 (its Sleep call returns within +0x60)",
        "[10:00:04.100] ; [freeze] main thread no present for 1100 ms (sample 1, sess=3 gclk=7000 iconic=0 fg=1 vis=1)",
        "[10:00:04.100] ; [freeze]   eip=ntdll.dll+0x9F0 esp=0012FF00 ebp=0012FF40",
        "[10:00:04.100] ; [freeze]   ebp-chain: mh.dll+0x1A2D5 mh.dll+0x5000",
        "[10:00:04.100] ; [freeze]   stack-ret: KERNELBASE.dll+0x1234@3 mh.dll+0x1A2D5@5",
        "[10:00:04.500] ; [freeze] frozen main thread ended: next present after 1510 ms (1 sample(s) taken)",
        "[10:00:05.000] ; [focus] WM_SIZE hwnd=0001 type=MINIMIZED iconic=1 fg=0 sess=3 gclk=1 tick=2",
    ]

    def one(lines, **kw):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, NET_LOG), "w") as fh:
                fh.write("\n".join(lines) + "\n")
            ns = argparse.Namespace(
                dirs=[d],
                expect_inject=0,
                expect_none=False,
                alttab=False,
                expect_frozen=False,
                expect_no_freeze=False,
                max_client_stall_ms=None,
                hitch=False,
                expect_hitch_inject=0,
            )
            for k, v in kw.items():
                setattr(ns, k, v)
            return run(ns)

    bad = 0

    def expect(name, fails, want_fail):
        nonlocal bad
        ok = bool(fails) == want_fail
        print("selftest %-34s %s" % (name, "ok" if ok else "WRONG (%s)" % fails))
        bad += 0 if ok else 1

    expect("good inject", one(good, expect_inject=1500), False)
    expect(
        "no capture",
        one(
            [
                l
                for l in good
                if "no present" not in l and "ebp-chain" not in l and "stack-ret" not in l
            ],
            expect_inject=1500,
        ),
        True,
    )
    expect(
        "wrong call site",
        one([l.replace("mh.dll+0x1A2D5", "mh.dll+0x9999") for l in good], expect_inject=1500),
        True,
    )
    expect("no ended line", one([l for l in good if "ended" not in l], expect_inject=1500), True)
    expect(
        "no inject line",
        one([l for l in good if "INJECT Sleep" not in l], expect_inject=1500),
        True,
    )
    expect("off arm clean", one(["; nothing"], expect_none=True), False)
    expect("off arm with capture", one(good, expect_none=True), True)
    expect("alttab focus seen", one(good, alttab=True), False)
    expect("alttab no focus", one([l for l in good if "[focus]" not in l], alttab=True), True)
    expect(
        "alttab expect frozen",
        one([l for l in good if "no present" not in l], alttab=True, expect_frozen=True),
        True,
    )
    expect("alttab no-freeze but froze", one(good, alttab=True, expect_no_freeze=True), True)
    expect(
        "alttab no-freeze ok",
        one([l for l in good if "no present" not in l], alttab=True, expect_no_freeze=True),
        False,
    )
    hitch_good = [
        "[10:00:03.000] ; [hitch] INJECT Sleep(700) from hitch_inject_sleep=mh.dll+0x2C530 (its Sleep call returns within +0x60)",
        "[10:00:03.700] ; [hitch] frame 703.2 ms (>= 100): pre-time_tick 700.4 | sim 2.3 (10 steps, slowest 0.5) | x",
        "[10:00:03.700] ; [hitch]   eip=ntdll.dll+0x7662C esp=000EE1FC ebp=000EE218 (sample 1, 105 ms into the frame)",
        "[10:00:03.700] ; [hitch]   ebp-chain: KERNELBASE.dll+0x1796DC mh.dll+0x2C581 mh.focus.exe+0x4E59A",
        "[10:00:03.700] ; [hitch]   stack-ret: ntdll.dll+0x916F9@0 mh.dll+0x2C581@39",
    ]
    expect("hitch inject ok", one(hitch_good, hitch=True, expect_hitch_inject=700), False)
    expect(
        "hitch wrong call site",
        one(
            [l.replace("mh.dll+0x2C581", "mh.dll+0x9999") for l in hitch_good],
            hitch=True,
            expect_hitch_inject=700,
        ),
        True,
    )
    expect(
        "hitch no sample",
        one(
            [
                l
                for l in hitch_good
                if "eip=" not in l and "ebp-chain" not in l and "stack-ret" not in l
            ],
            hitch=True,
            expect_hitch_inject=700,
        ),
        True,
    )
    expect("hitch no inject line", one(hitch_good[1:], hitch=True, expect_hitch_inject=700), True)
    expect(
        "hitch frame too short",
        one(
            [l.replace("frame 703.2", "frame 120.0") for l in hitch_good],
            hitch=True,
            expect_hitch_inject=700,
        ),
        True,
    )
    sym = symbolize("mh.focus.exe+0xD2A81")
    ok = sym.startswith("mh.focus.exe+0xD2A81[0x004D2A81 ") and "?]" not in sym
    print("selftest %-34s %s" % ("symbolize exe offset", "ok" if ok else "WRONG (%s)" % sym))
    bad += 0 if ok else 1
    ok = symbolize("mh.dll+0x2C581") == "mh.dll+0x2C581"
    print("selftest %-34s %s" % ("symbolize leaves a dll alone", "ok" if ok else "WRONG"))
    bad += 0 if ok else 1
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="*")
    ap.add_argument("--expect-inject", type=int, default=0)
    ap.add_argument("--expect-none", action="store_true")
    ap.add_argument("--alttab", action="store_true")
    ap.add_argument("--expect-frozen", action="store_true")
    ap.add_argument("--expect-no-freeze", action="store_true")
    ap.add_argument("--max-client-stall-ms", type=int, default=None)
    ap.add_argument("--hitch", action="store_true")
    ap.add_argument("--expect-hitch-inject", type=int, default=0)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    try:
        fails = run(a)
    except Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    for f in fails:
        print("FAIL: %s" % f)
    print("check_freeze_watchdog: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
