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


def read_orders(run_dir):
    """The player's recorded orders as hex strings of every byte but exec_time (the double at 0), in record order; None if no file."""
    for d in session_dirs(run_dir):
        for name in ("mh_rec_orders.bin", "mh_rec_orders.bin.gz"):
            p = os.path.join(d, name)
            if os.path.isfile(p):
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


def parse(text):
    """{label: {world, cam, screen, ids, n}} plus the guard lines. Drag K's box is the last `[drag_box] world` before dK_dragging."""
    text = "\n".join(STAMP.sub("", ln) for ln in text.splitlines())
    seg = text.split("; [mark] res_drag_box", 1)
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


def snapshot(run_dir):
    text = video_log(run_dir)
    if text is None:
        return None, ["no mh_video.log in %s" % run_dir]
    drags, skipped = parse(text)
    return {
        "drags": drags,
        "orders": read_orders(run_dir),
        "skipped": skipped,
        "armed": "draw guard armed" in text,
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


def problems(snap, oracle, stock, allow_skip=False):
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


def main(argv):
    if "--selftest" in argv:
        return selftest()
    stock, update, allow_skip = "--stock" in argv, "--update-oracle" in argv, "--allow-skip" in argv
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    snap, bad = snapshot(args[0])
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
