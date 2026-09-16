#!/usr/bin/env python3
"""check_inmem_patch_parity.py -- is the IN-MEMORY static patch the same patch mhpatch makes?

F1E's oracle, generalized at fork F4C-GATE from one hardcoded manifest to the SHIPPED SET: every
src/patcher manifest whose `class` is "shipped" (the compile list mh.dll carries, derived from the
manifests themselves -- see tools/gen_inmem_manifest.py). mh.dll, armed with `[patch] inmem=1` and
`[patch] manifest=<name>`, applies that manifest to the LOADED image and dumps every extent it wrote
READ BACK FROM THE PROCESS (mh/patch/inmem_patch.cpp, write_parity_dump). This compares that dump
against the file mhpatch.py produces from the same manifest and the same clean exe.

FOUR COMPARISONS, and the last two are what make the first two mean anything.

  1. PATCH EXTENTS -- each site's live bytes vs the reference file's bytes at the same VA.
  2. THE CAVE -- the live cave's bytes vs the appended section's raw data in the reference file.
  3. THE BAKED VALUE, at every declared ref -- the reference FILE must hold exactly the address the
     relocation metadata says that dword names at the baked placement. This is the one check here
     that is not about the DLL at all: it tests the generator's ref set against what mhpatch
     INDEPENDENTLY wrote, so a ref with the right offset and the wrong target -- which comparison
     (1) would happily mask out on both sides and call identical -- is caught. It needs no rig, and
     it is the substance of `--offline`.
  4. COMPLETENESS -- the reference file is diffed against the CLEAN exe, and every differing byte
     must fall in an extent the dump covers or in a class named below. Without (4), comparing only
     the extents the applier chose to report proves it copied its own list correctly and nothing
     more: an extent mhpatch writes and the applier never heard of would be invisible.

THE DIFFERENCE CLASSES, each of which must be justified rather than tolerated:

  * PE HEADER + SECTION TABLE. mhpatch appends a real section: NumberOfSections, SizeOfImage and a
    new 40-byte section header change, and the raw data lands past the old end of file. In memory
    there is no file and no section table -- VirtualAlloc supplies the mapping the header would have
    asked the loader for. EXPECTED, and by design not reproducible.
  * THE CAVE'S VA. VirtualAlloc is a request. If the cave lands anywhere but its baked VA, raw byte
    equality is the WRONG test: what must hold is that the cave's content is identical except at the
    placement-sensitive dwords the manifest declares, and that every one of those -- in the cave and
    in the patch sites that reach it -- resolves to the SAME absolute address the reference file's
    does. That is checked here, not assumed.
  * AN ADDED IMPORT DESCRIPTOR. A manifest with `imports` makes mhpatch rebuild the import table;
    the in-memory path has none (the proxy injector loads mh.dll, and an `asm` patch's
    `[import:...]` has no meaning without it). No shipped manifest has `imports`, which is asserted
    rather than assumed -- if one ever does, this tool refuses until the runtime equivalent exists.

WHAT `--offline` IS AND IS NOT (the G180 hand-off). Until F4C-GATE this file's only arm needed the
rig, so a refactor broke it for a day with every lint row green. `--offline` gives it a cheap arm
that runs in lint: it builds each shipped manifest's mhpatch reference (which by itself proves every
expected-bytes guard still matches the real clean EN exe), re-derives the refs, runs check (3)
against that file, and then drives the whole comparison over a SYNTHETIC dump. Be exact about what
that last part proves: the synthetic dump is built with this file's own relocation arithmetic, so
agreeing with itself proves the COMPARISON PLUMBING works, not that the DLL relocates correctly.
`--selftest` therefore plants mutations in the synthetic dump and requires each one to go red.
Only `--run` measures the DLL.

Usage:
    python tools/check_inmem_patch_parity.py --offline          # rig-free: the lint arm
    python tools/check_inmem_patch_parity.py --selftest          # --offline + planted negatives
    python tools/check_inmem_patch_parity.py --run               # the shipped set, on the rig
    python tools/check_inmem_patch_parity.py --run --manifest projectile_pool_relocate_EN
    python tools/check_inmem_patch_parity.py --dump <file> --manifest <name>
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PATCHER = os.path.join(REPO, "src", "patcher")
for p in (PATCHER, HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import lief  # noqa: E402

import gen_inmem_manifest as gim  # noqa: E402
import machine_config as machine  # noqa: E402
import mhpatch  # noqa: E402

CLEAN = machine.POLYGON_CLEAN + "/mh.exe"
TMP = os.path.join(REPO, "tmp")
LANE = os.path.join(machine.LANE_ROOT, "f1e_parity")

# run_gate's `inmem` unit (--run) and lint_repo's parity row (--selftest) run this tool in two
# processes at once; a fixed scratch name made the loser die on WinError 32 (filed at F5H). Every
# scratch path carries the pid, and stale scratch from finished runs is swept on startup.
_SCRATCH_TAG = "p%d" % os.getpid()


def scratch(leaf):
    return os.path.join(TMP, "inmem_parity_%s_%s" % (_SCRATCH_TAG, leaf))


def sweep_stale_scratch(max_age_s=24 * 3600):
    for p in glob.glob(os.path.join(TMP, "inmem_parity_p*_*")):
        if p.startswith(os.path.join(TMP, "inmem_parity_%s_" % _SCRATCH_TAG)):
            continue
        try:
            if time.time() - os.path.getmtime(p) > max_age_s:
                os.remove(p)
        except OSError:
            pass


# THE GATE UNIT. `--run` with no --manifest walks the whole shipped set; run_gate's unit runs this
# one, and the choice is measured rather than aesthetic (see the F4C-GATE report): it is the
# smallest live subject in the corpus (75 sites, 8 bodies), it is the behavioural no-op of the
# family -- a same-size relocation -- and it exercises exactly the mechanism the other two scale up
# (abs32_to_section refs into a relocated cave).
GATE_MANIFEST = "projectile_pool_relocate_EN"


def shipped():
    """The manifests this tool gates: mh.dll's compile list, derived from the corpus."""
    return [s.replace(".mh.patch.json", "") for s, _c in gim.shipped_manifests()]


def manifest_path(name_or_path):
    if os.path.sep in name_or_path or name_or_path.endswith(".json"):
        return name_or_path
    return os.path.join(PATCHER, "%s.mh.patch.json" % name_or_path)


# ---- the reference build -------------------------------------------------------------------------


def build_reference(clean, manifest, out):
    """mhpatch's own output, from a COPY of the clean exe. POLYGON_CLEAN is never written to."""
    os.makedirs(os.path.dirname(out), exist_ok=True)
    src = out + ".in.exe"
    shutil.copyfile(clean, src)
    report, warns = mhpatch.apply_manifest(src, manifest, out)
    os.remove(src)
    bad = [(hex(va), st) for va, st, _b in report if not st.startswith("OK")]
    if bad:
        raise SystemExit(
            "mhpatch reference build did not apply cleanly (%d of %d sites): %s"
            % (len(bad), len(report), bad[:4])
        )
    return report, warns


# ---- the live dump -------------------------------------------------------------------------------


def parse_dump(path):
    d = {"sections": {}, "sites": {}, "tail": {}, "meta": {}}
    for line in open(path, encoding="ascii", errors="replace"):
        t = line.split()
        if not t or t[0] == "#":
            continue
        if t[0] in ("manifest", "sha256", "applied", "relocated", "refusal"):
            d["meta"][t[0]] = " ".join(t[1:])
        elif t[0] == "section":
            kv = dict(x.split("=", 1) for x in t[3:])
            d["sections"][int(t[1])] = {
                "name": t[2],
                "preferred": int(kv["preferred"], 16),
                "actual": int(kv["actual"], 16),
                "vsize": int(kv["vsize"], 16),
                "datalen": int(kv["datalen"], 16),
            }
        elif t[0] == "tail":
            d["tail"][int(t[2])] = t[-1].split("=")[1] == "1"
        elif t[0] == "extent":
            idx, va, hexs = int(t[2]), int(t[3], 16), (t[4] if len(t) > 4 else "")
            (d["sections"] if t[1] == "section" else d["sites"])[idx] = {
                "va": va,
                "bytes": bytes.fromhex(hexs),
                **(d["sections"].get(idx, {}) if t[1] == "section" else {}),
            }
    return d


# ---- the comparison ------------------------------------------------------------------------------


class RefList(list):
    """The ref set, with a by-blob index built on first use.

    A plain list scan is O(refs) per blob, which on grand_all_caphike_storagecap_EN is 7,772 blobs x
    4,260 refs -- 33 M comparisons and 1.4 s of a 1.9 s comparison, to answer a lookup. It is a list
    subclass rather than a dict so every existing `for r in refs` still reads the same.
    """

    def by_owner(self, in_section, owner):
        idx = getattr(self, "_idx", None)
        if idx is None:
            idx = {}
            for r in self:
                idx.setdefault((r["in_section"], r["owner"]), []).append(r)
            self._idx = idx
        return idx.get((in_section, owner), [])


def _refs_for(man_refs, in_section, owner):
    if isinstance(man_refs, RefList):
        return man_refs.by_owner(in_section, owner)
    return [r for r in man_refs if r["in_section"] == in_section and r["owner"] == owner]


class _VaMap:
    """VA -> file offset, resolved once from the section table.

    mhpatch._va_to_offset walks lief's section list per call, which is fine for one site and not for
    the 7,770 grand_all_caphike_storagecap_EN has.
    """

    def __init__(self, pe):
        self.rows = sorted(
            (
                int(s.virtual_address) + int(pe.optional_header.imagebase),
                int(s.virtual_size),
                int(s.pointerto_raw_data),
                int(s.sizeof_raw_data),
            )
            for s in pe.sections
        )

    def __call__(self, va):
        for base, vsz, raw, rawsz in self.rows:
            if base <= va < base + max(vsz, rawsz):
                off = raw + (va - base)
                if off < raw + rawsz:
                    return off
        return None


def _u32(b, off):
    return int.from_bytes(b[off : off + 4], "little")


def _i32(b, off):
    return int.from_bytes(b[off : off + 4], "little", signed=True)


def derive_refs(manifest):
    """The same relocation metadata gen_inmem_manifest.py compiles into the DLL, re-derived here so
    the checker does not have to trust the HEADER it is checking.

    It goes through the generator's classifier rather than through a second decoder of its own, and
    that is the same independence it always had: what must not be trusted is the generated
    `manifest_*.gen.h` -- the artifact under test, which a stale regeneration could leave describing
    a manifest nobody applies any more. Re-deriving from the JSON keeps that.

    REPAIRED 2026-09-13 (F4C-COMP) after F4C renamed `_scan_refs` out from under it (dead-ends
    G180); GENERALIZED 2026-09-13 (F4C-GATE) to the third ref kind. A ref is returned as the applier
    reads it -- kind, target section, section-relative offset -- and the two SIDES of the comparison
    resolve it against their own bases, rather than the old shape which pre-resolved the target
    against the BAKED base and could only be right while nothing moved.
    """
    m = json.load(open(manifest, encoding="utf-8"))
    secs = [
        (
            int(str(s["vaddr"]), 16),
            int(str(s["vsize"]), 0),
            bytes.fromhex(re.sub(r"\s", "", s["data"])) if s.get("data") else b"",
        )
        for s in m.get("sections") or []
    ]
    c = gim._classify(manifest)
    if not c["complete"]:
        raise SystemExit("%s is not fully relocatable: %s" % (os.path.basename(manifest), c["why"]))
    refs = RefList()
    for in_sec, owner, off, kind, tsec, target in c["refs"]:
        refs.append(
            {
                "in_section": in_sec,
                "owner": owner,
                "off": off,
                "kind": kind,
                "tsec": tsec,
                # section-relative for the two *_to_section kinds; an absolute image VA for
                # rel32_to_image, which no placement changes.
                "target": target,
            }
        )
    return m, secs, refs


def ref_target(r, bases):
    """The absolute address ref `r` must name, given where the sections actually are."""
    if r["kind"] == "rel32_to_image":
        return r["target"]
    return bases[r["tsec"]] + r["target"]


def ref_reads(r, blob, blob_va):
    """What the dword at `r`'s offset in `blob` actually names, blob placed at `blob_va`."""
    if r["kind"] == "abs32_to_section":
        return _u32(blob, r["off"])
    return (blob_va + r["off"] + 4 + _i32(blob, r["off"])) & 0xFFFFFFFF


def _cmp_blob(what, refs, live_bytes, live_va, live_bases, file_bytes, file_va, file_bases, fail):
    """One blob, both sides. -> the count of masked (placement-sensitive) dwords.

    Every declared ref is resolved on BOTH sides against that side's own section bases and required
    to name the right address; then its four bytes are masked out and the remainder must be equal
    byte for byte. Masking without the resolve check would accept any value at all in those dwords,
    which is the failure this ordering exists to prevent.
    """
    mine, theirs = bytearray(live_bytes), bytearray(file_bytes)
    for r in refs:
        if r["off"] + 4 > len(mine) or r["off"] + 4 > len(theirs):
            fail.append("%s: ref +%#x lies outside the %d-byte blob" % (what, r["off"], len(mine)))
            continue
        got = ref_reads(r, live_bytes, live_va)
        want = ref_target(r, live_bases)
        if got != want:
            fail.append(
                "%s: %s +%#x resolves to %#x, not %#x" % (what, r["kind"], r["off"], got, want)
            )
        # (3) the BAKED value: what mhpatch independently wrote at the manifest's own placement.
        baked_got = ref_reads(r, file_bytes, file_va)
        baked_want = ref_target(r, file_bases)
        if baked_got != baked_want:
            fail.append(
                "%s: the REFERENCE FILE holds %#x at %s +%#x, but the relocation metadata says that "
                "dword names %#x -- the ref set disagrees with mhpatch's own output"
                % (what, baked_got, r["kind"], r["off"], baked_want)
            )
        mine[r["off"] : r["off"] + 4] = theirs[r["off"] : r["off"] + 4]
    if bytes(mine) != bytes(theirs):
        fail.append(
            "%s: bytes differ OUTSIDE the %d declared placement-sensitive dword(s)"
            % (what, len(refs))
        )
    return len(refs)


def _diff_runs(a, b):
    """Differing byte runs between two buffers, block-coarse then byte-exact. O(n) either way, but
    the block pass keeps the 5.8 MB grand_all reference out of a per-byte Python loop."""
    runs, n, BLK = [], min(len(a), len(b)), 4096
    i = 0
    while i < n:
        j = min(i + BLK, n)
        if a[i:j] == b[i:j]:
            i = j
            continue
        k = i
        while k < j:
            if a[k] != b[k]:
                s = k
                while k < n and a[k] != b[k]:
                    k += 1
                if runs and runs[-1][1] == s:
                    runs[-1] = (runs[-1][0], k)
                else:
                    runs.append((s, k))
            else:
                k += 1
        i = max(j, k)
    if len(b) > len(a):
        runs.append((len(a), len(b)))
    return runs


def compare(dump_path, manifest_path_, clean, ref_exe, say=print):
    m, secs, refs = derive_refs(manifest_path_)
    d = parse_dump(dump_path)
    pe = lief.PE.parse(ref_exe)
    va_to_off = _VaMap(pe)
    ref = bytearray(open(ref_exe, "rb").read())
    fail, notes = [], []

    say("live dump: %s" % dump_path)
    for k in ("manifest", "applied", "relocated", "refusal"):
        say("  %-10s %s" % (k, d["meta"].get(k)))
    want_name = os.path.basename(manifest_path_).replace(".mh.patch.json", "")
    if d["meta"].get("manifest") != want_name:
        fail.append(
            "the dump is for manifest `%s`, not `%s` -- comparing it would be meaningless"
            % (d["meta"].get("manifest"), want_name)
        )
        return fail, notes, 0, 0
    if d["meta"].get("applied") != "1":
        fail.append(
            "the live run did not apply the manifest (refusal: %s)" % d["meta"].get("refusal")
        )
        return fail, notes, 0, 0

    if m.get("imports"):
        fail.append(
            "manifest declares imports %s -- the in-memory path has no import-add equivalent"
            % list(m["imports"])
        )

    file_bases = [sva for sva, _v, _b in secs]
    live_bases = [
        d["sections"].get(i, {}).get("actual", sva) for i, (sva, _v, _b) in enumerate(secs)
    ]
    relocated = d["meta"].get("relocated") == "1"
    n_extents = n_bytes = n_refs = 0

    # ---- (1) the cave ----------------------------------------------------------------------------
    for i, (sva, svsz, blob) in enumerate(secs):
        live = d["sections"].get(i)
        if live is None:
            fail.append("section %d is missing from the live dump" % i)
            continue
        # No raw bytes for a zero-init section -- see synth_dump(); its content check is the tail.
        off = va_to_off(sva) if blob else 0
        file_bytes = bytes(ref[off : off + len(blob)]) if off is not None else None
        if file_bytes is None or len(file_bytes) != len(blob):
            fail.append("section %d (%#x) has no raw bytes in the reference exe" % (i, sva))
            continue
        n_extents += 1
        n_bytes += len(blob)
        srefs = _refs_for(refs, 1, i)
        n_refs += _cmp_blob(
            "section %d" % i,
            srefs,
            live["bytes"],
            live["actual"],
            live_bases,
            file_bytes,
            sva,
            file_bases,
            fail,
        )
        if live["actual"] != sva:
            notes.append(
                "section %d was RELOCATED (%08x -> %08x); %d bytes match outside the %d declared "
                "placement-sensitive dword(s), and every one of those resolves to the reference's "
                "target" % (i, sva, live["actual"], len(blob), len(srefs))
            )
        elif not fail:
            say(
                "  [ok] section %d %s @ %08x: %d bytes IDENTICAL to the reference exe"
                % (i, live["name"], sva, len(blob))
            )
        if not d["tail"].get(i, False):
            fail.append("section %d: the virtual tail past the data is not zero" % i)

    # ---- (2) the patch sites ---------------------------------------------------------------------
    site_fail_before = len(fail)
    for j, p in enumerate(m.get("patches", [])):
        va = int(str(p["va"]), 16)
        want = bytes.fromhex(re.sub(r"\s", "", p["bytes"]))
        live = d["sites"].get(j)
        if live is None:
            fail.append("site %d is missing from the live dump" % j)
            continue
        off = va_to_off(va)
        file_bytes = bytes(ref[off : off + len(want)]) if off is not None else b""
        n_extents += 1
        n_bytes += len(want)
        # A site never moves, so BOTH sides sit at the same VA; only the sections it points INTO
        # differ between them.
        n_refs += _cmp_blob(
            "site %d (%08x)" % (j, va),
            _refs_for(refs, 0, j),
            live["bytes"],
            va,
            live_bases,
            file_bytes,
            va,
            file_bases,
            fail,
        )
    n_sites = len(m.get("patches", []))
    if len(fail) == site_fail_before:
        say(
            "  [ok] %d site(s): bytes identical to the reference exe outside the declared "
            "placement-sensitive dwords, which track the live cave" % n_sites
        )

    # ---- (4) completeness: nothing mhpatch changed is outside the dump ----------------------------
    clean_b = open(clean, "rb").read()
    covered = []
    for j, p in enumerate(m.get("patches", [])):
        off = va_to_off(int(str(p["va"]), 16))
        if off is not None:
            covered.append((off, off + len(bytes.fromhex(re.sub(r"\s", "", p["bytes"])))))
    for i, (sva, svsz, blob) in enumerate(secs):
        off = va_to_off(sva)
        if off is not None:
            covered.append((off, off + len(blob)))
    # COVERAGE IS A UNION, not a lookup. Adjacent sites (the caphike family patches a base and its
    # bound four bytes apart) merge into ONE differing run that no single extent contains, so the
    # original "is this run inside some one extent" test reported real, fully-covered changes as
    # uncovered. Masking the union and then asking what is LEFT also reports the genuinely
    # unexplained part of a partly-covered run instead of the whole run.
    mask = bytearray(max(len(ref), len(clean_b)))
    one = b"\x01"
    for c0, c1 in covered:
        mask[c0:c1] = one * (c1 - c0)

    # The header end comes off the REFERENCE's own section count, not the clean file's plus one:
    # grand_all_caphike_storagecap_EN appends TWO sections, and a hardcoded +1 leaves the second
    # 40-byte section header looking like an unexplained change 40 bytes past the boundary.
    pe_off = int.from_bytes(ref[0x3C:0x40], "little")
    opt_off = pe_off + 24
    opt_sz = int.from_bytes(ref[pe_off + 20 : pe_off + 22], "little")
    hdr_end = opt_off + opt_sz + int.from_bytes(ref[pe_off + 6 : pe_off + 8], "little") * 40

    unexplained, n_hdr, n_grew = [], 0, 0
    for a, b in _diff_runs(clean_b, ref):
        i = a
        while i < b:
            if mask[i]:
                i += 1
                continue
            j = i
            while j < b and not mask[j]:
                j += 1
            if j <= hdr_end:
                n_hdr += 1
            elif i >= len(clean_b):
                n_grew += j - i
            else:
                unexplained.append((i, j))
            i = j
    if n_hdr:
        notes.append(
            "PE header/section table changed in %d run(s) -- expected: mhpatch appends a real "
            "section, the in-memory path replaces it with VirtualAlloc" % n_hdr
        )
    if n_grew:
        notes.append(
            "reference exe grew by %d bytes past the clean file's end -- the appended section's raw "
            "data (its mapped part is the `section` extent compared above) plus file-alignment "
            "padding the loader never maps" % n_grew
        )
    for a, b in unexplained[:20]:
        fail.append("mhpatch changed file bytes %#x..%#x that no dumped extent covers" % (a, b))
    if len(unexplained) > 20:
        fail.append("...and %d more uncovered run(s)" % (len(unexplained) - 20))

    notes.append("%d placement-sensitive dword(s) resolved on both sides" % n_refs)
    return fail, notes, n_extents, n_bytes


# ---- the offline arm (G180's correct move) --------------------------------------------------------


def synth_dump(manifest_path_, ref_exe, out_path, relocate_by=0x00200000, mutate=None):
    """A live dump as the applier WOULD write it, built from the reference file. No rig.

    This drives the whole comparison -- the parse, the ref resolution on both sides, the masking,
    the completeness pass -- with the sections displaced by `relocate_by`, so the relocated branch
    is exercised rather than assumed. It proves the CHECKER, not the DLL (see the module docstring);
    `mutate(kind, index, bytearray)` is how --selftest makes it lie.
    """
    m, secs, refs = derive_refs(manifest_path_)
    pe = lief.PE.parse(ref_exe)
    va_to_off = _VaMap(pe)
    ref = open(ref_exe, "rb").read()
    bases = [sva + relocate_by for sva, _v, _b in secs]
    name = os.path.basename(manifest_path_).replace(".mh.patch.json", "")

    L = [
        "# mh in-memory static-patch parity dump v1 (SYNTHETIC -- tools/check_inmem_patch_parity.py)",
        "manifest %s" % name,
        "sha256 %s" % (m.get("sha256") or ""),
        "applied 1",
        "relocated %d" % (1 if relocate_by else 0),
        "refusal -",
    ]

    def relocated_blob(blob, blob_va, own_refs):
        b = bytearray(blob)
        for r in own_refs:
            target = ref_target(r, bases)
            if r["kind"] == "abs32_to_section":
                b[r["off"] : r["off"] + 4] = target.to_bytes(4, "little")
            else:
                rel = (target - (blob_va + r["off"] + 4)) & 0xFFFFFFFF
                b[r["off"] : r["off"] + 4] = rel.to_bytes(4, "little")
        return b

    for i, (sva, svsz, blob) in enumerate(secs):
        # A ZERO-INIT section (the relocate/caphike family's array host) has no raw bytes at all --
        # mhpatch appends it with SizeOfRawData 0 and the loader zero-fills it, so there is nothing
        # in the file to read and nothing in the dump to compare. The `tail` verdict is what covers
        # it, and it covers the whole section rather than a prefix.
        raw = ref[va_to_off(sva) :][: len(blob)] if blob else b""
        b = relocated_blob(raw, bases[i], _refs_for(refs, 1, i))
        if mutate:
            mutate("section", i, b)
        L.append(
            "section %d %s preferred=%08X actual=%08X vsize=%08X datalen=%08X"
            % (i, m["sections"][i]["name"], sva, bases[i], svsz, len(blob))
        )
        L.append("extent section %d %08X %s" % (i, bases[i], b.hex()))
        L.append(
            "tail section %d %08X %08X allzero=1" % (i, bases[i] + len(blob), svsz - len(blob))
        )
    for j, p in enumerate(m.get("patches", [])):
        va = int(str(p["va"]), 16)
        n = len(bytes.fromhex(re.sub(r"\s", "", p["bytes"])))
        off = va_to_off(va)
        b = relocated_blob(ref[off : off + n], va, _refs_for(refs, 0, j))
        if mutate:
            mutate("site", j, b)
        L.append("extent site %d %08X %s" % (j, va, b.hex()))
    L.append("end")
    open(out_path, "w", encoding="ascii", newline="\n").write("\n".join(L) + "\n")
    return out_path


def offline_one(name, clean, say=print, mutate=None, relocate_by=0x00200000):
    """One manifest, rig-free. -> (fail, notes, extents, bytes, seconds)."""
    t0 = time.time()
    path = manifest_path(name)
    ref = scratch("ref_%s.exe" % name)
    build_reference(clean, path, ref)
    dump = scratch("synth_%s.dump" % name)
    synth_dump(path, ref, dump, relocate_by=relocate_by, mutate=mutate)
    fail, notes, n_ext, n_b = compare(dump, path, clean, ref, say=say)
    return fail, notes, n_ext, n_b, time.time() - t0


def offline(names, clean, say=print):
    rc = 0
    for name in names:
        say("")
        say("=== %s (offline) ===" % name)
        fail, notes, n_ext, n_b, secs = offline_one(name, clean, say=say)
        for n in notes:
            say("  [note] %s" % n)
        say("  compared %d extent(s), %d byte(s) in %.1fs" % (n_ext, n_b, secs))
        for f in fail[:10]:
            say("  MISMATCH: %s" % f)
        say("  %s" % ("OFFLINE OK" if not fail else "OFFLINE FAILED"))
        rc = rc or (1 if fail else 0)
    return rc


def selftest(clean):
    """The offline arm plus the planted negatives that prove it can go red."""
    ok = True

    def expect(nm, cond, detail=""):
        nonlocal ok
        print(
            "  [%s] %s%s"
            % ("ok" if cond else "FAIL", nm, ("  <- %s" % detail) if not cond and detail else "")
        )
        ok = ok and cond

    quiet = lambda *_a, **_k: None  # noqa: E731

    names = shipped()
    expect("the shipped set is non-empty and derived from the corpus", bool(names), names)
    # The gate unit must BE in the shipped set -- a gate pointed at a manifest nobody compiles in
    # would be green for ever and mean nothing.
    expect("the gate manifest is in the shipped set", GATE_MANIFEST in names, names)

    for name in names:
        fail, _n, n_ext, _b, secs = offline_one(name, clean, say=quiet)
        expect(
            "%s: offline parity is clean (%d extents, %.1fs)" % (name, n_ext, secs),
            not fail,
            fail[:3],
        )

    # ---- the planted negatives -------------------------------------------------------------------
    #
    # Each subject is CHOSEN by a property rather than named, so an arm cannot quietly become
    # vacuous when the shipped set changes: a manifest whose every site byte is covered by a ref has
    # no free byte to flip (projectile_pool_relocate_EN is exactly that -- 75 sites of four bytes,
    # each one whole-blob abs32), and a zero-init cave has no content to corrupt. The search below
    # finds a subject that has the property, and asserts that ONE EXISTS.
    sub = GATE_MANIFEST
    _m, _secs, refs = derive_refs(manifest_path(sub))
    site_ref = next(r for r in refs if r["in_section"] == 0)

    def wrong_target(kind, idx, b):
        # A ref dword that holds a plausible WRONG address: only the resolve check catches this,
        # because the masking would otherwise hide it.
        if kind == "site" and idx == site_ref["owner"]:
            cur = _u32(b, site_ref["off"])
            b[site_ref["off"] : site_ref["off"] + 4] = ((cur + 0x40) & 0xFFFFFFFF).to_bytes(
                4, "little"
            )

    fail, _n, _e, _b, _s = offline_one(sub, clean, say=quiet, mutate=wrong_target)
    expect(
        "a ref dword holding a wrong address makes the comparison RED",
        any("resolves to" in f for f in fail),
        fail[:2],
    )

    # A plain byte, covered by no ref: comparisons (1)/(2) alone must catch it.
    plain = None
    for name in names:
        _mm, _ss, rr = derive_refs(manifest_path(name))
        for j, p in enumerate(_mm.get("patches", [])):
            n = len(bytes.fromhex(re.sub(r"\s", "", p["bytes"])))
            cov = {o for r in _refs_for(rr, 0, j) for o in range(r["off"], r["off"] + 4)}
            free = [i for i in range(n) if i not in cov]
            if free:
                plain = (name, j, free[0])
                break
        if plain:
            break
    expect(
        "some shipped site has a byte no ref covers (the flip arm has a subject)",
        bool(plain),
        plain,
    )
    if plain:
        pname, pj, poff = plain

        def flip_plain(kind, idx, b):
            if kind == "site" and idx == pj:
                b[poff] ^= 0xFF

        fail, _n, _e, _b, _s = offline_one(pname, clean, say=quiet, mutate=flip_plain)
        expect(
            "a flipped byte outside every ref makes the comparison RED (%s site %d +%d)"
            % (pname, pj, poff),
            any("differ OUTSIDE" in f for f in fail),
            fail[:2],
        )

    # A corrupted CAVE byte. Only a manifest with initialised section data has one, and the parity
    # verdict F1E rests on is precisely about that cave, so the arm runs on whichever shipped
    # manifest has one rather than being skipped.
    caved = None
    for name in names:
        _mm, ss, _rr = derive_refs(manifest_path(name))
        if any(s[2] for s in ss):
            caved = name
            break
    expect(
        "some shipped manifest has an initialised cave (the corruption arm has a subject)",
        bool(caved),
        caved,
    )
    if caved:

        def zero_section(kind, idx, b):
            if kind == "section" and len(b):
                b[0] ^= 0xFF

        fail, _n, _e, _b, _s = offline_one(caved, clean, say=quiet, mutate=zero_section)
        expect(
            "a corrupted cave byte makes the comparison RED (%s)" % caved,
            any("differ OUTSIDE" in f for f in fail),
            fail[:2],
        )

    # The un-relocated path must work too: at relocate_by=0 the dump says `relocated 0` and every
    # blob must be byte-identical, refs included.
    fail, _n, _e, _b, _s = offline_one(sub, clean, say=quiet, relocate_by=0)
    expect("at the BAKED placement the comparison is byte-exact", not fail, fail[:3])

    # A dump for the wrong manifest must be refused rather than compared.
    other = [n for n in names if n != sub][0]
    ref = scratch("ref_%s.exe" % sub)
    d = synth_dump(manifest_path(sub), ref, scratch("mixed.dump"))
    fail, _n, _e, _b = compare(d, manifest_path(other), clean, ref, say=quiet)
    expect(
        "a dump for the WRONG manifest is refused",
        any("not `%s`" % other in f for f in fail),
        fail[:2],
    )

    print("check_inmem_patch_parity --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---- the live run --------------------------------------------------------------------------------


def provision():
    """Provision the stock-exe parity lane ONCE. Every manifest reuses it.

    A stock-exe lane is the whole point: its mh.focus.exe IS the retail EN exe (make_lane
    --stock-exe is the default since 2026-08-27) plus the msvfw32 proxy shim, so the loaded image is
    byte-exactly the one the manifest was baked against and the cave's baked VA is the image's real
    next-free VA. The APPLIED path needs that: workdir/mh_en carries no_cd + run_without_focus on
    disk, so an ordinary lane's guards would refuse (correct idempotence, useless as a subject).
    """
    dll = os.path.join(REPO, "src", "mh_dll", "Release", "mh.dll")
    if not os.path.exists(dll):
        raise SystemExit("build src/mh_dll/Release/mh.dll first (see src/mh_dll/README.md)")
    # THE LANE NUMBER IS ALLOCATED, not the literal 9 it was (fork F4H). 9 sits inside the capture
    # suite's block, and this is a GATE UNIT that launches beside the suite -- a shared lane number
    # is a shared single-instance mutex, and the loser dies at boot with no frame and no log line.
    import lane_alloc  # noqa: PLC0415

    subprocess.run(
        [
            sys.executable,
            os.path.join(HERE, "make_lane.py"),
            "--dst",
            LANE,
            "--lane",
            str(lane_alloc.lane("inmem", 0)),
        ],
        check=True,
    )


def run_lane(name, dump_out):
    """Arm the lane for ONE manifest, launch once, wait for the dump. -> seconds of rig time."""
    import make_lane  # noqa: F401  -- imported for its boot lock

    if os.path.exists(dump_out):
        os.remove(dump_out)
    # ONE config file (fork F2G). This rewrite CLOBBERS the lane ini make_lane just wrote, which
    # is deliberate -- the parity run wants a minimal config -- but it therefore also drops the
    # lane identity; lane 9 is provisioned above and this run is single-instance, so that is
    # harmless here and nowhere else. No [harness] section: the parity dump must run unharnessed.
    #
    # THE THREE KEYS, and each is load-bearing rather than tidy:
    #
    #  * `[net] enable=0` since fork F3G, and it is the proof rather than a preference: the patcher
    #    used to sit BEHIND that gate, so `[patch] inmem=1` with networking off was silently inert.
    #    The arm point is MH_Core_Arm's now (G68, docs/inmem-patching.md sec 3), so this run applies
    #    the manifest with the nine net steps skipped. A regression that puts it back behind a
    #    networking key turns this dump into `applied 0`, i.e. red, instead of nothing at all.
    #  * `[config] mode=original` since fork F4C-GATE, and it is what makes the SHIPPED set testable
    #    at all. The caphike/relocate family patches bodies this tree PROMOTES -- 4 of
    #    projectile_pool_relocate_EN's 8, 117 of grand_all_caphike_storagecap_EN's 530 -- and a site
    #    inside a promoted body is REFUSED by the C1 interlock, correctly: those bytes never execute,
    #    so the fix has to be carried by our implementation instead. Parity with mhpatch's on-disk
    #    output is a statement about the ORIGINAL image, so the lane runs the original engine. (The
    #    refusal itself is not untested -- it is patchtest's arm 6, off-rig.)
    #  * `[patch] manifest=` names which of the compiled-in alternatives to apply. Absent, the DLL
    #    refuses and lists them; it does not pick one.
    #
    # G181: this writes the whole file rather than appending a section, so there is no
    # first-match-wins hazard -- and it is read back below anyway, because a lane that was meant to
    # be configured a certain way and is not has not been configured, whatever the writer intended.
    ini = os.path.join(LANE, "mh_net.ini")
    text = (
        "[config]\nmode=original\n\n[net]\nenable=0\n\n[patch]\ninmem=1\nmanifest=%s\ndump=%s\ndump_exit=1\n"
        % (
            name,
            dump_out.replace("/", "\\"),
        )
    )
    with open(ini, "w", encoding="ascii") as f:
        f.write(text)
    back = open(ini, encoding="ascii").read()
    if back != text:
        raise SystemExit("the lane ini did not read back as written (%s)" % ini)
    stale = os.path.join(LANE, "mh_harness.ini")  # refused by the DLL if it survives
    if os.path.exists(stale):
        os.remove(stale)
    exe = os.path.join(LANE, "mh.focus.exe")
    t0 = time.time()
    with make_lane.boot_lock("parity run %s" % name):
        subprocess.run([exe], cwd=LANE, timeout=120)
    secs = time.time() - t0
    if not os.path.exists(dump_out):
        raise SystemExit(
            "the lane ran but wrote no dump -- did mh.dll load, and did it accept `[patch] "
            "manifest=%s`? see %s\\logs" % (name, LANE)
        )
    return secs


def main():
    ap = argparse.ArgumentParser(
        description="in-memory vs on-disk static-patch parity (F1E/F4C-GATE)"
    )
    ap.add_argument("--dump", help="compare an existing live dump (single manifest)")
    ap.add_argument(
        "--run", action="store_true", help="provision the lane and produce each dump first"
    )
    ap.add_argument(
        "--offline", action="store_true", help="rig-free arm over the selected manifests"
    )
    ap.add_argument("--selftest", action="store_true", help="--offline plus the planted negatives")
    ap.add_argument(
        "--manifest",
        action="append",
        help="manifest NAME (or path); repeatable. Default: every class `shipped` manifest",
    )
    ap.add_argument(
        "--gate", action="store_true", help="just the gate manifest (%s)" % GATE_MANIFEST
    )
    ap.add_argument("--clean", default=CLEAN)
    args = ap.parse_args()

    os.makedirs(TMP, exist_ok=True)
    sweep_stale_scratch()
    if args.selftest:
        return selftest(args.clean)

    names = args.manifest or ([GATE_MANIFEST] if args.gate else shipped())
    names = [os.path.basename(n).replace(".mh.patch.json", "") for n in names]
    unknown = [n for n in names if not os.path.exists(manifest_path(n))]
    if unknown:
        sys.exit("no such manifest: %s" % ", ".join(unknown))

    if args.offline:
        return offline(names, args.clean)

    if args.dump and len(names) > 1:
        sys.exit("--dump compares ONE manifest; name it with --manifest")

    if args.run:
        provision()

    rc, results = 0, []
    for name in names:
        print("")
        print("=== %s ===" % name)
        path = manifest_path(name)
        ref = scratch("ref_%s.exe" % name)
        t0 = time.time()
        report, warns = build_reference(args.clean, path, ref)
        print("reference: %s  (%d patch site(s) applied by mhpatch)" % (ref, len(report)))
        for w in warns:
            print("  WARN: %s" % w)
        ref_secs = time.time() - t0

        dump = args.dump or scratch("%s.dump" % name)
        lane_secs = 0.0
        if args.run:
            lane_secs = run_lane(name, dump)
        if not os.path.exists(dump):
            sys.exit("no live dump at %s -- run with --run, or point --dump at one" % dump)

        t0 = time.time()
        fail, notes, n_ext, n_bytes = compare(dump, path, args.clean, ref)
        cmp_secs = time.time() - t0
        for nte in notes:
            print("  [note] %s" % nte)
        print(
            "compared %d extent(s), %d byte(s)  [reference %.1fs, lane %.1fs, compare %.1fs]"
            % (n_ext, n_bytes, ref_secs, lane_secs, cmp_secs)
        )
        for f in fail[:10]:
            print("  MISMATCH: %s" % f)
        verdict = (
            "PARITY OK -- the in-memory result is the mhpatch result"
            if not fail
            else "PARITY FAILED"
        )
        print(verdict)
        results.append((name, not fail, ref_secs, lane_secs, cmp_secs))
        rc = rc or (1 if fail else 0)

    if len(results) > 1:
        print("")
        print("%-38s %-8s %8s %8s %8s" % ("manifest", "verdict", "ref s", "lane s", "cmp s"))
        for name, okk, a, b, c in results:
            print("%-38s %-8s %8.1f %8.1f %8.1f" % (name, "OK" if okk else "FAILED", a, b, c))
        print(
            "TOTAL %.1fs over %d manifest(s) -- %d OK, %d FAILED"
            % (
                sum(a + b + c for _n, _o, a, b, c in results),
                len(results),
                sum(1 for r in results if r[1]),
                sum(1 for r in results if not r[1]),
            )
        )
    return rc


if __name__ == "__main__":
    sys.exit(main())
