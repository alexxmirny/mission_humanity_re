#!/usr/bin/env python3
r"""
crash_report.py -- find and attribute Windows crash reports for a rig peer.

WHY THIS EXISTS. The rig could not tell a crash from a hang. When the game's sim thread dies the
process usually does NOT: the render loop keeps presenting, so `mh_frametime.log` goes on growing
and the harness -- which only ever watched the STEP COUNT -- reported

    [host] harness STALLED (at 1 steps for 120s)

That is the same words it uses for a genuine livelock, and it cost an hour on 2026-08-01 (AI1A):
the batch-A shadow arming crashed in llm_strat_unit_passive_engage_tick and
every symptom pointed at an infinite loop. Windows had written the answer to disk the whole time.

WHAT MAKES PER-LANE ATTRIBUTION EXACT. A WER report carries `AppPath` -- the FULL path of the
faulting binary -- so a crash in F:\games\mh_lanes\ui_sp_det\mh.focus.exe is distinguishable from one
in any other lane, and from another concurrent run's peer, without guessing by image name or by time
alone. That matters here specifically: this rig runs several lanes at once, they all run an exe
called `mh.focus.exe`, and killing or blaming by image name is a cross-test failure this repo already
refuses to do elsewhere (see local_kill in ui_test.py).

WHAT IT READS
  * %ProgramData%\Microsoft\Windows\WER\Report{Archive,Queue}\*\Report.wer  -- the report itself
  * %LOCALAPPDATA%\Microsoft\Windows\WER\Report{Archive,Queue}\*\Report.wer
  * %LOCALAPPDATA%\CrashDumps\*.dmp                                        -- if dump collection is on
Report.wer is a UTF-16LE `key=value` file. The fields that matter:
    AppPath        full path of the crashed binary   -> which LANE
    EventTime      FILETIME (100ns ticks since 1601) -> whether it belongs to THIS run
    Sig[0].Value   application name
    Sig[3].Value   FAULTING MODULE  (mh.focus.exe, or a DLL)
    Sig[6].Value   exception code   (c0000005 = access violation, ...)
    Sig[7].Value   exception OFFSET within the faulting module

TURNING THAT INTO A LOCATION. When the faulting module is the game exe, VA = offset + 0x00400000
(EN build, no ASLR -- the same premise the whole DLL rests on). The VA is then resolved against
docs/symbols.md, so the report names a FUNCTION rather than a hex number.

Usage:
  python tools/crash_report.py                       # everything from the last 30 minutes
  python tools/crash_report.py --since-min 5
  python tools/crash_report.py --lane F:\\games\\mh_lanes\\ui_sp_det
  python tools/crash_report.py --json
"""

import argparse
import bisect
import calendar
import glob
import json
import os
import re
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS = os.path.join(REPO, "docs", "symbols.md")
IMAGE_BASE = 0x00400000  # EN mh.exe, no ASLR

# The NTSTATUS values this rig actually produces. An unknown code still reports its hex -- the point
# of the table is that the common ones do not need a web search mid-debug.
EXCEPTION_NAMES = {
    "c0000005": "ACCESS_VIOLATION",
    "c000001d": "ILLEGAL_INSTRUCTION",
    "c0000025": "NONCONTINUABLE_EXCEPTION",
    "c000008c": "ARRAY_BOUNDS_EXCEEDED",
    "c0000090": "FLT_INVALID_OPERATION",
    "c0000094": "INTEGER_DIVIDE_BY_ZERO",
    "c0000095": "INTEGER_OVERFLOW",
    "c00000fd": "STACK_OVERFLOW",
    "c0000374": "HEAP_CORRUPTION",
    "c000041d": "FATAL_USER_CALLBACK_EXCEPTION",
    "c0000409": "STACK_BUFFER_OVERRUN",
    "80000003": "BREAKPOINT",
    "e06d7363": "CPP_EXCEPTION",
}


def wer_dirs():
    """Every WER report root that could hold a report for a process we launched.

    Both the per-machine and the per-user trees, and both Queue (not yet reported) and Archive
    (reported). Which one a crash lands in depends on the machine's WER consent settings, so
    scanning one of them is a silent coverage gap rather than a simplification.
    """
    roots = []
    for base in (os.environ.get("ProgramData"), os.environ.get("LOCALAPPDATA")):
        if not base:
            continue
        for sub in ("ReportArchive", "ReportQueue"):
            p = os.path.join(base, "Microsoft", "Windows", "WER", sub)
            if os.path.isdir(p):
                roots.append(p)
    return roots


def filetime_to_epoch(ft):
    """FILETIME (100 ns ticks since 1601-01-01 UTC) -> unix epoch seconds."""
    try:
        return int(ft) / 1e7 - 11644473600
    except (TypeError, ValueError):
        return None


def parse_report(path):
    """Parse one Report.wer into a flat dict. Returns None if it is not readable as one."""
    for enc in ("utf-16", "utf-8-sig", "utf-8"):
        try:
            with open(path, encoding=enc) as f:
                txt = f.read()
            if "EventType=" in txt or "Sig[0].Name=" in txt:
                break
        except (OSError, UnicodeError):
            continue
    else:
        return None
    d = {}
    for line in txt.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            d[k.strip()] = v.strip()
    return d or None


def _symbol_index():
    """(sorted function VAs, names) from the GENERATED symbol table.

    Deliberately docs/symbols.md and not a hand list: it is regenerated from Ghidra every session
    that renames anything, so a crash report cannot drift into naming a function by its old name.
    """
    if not os.path.isfile(SYMBOLS):
        return [], []
    addrs, names = [], []
    for m in re.finditer(
        r"^\|\s*`0x([0-9a-fA-F]{8})`\s*\|\s*`([^`]+)`",
        open(SYMBOLS, encoding="utf-8", errors="replace").read(),
        re.M,
    ):
        addrs.append(int(m.group(1), 16))
        names.append(m.group(2))
    order = sorted(range(len(addrs)), key=lambda i: addrs[i])
    return [addrs[i] for i in order], [names[i] for i in order]


_SYMS = None


def resolve_va(va):
    """`name+0xNN` for the function containing `va`, or None.

    Nearest-preceding-symbol, which is an ATTRIBUTION not a proof: a VA past the last function in a
    region will still bind to that function. The offset is printed so an implausible one is visible.
    """
    global _SYMS
    if _SYMS is None:
        _SYMS = _symbol_index()
    addrs, names = _SYMS
    if not addrs:
        return None
    i = bisect.bisect_right(addrs, va) - 1
    if i < 0:
        return None
    delta = va - addrs[i]
    if delta > 0x4000:  # further than any function in this binary is long
        return None
    return "%s+0x%x" % (names[i], delta) if delta else names[i]


def _under(path, root):
    """Is `path` the file `root`, or a file inside the directory `root`?

    A plain startswith is WRONG and the selftest catches it: lane `...\\ui_sp_det` would then also
    claim every crash in `...\\ui_sp_det2`, silently attributing one lane's crash to its neighbour --
    the precise confusion per-lane attribution exists to prevent. Match on a path BOUNDARY.
    """
    p = os.path.normcase(os.path.abspath(path))
    r = os.path.normcase(os.path.abspath(root)).rstrip("\\/")
    return p == r or p.startswith(r + os.sep) or p.startswith(r + "/")


def scan(since=None, app_path=None, exe=None, roots=None):
    """Crash reports matching the filters, newest first.

    since     -- unix epoch; keep reports at or after it. None = no time filter.
    app_path  -- a lane DIRECTORY or a full exe path; matched case-insensitively against AppPath.
                 This is the per-lane attribution and it is why concurrent lanes do not confuse it.
    exe       -- image name filter (e.g. "mh.focus.exe") for when app_path is unknown.
    roots     -- WER report roots to walk. Defaults to this machine's. A caller that has PULLED
                 reports off another box (a rig VM peer -- ui_test.remote_crash_lines) passes the
                 staging directory here, so the same parser, the same AppPath attribution and the
                 same VA->symbol resolution serve local and remote peers. Duplicating any of that
                 for the remote case would mean two things to keep in step, and the half nobody
                 runs is the half that rots.
    """
    out = []
    for root in roots if roots is not None else wer_dirs():
        for rep in glob.glob(os.path.join(root, "*", "Report.wer")):
            try:
                mtime = os.path.getmtime(rep)
            except OSError:
                continue
            d = parse_report(rep)
            if not d:
                continue
            ap = d.get("AppPath", "")
            when = filetime_to_epoch(d.get("EventTime")) or mtime
            if since is not None and when < since:
                continue
            if app_path and not _under(ap, app_path):
                continue
            if exe and os.path.basename(ap).lower() != exe.lower():
                continue
            code = (d.get("Sig[6].Value") or "").lower()
            off = d.get("Sig[7].Value") or ""
            module = d.get("Sig[3].Value") or ""
            va = None
            if module.lower() in ("mh.focus.exe", "mh.exe"):
                try:
                    va = int(off, 16) + IMAGE_BASE
                except ValueError:
                    va = None
            out.append(
                {
                    "report": rep,
                    "when": when,
                    "app_path": ap,
                    "app": d.get("Sig[0].Value") or os.path.basename(ap),
                    "module": module,
                    "code": code,
                    "code_name": EXCEPTION_NAMES.get(code, "?"),
                    "offset": off,
                    "va": va,
                    "symbol": resolve_va(va) if va else None,
                }
            )
    out.sort(key=lambda r: r["when"], reverse=True)
    return out


EVENT_FIELD_RE = re.compile(r"^\s*(Faulting [\w -]+|Exception code|Fault offset)\s*:\s*(.*)$")
EVENT_DATE_RE = re.compile(r"^\s*Date:\s*([0-9T:\-]+)")


def parse_appcrash_events(text, since=None, app_path=None, exe=None):
    r"""Application-log Event 1000 records, in the same shape scan() returns.

    THE SECOND EVIDENCE SOURCE, and on this rig it is the ONLY one that works where it matters.
    Measured 2026-08-27: the rig VMs have HKLM\..\Windows Error Reporting\Disabled = 1, so no
    Report.wer is ever written there -- a WER-file detector aimed at a VM peer would have been one
    that can never fire, which is precisely the "we checked and there was no crash" false negative
    this module's docstring exists to refuse. The Application log's "Application Error" event is
    written anyway, and it carries the same three things the .wer does: faulting module, fault
    offset, exception code. U21's own evidence came from exactly here.

    Text is `wevtutil qe Application /f:text` output. The fields:
        Faulting application name: mh.focus.exe, version: ...
        Faulting module name:      mh.focus.exe, version: ...
        Exception code:            0xc0000005
        Fault offset:              0x0001f3a3
        Faulting application path: C:\games\mh\mh.focus.exe
    """
    out, cur, when = [], {}, None

    def flush():
        if not cur.get("Faulting application path") and not cur.get("Faulting application name"):
            return
        ap = cur.get("Faulting application path", "")
        if app_path and not _under(ap, app_path):
            return
        if exe and os.path.basename(ap).lower() != exe.lower():
            return
        if since is not None and when is not None and when < since:
            return
        module = (cur.get("Faulting module name", "") or "").split(",")[0].strip()
        code = (cur.get("Exception code", "") or "").strip().lower().replace("0x", "")
        off = (cur.get("Fault offset", "") or "").strip().lower().replace("0x", "")
        va = None
        if module.lower() in ("mh.focus.exe", "mh.exe"):
            try:
                va = int(off, 16) + IMAGE_BASE
            except ValueError:
                va = None
        out.append(
            {
                "report": "Application event log (Event 1000)",
                "when": when,
                "app_path": ap,
                "app": (cur.get("Faulting application name", "") or "").split(",")[0].strip()
                or os.path.basename(ap),
                "module": module,
                "code": code,
                "code_name": EXCEPTION_NAMES.get(code, "?"),
                "offset": off,
                "va": va,
                "symbol": resolve_va(va) if va else None,
            }
        )

    for line in (text or "").splitlines():
        if line.startswith("Event["):
            flush()
            cur, when = {}, None
            continue
        m = EVENT_DATE_RE.match(line)
        if m and not when:
            try:
                when = calendar.timegm(time.strptime(m.group(1)[:19], "%Y-%m-%dT%H:%M:%S"))
            except ValueError:
                when = None
            continue
        m = EVENT_FIELD_RE.match(line)
        if m:
            cur.setdefault(m.group(1).strip(), m.group(2).strip())
    flush()
    out.sort(key=lambda r: r["when"] or 0, reverse=True)
    return out


def dumps(since=None):
    """Any .dmp files a configured LocalDumps collector wrote. Empty is normal -- dump collection is
    off by default on Windows, and its absence is NOT evidence that nothing crashed."""
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        return []
    out = []
    for p in glob.glob(os.path.join(base, "CrashDumps", "*.dmp")):
        try:
            m = os.path.getmtime(p)
        except OSError:
            continue
        if since is None or m >= since:
            out.append({"path": p, "when": m, "size": os.path.getsize(p)})
    return sorted(out, key=lambda r: r["when"], reverse=True)


def describe(rec):
    """One line, and it leads with the thing you act on: where it faulted."""
    where = rec["symbol"] or (
        ("0x%08x" % rec["va"]) if rec["va"] else "%s+%s" % (rec["module"], rec["offset"])
    )
    return "%s crashed in %s -- %s (%s) at %s" % (
        rec["app"],
        where,
        rec["code_name"],
        rec["code"],
        time.strftime("%H:%M:%S", time.localtime(rec["when"])),
    )


def report_for(app_path, since, limit=3, roots=None):
    """The lines a caller (ui_test) should print, or [] if nothing matched.

    Returns the report PATH too: the .wer holds the loaded-module list and the full signature set,
    which is the next thing anyone wants after the one-liner.
    """
    recs = scan(since=since, app_path=app_path, roots=roots)[:limit]
    lines = []
    for r in recs:
        lines.append("  [crash] " + describe(r))
        lines.append("  [crash] report: %s" % r["report"])
    return lines


SELFTEST_REPORT = """Version=1
EventType=APPCRASH
EventTime=134300537436040744
Sig[0].Name=Application Name
Sig[0].Value=mh.focus.exe
Sig[3].Name=Fault Module Name
Sig[3].Value=mh.focus.exe
Sig[6].Name=Exception Code
Sig[6].Value=c0000005
Sig[7].Name=Exception Offset
Sig[7].Value=000ee741
AppPath={APPPATH}
"""


# A real `wevtutil qe Application /f:text` pair: one game crash to find, one foreign crash to reject.
# The offset is U21's own (0x0001f3a3 -> 0x0041f3a3, llm_strat_pathfind_trace_route), so the parser is
# tested against the exact record this detector was built to catch.
SELFTEST_EVENTS = r"""Event[0]
  Log Name: Application
  Source: Application Error
  Date: 2026-07-25T18:23:11.4740000Z
  Event ID: 1000
  Description:
Faulting application name: mh.focus.exe, version: 0.0.0.0, time stamp: 0x3b0f2c1a
Faulting module name: mh.focus.exe, version: 0.0.0.0, time stamp: 0x3b0f2c1a
Exception code: 0xc0000005
Fault offset: 0x0001f3a3
Faulting process id: 0x1a2c
Faulting application path: C:\games\mh\mh.focus.exe
Report Id: 0d2f1e00-0000-0000-0000-000000000000

Event[1]
  Log Name: Application
  Source: Application Error
  Date: 2026-07-25T18:20:02.1000000Z
  Event ID: 1000
  Description:
Faulting application name: notepad.exe, version: 11.0.0.0, time stamp: 0x00000000
Faulting module name: KERNELBASE.dll, version: 10.0.28000.2525, time stamp: 0xc6477a45
Exception code: 0xc0000409
Fault offset: 0x001829bd
Faulting process id: 0x3630
Faulting application path: C:\Windows\System32\notepad.exe
Report Id: 09d416cc-0000-0000-0000-000000000000
"""


def selftest():
    """Prove the scanner finds a crash, ATTRIBUTES it to the right lane, and can say no.

    A diagnostic that silently matches nothing is worse than no diagnostic: it turns "we checked and
    there was no crash" into a false negative at exactly the moment someone is deciding whether a
    stall is a hang. So the negative cases carry as much weight as the positive one, and the lane
    filter is checked against a SIBLING lane whose path shares a prefix -- the case a naive
    startswith() gets wrong.
    """
    import shutil
    import tempfile

    fails = []

    def ck(name, ok):
        print("  %-58s %s" % (name, "ok" if ok else "FAIL"))
        if not ok:
            fails.append(name)

    tmp = tempfile.mkdtemp(prefix="crashsel_")
    try:
        lane = os.path.join(tmp, "lanes", "ui_sp_det")
        sibling = os.path.join(tmp, "lanes", "ui_sp_det2")  # shares ui_sp_det as a PREFIX
        os.makedirs(lane)
        os.makedirs(sibling)
        root = os.path.join(tmp, "WER", "ReportArchive")
        for tag, d in (("a", lane), ("b", sibling)):
            rd = os.path.join(root, "AppCrash_mh.focus.exe_%s" % tag)
            os.makedirs(rd)
            with open(os.path.join(rd, "Report.wer"), "w", encoding="utf-16") as f:
                f.write(SELFTEST_REPORT.replace("{APPPATH}", os.path.join(d, "mh.focus.exe")))

        global wer_dirs
        real = wer_dirs
        wer_dirs = lambda: [root]  # noqa: E731
        try:
            everything = scan(since=None)
            ck("finds both synthetic reports", len(everything) == 2)

            mine = scan(since=None, app_path=lane)
            ck("lane filter selects exactly one", len(mine) == 1)
            ck(
                "lane filter does NOT match the prefix-sharing sibling",
                len(mine) == 1
                and os.path.normcase(mine[0]["app_path"]).startswith(
                    os.path.normcase(lane) + os.sep
                ),
            )
            ck("sibling lane finds its own", len(scan(since=None, app_path=sibling)) == 1)

            r = mine[0] if mine else {}
            ck("exception code parsed", r.get("code") == "c0000005")
            ck("exception NAMED, not just hex", r.get("code_name") == "ACCESS_VIOLATION")
            ck("VA = offset + image base", r.get("va") == 0x000EE741 + IMAGE_BASE)
            ck("EventTime decoded to a sane epoch", 1.4e9 < (r.get("when") or 0) < 4e9)

            when = r.get("when") or 0
            ck("since AFTER the event excludes it", scan(since=when + 60, app_path=lane) == [])
            ck("since BEFORE the event includes it", len(scan(since=when - 60, app_path=lane)) == 1)
            ck("exe filter matches", len(scan(since=None, app_path=lane, exe="mh.focus.exe")) == 1)
            ck(
                "exe filter rejects another image",
                scan(since=None, app_path=lane, exe="other.exe") == [],
            )
            ck("report_for() produces lines", len(report_for(lane, when - 60)) >= 2)
            # The PULLED-REPORTS path (ui_test.remote_crash_lines): same parser, explicit root,
            # and it must not fall back to this machine's WER tree when given one.
            ck(
                "explicit roots= finds a pulled report",
                len(scan(since=None, app_path=lane, roots=[root])) == 1,
            )
            ck(
                "explicit roots= that holds nothing finds nothing (no silent local fallback)",
                scan(since=None, app_path=lane, roots=[os.path.join(tmp, "empty")]) == [],
            )
            ck(
                "report_for(roots=) lines match the local ones",
                report_for(lane, when - 60, roots=[root]) == report_for(lane, when - 60),
            )
            ck(
                "report_for() is empty for a clean lane",
                report_for(os.path.join(tmp, "nope"), 0) == [],
            )
        finally:
            wer_dirs = real
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # The Application-log parser -- the source that works on a box with WER disabled, i.e. every
    # rig VM. Same record shape as scan(), so describe() and the ui_test callers do not branch.
    ev = parse_appcrash_events(SELFTEST_EVENTS)
    ck("event log: both crash events parsed", len(ev) == 2)
    if len(ev) == 2:
        g = [r for r in ev if r["app"] == "mh.focus.exe"]
        ck("event log: faulting app parsed", len(g) == 1)
        ck("event log: exception NAMED", g and g[0]["code_name"] == "ACCESS_VIOLATION")
        ck("event log: VA = offset + image base", g and g[0]["va"] == 0x0001F3A3 + IMAGE_BASE)
        ck("event log: names the FUNCTION, not a hex number", bool(g and g[0]["symbol"]))
        if g and g[0]["symbol"]:
            print("      0x0041f3a3 -> %s" % g[0]["symbol"])
        ck(
            "event log: app_path filter selects one",
            len(parse_appcrash_events(SELFTEST_EVENTS, app_path=r"C:\games\mh")) == 1,
        )
        ck(
            "event log: app_path filter rejects a prefix-sharing sibling",
            parse_appcrash_events(SELFTEST_EVENTS, app_path=r"C:\games\mh2") == [],
        )
        w = g[0]["when"] or 0
        ck(
            "event log: since AFTER excludes",
            parse_appcrash_events(SELFTEST_EVENTS, since=w + 60) == [],
        )
        ck(
            # -600, not -60: the two fixture events are ~3 min apart, so a 60 s window would
            # correctly exclude the older one and the check would be testing its own arithmetic.
            "event log: since BEFORE includes",
            len(parse_appcrash_events(SELFTEST_EVENTS, since=w - 600)) == 2,
        )
    ck("event log: empty input is empty output, not a crash", parse_appcrash_events("") == [])

    # The live half: the symbol resolver is only useful if it actually has symbols loaded.
    sym = resolve_va(0x004EE741)
    ck("resolves a real VA against docs/symbols.md", bool(sym))
    if sym:
        print("      0x004ee741 -> %s" % sym)
    print("crash_report selftest: %d check(s), %d failure(s)" % (16 + 10 + 1, len(fails)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument(
        "--since-min", type=float, default=30.0, help="look back this many minutes (0 = no limit)"
    )
    ap.add_argument("--lane", help="lane directory or exe path -- match AppPath against it")
    ap.add_argument("--exe", help="image name filter, e.g. mh.focus.exe")
    ap.add_argument(
        "--dir",
        action="append",
        help="scan this WER root instead of the local ones (repeatable). For a directory of "
        "reports pulled off a rig VM; ui_test does this automatically for VM peers.",
    )
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="prove the scanner + its filters work")
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    since = None if a.since_min <= 0 else time.time() - a.since_min * 60
    recs = scan(since=since, app_path=a.lane, exe=a.exe, roots=a.dir)
    dmp = [] if a.dir else dumps(since)  # local-only signal; says nothing about a pulled set
    if a.json:
        print(json.dumps({"crashes": recs, "dumps": dmp}, indent=1))
        return 0
    if not recs:
        # Say what was searched. "no crashes" over the wrong window or the wrong lane looks exactly
        # like "no crashes", which is the failure mode this whole file exists to remove.
        print(
            "no crash reports in %s%s%s"
            % (
                ("the last %g min" % a.since_min) if since else "any window",
                (" for %s" % a.lane) if a.lane else "",
                (" [%s]" % a.exe) if a.exe else "",
            )
        )
        print("searched: %s" % (", ".join(a.dir or wer_dirs()) or "(no WER directories found)"))
        # G186 (measured 2026-09-15): WerFault cannot run on a non-interactive desktop, and rig
        # lanes run on the isolated mh_rig desktop BY DEFAULT -- so for a local lane this negative
        # is NO INFORMATION, not "no fault". Only a --no-desktop run (or a postmortem debugger)
        # can produce the record whose absence this message reports.
        print(
            "NOTE: a lane on the isolated mh_rig desktop (the default) is INVISIBLE to WER -- "
            "this result cannot rule out a fault there: on a non-interactive desktop "
            "NOTHING postmortem runs -- not WER, not LocalDumps, not the AeDebug JIT debugger"
        )
    for r in recs:
        print(describe(r))
        print("   app  : %s" % r["app_path"])
        print(
            "   fault: module=%s offset=%s%s"
            % (r["module"], r["offset"], (" va=0x%08x" % r["va"]) if r["va"] else "")
        )
        print("   report: %s" % r["report"])
    for d in dmp:
        print(
            "dump: %s (%.1f MB, %s)"
            % (d["path"], d["size"] / 1e6, time.strftime("%H:%M:%S", time.localtime(d["when"])))
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
