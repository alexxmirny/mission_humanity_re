#!/usr/bin/env python3
"""scan_libmh_vas.py -- the LINK-PROOF half of LIB-VA0: original VAs left in the BUILT libmh.

WHY THIS EXISTS AND WHAT IT ADDS TO gen_va_census.py
----------------------------------------------------
gen_va_census.py measures SOURCE: `mh::call::` tokens in the modules that become libmh. That is the
instrument LIB-VA0 owns and it is the one that routes each site to an owning item. But it can be
fooled by spelling -- a VA reached through a macro, a raw `(void *)mh::addr::X` literal handed to a
callback, a generated table -- and LIB-CRT found exactly that: all four qsort comparators were VAs
passed as DATA, invisible to a token scan, for as long as the clause claiming there were none had
existed.

This tool reads the OBJECT BYTES instead. `mh::call::X` inlines to
`detail::s_<shape>(0x00669f90u, ...)`, so the VA is a literal 4-byte immediate in .text; a
`(void *)mh::addr::X` in a `static const` calls-struct is the same immediate in .rdata. Nothing about
how the source spelled it survives, which is why LIB-VA0's clause 5 names this as the check that
"cannot be fooled by source spelling at all".

IT SCANS CODE **AND** INITIALISED DATA, and the first version did not -- see _scanned_ranges. A
code-only scan cannot see the exact class of dependency this tool exists to catch, and a mutation test
proved that in one run rather than leaving it to be discovered later.

IT DOES NOT SCAN DEBUG SECTIONS, and that part IS load-bearing in the other direction. A naive scan of
the whole .lib reports hits inside CodeView records, where two adjacent 16-bit fields happen to spell a
plausible address -- 2 of the 3 hits on the very first run were exactly that (`3d 01 4d 00` around the
local-variable names "player" and "target_ref" in sim_weapon_damage_calc.obj, reading as 0x004d013d =
struct_array_malloc_impl). A tool that cries wolf twice is a tool nobody runs a fourth time.

WHAT A HIT MEANS. Not automatically a bug. Three legitimate reasons for a VA in libmh:
  * an ENTRY SEAM (MH_EXPORT_REPLACE) has to know the original entry address to patch it;
  * a shadow/diagnostic arm that is compiled out of the shipping standalone lib but not out of this
    build of it;
  * a genuine remaining outward call, which is what gen_va_census already counts.
So this is a RATCHET over a recorded baseline, like gen_va_census: a rise names the new addresses and
their object files, a fall says re-record.

USAGE
    python tools/scan_libmh_vas.py                 # report
    python tools/scan_libmh_vas.py --check         # the ratchet (lint_repo)
    python tools/scan_libmh_vas.py --record        # re-record after a fall
    python tools/scan_libmh_vas.py --lib <path>    # a different build of the lib

The lib must be BUILT first; --check SKIPS (exit 0) when it is absent rather than failing, because a
fresh clone has no build tree and a lint that fails on that teaches people to ignore it.
"""

import argparse
import io
import json
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LIB = os.path.join(REPO, "src", "mh_dll", "libmh", "Release", "libmh.lib")
CALLS_H = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_calls.gen.h")
ADDRS_H = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_addrs.gen.h")
BASELINE = os.path.join(REPO, "tools", "data", "libmh_va_bytes.json")
# LIB-REF-SPLIT's adjudication file. THE ZERO CLAIM IS `zero attributable hits, plus every
# unattributable one carrying a recorded disposition` -- the X-SPINE printed-skips shape, one
# population over. An entry is keyed by (va, object, byte signature): the VA alone would excuse a
# real dependency that later appears in the same object, and (va, object) alone would excuse it if
# the surrounding code changed into something that genuinely names the address. A hit that matches
# no entry is RED, always -- nothing is ever adjudicated by pattern alone.
ADJUDICATION = os.path.join(REPO, "tools", "data", "libmh_va_adjudication.json")

# `detail::s_u32_EAX(0x004d0155u, ...)` / `inline constexpr uintptr_t name = 0x004ec792u;`
CALL_VA_RE = re.compile(r"^inline\s+[^;{]*?\b(\w+)\s*\([^)]*\)\s*\{[^}]*?\b0x([0-9a-fA-F]{6,8})u")
ADDR_VA_RE = re.compile(r"^inline constexpr uintptr_t\s+(\w+)\s*=\s*0x([0-9a-fA-F]{6,8})u")

# THE WHOLE IMAGE, not just its code range (LIB-REF-SPLIT, widened 2026-09-11 on the conductor's
# ruling). The old window stopped at 0x00600000, which is where the CODE ends -- and that made the
# tool blind to exactly the population this item had to remove: 465 of the state registry's stock
# bases live in .bss above it, as do the save table's address columns. A zero measured against the
# narrow window while 7195 original-image immediates stood above it is the vacuous-green shape the
# .text-only first version of this very tool already produced once.
#
# WIDENING IS SAFE HERE BECAUSE THE MATCH IS EXACT-VALUE, NOT RANGE. Every candidate is a named
# address out of the generated headers, so a hit means "these four bytes equal an address the tree
# names", never "this looks like a pointer". The residual risk is byte coincidence, which the
# adjudication file handles by (va, object, signature) with a recorded reason -- not by widening a
# tolerance. .bss ends at 0x1064dff (the EN import was extended to RU's extent).
VA_LO, VA_HI = 0x00401000, 0x01070000


def load_vas():
    """-> {va: [name, ...]} for every original address the generated headers name."""
    vas = {}
    for path, rx in ((CALLS_H, CALL_VA_RE), (ADDRS_H, ADDR_VA_RE)):
        if not os.path.exists(path):
            continue
        for line in io.open(path, encoding="utf-8", errors="replace"):
            m = rx.match(line.strip())
            if not m:
                continue
            va = int(m.group(2), 16)
            if VA_LO <= va < VA_HI:
                vas.setdefault(va, []).append(m.group(1))
    return vas


def _archive_members(data):
    """-> [(raw_name, offset, size)] over a COFF archive, plus the longnames blob."""
    out, longnames, i = [], None, 8
    while i + 60 <= len(data):
        hdr = data[i : i + 60]
        name = hdr[0:16].decode("latin1").strip()
        try:
            size = int(hdr[48:58].decode("latin1").strip())
        except ValueError:
            break
        start = i + 60
        if name.startswith("//"):
            longnames = data[start : start + size]
        else:
            out.append((name, start, size))
        i = start + size + (size & 1)
    return out, longnames


def _member_name(raw, longnames):
    """-> the archive member's name, NORMALISED to `<parent>/<basename>.obj`.

    NORMALISED BECAUSE THE RAW NAME IS BUILD PLUMBING, NOT IDENTITY (LIB-DISPATCH-SA, 2026-09-11).
    An archive member's name is the literal path the compiler was handed, i.e. it encodes IntDir's
    SPELLING. Measured today, one project, one folder, three different names for the same object:

        default, built through mh.sln          libmh/Release/rx_dispatch.obj
        default, built as a standalone project Release/rx_dispatch.obj
        IntDir pinned with $(ProjectDir)       <abs checkout>/libmh/Release/rx_dispatch.obj

    tools/data/libmh_va_adjudication.json keys every excused occurrence by (va, object, signature),
    so without normalisation the SAME LIB adjudicates differently depending on how it was built --
    all 125 rows report "match nothing in the built lib" the moment the build route changes. That
    turned an artifact-identity gate into a gate on MSBuild invocation style, which it was never
    meant to be, and it bit the moment libmh joined mh.sln (LIB-REF's reference host made the
    solution route the normal one).

    The BASENAME is the real key: the 623 TU names are unique across the lib, and everything to the
    left of it is the invocation. BOTH SIDES are normalised the same way -- this function and the
    adjudication file's keys as they are loaded (see _load_adjudication) -- so the committed rows go
    on matching verbatim and nothing needs re-recording. Reports still PRINT the path the archive
    carried, because when a hit is real the folder it came from is worth seeing."""
    if raw.startswith("/") and raw[1:].isdigit() and longnames is not None:
        off = int(raw[1:])
        end = longnames.index(b"\x00", off)
        raw = longnames[off:end].decode("latin1")
    return _obj_key(raw)


def _obj_key(name):
    """-> the route-independent object key: the bare `<tu>.obj`. See _member_name."""
    return name.rstrip("/").replace("\\", "/").rsplit("/", 1)[-1]


def _scanned_ranges(blk):
    """-> [(offset, size)] of the CODE and INITIALISED-DATA sections of one COFF object.

    CODE IS NOT ENOUGH, and finding that out is the reason this function is not named _text_ranges
    any more. The first version scanned executable sections only, on the theory that a VA reaches the
    original as a call. A mutation test refuted it in one run: reverting ONE qsort comparator from
    MH_CRT_CMP back to `(void *)mh::addr::group_move_scratch_cmp_dist_004cbe0c` and rebuilding left
    the ratchet GREEN -- because a calls-struct is a `static const` whose initialiser lands in
    .rdata, not in .text. The exact class of dependency this tool exists to catch (G155: a VA passed
    as DATA) was the one class it could not see.

    So: everything with CODE or INITIALISED_DATA set, minus the debug sections. `.debug$S`/`.debug$T`
    stay out because CodeView records produce false positives -- two adjacent 16-bit fields spelling
    a plausible address was 2 of the 3 hits on the very first run.
    """
    if len(blk) < 20:
        return []
    machine, nsec = struct.unpack_from("<HH", blk, 0)
    if machine != 0x014C:  # IMAGE_FILE_MACHINE_I386
        return []
    opt_size = struct.unpack_from("<H", blk, 16)[0]
    sec_off = 20 + opt_size
    out = []
    for k in range(nsec):
        off = sec_off + k * 40
        if off + 40 > len(blk):
            break
        name = blk[off : off + 8].rstrip(b"\x00").decode("latin1")
        raw_size, raw_ptr = struct.unpack_from("<II", blk, off + 16)
        chars = struct.unpack_from("<I", blk, off + 36)[0]
        if name.startswith(".debug") or name.startswith(".drectve"):
            continue
        # IMAGE_SCN_CNT_CODE (0x20) | IMAGE_SCN_CNT_INITIALIZED_DATA (0x40) | MEM_EXECUTE (0x20000000)
        if chars & (0x00000020 | 0x00000040 | 0x20000000):
            if raw_ptr and raw_size:
                out.append((raw_ptr, raw_size))
    return out


def scan(lib_path):
    """-> {"<0x%08x va>": {"names": [...], "objects": {obj: count}}} over code + init data."""
    data = io.open(lib_path, "rb").read()
    members, longnames = _archive_members(data)
    vas = load_vas()
    pats = {va: struct.pack("<I", va) for va in vas}
    found = {}
    for raw, start, size in members:
        blk = data[start : start + size]
        ranges = _scanned_ranges(blk)
        if not ranges:
            continue
        obj = _member_name(raw, longnames)
        for roff, rsize in ranges:
            text = blk[roff : roff + rsize]
            for va, pat in pats.items():
                n = text.count(pat)
                if not n:
                    continue
                key = "0x%08x" % va
                rec = found.setdefault(key, {"names": sorted(set(vas[va])), "objects": {}})
                rec["objects"][obj] = rec["objects"].get(obj, 0) + n
                # THE BYTE SIGNATURE, kept so an adjudication can be keyed on what the bytes ARE and
                # not merely on where they were seen. A four-byte window is a coincidence-prone
                # thing: 0x00498dc0 is reproduced exactly by `33 C0` + MSVC's 3-byte alignment nop
                # `8D 49 00`, in five objects whose source cannot name that callee. Recording the
                # eight bytes around each occurrence is what lets an exclusion say WHY, and what
                # makes it fail the day the surrounding code changes shape.
                i = text.find(pat)
                while i >= 0:
                    sig = text[max(0, i - 2) : i + 6].hex(" ")
                    rec.setdefault("sigs", {}).setdefault(obj, [])
                    if sig not in rec["sigs"][obj]:
                        rec["sigs"][obj].append(sig)
                    i = text.find(pat, i + 1)
    for rec in found.values():
        rec["objects"] = {k: rec["objects"][k] for k in sorted(rec["objects"])}
    return {k: found[k] for k in sorted(found)}


def render(found, lib_path):
    return (
        json.dumps(
            {
                "_generated_by": "tools/scan_libmh_vas.py --record -- do not hand-edit",
                "_measures": (
                    "4-byte little-endian original-image VAs (from addr/mh_calls.gen.h and "
                    "addr/mh_addrs.gen.h) appearing in the CODE or INITIALISED-DATA sections of "
                    "the built libmh.lib. Debug sections are excluded -- CodeView records produce "
                    "false positives; data sections are NOT, because a VA in a static initialiser "
                    "is exactly the dependency this tool exists to catch (see the docstring)."
                ),
                "_lib": os.path.relpath(lib_path, REPO).replace("\\", "/"),
                "total_vas": len(found),
                "total_sites": sum(sum(r["objects"].values()) for r in found.values()),
                "vas": found,
            },
            indent=1,
        )
        + "\n"
    )


def print_report(found):
    sites = sum(sum(r["objects"].values()) for r in found.values())
    print(
        "libmh VA byte-scan: %d distinct original VA(s) / %d immediate(s) in code + initialised data"
        % (len(found), sites)
    )
    for va, rec in found.items():
        print("  %s  %s" % (va, ", ".join(rec["names"])))
        for obj, n in rec["objects"].items():
            print("      x%-3d %s" % (n, obj))


def apply_adjudication(found):
    """Drop every occurrence carrying a recorded disposition; 1 if an entry matches nothing.

    Mutates `found` in place so --record and --check see the same set. Returns a process exit code.
    """
    if not os.path.exists(ADJUDICATION):
        return 0
    doc = json.load(io.open(ADJUDICATION, encoding="utf-8"))
    # The object column is normalised on BOTH sides -- see _member_name. The committed file records
    # `libmh/Release/x.obj`, which is what one build route happened to produce; the key that matters
    # is the TU, and normalising here is what lets those rows keep matching after libmh joined
    # mh.sln and the archive started naming its members differently.
    adj = {(e["va"], _obj_key(e["object"]), e["signature"]): e for e in doc.get("entries", [])}
    excused, stale = [], set(adj)
    for va in list(found):
        rec = found[va]
        for obj in list(rec["objects"]):
            # EVERY occurrence in this object must be adjudicated, not just the first. One object
            # can hold the same coincidence twice with different neighbouring bytes (order_queue.obj
            # does), and a rule that stopped at the first match would excuse the object while
            # leaving the second entry looking unused -- which the stale arm then reports, correctly,
            # as a licence nobody claimed.
            keys = [(va, obj, sig) for sig in rec.get("sigs", {}).get(obj, [])]
            if keys and all(k in adj for k in keys):
                excused.extend(keys)
                for k in keys:
                    stale.discard(k)
                del rec["objects"][obj]
        if not rec["objects"]:
            del found[va]
    if excused:
        print(
            "scan_libmh_vas: %d adjudicated occurrence(s) excused (tools/data/%s):"
            % (len(excused), os.path.basename(ADJUDICATION))
        )
        # The reason once per CLASS, not once per occurrence: five identical paragraphs is a wall
        # nobody reads, and the class is what a reviewer is actually judging.
        seen = set()
        for va, obj, sig in sorted(set(excused)):
            e = adj[(va, obj, sig)]
            print("    ~%s  %s  [%s]" % (va, obj, e["class"]))
            if e["class"] not in seen:
                seen.add(e["class"])
                print("        %s" % e["why"])
    if stale:
        # An adjudication that no longer matches anything is not harmless: it is a licence nobody is
        # using, and the next coincidence that happened to match it would be excused silently.
        print(
            "[scan_libmh_vas] FAIL: %d adjudication entr(ies) match nothing in the built lib -- "
            "remove them:" % len(stale),
            file=sys.stderr,
        )
        for va, obj, sig in sorted(stale):
            print("  %s  %s  [%s]" % (va, obj, sig), file=sys.stderr)
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", default=DEFAULT_LIB)
    ap.add_argument("--check", action="store_true", help="ratchet against the recorded baseline")
    ap.add_argument("--record", action="store_true", help="re-record the baseline after a fall")
    args = ap.parse_args()

    if not os.path.exists(args.lib):
        msg = "scan_libmh_vas: %s not built" % os.path.relpath(args.lib, REPO)
        if args.check:
            print(msg + " -- SKIPPED (build libmh to arm this check)")
            return 0
        print(msg, file=sys.stderr)
        return 2

    found = scan(args.lib)

    # THE ADJUDICATION FILTER RUNS BEFORE BOTH --record AND --check, and it must: a baseline that
    # recorded the adjudicated occurrences while the check filtered them out would disagree with
    # itself every run -- which is what happened the first time this was wired, --record writing 7
    # VAs and --check then reporting 5 of them as "went away".
    rc = apply_adjudication(found)
    if rc:
        return rc
    text = render(found, args.lib)

    if args.record:
        io.open(BASELINE, "w", encoding="utf-8", newline="\n").write(text)
        print_report(found)
        print("wrote %s" % os.path.relpath(BASELINE, REPO))
        return 0

    if args.check:
        if not os.path.exists(BASELINE):
            print("scan_libmh_vas: no baseline -- run --record", file=sys.stderr)
            return 1
        old = json.load(io.open(BASELINE, encoding="utf-8"))
        old_sites = {(va, obj) for va, rec in old["vas"].items() for obj in rec["objects"]}
        new_sites = {(va, obj) for va, rec in found.items() for obj in rec["objects"]}
        added = sorted(new_sites - old_sites)
        removed = sorted(old_sites - new_sites)
        if added:
            print(
                "[scan_libmh_vas] FAIL: %d original VA(s) newly reachable in the built libmh:"
                % len(added),
                file=sys.stderr,
            )
            for va, obj in added:
                print("  %s  %s  (%s)" % (va, obj, ", ".join(found[va]["names"])), file=sys.stderr)
            return 1
        if removed:
            print(
                "scan_libmh_vas: THE BASELINE IS STALE: %d VA site(s) went away, which is the "
                "point -- re-record with `python tools/scan_libmh_vas.py --record`." % len(removed)
            )
            for va, obj in removed:
                print("    -%s  %s" % (va, obj))
            return 1
        print(
            "scan_libmh_vas: %d VA(s) in the built libmh, unchanged from the baseline" % len(found)
        )
        return 0

    print_report(found)
    return 0


if __name__ == "__main__":
    sys.exit(main())
