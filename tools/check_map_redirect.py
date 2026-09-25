#!/usr/bin/env python3
"""check_map_redirect.py -- the post_check for mp:X2a's rig scenario (the two independent VMs).

Runs the SAME four assertions as check_map_transfer.py's `--expect-gate` (the claim, the gate, the
download, the client's own file unchanged), then adds the ONE line X2a exists to exercise for real:

    ; [map] client resolve stored mh_dl\\<stem>.<16 hex>.<ext> -- redirecting <base> to it

WHY THIS IS A SEPARATE ASSERTION FROM --expect-gate's. On a local lane (map_absent/map_conflict)
the client's `Maps\\<base>` IS the host's real content -- make_lane.py symlinks `Maps` to the one
shared game image -- so a broken open-redirect (map_transfer.cpp's `utils_open_file` replacement)
would still open MATCHING bytes and every other line in check_map_transfer.py would still read
green (its own docstring says so). X2a runs on the two independent rig VMs with the client's OWN
copy of the map genuinely mutated (tools/map_variant.py's one-bit flip, pushed by
tools/test_ui.py's run_x2a_map_variant), so THIS line -- the redirect actually being armed, naming
the `mh_dl\\` path it points at -- is the one that would go missing if the redirect were broken and
the game silently opened the client's own (differing) base file instead.

    python tools/check_map_redirect.py <host mh_net.log or run dir> <client ...>
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_map_transfer as cmt  # noqa: E402 -- reuse its regexes, read()/classify(), assertions

# See map_transfer.cpp client_resolve_now(), the Resolve::Stored arm: `name` there is
# dl_dir()+stored, i.e. "mh_dl\\<stem>.<16 hex>.<ext>" (DL_DIR_DEFAULT = "mh_dl\\").
RE_REDIRECTED = re.compile(
    r"; \[map\] client resolve stored (mh_dl\\.+?) -- redirecting (.+?) to it"
)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    logs = [a for a in argv if not a.startswith("--")]
    flags = [a for a in argv if a.startswith("--")]
    # Reuse check_map_transfer wholesale for the claim/gate/download/unchanged-file clauses --
    # duplicating them here would be a second copy of an oracle that already exists and is already
    # selftested (net_selftest maptest + check_map_transfer_selftest).
    rc = cmt.main(["--expect-gate", *flags, *logs])

    host, clients = cmt.classify(logs)
    if not clients:
        print("check_map_redirect: FAIL")
        print("  - no client log given -- the redirect line can only be read from the joiner")
        return 1
    bad = False
    for p, t in clients:
        name = cmt.peer_label(p)
        m = RE_REDIRECTED.search(t)
        if not m:
            print(
                "  - %s: no `; [map] client resolve stored ... -- redirecting ... to it` line -- "
                "the open-redirect was never armed (the client may have opened its own, differing, "
                "base file instead of the download)" % name
            )
            bad = True
            continue
        stored, base = m.group(1), m.group(2)
        if not cmt.RE_STORED_FORM.match(stored):
            print(
                "  - %s: the redirect points at '%s', not `mh_dl\\<stem>.<16 hex>.<ext>`"
                % (name, stored)
            )
            bad = True
            continue
        print("  %s: redirect armed -- '%s' -> %s" % (name, base, stored))
    if bad:
        print("check_map_redirect: FAIL")
        return 1
    if rc != 0:
        print("check_map_redirect: FAIL (check_map_transfer --expect-gate failed, see above)")
        return rc
    print("check_map_redirect: OK")
    return 0


# ---- the reader's own negative cases (registry: run_selftests / lint_repo, small enough to be a
# direct call rather than a separate suite) ---------------------------------------------------------


def selftest():
    import shutil
    import tempfile

    HASH = "d3c5707f54d7ecfb"
    host_menu = "; [map] host claim blue monday.mpm sha=%s size=462065\n" % HASH
    host_run = (
        "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
        "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
        "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
        "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n"
        "; [map] start OK -- every joiner reports the map we advertised\n" % (HASH, HASH)
    )
    cl_menu = (
        "; [map] client want blue monday.mpm sha=%s size=462065\n"
        "; [map] local before blue monday.mpm sha=%s size=462065\n"
        "; [map] client resolve missing -- waiting for the host's copy over channel C\n"
        % (HASH, HASH)
    )
    cl_run_redirected = (
        "; [map] client stored mh_dl\\blue monday.%s.mpm (462065 B, sha=%s) -- the local blue "
        "monday.mpm is untouched\n"
        "; [map] client resolve stored mh_dl\\blue monday.%s.mpm -- redirecting blue monday.mpm "
        "to it\n"
        "; [map] local after blue monday.mpm sha=%s size=462065\n" % (HASH, HASH, HASH, HASH)
    )
    cl_run_no_redirect = (
        "; [map] client stored mh_dl\\blue monday.%s.mpm (462065 B, sha=%s) -- the local blue "
        "monday.mpm is untouched\n"
        "; [map] local after blue monday.mpm sha=%s size=462065\n" % (HASH, HASH, HASH)
    )

    def plant(root, lane, runs):
        # ONLY THE FIRST run is the process's "_menu_" dir (written at boot, carries the host's
        # claim) -- every later one is "_sess_" (the match session). Tagging every entry "_menu_"
        # (as an earlier version of this helper did) makes read() treat each one as a NEW process
        # boundary and read only the newest single run, silently dropping the claim line.
        out = None
        for i, text in enumerate(runs):
            tag = "menu" if i == 0 else "sess"
            d = os.path.join(root, lane, "logs", "20260101T0000%02dZ_%s_run%d" % (i, tag, i))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(text)
            out = d
        return out

    cases = [
        (
            "the redirect line present is GREEN",
            [host_menu, host_run],
            [cl_menu, cl_run_redirected],
            0,
        ),
        (
            "MUTATION RED: the redirect never armed (client opened its own file)",
            [host_menu, host_run],
            [cl_menu, cl_run_no_redirect],
            1,
        ),
        (
            "MUTATION RED: a redirect pointing outside mh_dl\\ is caught",
            [host_menu, host_run],
            [
                cl_menu,
                cl_run_redirected.replace(
                    "resolve stored mh_dl\\blue monday.%s.mpm -- redirecting" % HASH,
                    "resolve stored blue monday.mpm -- redirecting",
                ),
            ],
            1,
        ),
    ]
    root = tempfile.mkdtemp(prefix="mh_maprr_selftest_")
    bad = 0
    try:
        for i, (label, host_runs, cl_runs, want) in enumerate(cases):
            case = os.path.join(root, "c%d" % i)
            h = plant(case, "host", host_runs)
            c = plant(case, "client", cl_runs)
            got = main([h, c])
            ok = got == want
            bad += 0 if ok else 1
            print("  %-4s %s (rc=%s, wanted %s)" % ("ok" if ok else "FAIL", label, got, want))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(
        "check_map_redirect --selftest: %s (%d case(s), %d failure(s))"
        % ("PASS" if bad == 0 else "FAIL", len(cases), bad)
    )
    return 1 if bad else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    sys.exit(main())
