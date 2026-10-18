#!/usr/bin/env python3
"""check_shift_rearm.py -- mods:D11: Shift+click keeps the just-placed building type armed.

The rows shift_rearm_build / shift_rearm_build_orig (the fix-off twin retired 2026-10-15) run the shift_rearm_build segment of ship_walk.txt / ship_orig_walk.txt on
2-almost.sav. The script writes `; [build] <label> armed= type= shift= placed= res=` lines (the harness verb `logbuild`) into
mh_video.log, the seam writes `; D11:` lines into mh_net.log, and the orders land in mh_rec_orders.bin. What must hold:

  * the seam armed (`; D11: shift re-arm armed at 00443AE9`) -- absence is RED, so the row is not vacuous;
  * A  three Shift places keep the type armed after each, a plain fourth disarms; four buildings were placed;
  * C  Shift + right-click cancels (armed -> 0);
  * D  a Shift click on an existing building issues no order and keeps the type armed;
  * B  the Shift chain stops, exactly once, on the click after which another copy is not affordable: armed stays on up to
       the last affordable click and is 0 after it, one `chain stopped (next copy refused)` line;
  * every placed building has its 0xea order and no more orders were issued than buildings placed (6 here): the re-arm
    adds no order of its own;
  * the seam's `kept armed` lines equal the Shift places that stayed armed.

  check_shift_rearm.py <run-dir>
  check_shift_rearm.py --mp <run-dir>   the 2-peer row d11_shift_chain_c1 (tools/uiscripts/mp_host_shift_chain.txt): armed after
                                        each of three Shift places, 0 after the plain fourth, four buildings and four orders,
                                        three `kept armed` lines (the lockstep hash compare is the harness's, not this checker's)
  check_shift_rearm.py ... --segment NAME [--session N] <run-dir>
                                        a multi-segment walk (ship_walk): the [build] lines between `; [mark] NAME` and the next
                                        mark, the per-game seam lines and orders of the Nth session that recorded orders
  check_shift_rearm.py --selftest
"""

import gzip
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_drag_box as cdb  # noqa: E402  session_dirs / STAMP / pop_session
import walk_segments  # noqa: E402

BUILD = re.compile(
    r"^; \[build\] (?P<label>\S+) armed=(?P<armed>-?\d+) type=(?P<type>-?\d+) shift=(?P<shift>\d+) placed=(?P<placed>\d+) res=(?P<res>[-\d,]+)\s*$"
)
REC_LEN, HEADER_LEN = 0x48, 8
ORDER_PLACE = 0xEA


def parse_build(text):
    """{label: {armed, type, placed, res}} -- the last line wins per label."""
    out = {}
    for ln in text.splitlines():
        m = BUILD.match(cdb.STAMP.sub("", ln.strip()).replace("; [script] ", ""))
        if not m:
            continue
        out[m.group("label")] = {
            "armed": int(m.group("armed")),
            "type": int(m.group("type")),
            "placed": int(m.group("placed")),
            "res": [int(x) for x in m.group("res").split(",")],
        }
    return out


def parse_d11(text):
    armed = bool(re.search(r"; D11: shift re-arm armed at 00443AE9", text))
    off = "shift re-arm OFF" in text
    kept = len(re.findall(r"; D11: shift re-arm type \d+ kept armed", text))
    stopped = len(
        re.findall(r"; D11: shift re-arm type \d+ chain stopped \(next copy refused\)", text)
    )
    return {"armed": armed, "off": off, "kept": kept, "stopped": stopped}


def count_orders(blob, building_type):
    """Place-building (0xea) orders of the given type in a mh_rec_orders.bin image."""
    body = blob[HEADER_LEN:]
    n = 0
    for i in range(len(body) // REC_LEN):
        r = body[i * REC_LEN : (i + 1) * REC_LEN]
        if (
            struct.unpack_from("<H", r, 16)[0] == ORDER_PLACE
            and struct.unpack_from("<i", r, 20)[0] == building_type
        ):
            n += 1
    return n


def problems(build, d11, orders):
    out = []
    need = [
        "start",
        "a_armed",
        "a_shift1",
        "a_shift2",
        "a_shift3",
        "a_plain",
        "c_armed",
        "c_cancel",
        "d_armed",
        "d_invalid",
        "b_shift1",
    ]
    for k in need:
        if k not in build:
            out.append("no `[build] %s` line" % k)
    if out:
        return out
    t = build["a_armed"]["armed"]
    if t <= 0:
        return ["a_armed: no building type armed by the tab click"]
    if not d11["armed"]:
        out.append(
            "the seam did not arm (no `; D11: shift re-arm armed at 00443AE9`)%s"
            % (" -- it is OFF" if d11["off"] else "")
        )
    for k in ("a_shift1", "a_shift2", "a_shift3"):
        if build[k]["armed"] != t:
            out.append(
                "%s: armed=%d, want %d (Shift held through the place click)"
                % (k, build[k]["armed"], t)
            )
    prev = build["a_armed"]["placed"]
    for k in ("a_shift1", "a_shift2", "a_shift3", "a_plain"):
        if build[k]["placed"] != prev + 1:
            out.append("%s: placed=%d, want %d" % (k, build[k]["placed"], prev + 1))
        prev = build[k]["placed"]
    if build["a_plain"]["armed"] != 0:
        out.append("a_plain: armed=%d after an unshifted place, want 0" % build["a_plain"]["armed"])
    if build["c_armed"]["armed"] != t:
        out.append("c_armed: the tab click did not arm again")
    if build["c_cancel"]["armed"] != 0:
        out.append(
            "c_cancel: Shift + right-click left armed=%d, want 0" % build["c_cancel"]["armed"]
        )
    if build["d_armed"]["armed"] != t or build["d_invalid"]["armed"] != t:
        out.append(
            "d_invalid: armed=%d, want %d (an invalid footprint keeps the type armed)"
            % (build["d_invalid"]["armed"], t)
        )
    if build["d_invalid"]["placed"] != build["d_armed"]["placed"]:
        out.append("d_invalid: a building was placed on an occupied footprint")
    bs = [build[k] for k in sorted(build) if k.startswith("b_shift")]
    flags = [b["armed"] == t for b in bs]
    if not bs or flags[-1] or not all(flags[:-1]):
        out.append(
            "chain B: armed flags %s, want armed on every click but the last, disarmed after it"
            % flags
        )
    elif min(b["res"][1] for b in bs) < 0:
        out.append("chain B: a stock went negative")
    if d11["stopped"] != 1:
        out.append("`chain stopped (next copy refused)` seen %d times, want 1" % d11["stopped"])
    want_kept = 3 + sum(1 for f in flags if f)
    if d11["kept"] != want_kept:
        out.append("`kept armed` seen %d times, want %d" % (d11["kept"], want_kept))
    first = build["a_armed"]["placed"]
    last = build[sorted(k for k in build if k.startswith("b_"))[-1]]["placed"]
    if orders is None:
        out.append("no mh_rec_orders.bin (the row needs [log] level=debug)")
    elif orders != last - first:
        out.append(
            "%d place orders of type %d vs %d buildings placed: the chain must issue exactly one order per placement"
            % (orders, t, last - first)
        )
    return out


def problems_mp(build, d11, orders):
    need = ["d11_armed", "d11_shift1", "d11_shift2", "d11_shift3", "d11_plain"]
    out = ["no `[build] %s` line" % k for k in need if k not in build]
    if out:
        return out
    t = build["d11_armed"]["armed"]
    if t <= 0:
        return ["d11_armed: no building type armed by the tab click"]
    if not d11["armed"]:
        out.append("the seam did not arm (no `; D11: shift re-arm armed at 00443AE9`)")
    for k in ("d11_shift1", "d11_shift2", "d11_shift3"):
        if build[k]["armed"] != t:
            out.append("%s: armed=%d, want %d" % (k, build[k]["armed"], t))
    if build["d11_plain"]["armed"] != 0:
        out.append(
            "d11_plain: armed=%d after an unshifted place, want 0" % build["d11_plain"]["armed"]
        )
    placed = build["d11_plain"]["placed"] - build["d11_armed"]["placed"]
    if placed != 4:
        out.append("%d buildings placed, want 4" % placed)
    if d11["kept"] < 3 or d11["stopped"] != 0:
        out.append(
            "`kept armed` x%d / `chain stopped` x%d, want >= 3 / 0" % (d11["kept"], d11["stopped"])
        )
    if orders is None:
        out.append("no mh_rec_orders.bin")
    elif orders != 4:
        out.append("%d place orders of type %d, want 4" % (orders, t))
    return out


def snapshot(run_dir, segment=None, session=None):
    """segment / session (tooling:TL-SUITE-FOLD): a multi-segment walk (ship_walk) starts a game per segment. The [build]
    lines are cut to `; [mark] <segment>`'s slice of mh_video.log; the seam's per-game D11 lines and the order record come from
    the Nth session dir that recorded orders (1-based); the process-level `armed at` banner stays whole-run."""
    vid = cdb.video_log(run_dir)
    net, orders = "", None
    if (
        vid is None
    ):  # a match's run dir is the SESSION dir; mh_video.log and the boot banner are in the process's menu dir before it
        logs = os.path.dirname(os.path.normpath(run_dir))
        menus = sorted(
            n
            for n in os.listdir(logs)
            if "_menu_" in n and n < os.path.basename(os.path.normpath(run_dir))
        )
        if menus:
            md = os.path.join(logs, menus[-1])
            vid = cdb.video_log(md)
            p0 = os.path.join(md, "mh_net.log")
            if os.path.isfile(p0):
                net += open(p0, encoding="utf-8", errors="replace").read()
    vid = vid or ""
    if segment:
        vid, why = walk_segments.slice_or_problem(vid, segment)
        if why:
            raise ValueError(why)
    t = None
    seen = 0
    dirs = cdb.session_dirs(run_dir)
    for i, d in enumerate(dirs):
        has_orders = [
            os.path.join(d, n)
            for n in ("mh_rec_orders.bin", "mh_rec_orders.bin.gz")
            if os.path.isfile(os.path.join(d, n))
        ]
        if has_orders:
            seen += 1
        # session None: every dir (the single-game rows); else the process banner dir (the first) and the Nth recorded session
        if session is not None and i > 0 and not (has_orders and seen == session):
            continue
        p = os.path.join(d, "mh_net.log")
        if os.path.isfile(p):
            net += open(p, encoding="utf-8", errors="replace").read()
        for q in has_orders:
            blob = open(q, "rb").read()
            orders = gzip.decompress(blob) if q.endswith(".gz") else blob
    build = parse_build(vid)
    first = "d11_armed" if "d11_armed" in build else "a_armed"
    if first in build and orders is not None:
        t = build[first]["type"]
        orders = count_orders(orders, t) if t > 0 else None
    return build, parse_d11(net), orders


def main(argv):
    if "--selftest" in argv:
        return selftest()
    segment, argv = walk_segments.pop_segment(argv)
    session, argv = cdb.pop_session(argv)
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    try:
        build, d11, orders = snapshot(args[0], segment, session)
    except ValueError as e:
        print("check_shift_rearm: FAIL -- %s" % e)
        return 1
    bad = (problems_mp if "--mp" in argv else problems)(build, d11, orders)
    for b in bad:
        print("  check_shift_rearm: " + b)
    print(
        "check_shift_rearm: %s"
        % ("FAIL" if bad else "PASS (%d labels, %d orders)" % (len(build), orders or 0))
    )
    return 1 if bad else 0


def _good():
    T = 21

    def b(armed, placed, r1=1215):
        return {"armed": armed, "type": T, "placed": placed, "res": [6800, r1, 6100, 4700, 0, 0]}

    build = {
        "start": b(0, 0),
        "a_armed": b(T, 1),
        "a_shift1": b(T, 2),
        "a_shift2": b(T, 3),
        "a_shift3": b(T, 4),
        "a_plain": b(0, 5),
        "c_armed": b(T, 5),
        "c_cancel": b(0, 5),
        "d_armed": b(T, 5),
        "d_invalid": b(T, 5),
        "b_shift1": b(T, 6, 215),
        "b_shift2": b(0, 7, 15),
        "b_end": b(0, 7, 15),
    }
    return build, {"armed": True, "off": False, "kept": 4, "stopped": 1}, 6


def selftest():
    import copy

    fails = 0
    build, d11, orders = _good()
    if problems(build, d11, orders):
        print("selftest: the good run is red: %s" % problems(build, d11, orders))
        fails += 1

    def red(name, mut):
        nonlocal fails
        b, d, o = copy.deepcopy((build, d11, orders))
        b, d, o = mut(b, d, o)
        if not problems(b, d, o):
            print("selftest: %s was NOT caught" % name)
            fails += 1

    red(
        "fix off (disarmed after a Shift place)",
        lambda b, d, o: (b["a_shift1"].update(armed=0) or b, d, o),
    )
    red("seam never armed", lambda b, d, o: (b, dict(d, armed=False, off=True), o))
    red("plain click keeps armed", lambda b, d, o: (b["a_plain"].update(armed=21) or b, d, o))
    red("cancel loses", lambda b, d, o: (b["c_cancel"].update(armed=21) or b, d, o))
    red("invalid click disarms", lambda b, d, o: (b["d_invalid"].update(armed=0) or b, d, o))
    red(
        "chain never stops",
        lambda b, d, o: (b["b_shift2"].update(armed=21) or b, dict(d, stopped=0), o),
    )
    red("extra order", lambda b, d, o: (b, d, 7))
    red("missing label", lambda b, d, o: (b.pop("a_plain") and b, d, o))
    red("kept count", lambda b, d, o: (b, dict(d, kept=3), o))
    red("no orders file", lambda b, d, o: (b, d, None))
    d11m = dict(d11, kept=3, stopped=0)
    mb = {
        k: dict(build["a_armed"], armed=21)
        for k in ("d11_armed", "d11_shift1", "d11_shift2", "d11_shift3")
    }
    mb["d11_armed"]["placed"] = 0
    for i, k in enumerate(("d11_shift1", "d11_shift2", "d11_shift3")):
        mb[k]["placed"] = i + 1
    mb["d11_plain"] = dict(mb["d11_armed"], armed=0, placed=4)
    if problems_mp(mb, d11m, 4):
        print("selftest: the good mp run is red: %s" % problems_mp(mb, d11m, 4))
        fails += 1
    if not problems_mp(dict(mb, d11_shift2=dict(mb["d11_shift2"], armed=0)), d11m, 4):
        print("selftest: mp disarm NOT caught")
        fails += 1
    blob = bytes(8)
    rec = bytearray(REC_LEN)
    struct.pack_into("<H", rec, 16, ORDER_PLACE)
    struct.pack_into("<i", rec, 20, 21)
    other = bytearray(rec)
    struct.pack_into("<i", other, 20, 5)
    if count_orders(blob + bytes(rec) + bytes(rec) + bytes(other), 21) != 2:
        print("selftest: count_orders miscounted")
        fails += 1
    print("check_shift_rearm --selftest: %s" % ("FAIL (%d)" % fails if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
