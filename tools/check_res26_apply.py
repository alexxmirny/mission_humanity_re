#!/usr/bin/env python3
"""check_res26_apply.py -- mods:RES26: the HUD shows the requested Interface size RIGHT AFTER Options Apply, while the menu (game mode 3) is still up.

The user's rc5 report: after an in-game Screen size change + Apply the presented HUD was 100% (one quad, Hp 0) until the game resumed. The percentage in force
for the dialog modes came from the stamp of the last strategic present, which still held the OLD frame size. Rows res_apply_menu_200_d3d11 / _gdi
(tools/uiscripts/res_apply_menu_200_*.txt) apply a Screen size change (phase a), an Interface size change (phase b) and read:

  1. LOG: every `view epoch N: ... pct in force P (requested R) ... game mode 3|4` line written AFTER the first strategic frame has P == R (the asked size is the one
     presented in the menu: a stale 100 over a 200 request is the bug), and the line is not a `(single quad)` one while R > 100;
  2. LOG: the Options apply of each phase leaves a mode-3 view epoch line at the new request (phase b: 150);
  3. PRESENTED: the side-panel list rows of the menu bcapture (r26_<ph>_menu__b) equal those of the resume bcapture (r26_<ph>_resume__b) -- one panel quad of
     pw = 160 * P / 100 columns in both, at the same size (cursor and minimap excluded: the rows are below the minimap).

  check_res26_apply.py <run-dir>
  check_res26_apply.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

VIEW = re.compile(
    r"; \[ui\] view epoch (\d+): present (\d+), pct in force (\d+) \(requested (\d+)\), screen (\d+)x(\d+), frame (\d+)x(\d+), "
    r"composition .*?(\(single quad\))?, game mode (\d+)"
)
LIST_TOP = 248
STRIP = 64
PHASES = (("a", 200), ("b", 150))


def views(text):
    out = []
    for m in VIEW.finditer(text):
        g = m.groups()
        out.append(
            dict(
                epoch=int(g[0]),
                pct=int(g[2]),
                req=int(g[3]),
                sw=int(g[4]),
                sh=int(g[5]),
                fw=int(g[6]),
                fh=int(g[7]),
                single=g[8] is not None or "(single quad)" in m.group(0),
                mode=int(g[9]),
            )
        )
    return out


def log_problems(text):
    out = []
    vs = views(text)
    first = next((i for i, v in enumerate(vs) if v["mode"] == 2), None)
    if first is None:
        return ["no game mode 2 view epoch line: the strategic frame never presented"], vs
    for v in vs[first:]:
        if v["mode"] not in (3, 4):
            continue
        if v["pct"] != v["req"] and v["req"] > 100:
            out.append(
                "view epoch %d (game mode %d, frame %dx%d): pct in force %d but %d requested -- the menu shows the old size (RES26)"
                % (v["epoch"], v["mode"], v["fw"], v["fh"], v["pct"], v["req"])
            )
        if v["single"] and v["req"] > 100:
            out.append(
                "view epoch %d (game mode %d, frame %dx%d): a single quad at a %d%% request (RES26)"
                % (v["epoch"], v["mode"], v["fw"], v["fh"], v["req"])
            )
    reqs = [v["req"] for v in vs[first:] if v["mode"] in (3, 4)]
    if 150 not in reqs:
        out.append(
            "no mode-3 view epoch line at the 150%% request of phase b (requests seen: %r)"
            % (sorted(set(reqs)),)
        )
    return out, vs


def panel_rows(path, pct):
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size
    pw = 160 * pct // 100
    mx = max(4, pw // 12)
    return im.crop((w - pw + mx, LIST_TOP * pct // 100 + 4, w - mx, h - STRIP - 16))


def crop_diff(a, b, pixdelta=24):
    import numpy as np

    x, y = np.asarray(a, dtype=np.int32), np.asarray(b, dtype=np.int32)
    if x.shape != y.shape:
        return 1.0
    return float((np.abs(x - y).max(axis=2) > pixdelta).mean())


def problems(run_dir, tol=0.03):
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir], []
    out, vs = log_problems(open(vlog, encoding="utf-8", errors="replace").read())
    rep = ["%d view epoch lines" % len(vs)]
    for ph, pct in PHASES:
        pm = os.path.join(run_dir, "capture_r26_%s_menu__b.bmp" % ph)
        pr = os.path.join(run_dir, "capture_r26_%s_resume__b.bmp" % ph)
        if not (os.path.isfile(pm) and os.path.isfile(pr)):
            out.append("phase %s: capture_r26_%s_menu__b.bmp / _resume__b.bmp missing" % (ph, ph))
            continue
        d = crop_diff(panel_rows(pm, pct), panel_rows(pr, pct))
        rep.append(
            "phase %s (%d%%): menu vs resume panel list rows differ in %.2f%% of pixels (tolerance %.0f%%)"
            % (ph, pct, d * 100, tol * 100)
        )
        if d > tol:
            out.append(
                "phase %s: the presented panel in the menu differs from the one after the resume in %.1f%% of the list rows (> %.0f%%): the HUD was not at the %d%% layout right after Apply"
                % (ph, d * 100, tol * 100, pct)
            )
    return out, rep


def selftest():
    fails = []
    good = (
        "; [ui] view epoch 5: present 103, pct in force 200 (requested 200), screen 2560x1440, frame 1920x1088, composition 2560x1440 at 0,0, game mode 2, presenter d3d11, hud epoch 1\n"
        "; [ui] view epoch 6: present 228, pct in force 200 (requested 200), screen 2560x1440, frame 1920x1440, composition 2080x1440 at 240,0, game mode 3, presenter d3d11, hud epoch 3\n"
        "; [ui] view epoch 7: present 328, pct in force 150 (requested 150), screen 2560x1440, frame 1920x1440, composition 2080x1440 at 240,0, game mode 3, presenter d3d11, hud epoch 4\n"
    )
    bad = good.replace(
        "pct in force 200 (requested 200), screen 2560x1440, frame 1920x1440, composition 2080x1440 at 240,0, game mode 3",
        "pct in force 100 (requested 200), screen 2560x1440, frame 1920x1440, composition 1920x1440 at 320,0 (single quad), game mode 3",
    )
    if log_problems(good)[0]:
        fails.append("good log must be clean: %r" % (log_problems(good)[0],))
    if len(log_problems(bad)[0]) < 2:
        fails.append(
            "a stale 100%% single quad at a 200%% request must be RED twice: %r"
            % (log_problems(bad)[0],)
        )
    if not log_problems(good.replace("150 (requested 150)", "200 (requested 200)"))[0]:
        fails.append("a missing 150% request must be RED")
    print("check_res26_apply selftest: %s" % ("FAIL" if fails else "ok"))
    for f in fails:
        print("SELFTEST FAIL:", f)
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if len(argv) != 1:
        print(__doc__)
        return 2
    probs, rep = problems(argv[0])
    for r in rep:
        print(r)
    for p in probs:
        print("RED: " + p)
    if not probs:
        print("res26 apply OK")
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
