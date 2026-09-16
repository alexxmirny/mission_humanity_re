#!/usr/bin/env python3
"""check_instrument_wiring.py -- fork F4F: the module LOG-SINK wirings stay wired, and cross as measured.

Every reimplementation module in `src/mh_dll/mh/` that has anything to say owns a private
`void (*g_log)(const char *)` and a `say()` that EARLY-RETURNS when it is null.  One arm-time line
per module hands that sink the process's single physical writer (`seam_log`, net_seams.cpp:95):

    mh::orders::set_logger(seam_log);     reimpl_probe.cpp     mh::desync::set_logger(seam_log);  net_seams.cpp
    mh::lockstep::set_logger(seam_log);   reimpl_probe.cpp     mh::ui::set_logger(seam_log);      net_seams.cpp
    mh::save::set_logger(seam_log);       reimpl_probe.cpp     mh::patch::set_logger(&seam_log);  inmem_install.cpp
    mh::ai::set_logger(seam_log);         reimpl_probe.cpp

Delete one of those lines and the module goes SILENT.  Nothing crashes, no oracle reds, no test
fails -- the module's own diagnostics simply stop existing, which is the absence-is-a-failure shape
The instrument-channel notes states by hand.  Fork F4 made that worse in one specific way: four of
these wirings now cross a DLL BOUNDARY (the sink lives in mh.dll, `set_logger` lives in libmh.dll and
is reached through a generated naked thunk), so "is it still wired" stopped being a question one file
answers.

---- WHY THIS IS NOT REDUNDANT WITH check_arm_order --------------------------------------------

`check_arm_order` gates the ORDER of the arm log, and a missing sink deletes lines from it, so for
most of these the order gate IS the behavioural proof -- a stronger one than any source scan, because
it watches the real boot.  Measured at HEAD against the committed `brokered` baseline (74 structural
steps; 23 of them emitted from inside libmh.dll, 51 from mh.dll, 0 from mh_net.dll or
mh_harness.dll):

    mh::orders    steps #8, #9          (order_queue.cpp say(); issue_promote.cpp reuses it)
    mh::lockstep  steps #10-#13         (turn_engine.cpp say())
    mh::ai        steps #14-#27, #43    (ai_state.cpp ai_say() -- EVERY sim/* promote report rides it)
    mh::desync    steps #62, #63        (desync_watch.cpp say())
    mh::patch     steps #28-#30         (inmem_patch.cpp say(), and OPTIONAL: `[patch] inmem` ships
                                         OFF, so an unarmed run proves nothing about this one)
    mh::save      -- NOTHING            save_live's `[save]` lines are save/load-time, not arm-time
    mh::ui        -- NOTHING            lobby_ui's lines are lobby-time, not arm-time

So the order gate UNCONDITIONALLY covers four of the seven, covers a fifth only when the run asked
for the patcher, and structurally cannot cover the last two: their lines are not in the arm window at
all, and the window is bounded by an end marker on purpose (reading to EOF would start gating the
scenario).  This file is that gap, and it pays for itself by also pinning the thing the order gate is
blind to by construction -- WHICH IMAGE each sink is defined in, which is the fact F4 changed and the
fact a future split would change again.

---- THE FIVE ASSERTIONS, all DERIVED (no hand list of modules) ---------------------------------

The module set is discovered, not written down: any header under `src/mh_dll/mh/` that DECLARES
`void set_logger(void (*)(const char *))` is a module with a sink, and a module added tomorrow is
gated the day it is added.

  1  WIRED       -- exactly one call site in the mh.dll roster that is not the module's own
                    definition TU.  Zero = the module went silent.  Two = two arms disagree.
  2  ONE SINK    -- that call passes `seam_log` (or `&seam_log`).  There is ONE physical writer in
                    the process (net_seams.cpp:89, CreateFileA+WriteFile); a wiring to anything else
                    is a second log nobody reads.
  3  CROSSING    -- the image that DEFINES set_logger, read off the .vcxproj rosters, must agree with
                    the committed libmh contract: defined in a libmh TU <=> a contract row named
                    `mh_<ns>_set_logger` exists.  This is the F4D/F4E fact stated as a check --
                    moving a module between images without regenerating the contract is a build
                    error there and a silent absent-bind here.
  4  NOT VACUOUS -- the scan must have FOUND modules and rosters.  A mis-rooted walk and a clean tree
                    produce the same empty result; refuse rather than pass.

  5  CHANNEL OWNERSHIP -- which IMAGES name each `mh_*.log` channel, against the committed
                    expectation below.  This is fork ruling Q4's closure condition turned into a
                    gate instead of left as prose.

---- ASSERTION 5, AND WHY IT IS THE Q4 RULING ---------------------------------------------------

F3 deferred gating the four UI channels (`mh_video`, `mh_input`, `mh_capture`, `mh_uidrive`) with a
stated trigger: "until per-module logs become load-bearing at F4".  F4 shipped four DLLs and the
trigger DID NOT OCCUR -- ruling Q3 kept the 13 oracle seams in mh.dll, so every writer of those four
is still an mh.dll TU and there is no per-module log among them to become load-bearing.  That is a
decision with evidence, and the evidence is exactly this table.  If a satellite ever starts writing
one of them, THIS row goes red naming the channel and the image, which is the moment to gate it.

One writer this scan structurally cannot see, and it is worth stating rather than hiding: the
harness's own writer never names `mh_harness.log`.  mh.dll composes that path once
(`core_arm.cpp:108`) and hands the whole `MH_CoreArmPaths` struct across the boundary at
`MH_Harness_Init`, so mh_harness.dll writes the file without ever containing its name.  The two
writers and the contract between them are documented in the instrument-channel notes.

usage:
  python tools/check_instrument_wiring.py             # the five assertions (exit 1 on violation)
  python tools/check_instrument_wiring.py --report    # ...and print the sink + channel tables
  python tools/check_instrument_wiring.py --selftest  # planted-violation reds, in a temp tree
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MSB = "{http://schemas.microsoft.com/developer/msbuild/2003}"

# The projects whose object sets become a SHIPPING image. mh_nettest compiles module sources too
# (it tests them), and libmh.vcxproj is the static-archive arm of the same roster as libmh_dll --
# neither says anything about which DLL a symbol ends up in at run time.
# The two directories module sources live in since fork F5O: mh/ for mh.dll's own modules and
# libmh/ for the shared roster. A membership test spelled `startswith("mh/")` would have dropped
# every libmh module out of discover() silently, which is how this gate first read the move:
# 3 modules instead of 9, and zero crossings into libmh.dll.
MODULE_TREES = ("mh/", "libmh/")

IMAGE_OF = {
    "mh": "mh.dll",
    "libmh": "libmh.dll",
    "mh_harness": "mh_harness.dll",
    "mh_net": "mh_net.dll",
}

# The one physical writer. Both spellings occur -- `seam_log` where the declaration is in scope,
# `&seam_log` at the inmem site; they are the same pointer.
SINK_NAMES = ("seam_log", "&seam_log")

# ---- assertion 5's committed expectation ---------------------------------------------------------
#
# channel -> (kind, the images whose TUs NAME the file). Measured 2026-09-13 at fork F4F over the
# four-DLL tree, and the FOURTEEN rows are themselves a finding: F3A's ruling spoke of five UI
# channels and the tree has fourteen. Four kinds, because "advisory vs gated" turned out to be too
# few to describe what is here:
#
#   gated      an arm-order template exists (tools/data/arm_order/*.json). Order IS the assertion.
#   advisory   ruling Q4's four. Reported present/absent by check_arm_order, never red. F4F CLOSED
#              the deferral: see the docstring -- no satellite writes any of them, so the trigger
#              the deferral named never occurred.
#   telemetry  a numeric/row stream with no arm window and no end marker (frametime, lockstep,
#              temporal, trace, gamemode) or a verb trace (launch). Its consumers parse ROWS; there
#              is no sequence-of-steps to gate, so it is outside this question entirely.
#   refusal    fork F4E ruling Q4's loud channel. DELIBERATELY multi-image and the only one that is:
#              mh.dll shouts when a configured satellite is missing, and the satellite shouts when
#              its own spine is. Both sides must be able to write it or the absent case is silent.
#
# `mh_overlay.log` is telemetry rather than advisory even though it looks like a UI sibling: the
# overlay installs no hook (gfx_overlay.cpp), which is why F3A's five-name scope text was a slip.
CHANNEL_OWNERS = {
    "mh_video.log": ("advisory", {"mh.dll"}),
    "mh_input.log": ("advisory", {"mh.dll"}),
    "mh_capture.log": ("advisory", {"mh.dll"}),
    "mh_uidrive.log": ("advisory", {"mh.dll"}),
    "mh_net.log": ("gated", {"mh.dll", "mh_net.dll"}),
    # mh.dll ALONE by this scan, and that is not the whole truth -- see the docstring: the harness's
    # writer is handed the path through MH_CoreArmPaths and never names the file.
    "mh_harness.log": ("gated", {"mh.dll"}),
    "mh_harness_refused.log": ("refusal", {"mh.dll", "mh_harness.dll"}),
    "mh_overlay.log": ("telemetry", {"mh.dll"}),
    "mh_frametime.log": ("telemetry", {"mh.dll"}),
    "mh_lockstep.log": ("telemetry", {"mh.dll"}),
    "mh_temporal.log": ("telemetry", {"mh.dll"}),
    "mh_trace.log": ("telemetry", {"mh.dll"}),
    "mh_gamemode.log": ("telemetry", {"mh.dll"}),
    "mh_launch.log": ("telemetry", {"mh.dll"}),
}

CHANNEL_RE = re.compile(r'"[^"\n]*?(mh_\w+\.log)"')
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)

DECL_RE = re.compile(
    r"^[ \t]*void\s+set_logger\s*\(\s*void\s*\(\s*\*\s*\w*\s*\)\s*"
    r"\(\s*const\s+char\s*\*\s*\w*\s*\)\s*\)\s*;",
    re.M,
)
DEF_RE = re.compile(
    r"^[ \t]*void\s+set_logger\s*\(\s*void\s*\(\s*\*\s*(\w+)\s*\)\s*"
    r"\(\s*const\s+char\s*\*\s*\w*\s*\)\s*\)\s*\{",
    re.M,
)
# `namespace mh::ui {` and the nested `namespace mh {` + `namespace ui {` spelling both occur.
NS_FLAT_RE = re.compile(r"^\s*namespace\s+mh::(\w+)\s*\{")
NS_NESTED_RE = re.compile(r"^\s*namespace\s+(\w+)\s*\{")


class Refusal(Exception):
    """A tree this tool cannot make a statement about. NEVER a pass."""


def rosters(src):
    """file (src-relative, forward slash) -> set of shipping image names."""
    out = {}
    for proj in sorted(os.listdir(src)):
        vp = os.path.join(src, proj, proj + ".vcxproj")
        if not os.path.isfile(vp) or proj not in IMAGE_OF:
            continue
        for cc in ET.parse(vp).iter(MSB + "ClCompile"):
            inc = cc.get("Include")
            if not inc:
                continue
            p = os.path.normpath(os.path.join(src, proj, inc))
            out.setdefault(os.path.relpath(p, src).replace("\\", "/"), set()).add(IMAGE_OF[proj])
    return out


def sources(src):
    """src-relative path -> text, for every C/C++ file under mh/ and the satellite projects."""
    out = {}
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ("Debug", "Release", "Release_asan", ".vs")]
        for f in files:
            if not f.endswith((".cpp", ".h", ".hpp", ".c", ".inl")):
                continue
            p = os.path.join(root, f)
            rel = os.path.relpath(p, src).replace("\\", "/")
            try:
                out[rel] = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                pass
    return out


CLOSE_RE = re.compile(r"^\s*\}\s*//\s*namespace\s+(?:mh::)?(\w+)")


def namespace_at(text, idx):
    """The innermost STILL-OPEN `mh::<x>` namespace enclosing character offset `idx`, or None.

    It has to track closes, not just openers: `ui_runtime.cpp` closes an anonymous-ish
    `namespace detail {` a line above `set_logger`, and a scan that only looked upward for the
    nearest `namespace N {` answered `detail` -- i.e. it silently failed to find mh::ui's sink at
    all, which in a gate reads as "this module has no wiring to check". So a `} // namespace N`
    seen on the way up cancels the next `namespace N {`. This tree always writes that closing
    comment (clang-format enforces it), which is what makes a line-based walk sound enough here."""
    closed = {}
    for ln in reversed(text[:idx].splitlines()):
        m = CLOSE_RE.match(ln)
        if m:
            closed[m.group(1)] = closed.get(m.group(1), 0) + 1
            continue
        m = NS_FLAT_RE.match(ln)
        if m:
            if closed.get(m.group(1)):
                closed[m.group(1)] -= 1
                continue
            return m.group(1)
        m = NS_NESTED_RE.match(ln)
        if m:
            name = m.group(1)
            if closed.get(name):
                closed[name] -= 1
                continue
            if name == "mh":
                return None  # directly inside `namespace mh {` -- not a module API namespace
            return name
    return None


def discover(src):
    """-> {ns: {...}} for every module that DECLARES a set_logger sink."""
    text = sources(src)
    rost = rosters(src)
    mods = {}
    for rel, body in sorted(text.items()):
        if not rel.startswith(MODULE_TREES) or not rel.endswith(".h"):
            continue
        for m in DECL_RE.finditer(body):
            ns = namespace_at(body, m.start())
            if not ns:
                continue
            mods.setdefault(ns, {"ns": ns, "header": rel, "dir": rel.rsplit("/", 1)[0]})
    # the DEFINITION names the TU, and the TU names the image
    for rel, body in sorted(text.items()):
        if not rel.startswith(MODULE_TREES) or not rel.endswith(".cpp"):
            continue
        for m in DEF_RE.finditer(body):
            ns = namespace_at(body, m.start())
            if ns in mods and "def" not in mods[ns]:
                mods[ns]["def"] = rel
                mods[ns]["images"] = sorted(rost.get(rel, set()))
    # THE CALL SITES. Three exclusions, each for a measured reason rather than tidiness:
    #   * not the module's OWN definition TU -- `set_logger` naming itself is the definition, and
    #     `mh/patch/inmem_install.cpp` shows why the exclusion cannot instead be "a different
    #     DIRECTORY": the patcher's arm lives beside the module it arms.
    #   * not a GENERATED contract file -- libmh_contract.gen.cpp carries the crossing's thunk and
    #     a `// void __cdecl mh::ai::set_logger(...)` comment above it. That comment matched as a
    #     wiring and made every crossing module read as DOUBLE-wired.
    #   * only TUs mh.dll compiles -- mh_nettest wires a capture sink of its own (that is what a
    #     test does), and the arm is mh.dll's.
    call_res = {ns: re.compile(r"mh::%s::set_logger\s*\(\s*([^)]*?)\s*\)" % ns) for ns in mods}
    for ns, mod in mods.items():
        mod["calls"] = []
        for rel, body in sorted(text.items()):
            if (
                not rel.startswith(MODULE_TREES)
                or rel == mod.get("def")
                or rel.endswith(".gen.cpp")
            ):
                continue
            if "mh.dll" not in rost.get(rel, set()):
                continue
            for m in call_res[ns].finditer(body):
                line = body[: m.start()].count("\n") + 1
                if body.splitlines()[line - 1].lstrip().startswith(("//", "*", "/*")):
                    continue
                mod["calls"].append((rel, line, m.group(1).strip()))
    return mods


def strip_comments(text):
    """Remove comments so a filename QUOTED INSIDE ONE is not read as a writer.

    It was: `net_diag.cpp:672` and `net_lockstep.cpp:1742` both end in
    `// g_log = "...\\mh_net.log"`, a trailing comment on a real code line -- so neither a
    starts-with-`//` test nor a naive scan tells them from an actual path composition.

    The `//` rule counts the quotes to its left: an EVEN count means the `//` is outside a string
    and the rest of the line is a comment; an ODD count means it is inside one (`"http://x"`)."""
    text = BLOCK_COMMENT_RE.sub(" ", text)
    out = []
    for ln in text.splitlines():
        i = ln.find("//")
        while i >= 0:
            if ln[:i].count('"') % 2 == 0:
                ln = ln[:i]
                break
            i = ln.find("//", i + 2)
        out.append(ln)
    return "\n".join(out)


def channel_writers(src):
    """-> {channel filename: {image, ...}} over every TU that NAMES it in a string literal."""
    rost = rosters(src)
    out = {}
    for rel, body in sorted(sources(src).items()):
        imgs = rost.get(rel, set())
        if not imgs:
            continue  # headers and test-only TUs: not part of any shipping image
        for m in CHANNEL_RE.finditer(strip_comments(body)):
            out.setdefault(m.group(1), set()).update(imgs)
    return out


def contract_slots(path):
    with open(path, encoding="utf-8") as fh:
        return {r["slot"] for r in json.load(fh)["rows"]}


def check(src, contract, say=print):
    mods = discover(src)
    slots = contract_slots(contract)
    fails = []
    if not mods:
        raise Refusal(
            "%s: NOT ONE module with a set_logger sink was found. The walker found nothing to "
            "check, which reads exactly like a clean tree and is not one." % src
        )
    rows = []
    for ns in sorted(mods):
        mod = mods[ns]
        calls = mod["calls"]
        images = mod.get("images") or []
        slot = "mh_%s_set_logger" % ns
        in_contract = slot in slots
        # 1 WIRED
        if len(calls) != 1:
            fails.append(
                "[%s] %d arm-time wiring site(s), expected exactly 1%s. A module whose sink is "
                "never set goes SILENT: say() early-returns on a null g_log, so every line it "
                "would have written simply does not exist and nothing reds."
                % (
                    ns,
                    len(calls),
                    (" (%s)" % ", ".join("%s:%d" % (c[0], c[1]) for c in calls)) if calls else "",
                )
            )
        # 2 ONE SINK
        for rel, line, arg in calls:
            if arg not in SINK_NAMES:
                fails.append(
                    "[%s] %s:%d wires the sink to %r, not the process's one physical writer "
                    "(seam_log). A second writer is a log nobody reads." % (ns, rel, line, arg)
                )
        # 3 CROSSING
        if not images:
            fails.append(
                "[%s] set_logger is DECLARED (%s) but its definition is in no shipping image's "
                "roster -- either the .cpp is unreferenced by every .vcxproj, or the definition "
                "was deleted and the declaration left behind." % (ns, mod["header"])
            )
        elif "libmh.dll" in images and not in_contract:
            fails.append(
                "[%s] defined in %s, which libmh.dll compiles -- so the wiring CROSSES the DLL "
                "boundary -- but the committed contract has no `%s` row. The call binds to nothing "
                "and the module is silent in the hosted build." % (ns, mod["def"], slot)
            )
        elif "libmh.dll" not in images and in_contract:
            fails.append(
                "[%s] defined in %s (image %s), but the committed contract still carries a `%s` "
                "row. The row is stale: mh.dll would resolve the symbol in its own image and the "
                "export is dead weight." % (ns, mod["def"], "+".join(images), slot)
            )
        rows.append(
            (
                ns,
                mod["def"] or "?",
                "+".join(images) or "?",
                "CROSSES" if in_contract else "in-image",
                "; ".join("%s:%d" % (c[0], c[1]) for c in calls) or "NONE",
            )
        )
    # 5 CHANNEL OWNERSHIP -- ruling Q4's closure condition, as a gate
    writers = channel_writers(src)
    if not writers:
        raise Refusal(
            "%s: NOT ONE `mh_*.log` channel writer was found. Assertion 5 scanned nothing, which "
            "reads exactly like a tree where every channel is correctly owned." % src
        )
    chan_rows = []
    for chan in sorted(set(writers) | set(CHANNEL_OWNERS)):
        got = writers.get(chan, set())
        if chan not in CHANNEL_OWNERS:
            fails.append(
                "[%s] a NEW instrument channel, written by %s, with no entry in CHANNEL_OWNERS. "
                "Rule it gated / advisory / telemetry / refusal and record that here -- an "
                "unlisted channel is one nobody ruled on." % (chan, "+".join(sorted(got)) or "?")
            )
            chan_rows.append((chan, "UNRULED", "+".join(sorted(got)) or "NONE"))
            continue
        kind, want = CHANNEL_OWNERS[chan]
        if got != want:
            extra = sorted(got - want)
            if kind == "advisory" and extra:
                fails.append(
                    "[%s] ADVISORY channel now written by %s as well as mh.dll. Fork ruling Q4 left "
                    "the four UI channels ungated because no satellite writes them; a satellite "
                    "writer IS the condition the deferral named, so this channel needs a gate (a "
                    "check_arm_order template, or its own) rather than this row."
                    % (chan, "+".join(extra))
                )
            else:
                fails.append(
                    "[%s] writer set MOVED: committed %s, measured %s. Either a new image started "
                    "writing this channel or one stopped -- both are decisions, neither is drift."
                    % (chan, "+".join(sorted(want)), "+".join(sorted(got)) or "NONE")
                )
        chan_rows.append((chan, kind, "+".join(sorted(got)) or "NONE"))
    # ...and the advisory set here must be the SAME four check_arm_order reports as advisory.
    # Imported rather than restated: two hand lists of the same ruling is how one of them goes stale
    # without anything noticing, and this one is a ruling's scope.
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import check_arm_order

    advisory = {c for c, (k, _) in CHANNEL_OWNERS.items() if k == "advisory"}
    if advisory != set(check_arm_order.UI_CHANNELS):
        fails.append(
            "the advisory sets disagree: CHANNEL_OWNERS says %s, check_arm_order.UI_CHANNELS says "
            "%s. They are the same ruling (Q4) and must name the same channels -- one of them has "
            "been edited alone." % (sorted(advisory), sorted(check_arm_order.UI_CHANNELS))
        )
    return fails, rows, chan_rows


def report(rows, chan_rows, say=print):
    say("  %-12s %-46s %-12s %-9s %s" % ("module", "defined in", "image", "wiring", "wired from"))
    for ns, d, img, cross, calls in rows:
        say("  %-12s %-46s %-12s %-9s %s" % ("mh::" + ns, d, img, cross, calls))
    say("")
    say("  %-16s %-9s %s" % ("channel", "ruling", "written by"))
    for chan, kind, imgs in chan_rows:
        say("  %-16s %-9s %s" % (chan, kind, imgs))


def run(say=print):
    src = os.path.join(REPO, "src", "mh_dll")
    contract = os.path.join(REPO, "tools", "data", "libmh_contract.json")
    return check(src, contract, say=say)


# ---- selftest -----------------------------------------------------------------------------------
#
# PLANT-IN-A-TEMP-TREE, for the reason check_fork_d8's docstring gives: assertions whose healthy
# answer is "found nothing wrong" cannot prove their own regexes on the real tree. Each arm copies
# the real src tree, breaks exactly one thing, and requires the SAME walker to go red.


def _plant(tmp, edit):
    dst = os.path.join(tmp, "src")
    shutil.copytree(
        os.path.join(REPO, "src", "mh_dll"),
        dst,
        ignore=shutil.ignore_patterns("Debug", "Release", "Release_asan", ".vs", "*.obj", "*.pdb"),
    )
    edit(dst)
    return dst


def selftest():
    ok = True

    def expect(name, cond, detail=""):
        nonlocal ok
        print(
            "  [%s] %s%s"
            % ("ok" if cond else "FAIL", name, (" -- " + detail) if detail and not cond else "")
        )
        ok = ok and cond

    contract = os.path.join(REPO, "tools", "data", "libmh_contract.json")

    # --- the real tree ---------------------------------------------------------------------------
    fails, rows, chan_rows = run(say=lambda *_: None)
    expect("the real tree PASSES all five assertions", not fails, "; ".join(fails)[:400])
    expect("at least six modules with a sink were discovered", len(rows) >= 6, str(len(rows)))
    expect(
        "at least one wiring CROSSES into libmh.dll and at least one stays in-image",
        any(r[3] == "CROSSES" for r in rows) and any(r[3] == "in-image" for r in rows),
        str([(r[0], r[3]) for r in rows]),
    )

    def edit_file(dst, rel, old, new, count=1):
        p = os.path.join(dst, rel)
        t = open(p, encoding="utf-8").read()
        assert old in t, "planted edit did not find its anchor in %s: %r" % (rel, old[:60])
        open(p, "w", encoding="utf-8", newline="").write(t.replace(old, new, count))

    # 1 WIRED: delete the save wiring -- the one the ORDER GATE cannot see.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(
            tmp,
            lambda d: edit_file(
                d, "mh/seams/reimpl_probe.cpp", "mh::save::set_logger(seam_log);", "/* gone */"
            ),
        )
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a DELETED wiring is RED (mh::save -- invisible to the arm-order gate)",
            any(x.startswith("[save]") for x in f),
            "; ".join(f)[:300],
        )

    # 1 WIRED (the other direction): a second arm wiring the same module.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(
            tmp,
            lambda d: edit_file(
                d,
                "mh/seams/net_seams.cpp",
                "mh::ui::set_logger(seam_log);",
                "mh::ui::set_logger(seam_log); mh::ui::set_logger(seam_log);",
            ),
        )
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a DUPLICATED wiring is RED (two arms disagreeing about the sink)",
            any(x.startswith("[ui]") for x in f),
            "; ".join(f)[:300],
        )

    # 2 ONE SINK: wire a module to something other than the process's one writer.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(
            tmp,
            lambda d: edit_file(
                d,
                "mh/seams/reimpl_probe.cpp",
                "mh::orders::set_logger(seam_log);",
                "mh::orders::set_logger(other_log);",
            ),
        )
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a wiring to a SECOND sink is RED", any("other_log" in x for x in f), "; ".join(f)[:300]
        )

    # 3 CROSSING: move a libmh-defined module's TU into mh.dll's roster without regenerating the
    # contract -- the F4D/F4E hazard, and the one no compile catches.
    def demote(d):
        p = os.path.join(d, "libmh", "libmh.vcxproj")
        t = open(p, encoding="utf-8").read()
        needle = '<ClCompile Include="save\\save_live.cpp" />'
        assert needle in t, "the libmh roster no longer names save_live.cpp -- re-anchor this arm"
        open(p, "w", encoding="utf-8", newline="").write(t.replace(needle, ""))
        p2 = os.path.join(d, "mh", "mh.vcxproj")
        t2 = open(p2, encoding="utf-8").read()
        anchor = '<ClCompile Include="seams\\net_seams.cpp" />'
        open(p2, "w", encoding="utf-8", newline="").write(
            t2.replace(
                anchor,
                anchor + '\n    <ClCompile Include="..\\libmh\\save\\save_live.cpp" />',
            )
        )

    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(tmp, demote)
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a module moved OUT of libmh with a stale contract row is RED",
            any(x.startswith("[save]") and "stale" in x for x in f),
            "; ".join(f)[:300],
        )

    # 3 CROSSING, the OTHER direction: a module moved INTO libmh without a contract row. It
    # compiles and links on both sides (the thunk is /alternatename'd only for rows that exist), so
    # the only symptom at run time is a module that says nothing.
    def promote(d):
        p = os.path.join(d, "libmh", "libmh.vcxproj")
        t = open(p, encoding="utf-8").read()
        anchor = '<ClCompile Include="state\\rebind_arming.cpp" />'
        assert anchor in t, (
            "the libmh roster no longer names rebind_arming.cpp -- re-anchor this arm"
        )
        open(p, "w", encoding="utf-8", newline="").write(
            t.replace(
                anchor, anchor + '\n    <ClCompile Include="..\\mh\\desync\\desync_watch.cpp" />'
            )
        )
        p2 = os.path.join(d, "mh", "mh.vcxproj")
        t2 = open(p2, encoding="utf-8").read()
        open(p2, "w", encoding="utf-8", newline="").write(
            t2.replace('<ClCompile Include="desync\\desync_watch.cpp" />', "")
        )

    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(tmp, promote)
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a module moved INTO libmh with no contract row is RED",
            any(x.startswith("[desync]") and "no `mh_desync_set_logger` row" in x for x in f),
            "; ".join(f)[:300],
        )

    # 5 CHANNEL OWNERSHIP: a SATELLITE starts writing one of ruling Q4's four advisory channels --
    # the condition the F3 deferral named, and the moment that channel has to be gated.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(
            tmp,
            lambda d: edit_file(
                d,
                "mh_net/net_transport.cpp",
                'wsprintfA(g_log_path, "%smh_net.log"',
                'wsprintfA(g_log_path, "%smh_uidrive.log"',
            ),
        )
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a SATELLITE writing an ADVISORY channel is RED (ruling Q4's condition)",
            any(x.startswith("[mh_uidrive.log]") and "ADVISORY" in x for x in f),
            "; ".join(f)[:300],
        )

    # 5 CHANNEL OWNERSHIP: a channel nobody has ruled on.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(
            tmp,
            lambda d: edit_file(d, "mh/seams/video.cpp", '"%smh_video.log"', '"%smh_brandnew.log"'),
        )
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "an UNRULED new channel is RED",
            any(x.startswith("[mh_brandnew.log]") for x in f),
            "; ".join(f)[:300],
        )

    # 5's comment discipline: the filename quoted INSIDE a comment is not a writer. Both real cases
    # (net_diag.cpp:672, net_lockstep.cpp:1742) are trailing comments on live code lines, so this
    # arm plants the same shape rather than a whole-line comment.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        src = _plant(
            tmp,
            lambda d: edit_file(
                d,
                "mh_net/net_transport.cpp",
                "const char *module_run_dir(void) {",
                'const char *module_run_dir(void) { // g_log = "...mh_uidrive.log"',
            ),
        )
        f = check(src, contract, say=lambda *_: None)[0]
        expect(
            "a channel name QUOTED IN A COMMENT is not counted as a writer",
            not f,
            "; ".join(f)[:300],
        )

    # 4 NOT VACUOUS: an empty tree REFUSES rather than passing.
    with tempfile.TemporaryDirectory(prefix="instwire_") as tmp:
        empty = os.path.join(tmp, "empty")
        os.makedirs(empty)
        refused = False
        try:
            check(empty, contract, say=lambda *_: None)
        except Refusal:
            refused = True
        expect("an EMPTY tree is REFUSED, not passed", refused)

    print("check_instrument_wiring --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--report", action="store_true", help="print the per-module table")
    ap.add_argument(
        "--selftest", action="store_true", help="planted-violation reds, in a temp tree"
    )
    args = ap.parse_args(argv)
    try:
        if args.selftest:
            return selftest()
        fails, rows, chan_rows = run()
        if args.report:
            report(rows, chan_rows)
        for f in fails:
            print("[FAIL] %s" % f)
        if not fails:
            print(
                "check_instrument_wiring: PASS -- %d module sink(s), each wired exactly once to "
                "seam_log (%d cross into libmh.dll, %d stay in-image); %d channel(s) owned as "
                "ruled, %d of them advisory"
                % (
                    len(rows),
                    sum(1 for r in rows if r[3] == "CROSSES"),
                    sum(1 for r in rows if r[3] != "CROSSES"),
                    len(chan_rows),
                    sum(1 for r in chan_rows if r[1] == "advisory"),
                )
            )
        return 1 if fails else 0
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
