#!/usr/bin/env python3
"""check_bcapture.py -- mods:RES23-S0: the presenter's real output agrees with the CPU re-composition.

`pcapture <n>` (capture_<n>.bmp) is the CPU's re-composition of the game frame into the presenter's quad list. `bcapture <n>`
(capture_<n>__b.bmp) is what the PRESENTER put on screen (gdi: the client area read back with BitBlt; d3d11: the back buffer after the
overlay pass, before Present). A presenter-only bug is invisible to pcapture by construction; this check is the proof that, on a given
row, the two are the same picture.

  For every capture_<n>.bmp that has a capture_<n>__b.bmp:
    1. the sizes are equal (the presenter's screen == the layout's screen);
    2. the fraction of pixels whose largest channel differs by more than --pixdelta stays within the tolerance.
       Tolerance (the filter tolerance): gdi -> 0.002 (COLORONCOLOR point sampling, the compositor's pixel-centre rule up to
       rounding at non-integer scales); d3d11 -> 0.01 with filter=point (the GPU's texel pick differs from the CPU's by one source
       pixel along scaled quads: measured 0.47% of a 2560x1440 screen at a 1.5x panel), 0.03 with linear/sharp/area. --tol overrides.
    3. no 32x32 tile has more than half of its pixels different (a missing layer, a black band or a moved quad is a BLOCK, edge
       aliasing is sparse).
  A `; bcapture <n> SKIPPED` line in mh_capture.log (the null presenter shows nothing) is reported and is NOT an error, unless NO pair was
  compared and --require-pair is given (the res19 rows).

  check_bcapture.py <run-dir> [--tol F] [--pixdelta N] [--require-pair] [--names a,b]
  check_bcapture.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def load(path):
    from PIL import Image

    return Image.open(path).convert("RGB")


def diff_stats(pa, pb, pixdelta):
    """(fraction of pixels differing by > pixdelta, max channel delta, bbox of those pixels or None)."""
    import numpy as np

    a = np.asarray(pa, dtype=np.int16)
    b = np.asarray(pb, dtype=np.int16)
    d = np.abs(a - b).max(axis=2)
    bad = d > pixdelta
    frac = float(bad.mean())
    if not bad.any():
        return frac, int(d.max()), None
    ys, xs = np.nonzero(bad)
    return frac, int(d.max()), (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def worst_tile(pa, pb, pixdelta, tile=32):
    """(worst fraction of differing pixels in any tile x tile block, its (x, y))."""
    import numpy as np

    d = (
        np.abs(np.asarray(pa, dtype=np.int16) - np.asarray(pb, dtype=np.int16)).max(axis=2)
        > pixdelta
    )
    h, w = d.shape
    hh, ww = h // tile * tile, w // tile * tile
    if hh == 0 or ww == 0:
        return 0.0, (0, 0)
    t = d[:hh, :ww].reshape(hh // tile, tile, ww // tile, tile).mean(axis=(1, 3))
    iy, ix = np.unravel_index(int(t.argmax()), t.shape)
    return float(t.max()), (int(ix) * tile, int(iy) * tile)


def presenter_filter(run_dir):
    """(backend, filter) from the `[gfx] backend=... filter=...` line of mh_video.log; ('?', '?') when absent."""
    log = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(log):
        return "?", "?"
    t = open(log, encoding="utf-8", errors="replace").read()
    m = re.search(r"; \[gfx\] backend=(\w+) ARMED.*?filter=(\w+)", t)
    return (m.group(1), m.group(2)) if m else ("?", "?")


def tolerance(run_dir, override):
    if override is not None:
        return override
    backend, filt = presenter_filter(run_dir)
    if backend == "d3d11":
        return 0.01 if filt == "point" else 0.03
    return 0.002


def skipped_names(run_dir):
    log = os.path.join(run_dir, "mh_capture.log")
    if not os.path.isfile(log):
        return []
    return re.findall(
        r"; bcapture (\S+) SKIPPED", open(log, encoding="utf-8", errors="replace").read()
    )


def check(run_dir, tol=None, pixdelta=24, require_pair=False, names=None):
    """Returns (problems, report lines)."""
    probs, rep = [], []
    t = tolerance(run_dir, tol)
    pairs = 0
    for fn in sorted(os.listdir(run_dir)):
        m = re.fullmatch(r"capture_(.+)__b\.bmp", fn)
        if not m:
            continue
        n = m.group(1)
        if names and n not in names:
            continue
        pcap = os.path.join(run_dir, "capture_%s.bmp" % n)
        if not os.path.isfile(pcap):
            rep.append("%s: no pcapture twin (capture_%s.bmp) -- not compared" % (n, n))
            continue
        a, b = load(pcap), load(os.path.join(run_dir, fn))
        pairs += 1
        if a.size != b.size:
            probs.append("%s: pcapture %s != bcapture %s" % (n, a.size, b.size))
            continue
        frac, mx, bbox = diff_stats(a, b, pixdelta)
        rep.append(
            "%s: %dx%d, %.4f%% of pixels differ by > %d (max channel delta %d%s), tolerance %.2f%%"
            % (
                n,
                a.size[0],
                a.size[1],
                frac * 100,
                pixdelta,
                mx,
                "" if bbox is None else ", bbox %r" % (bbox,),
                t * 100,
            )
        )
        if frac > t:
            probs.append(
                "%s: %.3f%% of pixels differ (> %.2f%% tolerance), bbox %r"
                % (n, frac * 100, t * 100, bbox)
            )
        wt, at = worst_tile(a, b, pixdelta)
        rep.append("%s: worst 32x32 tile %.0f%% different at %r" % (n, wt * 100, at))
        if wt > 0.5:
            probs.append(
                "%s: a 32x32 tile at %r is %.0f%% different (a block, not edge aliasing)"
                % (n, at, wt * 100)
            )
    for n in skipped_names(run_dir):
        rep.append("%s: bcapture SKIPPED (the null presenter shows nothing)" % n)
    if require_pair and pairs == 0:
        probs.append(
            "no capture_<n>.bmp / capture_<n>__b.bmp pair to compare (skipped: %r)"
            % (skipped_names(run_dir),)
        )
    return probs, rep


def selftest():
    import tempfile

    from PIL import Image

    fails = []
    with tempfile.TemporaryDirectory() as td:
        img = Image.new("RGB", (64, 48), (10, 120, 30))
        img.save(os.path.join(td, "capture_a.bmp"))
        img.save(os.path.join(td, "capture_a__b.bmp"))
        if check(td)[0]:
            fails.append("identical pair must be green")
        bad = img.copy()
        for x in range(64):
            for y in range(24):
                bad.putpixel((x, y), (200, 0, 0))
        bad.save(os.path.join(td, "capture_a__b.bmp"))
        if not check(td)[0]:
            fails.append("a half-different pair must be RED")
        blk = img.copy()
        for x in range(32, 64):
            for y in range(0, 32):
                blk.putpixel((x, y), (200, 0, 0))
        blk.save(os.path.join(td, "capture_a__b.bmp"))
        if not check(td, tol=1.0)[0]:
            fails.append("a differing 32x32 block must be RED even inside the fraction tolerance")
        Image.new("RGB", (32, 48)).save(os.path.join(td, "capture_a__b.bmp"))
        if not check(td)[0]:
            fails.append("a size mismatch must be RED")
    with tempfile.TemporaryDirectory() as td:
        if not check(td, require_pair=True)[0]:
            fails.append("--require-pair with nothing to compare must be RED")
        if check(td)[0]:
            fails.append("no pair and no --require-pair is not an error (null presenter)")
    for f in fails:
        print("SELFTEST FAIL:", f)
    print("check_bcapture selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    tol, pixdelta, req, names, run = None, 24, False, None, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--tol":
            tol = float(argv[i + 1])
            i += 1
        elif a == "--pixdelta":
            pixdelta = int(argv[i + 1])
            i += 1
        elif a == "--require-pair":
            req = True
        elif a == "--names":
            names = set(argv[i + 1].split(","))
            i += 1
        else:
            run = a
        i += 1
    if not run:
        print(__doc__)
        return 2
    probs, rep = check(run, tol, pixdelta, req, names)
    for r in rep:
        print("  bcapture: " + r)
    for p in probs:
        print("RED: " + p)
    if not probs:
        print("check_bcapture OK: the presenter output agrees with the CPU re-composition")
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
