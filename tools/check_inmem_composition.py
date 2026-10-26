#!/usr/bin/env python3
"""check_inmem_composition.py -- fork F4C-COMP: the static-patch/DLL composition census.

WHAT THIS KEEPS DERIVABLE. The Q6 ruling rests on a number -- "24% of the static-patch sites fall
inside a function some other mechanism claims" -- and a number in prose is a number nobody
re-measures. src/patcher holds 46,294 sites across 40 manifests; src/mh_dll promotes 401 bodies and
detours 56 entries; both move every week. So the census is RE-DERIVED here from committed inputs
only (no Ghidra, no rig, no build), written to a committed extract, and `--check` requires
byte-identity -- a manifest edit, a new MH_EXPORT_REPLACE or a new install_trampoline call site all
surface as a reviewable diff in that file instead of quietly invalidating the ruling.

---- THE FOUR INPUTS, AND WHY EACH IS THE ONE THE RUNTIME ACTUALLY USES ---------------------------

  src/patcher/*.mh.patch.json            every site VA (and its byte length, where it has one)
  tools/data/en_functions.json           EN function extents -- which BODY a site is in
  mh/addr/mh_patches.gen.h               `promotable_ranges` (class P) and `verified_ranges`
  src/mh_dll/mh/**                       the static entry-detour targets (class D), resolved out of
                                         install_jmp / install_trampoline call sites and the
                                         hook-point table through mh_addrs.gen.h / mh_export.gen.h

CONTAINMENT IS STRICT: `entry <= va <= end`, the exact predicate mh::hook::promoted_owner_of
evaluates at runtime. The loose reading (nearest preceding function owns everything after it) gives
10,323 instead of 10,127 for class P, and 468 sites lie in NO body at all -- data, and the gaps
Watcom leaves between translation units. Attributing those to a neighbour would inflate the census
by exactly the sites no interlock can ever fire on.

---- THE CLASSES, AND THE DISPOSITION EACH CARRIES (docs/inmem-patching.md) -----------------------

  P  inside a PROMOTABLE body        REFUSE when that body is promoted in the run. Already the
                                     applier's behaviour since F1E (hook/promoted.h C1), and the
                                     only class whose refusal needed no new mechanism.
  D  inside a STATIC-DETOUR TARGET   REFUSE the detour when it would kill the bytes -- a jmp
                                     install anywhere in the body, or a trampoline whose stolen
                                     entry window the site overlaps. Otherwise ALLOW: a trampoline
                                     steals 8 bytes and the body runs on, so a site past them
                                     composes. `entry_window` below is that collidable subset, and
                                     it is 93 of the 11,034 -- which is the whole argument for not
                                     refusing the class wholesale.
  V  inside a VERIFIED-but-not-      ALLOW with registration. A migration-ledger body that this
     promotable body                 build cannot promote is an original body like any other.

---- THE ARM THE RUNTIME INTERLOCK DELIBERATELY DOES NOT COVER ------------------------------------

`patch_bytes_guarded` writes at arbitrary addresses, not at entries, so a byte-exact overlap between
a registered DLL patch and a manifest site is not answerable from the body-granular registry -- and
it does not have to be, because it is decidable HERE, exactly, offline.

FULL EXTENTS (PT-CAP8 S0, 2026-10-19). The first version of this arm tested only a DLL patch's START
address against a site's extent. A manifest site is a bare 4-byte operand carved out of an
instruction; the DLL patch that replaces that instruction starts EARLIER and runs over it, so the
start-address test saw nothing: six patches the shipped hooks depend on (turret_clip_fix,
turret_clip_refill, mother_free_seed, move_near_enough, move_squad_merge_fix, move_reverse_now)
overlap the grand cap raise and the census said zero. Now every registered DLL patch carries its
byte length (`lens`, parallel to `addrs`, in dll_patch_manifest.json) and the test is interval
intersection, classified start / interior / contains / straddle. A row that has `addrs` but no
`lens` is REFUSED -- a missing length reads as 'no overlap'. `--verify-lens --exe PATH` proves each
extent ends on an instruction boundary of the clean EN exe (local only; needs the binary).
`cap_set_overlaps` is the subset against the v0.2.2 cap manifests.

usage:
  python tools/check_inmem_composition.py            # re-derive and WRITE the extract
  python tools/check_inmem_composition.py --check    # re-derive and require byte-identity (lint)
  python tools/check_inmem_composition.py --selftest # the derivation's own negatives
  python tools/check_inmem_composition.py --verify-lens [--exe PATH]  # lens end on instruction boundaries
"""

import argparse
import bisect
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PATCHER = os.path.join(REPO, "src", "patcher")
DLL = os.path.join(REPO, "src", "mh_dll")
EN_FUNCTIONS = os.path.join(REPO, "tools", "data", "en_functions.json")
PATCHES_HEADER = os.path.join(DLL, "mh", "addr", "mh_patches.gen.h")
DLL_PATCH_MANIFEST = os.path.join(REPO, "tools", "data", "dll_patch_manifest.json")
EXTRACT = os.path.join(REPO, "tools", "data", "inmem_composition_census.json")

# The v0.2.2 cap manifests are picked up by name (cap_v022_reloc_EN + cap_v022_limits_EN). The July
# grand + pool-caphike manifests are the generator's INPUT now and no longer part of the cap set; their
# overlaps stay visible in `registered_patch_overlaps`.
CAP_MANIFESTS = ()
# PT-CAP9-U1: the v0.2.3 units/soldiers pair joins the cap set (cap_v023_units_reloc_EN + cap_v023_units_limits_EN).
CAP_PREFIX = ("cap_v022_", "cap_v023_")
ALLOW = os.path.join(REPO, "tools", "data", "cap_compose_allow.json")
GUARD_HELPER = "patch_bytes_guarded_caps"

# The widest entry steal any install in this tree takes; must equal ENTRY_WINDOW in
# mh/hook/promoted.cpp, and assertion 5 below checks that it does rather than trusting the comment.
ENTRY_WINDOW = 8

ADDR_HEADERS = [
    os.path.join(DLL, "mh", "addr", "mh_addrs.gen.h"),
    os.path.join(DLL, "mh", "addr", "mh_export.gen.h"),
]

CONST_RE = re.compile(r"constexpr\s+uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)u?\s*;")
ALIAS_RE = re.compile(r"constexpr\s+uintptr_t\s+(\w+)\s*=\s*([^;]+);")
INSTALL_RE = re.compile(r"\b(?:install_jmp|install_trampoline)\s*\(\s*([^,)]+)")
# A hook-point row: {"name", "description", <target>, shape::..., ...}
HOOKROW_RE = re.compile(r'\{"(\w+)",\s*"[^"]*",\s*([^,]+),')


class Refusal(Exception):
    pass


def rel(path):
    """os.path.relpath that survives a temp tree on another drive (the --selftest case, where
    Windows puts TEMP on C: and the repo is on F: -- relpath RAISES across mounts)."""
    try:
        return os.path.relpath(path, REPO)
    except ValueError:
        return path


# ---- inputs --------------------------------------------------------------------------------------


def function_extents(path=EN_FUNCTIONS):
    """-> [(entry, end_inclusive, name)] sorted by entry."""
    doc = json.load(open(path, encoding="utf-8"))
    return sorted((int(r["entry"], 16), int(r["end"], 16), r["name"]) for r in doc["functions"])


def body_finder(extents):
    starts = [e[0] for e in extents]

    def find(va):
        i = bisect.bisect_right(starts, va) - 1
        if i < 0 or va > extents[i][1]:
            return None
        return extents[i]

    return find


def sites(patcher=PATCHER):
    """-> [(manifest, va, len)] for every patch carrying a `va`.

    `len` is 0 for the one `asm` patch in the corpus, which has no assembled bytes in the file. It
    still COUNTS as a site -- it has a VA and it claims a body -- but it contributes no extent, so
    no overlap test can fire on it. Dropping it instead would make the census total disagree with
    the manifest audit's 46,294 for a reason no reader could reconstruct.
    """
    out = []
    for path in sorted(glob.glob(os.path.join(patcher, "*.mh.patch.json"))):
        name = os.path.basename(path)[: -len(".mh.patch.json")]
        for p in json.load(open(path, encoding="utf-8")).get("patches", []):
            if "va" not in p:
                continue
            raw = p.get("bytes", "")
            out.append((name, int(str(p["va"]), 16), len(bytes.fromhex(raw.replace(" ", "")))))
    return out


def named_table(header, table):
    """The function NAMES in one `inline constexpr ... <table>[] = { {"name", ... }, ... };`."""
    text = open(header, encoding="utf-8").read()
    m = re.search(r"%s\[\] = \{(.*?)\n\};" % re.escape(table), text, re.S)
    if not m:
        raise Refusal(
            "%s does not contain a `%s[] = {...};` table. It is GENERATED "
            "(tools/gen_dll_patches.py) -- if its shape changed, this parser has to change with it, "
            "because an empty class P would read as 'no overlap' rather than as 'not looking'."
            % (rel(header), table)
        )
    return set(re.findall(r'\{"([A-Za-z_]\w*)",', m.group(1)))


def address_constants(headers=None):
    consts = {}
    for h in headers or ADDR_HEADERS:
        for m in CONST_RE.finditer(open(h, encoding="utf-8").read()):
            consts[m.group(1)] = int(m.group(2), 16)
    return consts


def _resolve(expr, local, consts):
    a = expr.strip().rstrip("u")
    a = re.sub(r"^\((?:uintptr_t|void\s*\*)\)", "", a).strip()
    if re.fullmatch(r"0x[0-9a-fA-F]+", a):
        return int(a, 16)
    key = a.split("::")[-1]
    return local.get(key) or consts.get(key)


def detour_targets(root=None, consts=None):
    """-> {va: "the source file that installs there"} for every STATICALLY addressed entry detour.

    Two shapes, and both are needed: the ~60 hand-written install_jmp / install_trampoline call
    sites in mh/seams, mh/ui and friends, and the hook-point TABLE (mh/hook/hookpoint.cpp), which is
    where F3C moved the harness's fifteen. The primitives' own definitions in mh/hook/detour.cpp are
    skipped -- their `target` parameter is not an address -- as is anything under attic/.

    A call site whose first argument does not resolve to a literal (a table row, a loop variable, a
    parameter) is simply not STATIC and contributes nothing; that is a real limit of the census and
    it is stated in the extract as `unresolved_install_sites` rather than hidden, because a rising
    count there means the census is measuring progressively less of the tree.
    """
    root = root or os.path.join(DLL, "mh")
    consts = address_constants() if consts is None else consts
    found, unresolved = {}, 0
    for dirpath, _dirs, files in os.walk(root):
        rel_dir = dirpath.replace(os.sep, "/")
        if "/attic" in rel_dir:
            continue
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            relp = rel(path).replace(os.sep, "/")
            src = open(path, encoding="utf-8", errors="replace").read()
            local = {}
            for m in ALIAS_RE.finditer(src):
                v = _resolve(m.group(2), local, consts)
                if v:
                    local[m.group(1)] = v
            in_hook = "/mh/hook/" in relp
            if not in_hook or relp.endswith("hookpoint.cpp"):
                for m in INSTALL_RE.finditer(src):
                    v = _resolve(m.group(1), local, consts)
                    if v:
                        found.setdefault(v, relp)
                    else:
                        unresolved += 1
            if relp.endswith("hookpoint.cpp"):
                for m in HOOKROW_RE.finditer(src):
                    v = _resolve(m.group(2), local, consts)
                    if v:
                        found.setdefault(v, relp)
    return found, unresolved


def promoted_entry_window_bytes(path=None):
    """ENTRY_WINDOW as the APPLIER declares it, parsed out of mh/hook/promoted.cpp."""
    path = path or os.path.join(DLL, "mh", "hook", "promoted.cpp")
    m = re.search(
        r"constexpr\s+int\s+ENTRY_WINDOW\s*=\s*(\d+)\s*;", open(path, encoding="utf-8").read()
    )
    if not m:
        raise Refusal(
            "mh/hook/promoted.cpp no longer declares `constexpr int ENTRY_WINDOW` -- the census "
            "cannot restate a constant it cannot read, and a hardcoded copy is how the two drift."
        )
    return int(m.group(1))


# ---- DLL patch extents and the full-extent overlap test -------------------------------------------------


def dll_patch_extents(path=DLL_PATCH_MANIFEST):
    """-> ([(patch_id, addr, len)], [dynamic patch ids]).

    REFUSES a row that has `addrs` and no (or a mis-sized) `lens`: a length that is not stated would
    be read by the overlap test as zero, i.e. as 'cannot overlap'."""
    out, dynamic = [], []
    for p in json.load(open(path, encoding="utf-8"))["patches"]:
        addrs = p.get("addrs") or []
        if not addrs:
            if p.get("dynamic"):
                dynamic.append(p["id"])
            continue
        lens = p.get("lens")
        if (
            not isinstance(lens, list)
            or len(lens) != len(addrs)
            or not all(isinstance(n, int) and n > 0 for n in lens)
        ):
            raise Refusal(
                "dll_patch_manifest.json row %r has %d addr(s) but lens=%r -- every address needs its write "
                "length (tools/check_inmem_composition.py docstring, FULL EXTENTS)."
                % (p["id"], len(addrs), lens)
            )
        for a, n in zip(addrs, lens):
            out.append((p["id"], int(a, 16), n))
    return out, dynamic


def classify_overlap(d_lo, d_hi, s_lo, s_hi):
    """How a DLL patch [d_lo,d_hi) meets a manifest site [s_lo,s_hi); None when they are disjoint."""
    if d_hi <= s_lo or s_hi <= d_lo:
        return None
    if d_lo == s_lo:
        return "start"
    if s_lo < d_lo and d_hi <= s_hi:
        return "inside_site"
    if d_lo < s_lo and s_hi <= d_hi:
        return "interior"  # the site lies wholly inside the DLL patch (the grand/hook shape)
    return "straddle"


def full_extent_overlaps(dll_extents, rows):
    """Interval intersection of every DLL patch against every site (manifest, va, len). rows with
    len 0 (the one `asm` patch) have no extent and cannot overlap."""
    ext = sorted((va, va + ln, manifest) for manifest, va, ln in rows if ln)
    starts = [e[0] for e in ext]
    maxhi, m = [], 0
    for e in ext:
        m = max(m, e[1])
        maxhi.append(m)
    out = []
    for pid, addr, ln in dll_extents:
        lo, hi = addr, addr + ln
        i = bisect.bisect_left(starts, hi) - 1
        while i >= 0 and maxhi[i] > lo:
            s_lo, s_hi, mname = ext[i]
            rel = classify_overlap(lo, hi, s_lo, s_hi)
            if rel:
                out.append(
                    {
                        "patch": pid,
                        "addr": "0x%08x" % addr,
                        "len": ln,
                        "manifest": mname,
                        "site": "0x%08x" % s_lo,
                        "site_len": s_hi - s_lo,
                        "relation": rel,
                    }
                )
            i -= 1
    return sorted(out, key=lambda r: (r["patch"], r["addr"], r["manifest"], r["site"]))


def excluded_embeds(data=os.path.join(REPO, "tools", "data")):
    """-> [(generator cache, va)] for every relocated-array dword a cap manifest generator EXCLUDED on purpose (its census class
    `excluded`): a dword that a DLL patch carries inside its own bytes and rebuilds from the live base (PT-CAP9-U5: the O1c local-decode
    carrier embeds the pending base). Such a dword is in NO manifest, so the site-extent intersection above can never see a patch that
    EMBEDS it -- this is the arm that does."""
    out = []
    for path in sorted(glob.glob(os.path.join(data, "cap_*_sites.json"))):
        doc = json.load(open(path, encoding="utf-8"))
        for r in doc.get("census", []):
            if r.get("cls") == "excluded":
                out.append((os.path.basename(path), int(r["va"])))
    return out


def embedded_overlaps(dll_extents, embeds):
    """Every excluded dword [va, va+4) that lies inside a registered DLL patch extent -> rows {patch, addr, len, cache, va}."""
    out = []
    for pid, addr, ln in dll_extents:
        for cache, va in embeds:
            if addr <= va and va + 4 <= addr + ln:
                out.append(
                    {
                        "patch": pid,
                        "addr": "0x%08x" % addr,
                        "len": ln,
                        "cache": cache,
                        "va": "0x%08x" % va,
                    }
                )
    return sorted(out, key=lambda r: (r["patch"], r["addr"], r["va"]))


# ---- the census ----------------------------------------------------------------------------------


def census():
    extents = function_extents()
    find = body_finder(extents)
    P = named_table(PATCHES_HEADER, "promotable_ranges")
    V = named_table(PATCHES_HEADER, "verified_ranges")
    targets, unresolved = detour_targets()
    D = sorted({find(va)[2] for va in targets if find(va)})
    dset = set(D)
    window = promoted_entry_window_bytes()

    rows = sites()
    per_manifest, window_rows = {}, []
    n_p = n_d = n_union = n_v_only = n_orphan = 0
    by_fn = {}
    for manifest, va, ln in rows:
        slot = per_manifest.setdefault(
            manifest, {"sites": 0, "promotable": 0, "detour_target": 0, "either": 0, "orphan": 0}
        )
        slot["sites"] += 1
        b = find(va)
        if b is None:
            n_orphan += 1
            slot["orphan"] += 1
            continue
        entry, _end, fname = b
        in_p, in_d = fname in P, fname in dset
        n_p += in_p
        n_d += in_d
        slot["promotable"] += in_p
        slot["detour_target"] += in_d
        if fname in V and fname not in P:
            n_v_only += 1
        if in_p or in_d:
            n_union += 1
            slot["either"] += 1
            by_fn[fname] = by_fn.get(fname, 0) + 1
            # The collidable subset: does this site's extent reach into the bytes an entry install
            # overwrites? `ln == 0` (the asm patch) has no extent and so cannot.
            if ln and va + ln > entry and va < entry + window:
                window_rows.append(
                    {"manifest": manifest, "va": "0x%08x" % va, "len": ln, "fn": fname}
                )

    # The byte-exact arm the runtime registry cannot answer: FULL-extent intersection (see the docstring).
    dll_ext, dynamic = dll_patch_extents()
    overlaps = full_extent_overlaps(dll_ext, rows)
    cap_overlaps = [
        r
        for r in overlaps
        if r["manifest"] in CAP_MANIFESTS or r["manifest"].startswith(CAP_PREFIX)
    ]

    return {
        "_generated_by": "tools/check_inmem_composition.py",
        "_what": (
            "The fork F4C-COMP composition census: how much of the src/patcher static-patch surface "
            "lands inside a function some other mh.dll mechanism claims. Re-derived from committed "
            "inputs only; `--check` requires byte-identity. Containment is STRICT (entry <= va <= "
            "end), the predicate mh::hook::promoted_owner_of evaluates at runtime."
        ),
        "entry_window_bytes": window,
        "sites_total": len(rows),
        "orphan_sites": n_orphan,
        "promotable_bodies": len(P),
        "verified_bodies": len(V),
        "detour_target_addrs": len(targets),
        "detour_target_bodies": len(D),
        "unresolved_install_sites": unresolved,
        "sites_in_promotable_body": n_p,
        "sites_in_detour_target": n_d,
        "sites_in_either": n_union,
        "sites_in_verified_not_promotable": n_v_only,
        "sites_in_entry_window_of_claimed_body": len(window_rows),
        "top_claimed_bodies": [
            {"fn": k, "sites": v}
            for k, v in sorted(by_fn.items(), key=lambda kv: (-kv[1], kv[0]))[:20]
        ],
        "entry_window_collisions": sorted(window_rows, key=lambda r: (r["manifest"], r["va"])),
        "dll_patch_extents": len(dll_ext),
        "dll_patches_dynamic_unchecked": dynamic,
        "registered_patch_overlaps": overlaps,
        "cap_set_overlaps": cap_overlaps,
        "embedded_reloc_overlaps": embedded_overlaps(dll_ext, excluded_embeds()),
        "detour_target_bodies_named": D,
    }


def render(doc):
    return json.dumps(doc, indent=2, sort_keys=False) + "\n"


# ---- the gate --------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="re-derive and require byte-identity")
    ap.add_argument("--selftest", action="store_true", help="the derivation's own negatives")
    ap.add_argument(
        "--verify-lens",
        action="store_true",
        help="lens end on instruction boundaries (needs the exe)",
    )
    ap.add_argument("--exe", help="clean EN mh.exe for --verify-lens")
    ap.add_argument(
        "--strict",
        action="store_true",
        help="also require every cap overlap to be allowed and to compose via patch_bytes_guarded_caps "
        "(red until PT-CAP8 S1 Slice C lands the hooks)",
    )
    args = ap.parse_args(argv)

    if args.verify_lens:
        return verify_lens(args.exe)

    if args.selftest:
        return selftest()

    try:
        doc = census()
    except Refusal as e:
        print("check_inmem_composition: REFUSED -- %s" % e)
        return 1
    text = render(doc)

    print(
        "%d site(s) over %d manifest(s): %d inside a promotable body, %d inside a static-detour "
        "target, %d in either (%d%%); %d of those reach an entry window."
        % (
            doc["sites_total"],
            len(glob.glob(os.path.join(PATCHER, "*.mh.patch.json"))),
            doc["sites_in_promotable_body"],
            doc["sites_in_detour_target"],
            doc["sites_in_either"],
            round(100.0 * doc["sites_in_either"] / max(1, doc["sites_total"])),
            doc["sites_in_entry_window_of_claimed_body"],
        )
    )
    print(
        "%d site(s) lie in no function body at all; %d registered DLL patch extent(s) overlap a manifest "
        "site (%d against the v0.2.2 cap manifests)."
        % (doc["orphan_sites"], len(doc["registered_patch_overlaps"]), len(doc["cap_set_overlaps"]))
    )

    if args.strict:
        bad = strict_failures(doc)
        for b in bad:
            print("  STRICT FAIL: %s" % b)
        print("check_inmem_composition --strict: %s" % ("FAIL (%d)" % len(bad) if bad else "ok"))
        if bad:
            return 1

    if not args.check:
        with open(EXTRACT, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print("wrote %s" % rel(EXTRACT))
        return 0

    if not os.path.isfile(EXTRACT):
        print("check_inmem_composition: MISSING %s -- run without --check" % rel(EXTRACT))
        return 1
    have = open(EXTRACT, encoding="utf-8").read()
    if have != text:
        print(
            "check_inmem_composition: DRIFT -- %s no longer matches the re-derivation.\n"
            "  The census is an INPUT to the Q6 ruling (docs/inmem-patching.md), so a change here "
            "is a change to how much of the static-patch surface is contested. Re-run without "
            "--check and review the diff." % rel(EXTRACT)
        )
        return 1
    print("ok: %s" % rel(EXTRACT))
    return 0


def verify_lens(exe=None):
    """Local-only: every registered code extent must END on an instruction boundary of the clean EN
    exe (the DLL guard's `expect` bytes are the vanilla instructions, so a wrong length shows up as a
    cut instruction). Data rows (kind: data) are skipped."""
    sys.path.insert(0, HERE)
    import scan_cap_reloc_sites as scan
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    exe = exe or scan.default_exe()
    if not exe:
        print("check_inmem_composition --verify-lens: no clean EN exe; pass --exe")
        return 1
    img = scan.Image(open(exe, "rb").read())
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    data_ids = {
        p["id"]
        for p in json.load(open(DLL_PATCH_MANIFEST, encoding="utf-8"))["patches"]
        if p.get("kind") != "code"
    }
    bad = 0
    ext, _dyn = dll_patch_extents()
    for pid, addr, ln in ext:
        if pid in data_ids:
            continue
        off = 0
        ok = False
        for ins in md.disasm(img.read(addr, ln + 16), addr):
            off += ins.size
            if off == ln:
                ok = True
                break
            if off > ln:
                break
        if not ok:
            bad += 1
            print(
                "  BAD LEN %s @0x%08x len %d does not end on an instruction boundary"
                % (pid, addr, ln)
            )
    print(
        "check_inmem_composition --verify-lens: %s (%d code extents)"
        % ("FAIL" if bad else "ok", len(ext))
    )
    return 1 if bad else 0


# ---- --strict: every cap overlap is allowed AND composes through the helper ----------------------------


def strict_failures(doc, check_source=True):
    """Each `cap_set_overlaps` row must be listed in cap_compose_allow.json (patch + site) and -- under
    `check_source` -- the patch's installing source must go through `patch_bytes_guarded_caps`, the
    guard that compares against the bytes ACTUALLY present (the applied manifest's relocated bytes)."""
    fails = []
    if not os.path.isfile(ALLOW):
        return ["%s is missing" % rel(ALLOW)]
    allow = json.load(open(ALLOW, encoding="utf-8"))["allowed"]
    ok = {(a["patch"], a["site"]) for a in allow}
    for r in doc["cap_set_overlaps"]:
        if (r["patch"], r["site"]) not in ok:
            fails.append(
                "overlap %s @%s x site %s is not in cap_compose_allow.json"
                % (r["patch"], r["addr"], r["site"])
            )
    emb_ok = {
        (a["patch"], a["va"]) for a in json.load(open(ALLOW, encoding="utf-8")).get("embedded", [])
    }
    for r in doc["embedded_reloc_overlaps"]:
        if (r["patch"], r["va"]) not in emb_ok:
            fails.append(
                "DLL patch %s @%s EMBEDS a relocated address at %s (excluded from %s) and is not in cap_compose_allow.json 'embedded'"
                % (r["patch"], r["addr"], r["va"], r["cache"])
            )
    have_emb = {(r["patch"], r["va"]) for r in doc["embedded_reloc_overlaps"]}
    for patch, va in sorted(emb_ok - have_emb):
        fails.append(
            "cap_compose_allow.json 'embedded' lists %s x %s, which no longer overlaps"
            % (patch, va)
        )
    have = {(r["patch"], r["site"]) for r in doc["cap_set_overlaps"]}
    for a in allow:
        if (a["patch"], a["site"]) not in have:
            fails.append(
                "cap_compose_allow.json lists %s x %s, which no longer overlaps"
                % (a["patch"], a["site"])
            )
        if check_source:
            src = os.path.join(REPO, a["source"])
            text = (
                open(src, encoding="utf-8", errors="replace").read() if os.path.isfile(src) else ""
            )
            if not re.search(
                r"%s\s*\(\s*%s\b" % (GUARD_HELPER, re.escape(a["install_token"])), text
            ):
                fails.append(
                    "%s does not install %s through %s(...)"
                    % (a["source"], a["patch"], GUARD_HELPER)
                )
    return fails


# ---- --selftest ------------------------------------------------------------------------------------


def selftest():
    """Planted negatives. A census is a COUNT, and a broken counter and a genuinely small overlap
    produce the same green -- so every step of the derivation is fired against inputs whose answer
    is known by construction, and the two parsers that can silently return an empty set are given a
    shape they must REFUSE."""
    import tempfile

    fails = []

    def expect(what, ok, detail=None):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)
            if detail is not None:
                print("       got: %r" % (detail,))

    ext = [(0x00401000, 0x0040100F, "fn_a"), (0x00402000, 0x00402FFF, "fn_b")]
    find = body_finder(ext)
    expect("strict containment: the entry byte is inside", find(0x00401000)[2] == "fn_a")
    expect("strict containment: the INCLUSIVE end byte is inside", find(0x0040100F)[2] == "fn_a")
    expect(
        "strict containment: one byte past the end belongs to NOBODY -- not to the nearest "
        "preceding body, which is the loose reading that makes the census unreproducible",
        find(0x00401010) is None,
    )
    expect("...and an address below every body is unowned", find(0x00400000) is None)

    with tempfile.TemporaryDirectory() as tmp:
        # 1. the table parser must REFUSE a header it cannot read, not return an empty class.
        empty = os.path.join(tmp, "mh_patches.gen.h")
        open(empty, "w", encoding="utf-8").write("// a header with no tables at all\n")
        try:
            named_table(empty, "promotable_ranges")
            expect("a header with no promotable_ranges table is REFUSED", False)
        except Refusal:
            expect("a header with no promotable_ranges table is REFUSED", True)
        good = os.path.join(tmp, "good.h")
        open(good, "w", encoding="utf-8").write(
            'inline constexpr x promotable_ranges[] = {\n    {"fn_a", 0x1u, 0x2u, "sim"},\n};\n'
        )
        expect(
            "...and a well-formed one parses its names",
            named_table(good, "promotable_ranges") == {"fn_a"},
        )

        # 2. ENTRY_WINDOW is READ from the applier, so the two cannot drift apart in silence.
        bad = os.path.join(tmp, "promoted.cpp")
        open(bad, "w", encoding="utf-8").write("// no such constant here\n")
        try:
            promoted_entry_window_bytes(bad)
            expect("a promoted.cpp without ENTRY_WINDOW is REFUSED", False)
        except Refusal:
            expect("a promoted.cpp without ENTRY_WINDOW is REFUSED", True)

        # 3. the install-site scanner resolves the three real shapes and counts what it cannot.
        root = os.path.join(tmp, "mh", "seams")
        os.makedirs(root)
        open(os.path.join(root, "x.cpp"), "w", encoding="utf-8").write(
            "constexpr uintptr_t ADDR_X = mh::addr::llm_thing;\n"
            "void f() {\n"
            "  install_trampoline(ADDR_X, d, &t, 8);\n"
            "  install_jmp(0x00401234u, d);\n"
            "  install_trampoline(mh::exp::addr_llm_other, d, &t, 8);\n"
            "  install_jmp(rows[i].site, d);\n"
            "}\n"
        )
        consts = {"llm_thing": 0x00405000, "addr_llm_other": 0x00406000}
        got, unres = detour_targets(os.path.join(tmp, "mh"), consts)
        expect(
            "a local alias, a bare literal and an mh::exp:: constant all resolve",
            sorted(got) == [0x00401234, 0x00405000, 0x00406000],
            sorted(hex(v) for v in got),
        )
        expect(
            "...and a non-static target is COUNTED as unresolved, never silently dropped",
            unres == 1,
        )

        # 4. WALKER LIVENESS. A scanner pointed at an empty tree must find nothing -- which is the
        # same answer a broken regex gives over the real tree, so the real run's count is asserted
        # to be nonzero below rather than trusted.
        os.makedirs(os.path.join(tmp, "empty"))
        expect(
            "an empty tree yields no detour targets",
            detour_targets(os.path.join(tmp, "empty"), consts)[0] == {},
        )

    # 4b. FULL-EXTENT overlap. The shapes the start-address test could not see.
    site = [("m", 0x1000, 4)]  # a 4-byte operand [0x1000,0x1004)
    expect(
        "a DLL patch that STARTS BEFORE a site and runs over it is an overlap (the grand/hook shape)",
        [r["relation"] for r in full_extent_overlaps([("p", 0x0FFD, 7)], site)] == ["interior"],
        full_extent_overlaps([("p", 0x0FFD, 7)], site),
    )
    expect(
        "...a patch that ends exactly where the site starts does NOT overlap",
        full_extent_overlaps([("p", 0x0FFC, 4)], site) == [],
    )
    expect(
        "...a patch that starts exactly where the site ends does NOT overlap",
        full_extent_overlaps([("p", 0x1004, 4)], site) == [],
    )
    expect(
        "...a patch that overlaps only the first byte of the site is a straddle",
        [r["relation"] for r in full_extent_overlaps([("p", 0x0FFC, 5)], site)] == ["straddle"],
    )
    expect(
        "...a patch at the site start is 'start'; one wholly inside a longer splice is 'inside_site'",
        [r["relation"] for r in full_extent_overlaps([("p", 0x1000, 2)], [("m", 0x1000, 8)])]
        == ["start"]
        and [r["relation"] for r in full_extent_overlaps([("p", 0x1002, 2)], [("m", 0x1000, 8)])]
        == ["inside_site"],
    )
    expect(
        "...a long early splice is still found behind a short later site",
        len(full_extent_overlaps([("p", 0x1040, 4)], [("m", 0x1000, 0x80), ("n", 0x1010, 4)])) == 1,
    )
    with tempfile.TemporaryDirectory() as tmp:
        bad = os.path.join(tmp, "dll_patch_manifest.json")
        json.dump({"patches": [{"id": "x", "addrs": ["0x00401000"]}]}, open(bad, "w"))
        try:
            dll_patch_extents(bad)
            expect("a row with addrs and no lens is REFUSED", False)
        except Refusal:
            expect("a row with addrs and no lens is REFUSED", True)
        json.dump({"patches": [{"id": "x", "addrs": ["0x00401000"], "lens": [0]}]}, open(bad, "w"))
        try:
            dll_patch_extents(bad)
            expect("a zero length is REFUSED", False)
        except Refusal:
            expect("a zero length is REFUSED", True)

    # 5. THE REAL DERIVATION, on the real tree: every class must be non-empty and the applier's
    # window constant must be the one this file states.
    try:
        doc = census()
    except Refusal as e:
        expect("the real census derives", False, str(e))
        return 1 if fails else 0
    expect(
        "the real census sees every manifest site", doc["sites_total"] > 46000, doc["sites_total"]
    )
    expect("...a non-empty promotable class", doc["sites_in_promotable_body"] > 0)
    expect("...a non-empty detour-target class", doc["sites_in_detour_target"] > 0)
    expect(
        "...and the union is at most their sum and at least the larger of them",
        max(doc["sites_in_promotable_body"], doc["sites_in_detour_target"])
        <= doc["sites_in_either"]
        <= doc["sites_in_promotable_body"] + doc["sites_in_detour_target"],
        doc["sites_in_either"],
    )
    expect(
        "the entry-window subset is a SUBSET of the union -- it is the collidable part, not a "
        "second population",
        doc["sites_in_entry_window_of_claimed_body"] <= doc["sites_in_either"],
    )
    expect(
        "the applier's ENTRY_WINDOW is the %d this census assumes" % ENTRY_WINDOW,
        doc["entry_window_bytes"] == ENTRY_WINDOW,
        doc["entry_window_bytes"],
    )
    known6 = {"0x00462ab9", "0x0047c6c8", "0x00491eac", "0x00482b5b", "0x0047ebe1", "0x00466bfe"}
    got6 = {
        r["addr"]
        for r in doc["registered_patch_overlaps"]
        if r["manifest"] == "grand_all_caphike_storagecap_EN"
    }
    expect(
        "the six overlaps PT-CAP8 found against the July grand raise are all still reported (full extents)",
        known6 <= got6,
        sorted(known6 - got6),
    )
    expect(
        "...and the start-address-only test would have reported none of them",
        not any(
            r["relation"] == "start"
            and r["addr"] in known6
            and r["manifest"] == "grand_all_caphike_storagecap_EN"
            for r in doc["registered_patch_overlaps"]
        ),
    )
    known6 = {
        "0x00462ab9",
        "0x0047c6c8",
        "0x00491eac",  # v0.2.2: buildings/turrets hooks
        "0x00482b5b",
        "0x0047ebe1",
        "0x00466bfe",  # v0.2.3 (PT-CAP9-U1): the three units hooks
        "0x00491166",  # mp:DSHEAL soldier_origin_anim_fix: the soldiers array disp inside the displaced MOVZX
    }
    cap_addrs = {r["addr"] for r in doc["cap_set_overlaps"]}
    expect(
        "against the cap manifests exactly the three buildings/turrets hooks, the three units "
        "hooks and the soldier-origin load overlap (the units one with two sites in move_near_enough)",
        cap_addrs == known6 and len(doc["cap_set_overlaps"]) == 8,
        sorted(cap_addrs),
    )
    expect(
        "every cap_set_overlaps row is in tools/data/cap_compose_allow.json",
        not strict_failures(doc, check_source=False),
        strict_failures(doc, check_source=False),
    )
    emb = embedded_overlaps(
        [("p", 0x1000, 27)], [("c.json", 0x1003), ("c.json", 0x1000 + 25), ("c.json", 0x2000)]
    )
    expect(
        "a DLL patch that EMBEDS an excluded relocated dword is found (inside), one that only touches its edge or sits elsewhere is not",
        [r["va"] for r in emb] == ["0x00001003"],
        emb,
    )
    expect(
        "the committed census sees the O1c local-decode carrier embedding the two excluded pending dwords",
        {(r["patch"], r["va"]) for r in doc["embedded_reloc_overlaps"]}
        >= {("order_wide_decode_local", "0x0046643d"), ("order_wide_decode_local", "0x00466451")},
    )
    expect(
        "every entry-window collision names a manifest, a VA and a function",
        all({"manifest", "va", "len", "fn"} <= set(r) for r in doc["entry_window_collisions"]),
    )

    print("check_inmem_composition --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
