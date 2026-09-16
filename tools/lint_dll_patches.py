#!/usr/bin/env python3
"""lint_dll_patches.py -- the STATIC half of the patch/seam interlock (C1).

The bug this prevents is a silence. A DLL byte patch and a promoted seam can target the same
function; because promotion only rewrites the entry's first 8 bytes, `patch_bytes_guarded` still sees
its expected bytes, still writes, and still reports "armed" -- into a body that will never execute.
The fix is inert and every log line says the opposite.

`hook/promoted.cpp` catches that at RUNTIME, which is necessary (some patch addresses are discovered
by a .text scan and cannot be enumerated ahead of time) but not sufficient: a runtime refusal is only
seen by whoever reads that run's log. This check makes the statically-known cases fail the BUILD.

Six checks, each independently reported:

  1. COMPLETENESS -- every `patch_bytes_guarded(...)` call site in the tree is registered in
     tools/data/dll_patch_manifest.json, keyed on (file, address expression). A new unregistered
     patch is a failure, which is what makes registration non-optional.
  2. NO STALE ENTRIES -- every registered {file, expr} still corresponds to a real call site.
  3. RESOLUTION -- every `kind: code` address resolves to exactly one function in
     tools/data/en_functions.json, and its derived owner matches the declared one. A Ghidra re-split
     or a moved boundary surfaces here as a diff rather than as a wrong assumption.
  4. DATA vs CODE -- every `kind: data` address resolves to NO function. A data patch landing inside
     a function body means one of the two labels is wrong.
  5. THE INTERLOCK -- any patch whose owner is PROMOTABLE (has an MH_EXPORT_REPLACE in the tree) must
     declare a `carrier`: how the same fix survives promotion. This is the check the item exists for.
     A carrier of "pending:<id>" is accepted but ANNOUNCED on every run -- it is the honest state while
     a migration is in flight, and an escape hatch that stopped announcing itself would be the very
     silence this file exists to break.
  6. DYNAMIC ENTRIES -- an entry that opts out of the address checks with `dynamic: true` must say why
     in `rationale`, so the exemption is a documented decision rather than a blank.
  7. THE DETOUR INTERLOCK (C9) -- the same collision, for the OTHER patching primitive. Checks 1-6
     see `patch_bytes_guarded` and nothing else, which is how D17 shipped: the resync-order clamp is
     an `install_trampoline` onto a function we promote, so the detour never armed, our body never
     carried the clamp, and every check here reported PASS. Any `install_trampoline`/`install_jmp`
     whose target resolves to a PROMOTABLE function must declare a carrier, exactly as a byte patch
     must.
  8. UNRESOLVED DETOUR TARGETS -- a detour whose target expression this file cannot resolve to a
     function (a table-driven installer, `t.addr` / `s.target` / `G[i].site`) must be registered
     `dynamic: true` with a `rationale`. Otherwise check 7 is silently blind wherever resolution
     fails, which would rebuild the original hole one indirection further out.

     THE D5 HOOK-POINT TABLE IS READ, NOT EXEMPTED (fork F3C). `mh/hook/hookpoint.cpp` arms every
     harness hook through one `install_trampoline(r.target, ...)` and one `install_jmp(r.target, ...)`
     over a TABLE of named points. Taking the easy road -- two `dynamic: true` rows -- would have
     turned fourteen statically-resolvable targets into two blanks, i.e. precisely the hole the
     paragraph above warns about. So this file parses the table and emits one synthetic detour site
     per armable row, carrying that row's own target expression; resolution, check 7 and the manifest
     keying then work exactly as they did when those expressions sat at fourteen separate call sites.
     A table it can no longer parse yields one UNRESOLVED site rather than none, so the failure is a
     red and not a silence.

WHY 7/8 DO NOT DEMAND A MANIFEST ROW FOR EVERY DETOUR SITE, unlike check 1 for byte patches: a
detour's target is a NAMED function (`mh::addr::x`, `mh::exp::addr_x`, or a `constexpr uintptr_t`
aliasing one), so this file can resolve it and answer the collision question WITHOUT being told. The
manifest exists to declare CARRIERS, and a detour that collides with nothing has no carrier to
declare. Registration is therefore required exactly where it carries information -- a real collision
(7) or a target we cannot see (8) -- which is 5 + 6 rows rather than 55 rows of `carrier: null`.

Usage: python tools/lint_dll_patches.py [-v]
"""

import argparse
import bisect
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(REPO, "src", "mh_dll")
MANIFEST = os.path.join(REPO, "tools", "data", "dll_patch_manifest.json")
FUNCS = os.path.join(REPO, "tools", "data", "en_functions.json")

CALL_RE = re.compile(r"patch_bytes_guarded\s*\(")
REPLACE_RE = re.compile(r"^\s*MH_EXPORT_REPLACE\(\s*([A-Za-z_]\w*)\s*,", re.M)

# ---- C9: the other patching primitive --------------------------------------------------------
DETOUR_RE = re.compile(r"\b(install_trampoline|install_jmp)\s*\(")
# `constexpr uintptr_t ADDR_X = 0x0049d8ef;`
HEXCONST_RE = re.compile(
    r"\b(?:constexpr|const)\s+(?:uintptr_t|uint32_t|unsigned\s+long|DWORD)\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)"
)
# `constexpr uintptr_t ADDR_X = mh::addr::llm_y;` -- the common form, and the useful one: it names
# the FUNCTION, so the collision question is answered without an address at all.
ALIASCONST_RE = re.compile(
    r"\b(?:constexpr|const)\s+uintptr_t\s+(\w+)\s*=\s*mh::(?:addr::(\w+)|exp::addr_(\w+))\s*;"
)
DIRECT_ADDR_RE = re.compile(r"^mh::(?:addr::(\w+)|exp::addr_(\w+))$")
HEX_LITERAL_RE = re.compile(r"^0x[0-9a-fA-F]+$")

# hook/ is where patch_bytes_guarded is DECLARED and DEFINED, not where it is used.
SKIP_FILES = ("mh/hook/patch.cpp", "mh/hook/patch.h")
# Same idea for the detour scan, plus one more: export.cpp's `install_jmp` IS the promotion, not a
# detour colliding with one. Flagging the promoter as its own collision would be a category error.
# THE PRIMITIVE ITSELF IS NOT A SITE. These files DECLARE, DEFINE or FORWARD install_trampoline /
# install_jmp rather than aiming one at an entry, so the scan would read each as an unresolvable
# detour whose target is a parameter -- five exemptions describing the same one promoter, which is
# the "rebuild the hole one indirection further out" failure check 8's docstring warns about, in
# reverse. The three F4D-PRE additions are the hook-service TABLE's three hops:
#   libmh/state/hook_api.h/.cpp     libmh's accessor -- the declaration and the `g_hook_api->` forward
#   mh/seams/libmh_hook_host.cpp mh.dll's forwarder onto the real mh::hook:: primitive
# Every REAL site still reaches the scan, because a site names an entry and these three name a
# parameter they were handed. save_live.cpp -- the one module caller, a PROMOTER -- keeps its own
# `detour_save_promote` manifest row and is deliberately NOT skipped.
SKIP_DETOUR_FILES = (
    "mh/hook/detour.cpp",
    "mh/hook/detour.h",
    "mh/hook/export.cpp",
    "libmh/state/hook_api.h",
    "libmh/state/hook_api.cpp",
    "mh/seams/libmh_hook_host.cpp",
)

# ---- D5: the NAMED HOOK POINTS, read as a table rather than as two unresolvable call sites -------
#
# hookpoint.cpp arms every harness hook through ONE `install_trampoline(r.target, ...)` and ONE
# `install_jmp(r.target, ...)`, `r` being a row of its TABLE. Left alone, that is exactly the hole
# check 8's docstring warns about -- "rebuild the original hole one indirection further out": fourteen
# statically-known targets would collapse into two `dynamic: true` exemptions and check 7 would stop
# answering the collision question for any of them, which is the question D17 was.
#
# So the table IS the call-site list here, and this file reads it. Each observe/replace row becomes a
# synthetic detour site carrying the row's own target EXPRESSION, so resolution, check 7 and the
# manifest keying all work exactly as they did when those expressions sat at fourteen separate sites.
# `register_only` rows hook no entry (target 0) and are skipped; the `neuter` row writes three bytes
# through no detour primitive at all, so it is out of this scan's subject the same way it always was.
HOOKPOINT_CPP = "mh/hook/hookpoint.cpp"
# `{"sim_tick", "the ... detour", mh::addr::llm_strat_sim_tick, shape::observe, ...}`
HOOKPOINT_ROW_RE = re.compile(
    r'\{\s*"(?P<id>\w+)"\s*,\s*"(?:[^"\\]|\\.)*"\s*,\s*(?P<target>[^,]+?)\s*,\s*shape::(?P<shape>\w+)\s*,'
)
HOOKPOINT_PRIMITIVE = {"observe": "install_trampoline", "replace": "install_jmp"}


def rel(path):
    return os.path.relpath(path, DLL).replace(os.sep, "/")


def strip_line_comments(src):
    """Blank out //-comments so a mention of the call in prose is not read as a call site."""
    out = []
    for line in src.split("\n"):
        i = line.find("//")
        out.append(line if i < 0 else line[:i])
    return "\n".join(out)


def call_sites():
    """[(file, expr, line)] for every real patch_bytes_guarded call in the DLL tree."""
    sites = []
    for dirpath, _dirs, files in os.walk(DLL):
        if "attic" in dirpath:
            continue
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".c", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            r = rel(path)
            if r in SKIP_FILES:
                continue
            src = strip_line_comments(open(path, encoding="utf-8", errors="replace").read())
            for m in CALL_RE.finditer(src):
                i = m.end()
                depth, j = 1, i
                while j < len(src) and depth:
                    c = src[j]
                    if c == "(":
                        depth += 1
                    elif c == ")":
                        depth -= 1
                    elif c == "," and depth == 1:
                        break
                    j += 1
                expr = " ".join(src[i:j].split())
                sites.append((r, expr, src.count("\n", 0, m.start()) + 1))
    return sites


def _first_arg(src, after):
    """The first call argument starting at `after`, whitespace-collapsed."""
    depth, j = 1, after
    while j < len(src) and depth:
        c = src[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 1:
            break
        j += 1
    return " ".join(src[after:j].split())


def address_aliases():
    """{constant name -> function name} for every `constexpr uintptr_t X = mh::addr::f;` in the tree.

    Two namespaces, because the seams use both: `mh::addr::<fn>` (the generated address header) and
    `mh::exp::addr_<fn>` (the export/thunk header). Either way what comes back is the FUNCTION NAME,
    which is what the collision question is actually about -- resolving to an integer and then back
    to a function through en_functions.json would add a second place to be stale for no gain.
    """
    alias, hexes = {}, {}
    for dirpath, _dirs, files in os.walk(DLL):
        if "attic" in dirpath:
            continue
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".c", ".h")):
                continue
            src = strip_line_comments(
                open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
            )
            for m in ALIASCONST_RE.finditer(src):
                alias.setdefault(m.group(1), m.group(2) or m.group(3))
            for m in HEXCONST_RE.finditer(src):
                hexes.setdefault(m.group(1), int(m.group(2), 16))
    return alias, hexes


def _resolve_target(expr, alias, hexes, owners):
    """Function name for a detour target EXPRESSION, or None when it cannot be resolved."""
    direct = DIRECT_ADDR_RE.match(expr)
    if direct:
        return direct.group(1) or direct.group(2)
    if expr in alias:
        return alias[expr]
    addr = hexes.get(expr)
    if addr is None and HEX_LITERAL_RE.match(expr):
        addr = int(expr, 16)
    if addr is None:
        return None
    row = owners.of(addr)
    return row["name"] if row else None


def _hookpoint_rows(src, r, alias, hexes, owners):
    """One synthetic detour site per armable row of the D5 hook-point table.

    REFUSES SILENCE: if the table cannot be found at all, a single unresolvable site is emitted so
    check 8 fires. A hookpoint.cpp whose table this regex stopped matching would otherwise contribute
    ZERO detour sites and read as a file that arms nothing -- the same absence-vs-nothing-to-find
    confusion the rest of this file exists to prevent.
    """
    out = []
    for m in HOOKPOINT_ROW_RE.finditer(src):
        prim = HOOKPOINT_PRIMITIVE.get(m.group("shape"))
        if not prim:
            continue  # register_only (no entry) / neuter (no detour primitive)
        expr = m.group("target").strip()
        out.append(
            (
                r,
                expr,
                src.count("\n", 0, m.start()) + 1,
                prim,
                _resolve_target(expr, alias, hexes, owners),
            )
        )
    if not out:
        out.append((r, "the D5 hook-point TABLE (unparseable)", 1, "install_trampoline", None))
    return out


def detour_sites(owners):
    """[(file, expr, line, primitive, target_function_or_None)] for every detour install.

    `target_function` is None when the expression cannot be resolved statically -- a table-driven
    installer indexing a struct (`t.addr`, `s.target`, `G[i].site`) or a parameter. Those are not
    ignored: check 8 makes each one declare itself.
    """
    alias, hexes = address_aliases()
    sites = []
    for dirpath, _dirs, files in os.walk(DLL):
        if "attic" in dirpath:
            continue
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".c", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            r = rel(path)
            if r in SKIP_DETOUR_FILES:
                continue
            src = strip_line_comments(open(path, encoding="utf-8", errors="replace").read())
            if r == HOOKPOINT_CPP:
                sites.extend(_hookpoint_rows(src, r, alias, hexes, owners))
                continue
            for m in DETOUR_RE.finditer(src):
                expr = _first_arg(src, m.end())
                name = _resolve_target(expr, alias, hexes, owners)
                sites.append((r, expr, src.count("\n", 0, m.start()) + 1, m.group(1), name))
    return sites


def promotable():
    """Every function the tree installs a C++ replacement for."""
    names = set()
    for dirpath, _dirs, files in os.walk(DLL):
        if "attic" in dirpath:
            continue
        for fn in files:
            if not fn.endswith((".cpp", ".c")):
                continue
            src = open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
            names.update(REPLACE_RE.findall(src))
    return names


class Owners:
    def __init__(self, path):
        doc = json.load(open(path, encoding="utf-8"))
        self.rows = doc["functions"]
        self.entries = [int(r["entry"], 16) for r in self.rows]

    def of(self, addr):
        i = bisect.bisect_right(self.entries, addr) - 1
        if i < 0:
            return None
        row = self.rows[i]
        return row if addr <= int(row["end"], 16) else None


def check_detours(manifest, prom, dsites, verbose=False, pending=None):
    """Checks 7 and 8 -- the detour primitive's half of the interlock (C9).

    Kept as its own function rather than folded into `check` because its registration rule is
    deliberately DIFFERENT: a byte patch must always be registered (check 1), a detour must be
    registered only where registration says something -- it collides, or we cannot see its target.
    """
    if pending is None:
        pending = []
    by_site = {}
    for p in manifest:
        for s in p.get("sources", []):
            by_site[(s["file"], s["expr"])] = p

    fail = []
    for f, expr, line, prim, name in dsites:
        p = by_site.get((f, expr))

        # 8. a target we cannot resolve must SAY so, or check 7 is blind here and nobody knows.
        if name is None:
            if not (p and p.get("dynamic")):
                fail.append(
                    f"UNRESOLVED DETOUR TARGET: {f}:{line} installs a {prim} onto `{expr}`, which "
                    f"this lint cannot resolve to a function -- so it CANNOT tell whether that "
                    f"target is promoted, and the C9 collision check is silently blind here. "
                    f"Register it in dll_patch_manifest.json with dynamic:true and a `rationale` "
                    f"saying what covers it instead (the runtime guard in hook/detour.cpp)."
                )
            elif not p.get("rationale"):
                fail.append(
                    f"UNJUSTIFIED DYNAMIC DETOUR: {p['id']} claims {f}:{line} with dynamic:true and "
                    f"no `rationale`. Say why the target cannot be resolved statically."
                )
            continue

        # 7. THE DETOUR INTERLOCK -- same collision as check 5, other primitive.
        if name not in prom:
            if p and p.get("carrier") and p["kind"] == "detour":
                fail.append(
                    f"STALE DETOUR CARRIER: {p['id']} declares a carrier for {f}:{line}, but its "
                    f"target {name} is no longer promoted by this tree. Drop the carrier or the row."
                )
            continue
        carrier = p.get("carrier") if p else None
        if not carrier:
            fail.append(
                f"DETOUR/SEAM COLLISION: {f}:{line} installs a {prim} onto the ENTRY of {name}, "
                f"which this tree PROMOTES (an MH_EXPORT_REPLACE installs our C++ body there). "
                f"Whichever arms second loses, and the loser's prologue guard reports it as "
                f"'unexpected prologue / already hooked' -- a wrong-build message from a good build, "
                f"because the guard fails for the RIGHT reason and reports the WRONG one. This is "
                f"exactly how the D14 clamp shipped inert for weeks. "
                f'Register it in dll_patch_manifest.json with kind:"detour" and a `carrier`: '
                f'"migrated:<ini key>" (our promoted body carries it), "rebind:<tracker id>" (the '
                f"C4/C6 protocol -- the detour keeps the entry and promotion rebinds its "
                f'fall-through), "pending:<tracker id>" (the fix is really lost and it is tracked), '
                f'or "none:<reason>".'
            )
        elif carrier.startswith("pending:"):
            pending.append(
                f"{p['id']} detours {name}, which is PROMOTED, and is NOT YET CARRIED ({carrier}). "
                f"Runs with that promotion on DO NOT HAVE this fix."
            )
        if verbose:
            print(f"  detour interlock: {f}:{line} -> promoted {name} -> carrier={carrier!r}")

    if verbose:
        resolved = sum(1 for d in dsites if d[4] is not None)
        print(f"  {len(dsites)} detour sites, {resolved} resolved to a function")
    return fail


def check(manifest, owners, prom, sites, verbose=False, pending=None):
    if pending is None:
        pending = []
    # kind:"detour" rows belong to check_detours and are invisible here -- otherwise check 2 would
    # report every one of them as a STALE MANIFEST ENTRY for having no patch_bytes_guarded site.
    manifest = [p for p in manifest if p.get("kind") != "detour"]
    registered = {}
    for p in manifest:
        for s in p.get("sources", []):
            registered.setdefault((s["file"], s["expr"]), []).append(p["id"])

    fail = []

    # 1. completeness
    seen = set()
    for f, expr, line in sites:
        key = (f, expr)
        seen.add(key)
        if key not in registered:
            fail.append(
                f"UNREGISTERED PATCH: {f}:{line} patches `{expr}` but no entry in "
                f"dll_patch_manifest.json claims it. Add one -- an unregistered patch cannot be "
                f"checked against the promoted-seam set, which is the whole point of the manifest."
            )

    # 2. stale entries
    for key, ids in sorted(registered.items()):
        if key not in seen:
            fail.append(
                f"STALE MANIFEST ENTRY: {ids} declares a patch at `{key[1]}` in {key[0]}, but no such "
                f"patch_bytes_guarded call site exists any more."
            )

    for p in manifest:
        pid = p["id"]
        declared = p.get("owner") or []
        if isinstance(declared, str):
            declared = [declared]

        # 6. dynamic entries must justify the exemption
        if p.get("dynamic"):
            if not p.get("rationale"):
                fail.append(
                    f"UNJUSTIFIED DYNAMIC ENTRY: {pid} sets dynamic:true (opting out of the address "
                    f"checks) with no `rationale`. Say why the addresses cannot be listed and what "
                    f"covers them instead."
                )
            continue

        derived = []
        for a in p.get("addrs", []):
            addr = int(a, 16)
            row = owners.of(addr)
            if p["kind"] == "code":
                # 3. resolution
                if row is None:
                    fail.append(
                        f"UNRESOLVED CODE PATCH: {pid} targets {a}, which is inside no function in "
                        f"en_functions.json. Either the address is wrong or the dump is stale."
                    )
                    continue
                derived.append(row["name"])
            else:
                # 4. data must not be inside a function
                if row is not None:
                    fail.append(
                        f"DATA PATCH INSIDE CODE: {pid} is declared kind:data but {a} lies inside "
                        f"{row['name']} ({row['entry']}..{row['end']}). One of the two labels is wrong."
                    )

        if p["kind"] == "code" and sorted(set(derived)) != sorted(set(declared)):
            fail.append(
                f"OWNER MISMATCH: {pid} declares owner {sorted(set(declared))} but its addresses "
                f"resolve to {sorted(set(derived))}. A Ghidra re-split or a moved boundary."
            )

        # 5. THE INTERLOCK
        for name in sorted(set(derived)):
            if name not in prom:
                continue
            carrier = p.get("carrier")
            if not carrier:
                fail.append(
                    f"PATCH/SEAM COLLISION: {pid} patches bytes inside {name}, which this tree "
                    f"PROMOTES (an MH_EXPORT_REPLACE installs our C++ body over its entry). Those "
                    f"bytes will never execute, so the fix is silently lost while the arming log "
                    f"still says 'armed'. Declare `carrier` on {pid}: "
                    f'"migrated:<ini key>" (the fix is a branch in our reimplemented body behind the '
                    f'same knob), "pending:<tracker id>" (the collision is '
                    f'real and tracked but not yet migrated), or "none:<reason>".'
                )
            elif carrier.startswith("pending:"):
                # NOT a failure, and NOT silent either. "pending" is the honest state while a
                # migration is in flight: the fix really is displaced, nothing carries it yet, and
                # saying "none:<reason>" instead would assert a deliberate decision that was never
                # made. It is reported on every run so the gap cannot decay into the status quo.
                pending.append(
                    f"{pid} is displaced by promoted {name} and NOT YET CARRIED ({carrier}). Runs with "
                    f"that promotion on do not have this fix."
                )
            if verbose:
                print(f"  interlock: {pid} inside promoted {name} -> carrier={carrier!r}")

    if verbose:
        print(
            f"  {len(sites)} call sites, {len(manifest)} manifest entries, {len(prom)} promotable functions"
        )
    return fail


# --- the negative case, kept executable ------------------------------------------------------------
#
# C1's acceptance test is "introduce a collision deliberately and watch the lint go red", and the bug
# being prevented is SILENCE -- so a check that has quietly stopped firing is the same failure wearing
# a different hat. Asserting the red path here rather than doing it by hand once means the guarantee
# survives every later edit to this file.
#
# Each case is (name, what it mutates, the substring the failure must contain).


def _blank_first_carrier(m, s):
    """Clear the carrier of the first patch that has one, so the collision check must fire.

    Deliberately id-free: a fixture that names a patch turns into a no-op the day that patch is
    retired, and a no-op mutation makes the case report DID NOT FIRE for a reason that has nothing
    to do with the check it is guarding. Raises rather than returning an unmutated manifest, because
    "nothing to mutate" must not read as "the check is broken".
    """
    for i, p in enumerate(m):
        if p.get("carrier"):
            return [dict(q, carrier=None) if j == i else q for j, q in enumerate(m)]
    raise AssertionError(
        "no patch in the manifest carries a `carrier`, so this negative case cannot construct a "
        "collision -- the fixture needs rethinking, not silencing"
    )


SELFTESTS = [
    (
        "collision: a patch inside a promoted body with no carrier",
        # SELECTED, NOT NAMED, since 2026-07-30 (C8-e). This case used to name `resync_trigger_gate`
        # -- and when C8-e retired that patch the mutation stopped mutating anything, so the lint had
        # nothing to go red about and the case reported DID NOT FIRE. That is precisely the failure
        # this whole SELFTESTS block exists to catch, and it caught itself, which is the good outcome;
        # the bad one is a fixture pinned to an id that a later deletion silently empties.
        # So: pick whatever patch currently has a carrier, and blank it. If the tree ever has no such
        # patch the mutation is a no-op again -- hence the explicit guard below rather than a quiet
        # pass.
        _blank_first_carrier,
        None,
        "PATCH/SEAM COLLISION",
    ),
    (
        "collision: a NEW patch placed inside a promoted body",
        lambda m, s: (
            m
            + [
                {
                    "id": "_selftest_new",
                    "sources": [
                        {"file": "mh/seams/net_lockstep.cpp", "expr": "ADDR_OVL_WAIT_CALL"}
                    ],
                    # 0x0049c400 is inside llm_net_lockstep_dispatch, which the tree promotes.
                    "addrs": ["0x0049c400"],
                    "kind": "code",
                    "owner": ["llm_net_lockstep_dispatch"],
                    "knob": None,
                    "carrier": None,
                }
            ]
        ),
        None,
        "PATCH/SEAM COLLISION",
    ),
    (
        "completeness: an unregistered patch call site",
        None,
        lambda s: s + [("mh/seams/_selftest.cpp", "0xdeadbeef", 1)],
        "UNREGISTERED PATCH",
    ),
    (
        "resolution: a code patch that is inside no function",
        lambda m, s: [
            dict(p, addrs=["0x00401001"]) if p["id"] == "resync_wait_fix" else p for p in m
        ],
        None,
        "UNRESOLVED CODE PATCH",
    ),
    (
        "labels: a data patch that is really inside a function",
        lambda m, s: [
            dict(p, addrs=["0x0049c400"]) if p["id"] == "browser_row_count_fmt" else p for p in m
        ],
        None,
        "DATA PATCH INSIDE CODE",
    ),
    (
        "ownership: a declared owner that no longer owns the address",
        lambda m, s: [
            dict(p, owner=["llm_strat_sim_tick"]) if p["id"] == "resync_wait_fix" else p for p in m
        ],
        None,
        "OWNER MISMATCH",
    ),
    (
        "exemption: dynamic:true with no rationale",
        lambda m, s: [
            {k: v for k, v in p.items() if k != "rationale"}
            if p["id"] == "video_view_array_reloc"
            else p
            for p in m
        ],
        None,
        "UNJUSTIFIED DYNAMIC ENTRY",
    ),
    (
        "staleness: a manifest entry whose call site is gone",
        None,
        lambda s: [x for x in s if x[1] != "ADDR_RESYNC_STUB_RET"],
        "STALE MANIFEST ENTRY",
    ),
]


def _drop_first_detour_carrier(m, d):
    """Clear the carrier of the first kind:"detour" row that has one, so check 7 must fire.

    Id-free for the same reason `_blank_first_carrier` is: a fixture pinned to an id turns into a
    no-op the day that row is retired, and a no-op mutation reports DID NOT FIRE for a reason that
    has nothing to do with the check being guarded.
    """
    for i, p in enumerate(m):
        if p.get("kind") == "detour" and p.get("carrier"):
            return [dict(q, carrier=None) if j == i else q for j, q in enumerate(m)]
    raise AssertionError(
        "no kind:\"detour\" row in the manifest carries a `carrier`, so this negative case cannot "
        "construct a collision -- the fixture needs rethinking, not silencing"
    )


def _unpromote_carried_detours(m, d, prom):
    """Drop from the promotable set every function a CARRYING detour row targets.

    That makes each of those carriers stale by construction, which is what check 7's staleness arm
    is for: a carrier that outlives the promotion it was written against is a claim about a
    collision that no longer exists, and leaving it in place quietly misdescribes the build.
    """
    carried = {
        s["expr"]: p
        for p in m
        if p.get("kind") == "detour" and p.get("carrier")
        for s in p["sources"]
    }
    targets = {nm for (f, e, _l, _pr, nm) in d if nm and e in carried}
    if not targets:
        raise AssertionError(
            "no carrying detour row resolves to a promoted function, so this negative case cannot "
            "construct a stale carrier -- the fixture needs rethinking, not silencing"
        )
    return prom - targets


# Detour negative cases: (name, mutate_manifest, mutate_detour_sites, mutate_promotable, expected).
DETOUR_SELFTESTS = [
    (
        "detour collision: a detour onto a promoted entry with no carrier",
        _drop_first_detour_carrier,
        None,
        None,
        "DETOUR/SEAM COLLISION",
    ),
    (
        "detour collision: a NEW detour placed onto a promoted entry",
        None,
        lambda d: (
            d
            + [
                (
                    "mh/seams/_selftest.cpp",
                    "mh::addr::llm_net_lockstep_dispatch",
                    1,
                    "install_trampoline",
                    "llm_net_lockstep_dispatch",
                )
            ]
        ),
        None,
        "DETOUR/SEAM COLLISION",
    ),
    (
        "detour blindness: an unresolvable target that declares nothing",
        None,
        lambda d: d + [("mh/seams/_selftest.cpp", "tbl[i].site", 1, "install_trampoline", None)],
        None,
        "UNRESOLVED DETOUR TARGET",
    ),
    (
        "detour exemption: dynamic:true with no rationale",
        lambda m, d: [
            {k: v for k, v in p.items() if k != "rationale"}
            if p.get("kind") == "detour" and p.get("dynamic")
            else p
            for p in m
        ],
        None,
        None,
        "UNJUSTIFIED DYNAMIC DETOUR",
    ),
    (
        "detour staleness: a carrier for a target nothing promotes any more",
        None,
        None,
        _unpromote_carried_detours,
        "STALE DETOUR CARRIER",
    ),
]


def selftest(manifest, owners, prom, sites, dsites):
    """Assert every check still goes red on the defect it exists to catch."""
    bad = 0
    # A `pending:` carrier is the one non-failing outcome, so it needs its own assertion: accepted,
    # but LOUD. An escape hatch that stopped announcing itself would be the same silence again.
    seen_pending = []
    check(manifest, owners, prom, sites, pending=seen_pending)
    # Mirrors check()'s own filter: a kind:"detour" row's pending carrier is check_detours' to
    # report, and counting it here would assert the byte-patch reporter fired on someone else's row.
    has_pending = any(
        p.get("carrier", "").startswith("pending:")
        for p in manifest
        if p.get("carrier") and p.get("kind") != "detour"
    )
    if has_pending and not seen_pending:
        bad += 1
        print("  [DID NOT FIRE] pending: carriers are reported rather than silently accepted")
    elif has_pending:
        print(f"  [ok] pending: carriers are reported ({len(seen_pending)} announced)")
    for name, mutate_m, mutate_s, expect in SELFTESTS:
        m = mutate_m(manifest, sites) if mutate_m else manifest
        s = mutate_s(sites) if mutate_s else sites
        fails = check(m, owners, prom, s)
        hit = any(expect in f for f in fails)
        print(f"  [{'ok' if hit else 'DID NOT FIRE'}] {name}")
        if not hit:
            bad += 1
            print(f"        expected a failure containing {expect!r}; got {fails or 'nothing'}")
    # ---- C9: the detour half, same discipline ----------------------------------------------------
    dpending = []
    check_detours(manifest, prom, dsites, pending=dpending)
    has_dpending = any(
        p.get("carrier", "").startswith("pending:")
        for p in manifest
        if p.get("kind") == "detour" and p.get("carrier")
    )
    if has_dpending and not dpending:
        bad += 1
        print(
            "  [DID NOT FIRE] pending: detour carriers are reported rather than silently accepted"
        )
    elif has_dpending:
        print(f"  [ok] pending: detour carriers are reported ({len(dpending)} announced)")
    for name, mutate_m, mutate_d, mutate_p, expect in DETOUR_SELFTESTS:
        m = mutate_m(manifest, dsites) if mutate_m else manifest
        d = mutate_d(dsites) if mutate_d else dsites
        p = mutate_p(m, d, prom) if mutate_p else prom
        fails = check_detours(m, p, d)
        hit = any(expect in f for f in fails)
        print(f"  [{'ok' if hit else 'DID NOT FIRE'}] {name}")
        if not hit:
            bad += 1
            print(f"        expected a failure containing {expect!r}; got {fails or 'nothing'}")

    # The mutations must be the ONLY reason it goes red, or a check could be "passing" on the back of
    # an unrelated pre-existing failure.
    base = check(manifest, owners, prom, sites) + check_detours(manifest, prom, dsites)
    if base:
        bad += 1
        print(
            f"  [FAIL] the unmutated tree is already red ({len(base)} problem(s)) -- selftest is meaningless"
        )
    ncases = len(SELFTESTS) + len(DETOUR_SELFTESTS)
    print(f"lint_dll_patches selftest: {'PASS' if not bad else 'FAIL'} ({ncases} cases, {bad} bad)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument(
        "--selftest", action="store_true", help="assert every check still goes red on its defect"
    )
    args = ap.parse_args()

    manifest = json.load(open(MANIFEST, encoding="utf-8"))["patches"]
    owners = Owners(FUNCS)
    prom = promotable()
    sites = call_sites()
    dsites = detour_sites(owners)

    if args.selftest:
        return selftest(manifest, owners, prom, sites, dsites)

    pending = []
    fail = check(manifest, owners, prom, sites, args.verbose, pending)
    fail += check_detours(manifest, prom, dsites, args.verbose, pending)
    for f in fail:
        print("lint_dll_patches: " + f)
    for w in pending:
        print("lint_dll_patches: PENDING MIGRATION: " + w)
    tail = f", {len(pending)} pending migration(s)" if pending else ""
    print(f"lint_dll_patches: {'PASS' if not fail else 'FAIL'} ({len(fail)} problem(s){tail})")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
