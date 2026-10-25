#!/usr/bin/env python3
"""
tools/check_imgui_heat_s150.py -- mods:M1WL v6: the post_check of the imgui_heat_s150 UI row (the F11 panel and the heat map at ui_scale 150).

The row (ini/imgui_heat_s150.ini, imgui_heat_s150.txt) runs the d3d11 presenter at ui_scale=150 and resizes the window wider than the composition, so the image
sits between letterbox bars (the user's layout: a 150 % HUD, the map 1:1 off-centre, the panel and the strip enlarged). The proofs are in mh_video.log:

  window  `; [imgui] window X0,Y0..X1,Y1 image L,T..R,B`   every logged window rect, once the image has its bars, lies inside the image rect (rc11 put the first
          position at client 16,16 -- the left bar, where the pointer cannot go)
  cursor  `; [imgui] cursor game GX,GY = client CX,CY`      ImGui's pointer is the DRAWN cursor: the client point equals the game point pushed through the
          layout (`; [ui] ui_scale ... map MWxMH 1:1, panel PW wide, strip SH high, composition CWxCH at X,Y`) by THIS file's own arithmetic, over the map, the panel and
          the strip, within 3 px (rc11 mapped everything through the plain letterbox: 40 px off on the map)

The heat map is a presented image: the row's bcapture is the thing to look at.

  python tools/check_imgui_heat_s150.py <session-run-dir>
"""

import os
import re
import sys

LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)% on (\d+)x(\d+), frame (\d+)x(\d+): map (\d+)x(\d+) 1:1, panel (\d+) wide \((\d+) frame rows\), strip (\d+) high, composition (\d+)x(\d+) at (\d+),(\d+)"
)
WINDOW = re.compile(
    r"; \[imgui\] window (-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) image (-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+)"
)
CURSOR = re.compile(r"; \[imgui\] cursor game (-?\d+),(-?\d+) = client (-?\d+),(-?\d+)")
TOL = 3


def read(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def find(run_dir, name):
    p = os.path.join(run_dir, name)
    if os.path.exists(p):
        return read(p)
    out = []
    for d in sorted(os.listdir(run_dir)) if os.path.isdir(run_dir) else []:
        q = os.path.join(run_dir, d, name)
        if os.path.exists(q):
            out += read(q)
    return out


def game_to_client(lay, gx, gy):
    """The game point's client pixel (centre convention) by the quad layout: map 1:1 at the top left of the composition; the panel (frame columns
    W-160..) and the strip (frame rows H-32.., under the map) at the HUD scale s = PW / 160."""
    _pct, _sw, _sh, fw, fh, mw, mh, pw, _rows, _sth, _cw, _ch, cx, cy = lay
    s = pw / 160.0
    if gx >= fw - 160:  # the panel: full height
        return cx + mw + (gx - (fw - 160) + 0.5) * s, cy + (gy + 0.5) * s
    if gy >= fh - 32:  # the strip: under the map, scaled in both axes
        return cx + (gx + 0.5) * s, cy + mh + (gy - (fh - 32) + 0.5) * s
    return cx + gx, cy + gy


def region_of(lay, gx, gy):
    return "panel" if gx >= lay[3] - 160 else "strip" if gy >= lay[4] - 32 else "map"


def judge(lines):
    """lines: mh_video.log in order (the layout lines and the overlay's lines share it)."""
    f = []
    idx = [i for i, ln in enumerate(lines) if (m := LAYOUT.search(ln)) and int(m.group(1)) == 150]
    lays = [tuple(int(v) for v in LAYOUT.search(lines[i]).groups()) for i in idx]
    if not lays:
        return [
            "no `; [ui] ui_scale 150% ... composition` layout line: the scale never took effect"
        ]
    lay = lays[-1]
    cx, cw = lay[12], lay[10]
    if cx <= 0:
        f.append(
            "the composition sits at x=%d: the row needs letterbox bars (a window wider than the composition)"
            % cx
        )
    after = lines[idx[-1] :]  # what happened once the final layout stood
    wins = [tuple(int(v) for v in m.groups()) for m in map(WINDOW.search, after) if m]
    barred = [
        w for w in wins if w[4] == cx and w[6] == cx + cw
    ]  # the lines logged once the image has its bars
    if not barred:
        f.append("no `; [imgui] window` line with the barred image rect (%d..%d)" % (cx, cx + cw))
    for w in barred:
        if w[0] < w[4] or w[1] < w[5] or w[2] > w[6] or w[3] > w[7]:
            f.append("the window %d,%d..%d,%d lies outside the image %d,%d..%d,%d" % w)
    if barred and not any(w[2] == w[6] or w[3] == w[7] for w in barred):
        f.append(
            "no window rect reached the image's right / bottom edge: the drag past the edge never engaged the clamp"
        )
    cur = [tuple(int(v) for v in m.groups()) for m in map(CURSOR.search, after) if m]
    seen = set()
    for gx, gy, px, py in cur:
        if gx >= lay[3] or gy >= lay[4]:
            continue
        ex, ey = game_to_client(lay, gx, gy)
        region = region_of(lay, gx, gy)
        if abs(px - ex) > TOL or abs(py - ey) > TOL:
            f.append(
                "cursor game %d,%d (%s): ImGui at client %d,%d, the layout says %.1f,%.1f"
                % (gx, gy, region, px, py, ex, ey)
            )
        else:
            seen.add(region)
    for region in ("map", "panel", "strip"):
        if region not in seen:
            f.append("no cursor line over the %s matched (the probe never reached it)" % region)
    return f


def selftest():
    lay = "; [ui] ui_scale 150% on 1900x800, frame 1216x800: map 1056x752 1:1, panel 240 wide (534 frame rows), strip 48 high, composition 1296x800 at 302,0; ideal render 1824x800"
    L = tuple(int(v) for v in LAYOUT.search(lay).groups())
    px, py = game_to_client(L, 1130, 200)
    sx, sy = game_to_client(L, 500, 790)
    good_net = [
        "; [imgui] window 302,16..642,464 image 302,0..1598,800",
        "; [imgui] window 1258,326..1598,800 image 302,0..1598,800",
        "; [imgui] cursor game 400,300 = client 702,300",
        "; [imgui] cursor game 1130,200 = client %d,%d" % (round(px), round(py)),
        "; [imgui] cursor game 500,790 = client %d,%d" % (round(sx), round(sy)),
    ]
    bad = 0

    def case(name, net, video, want_red):
        nonlocal bad
        red = bool(judge(video + net if video else net))
        bad += red != want_red
        print("  %-52s %s" % (name, "ok" if red == want_red else "WRONG"))

    case("good run", good_net, [lay], False)
    case("no layout line", good_net, [], True)
    case(
        "window left of the image (rc11)",
        ["; [imgui] window 16,16..356,464 image 302,0..1598,800"] + good_net[1:],
        [lay],
        True,
    )
    case(
        "window past the image's bottom",
        ["; [imgui] window 302,16..642,900 image 302,0..1598,800"] + good_net[1:],
        [lay],
        True,
    )
    case(
        "cursor through the plain letterbox (rc11)",
        good_net[:2] + ["; [imgui] cursor game 400,300 = client 742,300"] + good_net[3:],
        [lay],
        True,
    )
    case("the drag never reached the edge", good_net[:1] + good_net[2:], [lay], True)
    case("no panel row", good_net[:3] + good_net[4:], [lay], True)
    print("check_imgui_heat_s150 --selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    if len(sys.argv) != 2:
        print("usage: check_imgui_heat_s150.py <session-run-dir> | --selftest")
        sys.exit(2)
    d = sys.argv[1]
    fails = judge(find(d, "mh_video.log"))
    for x in fails:
        print("  FAIL: " + x)
    print("check_imgui_heat_s150: %s" % ("FAIL (%d clause(s))" % len(fails) if fails else "PASS"))
    sys.exit(1 if fails else 0)
