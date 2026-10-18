#!/usr/bin/env python3
"""check_drag_box.py -- RES12: the drag-select box behaves exactly as in vanilla.

The rows res_drag_stock and res_drag_box run the SAME scripted drags (tools/uiscripts/res_drag_*.txt) on 2-almost.sav, a 64-tile map:

  res_drag_stock  1024x768, the wrap splice OFF ([video] wrap_window=0): the game's own corner conversion. THE VANILLA REFERENCE.
  res_drag_box    the same save at 1920x1056 with the biased wrap window and the drag-box fix ON (shipped configuration, and with
                  [config] mode=original in res_drag_box_orig).

Each drag logs to mh_video.log: `; [sel] <label> cam=X,Y gest= btn= n= bldg= ids=...` (the selection the release produced) and,
from the draw guard, `; [drag_box] world L,T..R,B (w W h H) cam X,Y screen l,t..r,b` (the box in world px the game chose, and the
outline the draw was handed). The last box before each `dK_dragging` line is drag K's outline.

The oracle tools/data/drag_box_vanilla.json is the stock arm's record. What must hold:
  * stock arm (--stock): every drag equals the oracle exactly (world box, camera, outline, selected unit ids), and so does the order
    the walk issues (mh_rec_orders.bin, every field but exec_time) -- the oracle itself is regression-guarded;
  * wide arm: the world box, the camera, the selected unit ids and the order bytes equal the oracle EXACTLY (selection and orders are
    computed from world corners only, which the fix never touches); the outline equals the oracle's after clamping both to the
    stock window ([-1, 1024] x [-1, 768]) -- where the vanilla outline is drawn, the wide arm draws the same one;
  * both arms: no `SKIPPED an inverted outline` line unless --allow-skip (the guard must not be what makes the box right), no outline
    with right < left or bottom < top, no `drag_box] draw guard NOT armed`.
Absence of a drag is RED.

  check_drag_box.py [--stock | --update-oracle] [--allow-skip] <run-dir>
  check_drag_box.py --wide <run-dir>     RES13: the res_drag_wide row (drags wider than half the map), see wide_problems
  check_drag_box.py ... --segment NAME [--session N] <run-dir>
                                         a multi-segment walk (ship_walk): judge only the lines between `; [mark] NAME` and
                                         the next mark, and the orders of the Nth game session that recorded any
  check_drag_box.py --selftest
"""

import gzip
import json
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ORACLE = os.path.join(HERE, "data", "drag_box_vanilla.json")
LABELS = [
    "d1_inside",
    "d2_reversed",
    "d3_small",
    "d4_seam",
    "d5_anchor_left",
    "d6_anchor_right",
    "d7_anchor_up",
    "d8_anchor_down",
]
STOCK_W, STOCK_H = 1024, 768  # the stock arm's window: the draw clamps to it
BIG = 2048  # the 64-tile map in px

SEL = re.compile(
    r"^; \[sel\] (?P<label>\S+) cam=(?P<cx>-?\d+),(?P<cy>-?\d+) gest=(?P<gest>-?\d+) btn=(?P<btn>-?\d+) n=(?P<n>-?\d+) bldg=(?P<bldg>-?\d+) ids=(?P<ids>[0-9,]*)\s*$"
)
BOX = re.compile(
    r"^; \[drag_box\] world (?P<wl>-?\d+),(?P<wt>-?\d+)\.\.(?P<wr>-?\d+),(?P<wb>-?\d+) \(w (?P<w>-?\d+) h (?P<h>-?\d+)\) "
    r"cam (?P<cx>-?\d+),(?P<cy>-?\d+) screen (?P<l>-?\d+),(?P<t>-?\d+)\.\.(?P<r>-?\d+),(?P<b>-?\d+)\s*$"
)
STAMP = re.compile(r"^\[[0-9:.]+\] ")
sys.path.insert(0, HERE)
import walk_segments  # noqa: E402  the `; [mark] <segment>` slicer shared by the multi-segment walks

ORDER_LEN, REC_LEN, HEADER_LEN = 0x44, 0x48, 8


def video_log(run_dir):
    p = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def session_dirs(run_dir):
    """The menu dir and every later run dir up to the next process's menu dir."""
    run_dir = os.path.normpath(run_dir)
    logs = os.path.dirname(run_dir)
    names = sorted(n for n in os.listdir(logs) if os.path.isdir(os.path.join(logs, n)))
    out = []
    for i, n in enumerate([n for n in names if n >= os.path.basename(run_dir)]):
        if i > 0 and "_menu_" in n:
            break
        out.append(os.path.join(logs, n))
    return out


def read_orders(run_dir, session=None):
    """The player's recorded orders as hex strings of every byte but exec_time (the double at 0), in record order; None if no file.
    `session` (1-based) picks the Nth game session of a multi-segment walk that recorded orders (a walk starts one game per
    segment, each in its own session dir); None keeps the single-game rows' rule, the first session that has the file."""
    seen = 0
    for d in session_dirs(run_dir):
        for name in ("mh_rec_orders.bin", "mh_rec_orders.bin.gz"):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                seen += 1
                if session is not None and seen != session:
                    continue
                blob = open(p, "rb").read()
                if p.endswith(".gz"):
                    blob = gzip.decompress(blob)
                body = blob[HEADER_LEN:]
                recs = [
                    body[i * REC_LEN + 4 : (i + 1) * REC_LEN] for i in range(len(body) // REC_LEN)
                ]
                # the player's own orders (owner 0 on 2-almost), up to the last selection record (kind 0): what the process does
                # after the script's last drag is not the drags' doing (a unit that arrived re-orders itself, and the two arms'
                # processes end at different sim steps). A move order is re-queued every step until the unit arrives, one record
                # per step (118 of them, the count moves with the timing): the kind-0x80 records collapse to their distinct
                # bytes. The records of one step carry no meaningful order (the unit ids of a selection come out in either
                # order), so the list is sorted: the claim is the SET of orders issued.
                steps = [struct.unpack_from("<i", body, i * REC_LEN)[0] for i in range(len(recs))]
                mine = [(st, r) for st, r in zip(steps, recs) if r[0x0A] & 0x0F == 0]
                cut = max([st for st, r in mine if r[0x0A] & 0xF0 == 0] or [0])
                seen, out = set(), []
                for st, r in mine:
                    if st > cut:
                        continue
                    if r[0x0A] & 0xF0 == 0x80:
                        if r[8:] in seen:
                            continue
                        seen.add(r[8:])
                    out.append(r[8:].hex())
                return sorted(out)
    return None


def parse(text, premarked=False):
    """{label: {world, cam, screen, ids, n}} plus the guard lines. Drag K's box is the last `[drag_box] world` before dK_dragging.
    `premarked`: the text is already one walk segment's slice (tools/walk_segments.py), not a whole log to cut at the row's mark."""
    text = "\n".join(STAMP.sub("", ln) for ln in text.splitlines())
    if premarked:
        body = text
    else:
        seg = re.split(r"; \[mark\] res_drag_(?:box|wide)", text, maxsplit=1)
        body = seg[1] if len(seg) == 2 else ""
    drags, last_box, skipped = {}, None, []
    for ln in body.splitlines():
        m = BOX.match(ln)
        if m:
            last_box = {k: int(v) for k, v in m.groupdict().items()}
            continue
        if "[drag_box] SKIPPED" in ln:
            skipped.append(ln)
            continue
        m = SEL.match(ln)
        if not m:
            continue
        lab = m.group("label")
        if not re.match(r"d\d", lab):
            continue
        if lab.endswith("_dragging"):
            k = lab[: -len("_dragging")]
            drags.setdefault(k, {})["box"] = last_box
        elif lab.endswith("_press"):  # RES14: the camera the drag was pressed at
            k = lab[: -len("_press")]
            drags.setdefault(k, {})["press_cam"] = [int(m.group("cx")), int(m.group("cy"))]
        else:
            k = next((x for x in LABELS if x.startswith(lab + "_") or x == lab), lab)
            d = drags.setdefault(k, {})
            d["ids"] = [int(x) for x in m.group("ids").split(",") if x]
            d["cam"] = [int(m.group("cx")), int(m.group("cy"))]
            d["n"] = int(m.group("n"))
    # d1_dragging -> key d1 ; a final label d1_inside -> d1 : normalise both to the d<k> prefix
    out = {}
    for k, v in drags.items():
        out.setdefault(k.split("_")[0], {}).update(v)
    return out, skipped


def snapshot(run_dir, segment=None, session=None):
    text = video_log(run_dir)
    if text is None:
        return None, ["no mh_video.log in %s" % run_dir]
    seg, why = walk_segments.slice_or_problem(text, segment)
    if why:
        return None, [why]
    drags, skipped = parse(seg, premarked=bool(segment))
    return {
        "drags": drags,
        "orders": read_orders(run_dir, session),
        "skipped": skipped,
        "armed": "draw guard armed" in text,
        "dir_armed": "drag direction armed (4/4 sites)" in text,
    }, []


def clamp(v, hi):
    return max(-1, min(v, hi))


def stock_axis(frm, to, big=BIG):
    """llm_strat_pixel_delta_wrapped on one axis: the short way round the torus, result in [-big/2, big/2]."""
    sign, delta = (-1, frm - to) if frm > to else (1, to - frm)
    if big // 2 < delta:
        delta, sign = big - delta, -sign
    return sign * delta


def faithful(box):
    """True when the vanilla outline of this box is the box: neither axis's two corners are folded apart by the +-big/2 wrap
    (vanilla converts each corner on its own, so a corner more than half a map from the camera lands on the wrong side and the
    outline is lost or inverted). Only such a drag's outline is compared; a vanilla view cannot show the others."""
    return (
        stock_axis(box["cx"], box["wr"]) - stock_axis(box["cx"], box["wl"])
        == (box["wr"] - box["wl"]) % BIG
        and stock_axis(box["cy"], box["wb"]) - stock_axis(box["cy"], box["wt"])
        == (box["wb"] - box["wt"]) % BIG
    )


def outline(box):
    return [box["l"], box["t"], box["r"], box["b"]]


def norm(o):
    return [clamp(o[0], STOCK_W), clamp(o[1], STOCK_H), clamp(o[2], STOCK_W), clamp(o[3], STOCK_H)]


# RES14: the drags where the fix DEPARTS from the vanilla record on purpose. d7's anchor is scrolled ~1016 px off the top while
# the button is held, so the drag (anchor at y -1016 to the cursor at 100) is 1116 px tall -- more than half the map, and the
# stock shorter-arc rule flips it: vanilla draws an inverted outline and selects the complementary 932 px arc. The fix draws and
# selects the anchor-to-cursor box instead (the player's report, 2026-10-13). Its selection (no unit; a building) and the order
# record are still compared with the oracle; only the world box and the outline are replaced here.
DEPARTS = {
    "d7": {"world": [1840, 1736, 1940, 804], "outline": [400, -1, 500, 100]},
}


def problems(snap, oracle, stock, allow_skip=False, departs=None):
    departs = DEPARTS if departs is None else departs
    out = []
    if not snap["armed"]:
        out.append("no `[drag_box] draw guard armed` line")
    if snap["skipped"] and not allow_skip:
        out.append(
            "the draw guard skipped an inverted outline (%d): %s"
            % (len(snap["skipped"]), snap["skipped"][0])
        )
    want = sorted(oracle["drags"])
    for k in want:
        d, o = snap["drags"].get(k), oracle["drags"][k]
        if not d or "box" not in d or "ids" not in d or d["box"] is None:
            out.append("%s: no box / selection logged (the walk never got there)" % k)
            continue
        b = d["box"]
        ob = o["box"]
        if (
            k in departs
        ):  # RES14: the declared departure -- the anchor-to-cursor box, not the vanilla flip
            dep = departs[k]
            if [b["wl"], b["wt"], b["wr"], b["wb"]] != dep["world"]:
                out.append(
                    "%s: world box %s != the anchor-to-cursor box %s (RES14)"
                    % (k, [b["wl"], b["wt"], b["wr"], b["wb"]], dep["world"])
                )
            if outline(b) != dep["outline"]:
                out.append(
                    "%s: outline %s != the anchor-to-cursor outline %s (RES14)"
                    % (k, outline(b), dep["outline"])
                )
            if [b["cx"], b["cy"]] != [ob["cx"], ob["cy"]]:
                out.append(
                    "%s: camera %s != vanilla %s" % (k, [b["cx"], b["cy"]], [ob["cx"], ob["cy"]])
                )
            if d["ids"] != o["ids"]:
                out.append("%s: selected ids %s != vanilla %s" % (k, d["ids"], o["ids"]))
            continue
        if [b["wl"], b["wt"], b["wr"], b["wb"]] != [ob["wl"], ob["wt"], ob["wr"], ob["wb"]]:
            out.append(
                "%s: world box %s != vanilla %s"
                % (
                    k,
                    [b["wl"], b["wt"], b["wr"], b["wb"]],
                    [ob["wl"], ob["wt"], ob["wr"], ob["wb"]],
                )
            )
        if [b["cx"], b["cy"]] != [ob["cx"], ob["cy"]]:
            out.append(
                "%s: camera %s != vanilla %s" % (k, [b["cx"], b["cy"]], [ob["cx"], ob["cy"]])
            )
        if d["ids"] != o["ids"]:
            out.append("%s: selected ids %s != vanilla %s" % (k, d["ids"], o["ids"]))
        got, ref = outline(b), outline(ob)
        if not stock and (
            b["r"] < b["l"] or b["b"] < b["t"]
        ):  # (vanilla itself inverts a folded box and its draw then bails)
            out.append("%s: inverted outline %s" % (k, got))
        if stock:
            if got != ref:
                out.append("%s: outline %s != the oracle %s" % (k, got, ref))
        elif not faithful(ob):
            pass  # the vanilla outline is lost to the +-big/2 fold (see faithful); only the order above is asserted
        elif norm(got) != norm(ref):
            out.append(
                "%s: outline %s (clamped %s) != vanilla %s (clamped %s)"
                % (k, got, norm(got), ref, norm(ref))
            )
    for k in snap["drags"]:
        if k not in oracle["drags"]:
            out.append("%s: not in the oracle" % k)
    if snap["orders"] is None:
        out.append("no mh_rec_orders.bin (the row needs [log] level=debug)")
    elif snap["orders"] != oracle["orders"]:
        out.append(
            "orders differ: %d recorded vs %d in the oracle (first %s)"
            % (len(snap["orders"]), len(oracle["orders"]), (snap["orders"] or ["-"])[0][:60])
        )
    elif not oracle["orders"]:
        out.append("the oracle holds no order")
    return out


# RES13: res_drag_wide. Camera world (1440, 1696); a screen point is world = camera + screen (mod 2048). The drags are wider than
# half the map (1400 px against 1024), so there is no vanilla record to equal (a retail view cannot make them): the claim is
# what the player sees -- the outline runs from the anchor to the cursor ON SCREEN, the world box is the 1400 px one (not the
# complementary 648 px arc the shorter-arc rule picks), and the selection is what that box holds, the same either way round.
WIDE_CAM = [1440, 1696]
WIDE = {  # drag -> (world l,t,r,b, outline l,t,r,b) ; d9 anchor (100,300) -> cursor (1500,700), d10 the reverse
    "d9": ([1540, 1996, 892, 348], [100, 300, 1500, 700]),
    "d10": ([1540, 1996, 892, 348], [100, 300, 1500, 700]),
}


# RES14: drags whose anchor the CAMERA scrolls off screen mid-drag (arrow keys held with the button down), until the box is wider
# (d11) / taller (d12) than half the map -- the stock shorter-arc rule flips such a box at any view size, vanilla included. The
# anchor is pressed at a screen point at the camera logged as dK_press; the cursor ends at a screen point. The world box must
# run from the anchor to the cursor (both drags are anchor-left / anchor-top), the outline from the anchor's scrolled screen
# position to the cursor, and the selection hold the units the d1 box holds (they lie inside both boxes).
SCROLLED = {  # drag -> (anchor screen x,y at the press, cursor screen x,y at the end, the scrolled axis)
    "d11": ((500, 300), (1500, 700), 0),
    "d12": ((550, 100), (850, 850), 1),
}


def _signed(v):
    return (v + BIG // 2) % BIG - BIG // 2


def scrolled_problems(snap, oracle):
    out = []
    base = oracle["drags"]["d1"]["ids"]
    for k, (anc, cur, axis) in sorted(SCROLLED.items()):
        d = snap["drags"].get(k)
        if not d or not d.get("box") or "ids" not in d or "press_cam" not in d:
            out.append("%s: no press camera / box / selection logged" % k)
            continue
        b, pc = d["box"], d["press_cam"]
        cam = [b["cx"], b["cy"]]
        moved = [_signed(cam[i] - pc[i]) for i in (0, 1)]
        sa = [
            anc[i] - moved[i] for i in (0, 1)
        ]  # the anchor's screen position after the scroll (unwrapped)
        if cur[axis] - sa[axis] <= BIG // 2:
            out.append(
                "%s: the camera moved %s -- the drag is only %d px on the scrolled axis, not wider than half the map (the test did not test)"
                % (k, moved, cur[axis] - sa[axis])
            )
            continue
        world = [
            (pc[0] + anc[0]) % BIG,
            (pc[1] + anc[1]) % BIG,
            (cam[0] + cur[0]) % BIG,
            (cam[1] + cur[1]) % BIG,
        ]
        if [b["wl"], b["wt"], b["wr"], b["wb"]] != world:
            out.append(
                "%s: world box %s != anchor-to-cursor %s (the complementary arc?)"
                % (k, [b["wl"], b["wt"], b["wr"], b["wb"]], world)
            )
        if outline(b) != [max(-1, sa[0]), max(-1, sa[1]), cur[0], cur[1]]:
            out.append(
                "%s: outline %s != scrolled anchor to cursor %s"
                % (k, outline(b), [max(-1, sa[0]), max(-1, sa[1]), cur[0], cur[1]])
            )
        if not set(base) <= set(d["ids"]):
            out.append("%s: selected ids %s lack the units inside the box %s" % (k, d["ids"], base))
    return out


def wide_problems(snap, oracle):
    out = scrolled_problems(snap, oracle)
    if not snap["armed"]:
        out.append("no `[drag_box] draw guard armed` line")
    if not snap.get("dir_armed"):
        out.append(
            "no `[drag_dir] drag direction armed (4/4 sites)` line (drag_dir_fix off or a site not armed)"
        )
    if snap["skipped"]:
        out.append("the draw guard skipped an inverted outline: %s" % snap["skipped"][0])
    got = {}
    for k, (world, outl) in sorted(WIDE.items()):
        d = snap["drags"].get(k)
        if not d or not d.get("box") or "ids" not in d:
            out.append("%s: no box / selection logged" % k)
            continue
        b = d["box"]
        got[k] = d["ids"]
        if [b["cx"], b["cy"]] != WIDE_CAM:
            out.append("%s: camera %s != %s" % (k, [b["cx"], b["cy"]], WIDE_CAM))
        if [b["wl"], b["wt"], b["wr"], b["wb"]] != world:
            out.append(
                "%s: world box %s != the on-screen one %s (the complementary arc?)"
                % (k, [b["wl"], b["wt"], b["wr"], b["wb"]], world)
            )
        if outline(b) != outl:
            out.append("%s: outline %s != anchor-to-cursor on screen %s" % (k, outline(b), outl))
        base = oracle["drags"]["d1"][
            "ids"
        ]  # the screen box 600..800 x 350..700 lies inside both drags' boxes
        if not set(base) <= set(d["ids"]):
            out.append("%s: selected ids %s lack the units inside the box %s" % (k, d["ids"], base))
    if len(got) == 2 and got["d9"] != got["d10"]:
        out.append("the two directions select different units: %s vs %s" % (got["d9"], got["d10"]))
    return out


def pop_session(argv):
    """(N or None, argv without `--session N`): the Nth game session of a multi-segment walk that recorded orders (1-based)."""
    argv = list(argv)
    if "--session" in argv:
        i = argv.index("--session")
        return int(argv[i + 1]), argv[:i] + argv[i + 2 :]
    return None, argv


def main(argv):
    if "--selftest" in argv:
        return selftest()
    segment, argv = walk_segments.pop_segment(argv)
    session, argv = pop_session(argv)
    if "--wide" in argv:
        args = [a for a in argv if not a.startswith("--")]
        if len(args) != 1:
            print(__doc__)
            return 2
        snap, bad = snapshot(args[0], segment, session)
        if not bad:
            with open(ORACLE) as f:
                bad = wide_problems(snap, json.load(f))
        for b in bad:
            print("  check_drag_box: " + b)
        print("check_drag_box (wide drags): %s" % ("FAIL" if bad else "PASS"))
        return 1 if bad else 0
    stock, update, allow_skip = "--stock" in argv, "--update-oracle" in argv, "--allow-skip" in argv
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    snap, bad = snapshot(args[0], segment, session)
    if bad:
        print("check_drag_box: FAIL -- " + "; ".join(bad))
        return 1
    if update:
        keep = {"drags": snap["drags"], "orders": snap["orders"] or []}
        os.makedirs(os.path.dirname(ORACLE), exist_ok=True)
        with open(ORACLE, "w", newline="\n") as f:
            json.dump(keep, f, indent=1, sort_keys=True)
            f.write("\n")
        print(
            "check_drag_box: wrote %s (%d drags, %d orders)"
            % (ORACLE, len(keep["drags"]), len(keep["orders"]))
        )
        return 0
    with open(ORACLE) as f:
        oracle = json.load(f)
    bad = problems(snap, oracle, stock, allow_skip)
    for b in bad:
        print("  check_drag_box: " + b)
    print(
        "check_drag_box (%s): %s"
        % (
            "stock arm" if stock else "wide arm",
            "FAIL"
            if bad
            else "PASS (%d drags, %d orders)" % (len(oracle["drags"]), len(oracle["orders"])),
        )
    )
    return 1 if bad else 0


def selftest():
    fails = 0

    def box(wl, wt, wr, wb, l, t, r, b, cam=(1440, 1696)):
        return {
            "wl": wl,
            "wt": wt,
            "wr": wr,
            "wb": wb,
            "w": 0,
            "h": 0,
            "cx": cam[0],
            "cy": cam[1],
            "l": l,
            "t": t,
            "r": r,
            "b": b,
        }

    drag = lambda bx, ids: {"box": bx, "ids": ids, "cam": [1440, 1696], "n": len(ids)}  # noqa: E731
    oracle = {
        "drags": {
            "d1": drag(box(10, 20, 30, 40, 100, 100, 300, 300), [2, 5]),
            "d5": drag(box(10, 20, 30, 40, -1, 100, 500, 300), [2]),
        },
        "orders": ["aa"],
    }
    good = {
        "armed": True,
        "skipped": [],
        "orders": ["aa"],
        "drags": json.loads(json.dumps(oracle["drags"])),
    }

    def run(label, want_red, mut, stock=False):
        nonlocal fails
        s = json.loads(json.dumps(good))
        mut(s)
        red = bool(problems(s, oracle, stock))
        ok = red == want_red
        fails += not ok
        print("  %s %s" % ("ok  " if ok else "FAIL", label))

    run("identical", False, lambda s: None)
    run("identical (stock arm)", False, lambda s: None, True)
    # RES14: a declared departure (d1 here) passes as the anchor-to-cursor box and fails as the vanilla one
    dep = {"d1": {"world": [30, 20, 10, 40], "outline": [-1, 100, 300, 300]}}

    def rund(label, want_red, mut):
        nonlocal fails
        s = json.loads(json.dumps(good))
        mut(s)
        red = bool(problems(s, oracle, True, departs=dep))
        ok = red == want_red
        fails += not ok
        print("  %s %s" % ("ok  " if ok else "FAIL", label))

    rund(
        "departure: the anchor-to-cursor box",
        False,
        lambda s: s["drags"]["d1"]["box"].update(wl=30, wr=10, l=-1),
    )
    rund("departure: the vanilla box is now red", True, lambda s: None)
    rund(
        "departure: the right box with the wrong outline",
        True,
        lambda s: s["drags"]["d1"]["box"].update(wl=30, wr=10),
    )
    rund(
        "departure: its selection still matches vanilla",
        True,
        lambda s: (
            s["drags"]["d1"]["box"].update(wl=30, wr=10, l=-1),
            s["drags"]["d1"].update(ids=[9]),
        ),
    )
    run(
        "wide arm: the outline beyond the stock window is clamped away",
        False,
        lambda s: s["drags"]["d5"]["box"].update(l=-400),
    )
    run(
        "stock arm: any outline change is red",
        True,
        lambda s: s["drags"]["d5"]["box"].update(l=-400),
        True,
    )
    run(
        "wide arm: the sliver on the far right (the D13c picture)",
        True,
        lambda s: s["drags"]["d5"]["box"].update(l=1548, r=1900),
    )
    run(
        "wide arm: an inverted outline",
        True,
        lambda s: s["drags"]["d1"]["box"].update(l=900, r=700),
    )
    run("different selection", True, lambda s: s["drags"]["d1"].update(ids=[2]))
    run("different world box", True, lambda s: s["drags"]["d1"]["box"].update(wr=31))
    run("different order bytes", True, lambda s: s.update(orders=["ab"]))
    run("no order recorded", True, lambda s: s.update(orders=None))
    run("a drag missing", True, lambda s: s["drags"].pop("d5"))
    run(
        "the guard skipped a box",
        True,
        lambda s: s.update(skipped=["; [drag_box] SKIPPED an inverted outline"]),
    )
    run("guard not armed", True, lambda s: s.update(armed=False))
    oracle["drags"]["d1"]["ids"] = [2, 5]
    wide_good = {
        "armed": True,
        "dir_armed": True,
        "skipped": [],
        "drags": {k: {"box": box(*w, *o), "ids": [2, 5]} for k, (w, o) in WIDE.items()},
    }
    # d11: pressed at cam 1440 (anchor world 1940,1996), scrolled +704 px right -> cam 96; cursor 1500,700 -> world 1596,348
    wide_good["drags"]["d11"] = {
        "box": box(1940, 1996, 1596, 348, -1, 300, 1500, 700, cam=(96, 1696)),
        "ids": [2, 5],
        "press_cam": [1440, 1696],
    }
    # d12: pressed at cam 1440,1696 (anchor world 1990,1796), scrolled +704 px down -> cam y 352; cursor 850,850 -> 242,1202
    wide_good["drags"]["d12"] = {
        "box": box(1990, 1796, 242, 1202, 550, -1, 850, 850, cam=(1440, 352)),
        "ids": [2, 5],
        "press_cam": [1440, 1696],
    }

    def runw(label, want_red, mut):
        nonlocal fails
        s = json.loads(json.dumps(wide_good))
        mut(s)
        red = bool(wide_problems(s, oracle))
        if red != want_red:
            print("    ", wide_problems(s, oracle))
        ok = red == want_red
        fails += not ok
        print("  %s %s" % ("ok  " if ok else "FAIL", label))

    runw("wide: as expected", False, lambda s: None)
    runw(
        "wide: the complementary arc (world box 892..1540)",
        True,
        lambda s: s["drags"]["d1" if False else "d9"]["box"].update(wl=892, wr=1540),
    )
    runw(
        "wide: the complementary outline",
        True,
        lambda s: s["drags"]["d9"]["box"].update(l=1500, r=2148),
    )
    runw(
        "wide: the selection lost the units in the box",
        True,
        lambda s: s["drags"]["d10"].update(ids=[]),
    )
    runw("wide: the two directions differ", True, lambda s: s["drags"]["d10"].update(ids=[2, 5, 7]))
    runw("wide: the direction fix not armed", True, lambda s: s.update(dir_armed=False))
    runw("wide: a drag missing", True, lambda s: s["drags"].pop("d10"))
    runw(
        "scrolled: the stock flip (world box 1596..1940, the complementary arc)",
        True,
        lambda s: s["drags"]["d11"]["box"].update(wl=1596, wr=1940),
    )
    runw(
        "scrolled: the outline from the anchor's alias on the right",
        True,
        lambda s: s["drags"]["d11"]["box"].update(l=1844),
    )
    runw(
        "scrolled: the vertical flip",
        True,
        lambda s: s["drags"]["d12"]["box"].update(wt=1202, wb=1796),
    )
    runw(
        "scrolled: the selection lost the units", True, lambda s: s["drags"]["d12"].update(ids=[7])
    )
    runw(
        "scrolled: the camera barely moved (the case is not exercised)",
        True,
        lambda s: s["drags"]["d11"].update(press_cam=[96 - 100, 1696]),
    )
    runw("scrolled: no press camera logged", True, lambda s: s["drags"]["d12"].pop("press_cam"))
    text = (
        "[00:00:01.000] ; [drag_box] draw guard armed at 004276a2: x\n"
        "[00:00:02.000] ; [mark] res_drag_box\n"
        "[00:00:03.000] ; [sel] start cam=1440,1696 gest=0 btn=0 n=0 bldg=1 ids=\n"
        "[00:00:04.000] ; [drag_box] world 10,20..30,40 (w 0 h 0) cam 1440,1696 screen 100,100..300,300\n"
        "[00:00:05.000] ; [sel] d1_dragging cam=1440,1696 gest=2 btn=1 n=0 bldg=1 ids=\n"
        "[00:00:06.000] ; [sel] d1_inside cam=1440,1696 gest=0 btn=0 n=2 bldg=0 ids=2,5\n"
    )
    d, sk = parse(text)
    ok = (
        d
        == {
            "d1": {
                "box": box(10, 20, 30, 40, 100, 100, 300, 300),
                "ids": [2, 5],
                "cam": [1440, 1696],
                "n": 2,
            }
        }
        and not sk
    )
    fails += not ok
    print("  %s parse" % ("ok  " if ok else "FAIL"))
    print("check_drag_box --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
