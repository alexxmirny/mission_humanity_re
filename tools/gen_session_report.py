#!/usr/bin/env python3
# tools/gen_session_report.py -- generate a self-contained HTML session report with inline-SVG perf
# plots from MP run logs. Reads a played run (host+vm frametime/lockstep) + the latency-test runs
# and writes the HTML file named on the command line. No external deps (pure stdlib; SVG inline).
#
# Usage: python tools/gen_session_report.py <out.html>    (no argument -> tmp/session_report.html)
#   Run-folder paths are configured in RUNS below (edit to point at the runs to report).

import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --- run sources (the current run) ---
# Historical run artefacts under the RU install root; edit RUNS to point at the runs to report.
PLAYED = f"{machine.RU_POLYGON}/replays/endgame-desync-2026-07-11"  # host/ + vm/ (300ms, full game)
LAT = {  # lockstep.log per lookahead setting
    100: f"{machine.RU_POLYGON}/logs/20260711_041419_host/mh_lockstep.log",
    200: f"{machine.RU_POLYGON}/logs/20260711_041624_host/mh_lockstep.log",
    300: PLAYED + "/host/mh_lockstep.log",
}


# ---------------------------------------------------------------- log parsing
def frametime_series(path):
    """(t_s, delta_ms) per frame, from qpc_us log."""
    us = [int(l.split()[0]) for l in open(path, errors="replace") if l[:1].isdigit()]
    if len(us) < 2:
        return []
    t0 = us[0]
    return [
        ((us[i] - t0) / 1e6, (us[i] - us[i - 1]) / 1000.0)
        for i in range(1, len(us))
        if us[i] >= us[i - 1]
    ]


def lockstep_rows(path):
    return [[int(x) for x in l.split()] for l in open(path, errors="replace") if l[:1].isdigit()]


def bucketize(series, nbuck=340, agg="max"):
    """series=[(x,y)] -> [(x_mid, agg_y)] over nbuck equal-x buckets."""
    if not series:
        return []
    xs = [p[0] for p in series]
    lo, hi = xs[0], xs[-1]
    span = max(1e-9, hi - lo)
    buck = [[] for _ in range(nbuck)]
    for x, y in series:
        i = min(nbuck - 1, int((x - lo) / span * nbuck))
        buck[i].append(y)
    out = []
    for i, b in enumerate(buck):
        if b:
            xm = lo + (i + 0.5) / nbuck * span
            out.append((xm, max(b) if agg == "max" else sum(b) / len(b)))
    return out


def freeze_stats(path, thr=150):
    rows = lockstep_rows(path)
    if len(rows) < 3:
        return None
    walls = [r[0] for r in rows]
    gaps = [b - a for a, b in zip(walls, walls[1:])]
    fr = [g for g in gaps if g >= thr]
    total = walls[-1] - walls[0]
    since = sorted(r[12] for r in rows)
    return dict(
        span=total / 1000.0,
        frozen=sum(fr) / 1000.0,
        nfreeze=len(fr),
        pct=100 * sum(fr) / max(1, total),
        since_p95=since[int(len(since) * 0.95)],
        step=rows[-1][7],
    )


# ---------------------------------------------------------------- SVG helpers
def svg_lines(
    series_list,
    w=760,
    h=220,
    pad=42,
    ymax=None,
    ylabel="",
    xlabel="",
    colors=None,
    legend=None,
    yticks=None,
    ylog=False,
):
    """series_list = [[(x,y),...], ...]. Returns an <svg> string (theme-aware via currentColor/vars)."""
    colors = colors or ["var(--accent)", "var(--warn)", "var(--bad)"]
    allx = [x for s in series_list for x, _ in s] or [0, 1]
    ally = [y for s in series_list for _, y in s] or [0, 1]
    x0, x1 = min(allx), max(allx)
    y0 = 0
    y1 = ymax if ymax else (max(ally) * 1.1 or 1)
    xspan = max(1e-9, x1 - x0)
    yspan = max(1e-9, y1 - y0)

    def px(x):
        return pad + (x - x0) / xspan * (w - pad - 8)

    def py(y):
        return h - pad - (y - y0) / yspan * (h - pad - 12)

    parts = ['<svg viewBox="0 0 %d %d" class="chart" role="img">' % (w, h)]
    # grid + y ticks
    ticks = yticks or [y1 * f for f in (0, 0.25, 0.5, 0.75, 1)]
    for t in ticks:
        yy = py(t)
        parts.append(
            '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" class="grid"/>' % (pad, yy, w - 8, yy)
        )
        parts.append(
            '<text x="%d" y="%.1f" class="axl" text-anchor="end">%s</text>'
            % (pad - 6, yy + 3, _fmt(t))
        )
    # x ticks (5)
    for f in (0, 0.25, 0.5, 0.75, 1):
        xv = x0 + f * xspan
        parts.append(
            '<text x="%.1f" y="%d" class="axl" text-anchor="middle">%ds</text>'
            % (px(xv), h - pad + 16, xv)
        )
    # series
    for i, s in enumerate(series_list):
        if not s:
            continue
        pts = " ".join("%.1f,%.1f" % (px(x), py(y)) for x, y in s)
        parts.append(
            '<polyline points="%s" fill="none" stroke="%s" stroke-width="1.5" opacity="0.9"/>'
            % (pts, colors[i % len(colors)])
        )
    if ylabel:
        parts.append(
            '<text x="12" y="%d" class="axtitle" transform="rotate(-90 12 %d)">%s</text>'
            % (h // 2, h // 2, ylabel)
        )
    # legend
    if legend:
        lx = pad + 8
        for i, lab in enumerate(legend):
            parts.append(
                '<rect x="%d" y="10" width="11" height="11" rx="2" fill="%s"/>'
                % (lx, colors[i % len(colors)])
            )
            parts.append('<text x="%d" y="20" class="leg">%s</text>' % (lx + 16, lab))
            lx += 22 + len(lab) * 7
    parts.append('</svg>')
    return "".join(parts)


def svg_bars(labels, values, w=760, h=210, pad=42, unit="%", colors=None, ymax=None):
    colors = colors or ["var(--bad)"] * len(labels)
    y1 = ymax or (max(values) * 1.15 or 1)
    bw = (w - pad - 20) / len(labels)

    def py(y):
        return h - pad - y / y1 * (h - pad - 20)

    parts = ['<svg viewBox="0 0 %d %d" class="chart" role="img">' % (w, h)]
    for t in (0, y1 * 0.25, y1 * 0.5, y1 * 0.75, y1):
        yy = py(t)
        parts.append(
            '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" class="grid"/>' % (pad, yy, w - 8, yy)
        )
        parts.append(
            '<text x="%d" y="%.1f" class="axl" text-anchor="end">%s</text>'
            % (pad - 6, yy + 3, _fmt(t))
        )
    for i, (lab, v) in enumerate(zip(labels, values)):
        x = pad + 10 + i * bw
        yy = py(v)
        parts.append(
            '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="3" fill="%s"/>'
            % (x, yy, bw * 0.6, h - pad - yy, colors[i])
        )
        parts.append(
            '<text x="%.1f" y="%.1f" class="barval" text-anchor="middle">%s%s</text>'
            % (x + bw * 0.3, yy - 6, _fmt(v), unit)
        )
        parts.append(
            '<text x="%.1f" y="%d" class="axl" text-anchor="middle">%s</text>'
            % (x + bw * 0.3, h - pad + 16, lab)
        )
    parts.append('</svg>')
    return "".join(parts)


def _fmt(v):
    return "%d" % round(v) if abs(v - round(v)) < 0.05 else "%.1f" % v


# ---------------------------------------------------------------- build
def main():
    # THE DEFAULT IS A SCRATCH PATH, NOT A DATED ONE (fork F5M S4b). It used to fall back to one
    # historical report filename under the committed report tree -- a path that was never the right
    # answer for any run but the one it was typed for, and that pinned this generator to a directory
    # the public cut does not carry. The report is an artifact of the run you just did, so an
    # unnamed run writes to scratch and says where; a run worth keeping gets an explicit argument.
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "tmp", "session_report.html")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if len(sys.argv) <= 1:
        print("no output path given -- writing %s" % out)

    # frame-time timelines (played run)
    hf = bucketize(frametime_series(PLAYED + "/host/mh_frametime.log"), agg="max")
    vf = bucketize(frametime_series(PLAYED + "/vm/mh_frametime.log"), agg="max")
    hfm = bucketize(frametime_series(PLAYED + "/host/mh_frametime.log"), agg="mean")
    vfm = bucketize(frametime_series(PLAYED + "/vm/mh_frametime.log"), agg="mean")
    ft_svg = svg_lines(
        [hfm, vfm],
        ymax=80,
        ylabel="present time (ms)",
        colors=["var(--accent)", "var(--warn)"],
        legend=["host (~57 fps)", "VM (~32 fps)"],
        yticks=[0, 17, 33, 50, 80],
    )

    # since_rx timeline: played (300ms, healthy) vs the 200ms freeze-storm
    def since_series(path):
        rows = lockstep_rows(path)
        if not rows:
            return []
        w0 = rows[0][0]
        return bucketize([((r[0] - w0) / 1000.0, r[12]) for r in rows], agg="max")

    sr_played = since_series(PLAYED + "/host/mh_lockstep.log")
    sr_200 = since_series(LAT[200])
    sr_svg = svg_lines(
        [sr_played, sr_200],
        ymax=2200,
        ylabel="since last peer rx (ms)",
        colors=["var(--ok)", "var(--bad)"],
        legend=["300ms lookahead (played game)", "200ms lookahead (test)"],
        yticks=[0, 500, 1000, 1500, 2000],
    )

    # latency cliff
    fs = {k: freeze_stats(v) for k, v in LAT.items()}
    cliff_svg = svg_bars(
        ["100 ms\n1 tick", "200 ms\n2 ticks", "300 ms\n3 ticks"],
        [fs[100]["pct"], fs[200]["pct"], fs[300]["pct"]],
        unit="%",
        colors=["var(--bad)", "var(--bad)", "var(--ok)"],
        ymax=100,
    )

    repl = {
        "%%FT_SVG%%": ft_svg,
        "%%SR_SVG%%": sr_svg,
        "%%CLIFF_SVG%%": cliff_svg,
        "%%F100%%": _fmt(fs[100]["pct"]),
        "%%F200%%": _fmt(fs[200]["pct"]),
        "%%F300%%": _fmt(fs[300]["pct"]),
        "%%S100%%": str(fs[100]["since_p95"]),
        "%%S200%%": str(fs[200]["since_p95"]),
        "%%S300%%": str(fs[300]["since_p95"]),
    }
    HTML = TEMPLATE
    for k, v in repl.items():
        HTML = HTML.replace(k, v)
    with open(out, "w", encoding="utf-8") as f:
        f.write(HTML)
    print("wrote", out, "(%d KB)" % (len(HTML) // 1024))


# ---------------------------------------------------------------- HTML template (see bottom)
TEMPLATE = (
    open(os.path.join(REPO, "tools", "session_report_template.html"), encoding="utf-8").read()
    if os.path.exists(os.path.join(REPO, "tools", "session_report_template.html"))
    else None
)

if __name__ == "__main__":
    if TEMPLATE is None:
        print("ERROR: tools/session_report_template.html missing")
        sys.exit(2)
    main()
