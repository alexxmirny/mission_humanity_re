"""check_sim_addresses.py -- no game-state ADDRESS reaches a sim translation (RI-SIM / SIM0, W2).

sim/sim_state.h's W2 says the write store hands out no address, so nothing in a translated sim body
can cache a roster base or hold a pointer across an ST2 rebase. The compiler enforces half of that:
`sim_store`'s members are private, so the store cannot leak one. It cannot enforce the other half --
a TU that simply calls `mh::state::ptr<unit>(RID_UNITS)` itself, or casts a literal VA, gets exactly
the address W2 removed, and it compiles, runs, and passes every other check in the gate.

So this is the text rule that closes it: **`libmh/sim/sim_state.cpp` is the only file under `libmh/sim/`
allowed to name the region registry's `ptr<>` or a game data address.** Everything else asks the
interface for a reference.

WHY A TEXT CHECK AND NOT A CLEVERER ONE. The property is "this token does not appear in these
files", which is exactly what a grep decides and exactly what a type system cannot. The risk of a
text check is that it silently stops matching, so `--selftest` synthesises both violations in a temp
directory and requires the scanner to catch each one -- and requires a clean file to pass, so a
scanner that flagged everything could not report green either.

WHAT IT DOES NOT CATCH, stated so nobody reads more into a green run: a violation written through an
alias (`using mh::state::ptr;`), assembled from string pieces, or reached via a helper in another
directory. Those are deliberate acts, which is the line SIM0's header already draws -- this check is
against drift, not against an author who has decided to do it.

-------------------------------------------------------------------------------------------------
SB-BIND T0 (2026-09-06): TWO DEFECTS FIXED, both measured before they were fixed. See
docs/state-boundary.md D6.1.

(1) THE LITERAL-VA WINDOW WAS WRONG FOR 71% OF THE REGISTRY. It was the hardcoded range
    0x00A00000..0x01100000, on the stated premise that everything below is "small constants (masks,
    strides, enum values)". FALSE for 583 of the registry's 823 regions -- every .data/.rdata region,
    down to base 0x453e2d. A raw literal for _G_LLM_NET_SEND_BUF (0x5d55cc) passed the gate.
    THE FIX IS MEMBERSHIP, NOT A WIDER RANGE, and that distinction is the measurement: widening the
    range to the registry's true span (0x453e2d..0x1070415) would newly flag 248 lines in the libmh
    modules, ALL of them code VAs transcribed inside disassembly block comments -- while widening by
    REGION MEMBERSHIP flags exactly 0 new lines and still catches all 583. So a literal is a game
    address iff it falls inside a registry region, plus the legacy window is kept for the
    un-registered .bss (the registry is not every global -- 2057 of the state matrix's regions
    resolve into no registry region at all).
    That measurement also exposed a scanner bug it had been masking: only lines whose STRIPPED form
    starts with // or * or /* were treated as comments, so the body of a /* ... */ block leaked into
    the scan. Now stripped properly, which is what makes membership safe to turn on.

(2) THE NAMED FORM WAS NEVER CHECKED AT ALL, and the gate pulled the wrong way because of it.
    `ptr<>` was refused outside a binder while `mh::addr::_G_LLM_WHATEVER` -- the same address under
    a generated name, and equally unable to follow a rebase -- was permitted everywhere. So
    "only turn_engine.cpp is registry-routed" was this gate's DESIGN, not drift, and SB-BIND's
    build-check clause was unfireable (lint_libmh_layering DEFINES addr/*.gen.h as the named shim,
    so the named form already WAS the sanctioned route).
    It is now a violation -- RATCHETED, because 191 of them are legal today (counted per LINE, as
    the baseline is) and a hard rule would just be reverted. The whole 191 is lockstep (160 in 12
    files) and save (31 in 2) -- exactly SB-BIND's two tranches; sim, ai, orders, tact and state
    are already at zero, so this rule is a wall for them from day one rather than a promise.
    Baseline: tools/data/named_va_baseline.json, the same shape as
    lint_source_narration.py's. A file that EXCEEDS its count fails; a file absent from the baseline
    has an implicit 0, so a NEW named VA anywhere is an immediate failure; --update-baseline only
    ever lowers. SB-BIND's lockstep and save tranches drain it to zero.
    WHY THE RATCHET COUNTS EVERY `data`-SECTION SYMBOL, not just the registry-backed ones: otherwise
    adding a registry row (which SB-BIND must do for the 8 registry-less lockstep globals) would
    RAISE a file's count without anyone editing that file, and the ratchet would refuse it. Counting
    the data section makes the number a property of the code alone. Code-section symbols are NOT
    counted: a function VA is call-shim territory (mh_calls.gen.*), never bind-table territory, and
    a qsort comparator's address is load-bearing (mh_addrs.gen.h).

Usage:
  python tools/check_sim_addresses.py                   # the gate
  python tools/check_sim_addresses.py --selftest        # prove the scanner still fires
  python tools/check_sim_addresses.py --report          # per-module named-VA drain counts
  python tools/check_sim_addresses.py --update-baseline # tighten after a drain (lowers only)
"""

import argparse
import bisect
import json
import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM_DIR = os.path.join(REPO, "src", "mh_dll", "libmh", "sim")
STATE_REGIONS = os.path.join(REPO, "tools", "data", "state_regions.json")
ADDR_MANIFEST = os.path.join(REPO, "tools", "data", "dll_addr_manifest.json")
BASELINE = os.path.join(REPO, "tools", "data", "named_va_baseline.json")

# The ONE file that binds the interface to the registry. Its whole job is to name these.
BINDER = "sim_state.cpp"


# F5O: the seven roster domains are under libmh/ now; desync/ is mh.dll's own instrument and
# stayed. The tree is derived from the module name so a new row needs no second decision.
LIBMH_MODULES = ("sim", "ai", "orders", "tact", "save", "state", "lockstep")


def _mod(name):
    tree = "libmh" if name in LIBMH_MODULES else "mh"
    return os.path.join(REPO, "src", "mh_dll", tree, name)


# TACT0 (2026-08-24): the tactical module has the same W2 and therefore the same text rule. Each
# module gets its own (directory, binders) pair rather than one shared scan, because "the binder" is
# per module -- a libmh/tact/ TU naming `ptr<>` is a violation even though libmh/sim/sim_state.cpp may.
# LIB0 (2026-09-01): extended to ALL SEVEN libmh modules -- the lib boundary makes "no TU names a VA
# outside its binder" a per-module invariant, not a sim-only one. Binder sets MEASURED, not assumed:
# each module already concentrated its bindings in these files (ai 125 sites in ai_state.cpp alone;
# state has zero and a nominal binder so a first violation is caught). save_table.gen.h is GENERATED
# -- an address list by design, the save format's registry view. The scan is recursive since the
# same change (os.listdir missed sim/resid/ and orders/issue/ entirely).
MODULES = [
    (SIM_DIR, frozenset({BINDER}), "libmh/sim/"),
    (_mod("tact"), frozenset({"tact_state.cpp"}), "libmh/tact/"),
    (_mod("ai"), frozenset({"ai_state.cpp"}), "libmh/ai/"),
    (_mod("orders"), frozenset({"order_queue.cpp", "issue_state.cpp"}), "libmh/orders/"),
    # TWO binders. save_table.gen.h carries every SAVE BLOCK's address; save_state.cpp carries the
    # state the LIVE arm reaches for around the blocks (the save dir, the LZW workspace, the planet
    # clock, the camera, the version table) -- 31 sites that were raw `mh::addr::` until SB-BIND T5.
    (_mod("save"), frozenset({"save_table.gen.h", "save_state.cpp"}), "libmh/save/"),
    (_mod("state"), frozenset({"region_runtime.h"}), "libmh/state/"),
    # TWO binders, like libmh/orders/. lockstep_state.cpp holds the twelve translated-logic state
    # structs; overlay_hoist.cpp is the LIB-ABI stage-E host arm, whose every function is a one-line
    # read/write of one game global handed to the engine as an op -- it contains no translated logic
    # for a bound struct to keep away from an address. See that file's own banner. (SB-BIND T4; it
    # was `turn_engine.cpp` alone while the module's twelve binders were scattered across 12 TUs and
    # spelled their addresses as `mh::addr::` instead of ptr<>, which is what T4 drained.)
    (_mod("lockstep"), frozenset({"lockstep_state.cpp", "overlay_hoist.cpp"}), "libmh/lockstep/"),
    # F3D (2026-09-13): mh/desync/ is the D21 instrument, EXTRACTED out of mh/lockstep so the fork's
    # config (1) carries no reference into the closure. It arrives with one file, which is also its
    # binder -- so this row is VACUOUS TODAY, and it is here anyway for the same reason libmh/state/'s
    # nominal binder is: the invariant is per MODULE, and a module that joins the list only once it
    # has a second file joins it by someone noticing, which is not a mechanism. The extraction is
    # what made the row necessary: desync_watch stopped reaching through mh::lockstep::host_binds()
    # and now resolves RID_G_TEXT_TMP / RID_GAME_SESSION_MODE through mh::state::ptr<> itself, i.e.
    # it names addresses where before it borrowed someone else's binder.
    (_mod("desync"), frozenset({"desync_watch.cpp"}), "mh/desync/"),
]

# `mh::state::ptr<T>(...)` in any of its spellings once `using namespace mh::state` is in scope.
RE_REGISTRY_PTR = re.compile(r"\bptr\s*<")
RE_GAME_VA = re.compile(r"\b0[xX]0*([0-9a-fA-F]{6,8})\b")
# The LEGACY window, kept for the un-registered .bss: the registry is not every global in the binary
# (2057 of the state matrix's referenced regions resolve into no registry region), so membership
# alone would lose coverage this check already had. Region membership is checked FIRST and is the
# part that grew; this is the residue.
VA_LO = 0x00A00000
VA_HI = 0x01100000

# `mh::addr::NAME` -- the generated named form of the same address. Only DATA-section symbols count;
# see the module header for why (and why the ratchet counts them all, registry-backed or not).
RE_NAMED_VA = re.compile(r"\bmh::addr::([A-Za-z_][A-Za-z0-9_]*)")

# `// ADDR-OK: <reason>` on the same line exempts it. Deliberately verbose so it reads as a decision
# in a diff; there are none in the tree today and a new one should have to justify itself.
RE_EXEMPT = re.compile(r"//.*\bADDR-OK\b")

_REGIONS = None  # [(base, end)] sorted, plus the bases list for bisect
_DATA_SYMBOLS = None


def _load_regions():
    """Sorted (base, end) intervals over the region registry. `end` uses the larger of size/extent
    so a save block's overrun tail still reads as 'inside a region' -- the point is 'is this literal
    a state address', and a byte in a region's reach is."""
    global _REGIONS
    if _REGIONS is None:
        with open(STATE_REGIONS, encoding="utf-8") as fh:
            regs = json.load(fh)["regions"]
        iv = sorted(
            (r["base"], r["base"] + max(r["size"], r["extent"], 1), r["name"]) for r in regs
        )
        _REGIONS = (iv, [x[0] for x in iv])
    return _REGIONS


def region_covering(va):
    """The registry region containing `va`, or None. Regions do not overlap by declared size (0
    pairs, measured 2026-09-06), so the walk back is short."""
    iv, bases = _load_regions()
    i = bisect.bisect_right(bases, va) - 1
    while i >= 0:
        base, end, name = iv[i]
        if end > va:
            return name
        i -= 1
    return None


def data_symbols():
    """Names in the addr manifest's `data` section. Code and anchor sections are deliberately
    excluded -- a function VA belongs to the call shim, not the bind table."""
    global _DATA_SYMBOLS
    if _DATA_SYMBOLS is None:
        with open(ADDR_MANIFEST, encoding="utf-8") as fh:
            _DATA_SYMBOLS = frozenset(d["name"] for d in json.load(fh)["data"])
    return _DATA_SYMBOLS


def strip_comments(text):
    """Blank out comment content, keeping line structure. The old scanner only skipped lines whose
    STRIPPED form began with // or * or /*, so the body of a `/* ... */` block leaked into the scan
    -- harmless while the VA window was narrow, and 248 false positives the moment it was not."""
    out = []
    in_block = False
    for line in text.splitlines():
        buf = []
        i = 0
        while i < len(line):
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = len(line)
                    break
                in_block = False
                i = j + 2
                continue
            if line.startswith("//", i):
                break  # rest of the line is a comment
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def violations_in(path, text):
    """(line_number, kind, line) for each offending line. Comment content is skipped -- the headers
    in this module quote addresses constantly and that is documentation, not a binding."""
    out = []
    raw = text.splitlines()
    for i, code in enumerate(strip_comments(text), 1):
        line = raw[i - 1]
        if RE_EXEMPT.search(line):
            continue
        if RE_REGISTRY_PTR.search(code):
            out.append((i, "registry ptr<>", line.strip()))
            continue
        hit = None
        for m in RE_GAME_VA.finditer(code):
            val = int(m.group(1), 16)
            name = region_covering(val)
            if name is not None:
                hit = "game VA 0x%08x (region %s)" % (val, name)
                break
            if VA_LO <= val < VA_HI:
                hit = "game VA 0x%08x (unregistered .bss)" % val
                break
        if hit:
            out.append((i, hit, line.strip()))
            continue
        syms = data_symbols()
        for m in RE_NAMED_VA.finditer(code):
            if m.group(1) in syms:
                out.append((i, "named state VA mh::addr::%s" % m.group(1), line.strip()))
                break
    return out


def scan_dir(directory, binder=BINDER):
    """Returns a list of (relpath, line, kind, text). `binder` is a filename or a set of them;
    the scan is RECURSIVE (os.listdir silently skipped sim/resid/ and orders/issue/ until LIB0)."""
    binders = {binder} if isinstance(binder, str) else set(binder)
    found = []
    for root, _dirs, files in os.walk(directory):
        for name in sorted(files):
            if not name.endswith((".cpp", ".h")):
                continue
            if name in binders:
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            rel = os.path.relpath(path, directory)
            for ln, kind, line in violations_in(path, text):
                found.append((rel, ln, kind, line))
    return found


NAMED_KIND = "named state VA "


def is_named(kind):
    return kind.startswith(NAMED_KIND)


def scan_modules():
    """(label, repo_rel_path, line, kind, text) over every configured module. Repo-relative because
    the ratchet baseline is keyed on a path a human can grep for."""
    found = []
    for directory, binder, label in MODULES:
        if not os.path.isdir(directory):
            continue
        for rel, ln, kind, line in scan_dir(directory, binder):
            abs_path = os.path.join(directory, rel)
            repo_rel = os.path.relpath(abs_path, REPO).replace(os.sep, "/")
            found.append((label, repo_rel, ln, kind, line))
    return found


def load_baseline():
    try:
        with open(BASELINE, encoding="utf-8") as fh:
            return {k: v for k, v in json.load(fh).items() if not k.startswith("_")}
    except (OSError, ValueError):
        return {}


def named_counts(found):
    counts = {}
    for _label, repo_rel, _ln, kind, _line in found:
        if is_named(kind):
            counts[repo_rel] = counts.get(repo_rel, 0) + 1
    return counts


def check_named(found, baseline=None):
    """(problem strings, ok). A file fails only when it EXCEEDS its baseline; a file the baseline
    does not mention has an implicit 0, so a new named VA anywhere is an immediate failure."""
    baseline = load_baseline() if baseline is None else baseline
    probs = []
    for path, n in sorted(named_counts(found).items()):
        allowed = baseline.get(path, 0)
        if n > allowed:
            probs.append(
                "%s: %d named state VA(s) (mh::addr::<data symbol>), baseline %d -- reach it "
                "through the module's state view instead; the binder is the one file that may bind"
                % (path, n, allowed)
            )
    return probs, not probs


def update_baseline():
    counts = named_counts(scan_modules())
    old = load_baseline()
    raised = [p for p, n in counts.items() if n > old.get(p, 0)] if old else []
    if raised:
        sys.exit(
            "refusing to RAISE the baseline for: %s\nThe ratchet only tightens -- route those "
            "through the module's state view (or mark a genuinely legit line ADDR-OK)."
            % ", ".join(sorted(raised))
        )
    doc = {
        "_comment": "Ratchet baseline for tools/check_sim_addresses.py: per-file counts of "
        "pre-existing `mh::addr::<data symbol>` uses in the libmh modules (SB-BIND T0, "
        "2026-09-06). The check fails a file that EXCEEDS its count; a file absent here has an "
        "implicit 0; --update-baseline only LOWERS entries. SB-BIND's lockstep and save tranches "
        "drain this to zero -- see docs/state-boundary.md D6.1-D6.2."
    }
    doc.update({p: counts[p] for p in sorted(counts)})
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")
    dropped = sorted(set(old) - set(counts))
    print(
        "named-VA baseline updated: %d file(s), %d use(s) total%s"
        % (
            len(counts),
            sum(counts.values()),
            "; dropped %d now-clean file(s)" % len(dropped) if dropped else "",
        )
    )


def report():
    """The drain number, per module and per file -- SB-BIND's done_when wants it PRINTED."""
    found = scan_modules()
    baseline = load_baseline()
    per_module = {}
    for label, repo_rel, _ln, kind, _line in found:
        if is_named(kind):
            per_module.setdefault(label, {}).setdefault(repo_rel, 0)
            per_module[label][repo_rel] += 1
    total = 0
    print("=== named state VAs remaining, per module (SB-BIND drain) ===")
    for _directory, _binder, label in MODULES:
        files = per_module.get(label, {})
        n = sum(files.values())
        total += n
        print("  %-12s %4d use(s) in %d file(s)" % (label, n, len(files)))
        for path, c in sorted(files.items(), key=lambda x: -x[1]):
            print("      %4d  %-58s baseline %d" % (c, path, baseline.get(path, 0)))
    print("  %-12s %4d total" % ("ALL", total))
    return 0


def selftest():
    """Both directions, in a temp dir: a clean file must pass and each violation must be caught."""
    fails = 0
    with tempfile.TemporaryDirectory(prefix="mh_simaddr_") as wd:
        clean = (
            "#include \"sim/sim_state.h\"\n"
            "namespace mh::sim {\n"
            "// 0x00dd8c48 is the roster base -- quoting it in a COMMENT must stay legal.\n"
            "int f(const sim_view &v) { return (int)unit_of(v, 0, 0).unit_proto_id; }\n"
            "}\n"
        )
        open(os.path.join(wd, "clean.cpp"), "w", encoding="utf-8").write(clean)
        if scan_dir(wd, binder="__none__"):
            print("  FAIL: a clean file was reported as a violation")
            fails += 1
        else:
            print("  ok: the clean file passes (so the scanner is not flagging everything)")

        os.remove(os.path.join(wd, "clean.cpp"))
        open(os.path.join(wd, "bad_ptr.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { auto *u = mh::state::ptr<unit>(RID_UNITS); (void)u; } }\n"
        )
        hits = scan_dir(wd, binder="__none__")
        if not any(k == "registry ptr<>" for _, _, k, _ in hits):
            print("  FAIL: a raw registry ptr<> was NOT caught")
            fails += 1
        else:
            print("  ok: a raw registry ptr<> is caught")

        os.remove(os.path.join(wd, "bad_ptr.cpp"))
        open(os.path.join(wd, "bad_va.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { auto *u = (unit *)0x00dd8c48; (void)u; } }\n"
        )
        hits = scan_dir(wd, binder="__none__")
        if not any(k.startswith("game VA") for _, _, k, _ in hits):
            print("  FAIL: a literal game VA was NOT caught")
            fails += 1
        else:
            print("  ok: a literal game VA is caught")

        os.remove(os.path.join(wd, "bad_va.cpp"))
        open(os.path.join(wd, "small.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { unsigned m = 0x0000ffff; (void)m; } }\n"
        )
        if scan_dir(wd, binder="__none__"):
            print("  FAIL: a small constant (0x0000ffff) was reported as a game address")
            fails += 1
        else:
            print("  ok: a small constant is not mistaken for an address")

        # --- SB-BIND T0 arms (2026-09-06) -------------------------------------------------
        # THE HOLE THAT MOTIVATED THE CHANGE: a .data/.rdata region VA, below the old window's
        # 0x00A00000 floor. 583 of 823 regions live down there and every one of them used to pass.
        os.remove(os.path.join(wd, "small.cpp"))
        open(os.path.join(wd, "low_va.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { auto *b = (unsigned char *)0x005d55cc; (void)b; } }\n"
        )
        hits = scan_dir(wd, binder="__none__")
        if not any(k.startswith("game VA") for _, _, k, _ in hits):
            print("  FAIL: a sub-0xA00000 REGION VA (0x005d55cc) was NOT caught -- the D6.1 hole")
            fails += 1
        else:
            print("  ok: a sub-0xA00000 region VA is caught (the old window missed 583 regions)")

        # ... and the reason membership was chosen over a wider range: a CODE VA in the same span
        # must stay legal, or 248 disassembly-comment lines light up.
        os.remove(os.path.join(wd, "low_va.cpp"))
        open(os.path.join(wd, "code_va.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { auto p = (void *)0x004d0596; (void)p; } }\n"
        )
        if scan_dir(wd, binder="__none__"):
            print("  FAIL: a CODE VA (0x004d0596) in the region span was flagged as state")
            fails += 1
        else:
            print("  ok: a code VA in the region span is not mistaken for state")

        # THE BLOCK-COMMENT STRIPPER, which is what makes the above safe. A disassembly transcript
        # inside /* ... */ whose lines do not begin with '*' used to leak into the scan.
        os.remove(os.path.join(wd, "code_va.cpp"))
        open(os.path.join(wd, "block.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim {\n/*\n  mov eax, [0x00dd8c48]   ; the roster base\n*/\n"
            "int f() { return 0; }\n}\n"
        )
        if scan_dir(wd, binder="__none__"):
            print("  FAIL: a VA inside a /* ... */ block was reported as a binding")
            fails += 1
        else:
            print("  ok: a VA inside a block comment is skipped")

        # THE NAMED FORM -- the rule that did not exist. Uses a symbol that must be in the data
        # section; asserted here so a manifest reshuffle cannot quietly turn this arm into a no-op.
        os.remove(os.path.join(wd, "block.cpp"))
        probe = "_G_LLM_NET_SEND_BUF"
        if probe not in data_symbols():
            print("  FAIL: probe symbol %s is no longer in the manifest's data section" % probe)
            fails += 1
        open(os.path.join(wd, "named.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { auto *b = (unsigned char *)mh::addr::%s; (void)b; } }\n"
            % probe
        )
        hits = scan_dir(wd, binder="__none__")
        if not any(is_named(k) for _, _, k, _ in hits):
            print("  FAIL: a named state VA (mh::addr::%s) was NOT caught" % probe)
            fails += 1
        else:
            print("  ok: a named state VA is caught")

        # ... and a CODE symbol under the same namespace must NOT be, or the call shim breaks.
        os.remove(os.path.join(wd, "named.cpp"))
        open(os.path.join(wd, "named_code.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { void f() { auto p = (void *)mh::addr::llm_strat_time_tick; "
            "(void)p; } }\n"
        )
        if scan_dir(wd, binder="__none__"):
            print("  FAIL: mh::addr::<code symbol> was flagged -- that is call-shim territory")
            fails += 1
        else:
            print("  ok: a named CODE VA is not flagged")

        # THE RATCHET, both directions, on synthetic counts -- never the live tree.
        fake = [
            (
                "libmh/sim/",
                "src/mh_dll/libmh/sim/x.cpp",  # CITATION-OK
                3,
                "named state VA mh::addr::A",
                "",
            ),
            (
                "libmh/sim/",
                "src/mh_dll/libmh/sim/x.cpp",  # CITATION-OK
                9,
                "named state VA mh::addr::B",
                "",
            ),
            (
                "libmh/sim/",
                "src/mh_dll/libmh/sim/y.h",  # CITATION-OK
                1,
                "named state VA mh::addr::C",
                "",
            ),
        ]
        probs, ok = check_named(fake, baseline={"src/mh_dll/libmh/sim/x.cpp": 2})  # CITATION-OK
        if ok or len(probs) != 1 or "y.h" not in probs[0]:
            print("  FAIL: a file absent from the baseline must fail on its implicit 0")
            fails += 1
        else:
            print("  ok: a file absent from the baseline fails on its implicit 0")
        probs, ok = check_named(
            fake,
            baseline={
                "src/mh_dll/libmh/sim/x.cpp": 2,  # CITATION-OK
                "src/mh_dll/libmh/sim/y.h": 1,  # CITATION-OK
            },
        )
        if not ok:
            print("  FAIL: at baseline everywhere must be clean (over-refusal arm)")
            fails += 1
        else:
            print("  ok: at baseline everywhere is clean")
        probs, ok = check_named(
            fake,
            baseline={
                "src/mh_dll/libmh/sim/x.cpp": 1,  # CITATION-OK
                "src/mh_dll/libmh/sim/y.h": 1,  # CITATION-OK
            },
        )
        if ok:
            print("  FAIL: a file growing past its baseline must fail")
            fails += 1
        else:
            print("  ok: a file growing past its baseline fails")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--report", action="store_true", help="per-module named-VA drain counts")
    ap.add_argument(
        "--update-baseline", action="store_true", help="tighten the named-VA ratchet (lowers only)"
    )
    args = ap.parse_args()

    if args.selftest:
        print("=== check_sim_addresses --selftest ===")
        fails = selftest()
        print("%d failure(s)" % fails)
        return 1 if fails else 0
    if args.update_baseline:
        update_baseline()
        return 0
    if args.report:
        return report()

    rc = 0
    summary = []
    for directory, binder, label in MODULES:
        if not os.path.isdir(directory):
            # libmh/tact/ did not exist before TACT0, and a module that is not there yet is not a
            # failure -- but it is reported, so "no violations" can never mean "nothing scanned".
            summary.append("%s absent (nothing to scan)" % label)
            continue
        # The named form is RATCHETED and reported below, not here -- 197 of them are legal today.
        found = [h for h in scan_dir(directory, binder) if not is_named(h[2])]
        binder_names = ", ".join(sorted({binder} if isinstance(binder, str) else binder))
        n_files = sum(
            1
            for _root, _dirs, files in os.walk(directory)
            for n in files
            if n.endswith((".cpp", ".h"))
        )
        if found:
            print("=== check_sim_addresses [%s]: %d violation(s) ===" % (label, len(found)))
            for name, ln, kind, line in found:
                print("  %s:%d  %s" % (name, ln, kind))
                print("      %s" % line[:140])
            print(
                "  Only %s may bind an address. Add a member + accessor there instead."
                % binder_names
            )
            rc = 1
        else:
            summary.append(
                "%s %d file(s), none names an address outside %s" % (label, n_files, binder_names)
            )

    # The ratcheted half: the named form, against tools/data/named_va_baseline.json.
    all_found = scan_modules()
    probs, ok = check_named(all_found)
    counts = named_counts(all_found)
    if not ok:
        print("=== check_sim_addresses: named state VA(s) over baseline ===")
        for p in probs:
            print("  %s" % p)
        print(
            "  This is SB-BIND's ratchet: it only ever tightens. If a drain LOWERED a count, run\n"
            "  `python tools/check_sim_addresses.py --update-baseline` in the same commit."
        )
        rc = 1
    if rc == 0:
        print("check_sim_addresses: ok -- " + "; ".join(summary))
        print(
            "  named state VAs: %d use(s) in %d file(s), all at or under baseline "
            "(SB-BIND drain target 0; --report for the breakdown)"
            % (sum(counts.values()), len(counts))
        )
    return rc


if __name__ == "__main__":
    sys.exit(main())
