"""check_selftest_roster.py -- fork F5I: the committed roster equals the exe's own suite table.

THE DRIFT THIS CLOSES. The gate's suite list existed three times and nothing compared the copies:
a hand-maintained tuple in tools/run_selftests.py, the dispatch table in
src/mh_dll/mh_nettest/net_selftest.cpp, and that dispatcher's hand-written unknown-mode message.
A suite added to the exe and not to the tuple simply never ran, and the gate stayed green while
doing less -- which is the one failure a gate cannot report about itself.

F5I made the table the single source and tools/data/selftest_roster.json the committed statement of
it. There are then two places the statement can be checked, and BOTH are gates because they catch
different things:

  * tools/run_selftests.py asserts the roster against the LIVE `--list-suites` output of the exe it
    is about to run. That is the stronger check -- it reads the binary -- but it needs a build.
  * THIS runs in lint_repo.py, which has no build, so it parses the SOURCE table instead. It fails
    on a table edit at the moment of the edit rather than at the next full gate run, and it is the
    arm that still works in CI on a machine with no toolchain.

Neither subsumes the other: source without a build cannot see a compile-time exclusion, and a live
listing cannot be taken on a machine that has not built. Keep both.

TWO EXES SINCE F5I S2, AND THAT IS WHAT THE PARTITION ASSERTIONS ARE FOR. Each executable has its
own SUITE_TABLE and the roster's `exe` column says which one answers to each suite. Three ways that
can go wrong, none of which a per-exe equality alone would catch, because each leaves both tables
internally consistent:

  * a suite gate-flagged in BOTH tables -- the gate then runs it in two arms, and whichever arm is
    wrong is hidden by the one that passes;
  * a suite gate-flagged in NEITHER -- it silently stops being part of the gate, which is the
    original F5I defect one level up;
  * a roster row naming an exe no source is registered for -- a rename that leaves the row pointing
    at nothing, which would otherwise read as "that exe has no suites" and pass.

So the check is a PARTITION: per exe, the gate-flagged set equals that exe's roster rows; the union
over exes equals the whole roster; every roster exe is in the source map; and |roster| == 32.

Usage:
  python tools/check_selftest_roster.py            # the gate
  python tools/check_selftest_roster.py --selftest # prove the comparison still fires
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROSTER = os.path.join(REPO, "tools", "data", "selftest_roster.json")
SOURCES = {
    # exe (the roster's `exe` column) -> the .cpp whose SUITE_TABLE defines it
    "net_selftest": os.path.join(REPO, "src", "mh_dll", "mh_nettest", "net_selftest.cpp"),
    "libmh_selftest": os.path.join(REPO, "src", "mh_dll", "libmh_test", "libmh_selftest.cpp"),
}
# F5I measured 32; +1 at SES0 (sessionidtest); +1 at mp:T0 (udpwiretest); +1 at mp:SES1
# (sessiondirtest); +1 at mp:T1 (udploopbacktest); +1 at mp:SES2 (logrottest); +1 at mp:U40
# (relinktest); +1 at mp:T1b (udprelinktest); +1 at mp:T2 (udpbulktest); +1 at mp:T3 (udpstatstest);
# +1 at mp:X1 (udpsnaptest); +1 at mp:R3 (udppunchtest); +1 at mp:X2 (maptest); +1 at mp:R6
# (udproomtest); +1 at mp:R3e (udprelaytest); +1 at mp:R7a (netcfgtest); +1 at TL-HARN4
# (inireadtest); +1 at dist:LA13 (runctxtest); +1 at mp:U41b (qmatchtest); +1 at mp:U19i (gpfgtest).
# A change here is a deliberate edit, not a drift
EXPECT_COUNT = 53  # tooling:TL-SUITE-COUNTERS, 2026-09-26: diagtest added (was 52)

# A table row: {"name", <gate>, <adapter>},  -- clang-format pads the name column, so the whitespace
# is free-form. Comment lines never match, because a `//` line has no leading `{"`.
RE_ROW = re.compile(r'^\s*\{"([A-Za-z0-9_\-]+)"\s*,\s*(true|false)\s*,', re.M)


def rel(path):
    """Repo-relative when possible. --selftest plants its copies under %TEMP%, which on this box is
    a different DRIVE, and ntpath.relpath raises rather than giving up on one."""
    try:
        return os.path.relpath(path, REPO)
    except ValueError:
        return path


def parse_table(path):
    """The gate-flagged names in one source's SUITE_TABLE, plus the row count for sanity."""
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r"SUITE_TABLE\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return None, None
    rows = RE_ROW.findall(m.group(1))
    return {n for n, g in rows if g == "true"}, [n for n, _ in rows]


def check(roster_path=ROSTER, sources=None):
    sources = sources or SOURCES
    with open(roster_path, encoding="utf-8") as fh:
        roster = json.load(fh)
    rows = roster["suites"]
    fails = []

    names = [r["suite"] for r in rows]
    if len(names) != len(set(names)):
        dup = sorted({n for n in names if names.count(n) > 1})
        fails.append("the roster lists a suite twice: %s" % ", ".join(dup))
    if len(rows) != EXPECT_COUNT:
        fails.append(
            "the roster has %d suite(s), expected %d -- if that is intended, change EXPECT_COUNT "
            "in this file in the same commit" % (len(rows), EXPECT_COUNT)
        )

    for r in rows:
        if r["exe"] not in sources:
            fails.append(
                "suite %s names exe %r, which no source is registered for" % (r["suite"], r["exe"])
            )

    # THE PARTITION, across the exes rather than within one. Collected first so the two cross-exe
    # failures can be named as themselves instead of as per-exe mismatches a reader has to
    # intersect by hand.
    gated_by_exe = {}
    for exe, path in sources.items():
        gated, _all_rows = parse_table(path)
        gated_by_exe[exe] = gated if gated is not None else set()

    names_seen = [e for e in sorted(sources)]
    both = set()
    for i, a in enumerate(names_seen):
        for b in names_seen[i + 1 :]:
            both |= gated_by_exe[a] & gated_by_exe[b]
    if both:
        fails.append(
            "gate-flagged in MORE THAN ONE exe: %s -- the gate would run it in two arms and a wrong "
            "arm would be hidden by the right one" % ", ".join(sorted(both))
        )

    union = set()
    for g in gated_by_exe.values():
        union |= g
    orphans = sorted({r["suite"] for r in rows} - union)
    if orphans:
        fails.append(
            "in the roster but gate-flagged in NO exe: %s -- the gate would skip it silently, which "
            "is the drift this file exists to catch" % ", ".join(orphans)
        )

    # THE SECOND COPY, UN-GATED (F5I S3). A row can also be present-but-not-gate-flagged, which is
    # legitimate for the transport/rig modes and for `seamtest` -- but NOT for a name the roster
    # assigns to the OTHER exe. That shape is exactly what S2 was (every spine suite still runnable
    # from net_selftest.exe with gate:false), and it is what S3 removed, because it makes every
    # stale `net_selftest.exe aitest` in a doc, a script or an agent brief keep working -- running a
    # second copy of the suite in the wrong arm and exiting 0 -- instead of exiting 2 with the mode
    # list. The partition checks above cannot see it: both tables stay internally consistent and the
    # gate-flagged sets stay disjoint. FATAL rather than a note: a consumer that must be retargeted
    # is only findable while the old name is broken.
    for exe, path in sources.items():
        gated, all_rows = parse_table(path)
        if gated is None or not all_rows:
            continue  # reported by the per-exe arm below
        ungated = set(all_rows) - gated
        theirs = {r["suite"] for r in rows if r["exe"] != exe}
        poach = sorted(ungated & theirs)
        if poach:
            fails.append(
                "%s carries un-gated row(s) for another exe's roster suite: %s -- delete the row so "
                "the name exits 2 here; leaving it runnable lets a stale consumer run the suite in "
                "the wrong arm and print green" % (exe, ", ".join(poach))
            )

    for exe, path in sources.items():
        want = {r["suite"] for r in rows if r["exe"] == exe}
        gated, all_rows = parse_table(path)
        if gated is None:
            fails.append("no SUITE_TABLE found in %s" % rel(path))
            continue
        if not all_rows:
            fails.append("SUITE_TABLE in %s parsed to zero rows" % rel(path))
            continue
        missing, extra = sorted(want - gated), sorted(gated - want)
        if missing:
            fails.append(
                "%s: in the roster, not gate-flagged in the table: %s" % (exe, ", ".join(missing))
            )
        if extra:
            fails.append(
                "%s: gate-flagged in the table, not in the roster: %s" % (exe, ", ".join(extra))
            )

    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print(
            "check_selftest_roster: %d problem(s). The TABLE is the source of truth -- edit\n"
            "  %s to match it, not the other way round." % (len(fails), rel(roster_path))
        )
        return 1
    print(
        "check_selftest_roster: ok -- %d gate suite(s) across %d exe(s) (%s), roster == table"
        % (
            len(rows),
            len(sources),
            ", ".join(
                "%s %d" % (e, len([r for r in rows if r["exe"] == e])) for e in sorted(sources)
            ),
        )
    )
    return 0


def selftest():
    """Plant each mismatch class and require a red. A comparison never shown to fail is not a gate."""
    with open(ROSTER, encoding="utf-8") as fh:
        good = json.load(fh)
    fails = 0
    tmpdir = tempfile.mkdtemp(prefix="f5i_roster_")
    try:
        planted = [0]

        def plant_table(rows):
            """A synthetic SUITE_TABLE source, written to tmpdir, returned as a path.

            The cross-exe arms below pair one of these with the REAL other table, which is the only
            way to plant a defect that is invisible to either table on its own."""
            body = "".join(
                '    {"%s", %s, adapt_void<run_x>},\n' % (n, "true" if g else "false")
                for n, g in rows
            )
            planted[0] += 1
            sp = os.path.join(tmpdir, "planted_%d.cpp" % planted[0])
            with open(sp, "w", newline="\n", encoding="utf-8") as fh:
                fh.write("static const suite_row SUITE_TABLE[] = {\n%s};\n" % body)
            return sp

        def red(label, roster_obj=None, source_text=None, sources=None):
            nonlocal fails
            rp = ROSTER
            srcs = dict(SOURCES)
            if roster_obj is not None:
                rp = os.path.join(tmpdir, "roster_%s.json" % label)
                with open(rp, "w", newline="\n", encoding="utf-8") as fh:
                    json.dump(roster_obj, fh, indent=2)
            if source_text is not None:
                # Only the net-side source is replaced; the other table stays real, so the arm
                # measures the planted defect rather than an empty second exe.
                sp = os.path.join(tmpdir, "src_%s.cpp" % label.replace(" ", "_"))
                with open(sp, "w", newline="\n", encoding="utf-8") as fh:
                    fh.write(source_text)
                srcs["net_selftest"] = sp
            if sources is not None:
                srcs = sources
            if check(rp, srcs) == 0:
                print("  [FAIL] %s did NOT go red" % label)
                fails += 1
            else:
                print("  ok: %s is caught" % label)

        # (a) a suite dropped from the roster -- the "added to the exe, never gated" class
        dropped = json.loads(json.dumps(good))
        dropped["suites"] = dropped["suites"][:-1]
        red("a suite missing from the roster", roster_obj=dropped)

        # (b) a suite in the roster that the table does not gate -- the "renamed/removed" class
        renamed = json.loads(json.dumps(good))
        renamed["suites"][0]["suite"] = "ghosttest"
        red("a roster suite the table does not carry", roster_obj=renamed)

        # (c) a roster suite pointing at an exe nobody builds
        wrong_exe = json.loads(json.dumps(good))
        wrong_exe["suites"][0]["exe"] = "some_other_exe"
        red("a roster suite naming an unknown exe", roster_obj=wrong_exe)

        # (d) the table itself losing its gate flag on a row. The anchor is `hostapitest`, which
        #     STAYS net-side: `aitest` was the anchor until F5I S2 moved it, and a row that is
        #     legitimately `false` in this table cannot demonstrate a LOST flag.
        with open(SOURCES["net_selftest"], encoding="utf-8") as fh:
            src = fh.read()
        ungated = re.sub(r'(\{"hostapitest"\s*,\s*)true', r"\1false", src, count=1)
        assert ungated != src, "the selftest's own anchor row moved -- fix this file"
        red("a table row losing its gate flag", source_text=ungated)

        # (e) a table that cannot be parsed at all reads as a failure, not as an empty match
        red("a source with no SUITE_TABLE", source_text="int main() { return 0; }\n")

        # ---- the two CROSS-EXE defects (F5I S2) ------------------------------------------------
        mine = [r["suite"] for r in good["suites"] if r["exe"] == "libmh_selftest"]
        theirs = [r["suite"] for r in good["suites"] if r["exe"] == "net_selftest"]
        assert mine and theirs, "both exes must own suites -- fix this file"

        # (f) a suite gate-flagged in BOTH: the second table claims one of the first's.
        both = dict(SOURCES)
        both["libmh_selftest"] = plant_table([(n, True) for n in mine + theirs[:1]])
        red("a suite gate-flagged in BOTH exes", sources=both)

        # (g) a suite gate-flagged in NEITHER: the second table drops one of its own.
        neither = dict(SOURCES)
        neither["libmh_selftest"] = plant_table([(n, n != mine[0]) for n in mine])
        red("a suite gate-flagged in NEITHER exe", sources=neither)

        # (h) F5I S3: the S2 shape coming back -- net_selftest keeps a RUNNABLE, un-gated row for a
        #     suite the roster assigns to libmh_selftest. The gate-flagged sets stay disjoint and
        #     their union still covers the roster, so (f) and (g) are both silent; only this arm
        #     sees it.
        poach = dict(SOURCES)
        poach["net_selftest"] = plant_table(
            [(n, True) for n in theirs] + [(mine[0], False)]  # ungated copy of the other exe's own
        )
        red("an un-gated row for the OTHER exe's suite", sources=poach)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("=== check_selftest_roster --selftest: %d failure(s) ===" % fails)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="the gate (default)")
    ap.add_argument("--selftest", action="store_true", help="prove the comparison can go red")
    args = ap.parse_args()
    if args.selftest:
        print("=== check_selftest_roster --selftest ===")
        return selftest()
    return check()


if __name__ == "__main__":
    sys.exit(main())
