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
    python tools/check_inmem_patch_parity.py --run --caps         # PT-CAP8: both cap manifests, raise=0 and raise=1 lanes

THE CAP MANIFESTS' LIVE ARM (PT-CAP8 slice C). They are applied by mh/patch/caps_install.cpp at boot, not by
`[patch] inmem`, so the lane is `[caps] raise=<0|1>` + `[patch] dump=<path> dump_exit=1`: the caps path writes the
reloc manifest's dump to <path> and the limit manifest's to <path>.limits, right AFTER the gameplay settings apply
(so a raise=1 lane dumps the ON image). Two lanes: raise=0 (reloc sites == mhpatch reference; limit sites == the
VANILLA bytes, i.e. the reloc-only exe) and raise=1 (everything == the mhpatch reference chain). The boot also
logs `; [caps] relocation APPLIED (...) in N ms; limits OFF` -- the tool reads N back and holds it to a budget.
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


def caps_names():
    """PT-CAP8 S1: the v0.2.2 cap manifests in APPLY order (reloc, then limits). Not in the shipped
    compile list (they are not selectable by `[patch] manifest=`), but gated here all the same."""
    return [s.replace(".mh.patch.json", "") for s, _c, _k in gim.caps_manifests()]


def is_toggle(name):
    return gim.classes().get(name) in ("caps_limits", "caps_units_limits", "caps_ai", "caps_orders")


def manifest_path(name_or_path):
    if os.path.sep in name_or_path or name_or_path.endswith(".json"):
        return name_or_path
    return os.path.join(PATCHER, "%s.mh.patch.json" % name_or_path)


# ---- the reference build -------------------------------------------------------------------------


def build_reference_chain(clean, name, out):
    """-> (base_exe, report, warns). The cap manifests layer on each other (a section's VA is only the next-free VA
    once the earlier manifests' sections exist), so the reference of the k-th one in apply order (reloc22, limits22, ai,
    reloc23, limits23) is mhpatch over the k-1 before it, and the completeness pass must diff against that
    prerequisite-applied file, not the clean one. mhpatch's sha256 guard is lifted for every step after the first (its
    input is no longer the clean exe by construction). A non-cap manifest (and the first cap one) builds from clean."""
    path = manifest_path(name)
    order = caps_names()
    if name not in order or order.index(name) == 0:
        report, warns = build_reference(clean, path, out)
        return clean, report, warns
    base = clean
    for i, pre in enumerate(order[: order.index(name)]):
        nxt = scratch("base_%s.exe" % pre)
        src = manifest_path(pre)
        if i:
            doc = json.load(open(src, encoding="utf-8"))
            doc.pop("sha256", None)
            src = scratch("nosha_%s.json" % pre)
            with open(src, "w", encoding="utf-8") as fh:
                json.dump(doc, fh)
        build_reference(base, src, nxt)
        base = nxt
    doc = json.load(open(path, encoding="utf-8"))
    doc.pop("sha256", None)
    tmp_manifest = scratch("nosha_%s.json" % name)
    with open(tmp_manifest, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)
    report, warns = build_reference(base, tmp_manifest, out)
    return base, report, warns


def dump_for(dump, name):
    """The live dump a cap manifest is compared against (caps_install.cpp MH_Caps_DumpIfAsked names them)."""
    return {
        "cap_v022_reloc_EN": dump,
        "cap_v022_limits_EN": dump + ".limits",
        "cap_v023_units_reloc_EN": dump + ".units",
        "cap_v023_units_limits_EN": dump + ".units.limits",
        "cap_v022_aiscratch_EN": dump + ".ai",
        "cap_v023_orders_EN": dump + ".orders",
    }.get(name, dump)


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


def live_is_synthetic(dump_path):
    """True for a dump synth_dump() wrote (the offline arm): no DLL ran, so no hook rewrote anything."""
    try:
        return "SYNTHETIC" in open(dump_path, encoding="ascii", errors="replace").readline()
    except OSError:
        return False


def compare(dump_path, manifest_path_, clean, ref_exe, say=print, sites_vs=None, skip_ranges=()):
    """-> (fail, notes, extents, bytes). `sites_vs` (PT-CAP8): an exe whose bytes at the patch sites are what the
    live image must hold INSTEAD of the reference's -- the OFF state of a toggle (limit-layer) manifest, whose sites
    are the vanilla bytes (no refs to resolve) until the toggle writes them. `skip_ranges` (PT-CAP9-U4): (lo, hi) VA ranges
    that a LATER toggle manifest's sites rewrite when ON; a site of this manifest inside one is not comparable against the
    live image (the toggle's bytes sit over it) and is covered by that manifest's own ON-state compare."""
    m, secs, refs = derive_refs(manifest_path_)
    d = parse_dump(dump_path)
    pe = lief.PE.parse(ref_exe)
    va_to_off = _VaMap(pe)
    ref = bytearray(open(ref_exe, "rb").read())
    clean_bytes = open(clean, "rb").read()
    live_compose = (
        compose_sites()
        if os.path.basename(manifest_path_).startswith(("cap_v022_", "cap_v023_"))
        and not live_is_synthetic(dump_path)
        else {}
    )
    vs = bytearray(open(sites_vs, "rb").read()) if sites_vs else None
    vs_map = _VaMap(lief.PE.parse(sites_vs)) if sites_vs else None
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
            if "aiscratch" in os.path.basename(str(manifest_path_)) or "orders" in os.path.basename(
                str(manifest_path_)
            ):
                notes.append(
                    "section %d: the tail holds the 0xA5 guard bytes the layer adopt writes (by design; layout is covered by caps_selftest)"
                    % i
                )
            else:
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
        if live_compose and va in live_compose and live["bytes"] != want:
            # A composing DLL hook (cap_compose_allow.json) rewrote the bytes around this operand AFTER the
            # manifest: it keeps its site and replicates the displaced instruction against mh_caps_base[].
            # What must hold is that the bytes are NOT the vanilla ones (the hook is really installed).
            vanilla = bytes(clean_bytes[off : off + len(want)]) if off is not None else b""
            if live["bytes"] == vanilla:
                fail.append(
                    "site %d (%08x): the composing hook %s is NOT installed (the live bytes are still vanilla)"
                    % (j, va, live_compose[va])
                )
            else:
                notes.append(
                    "site %d (%08x) is rewritten by the composing DLL hook %s (cap_compose_allow.json) -- "
                    "not compared against the manifest's bytes" % (j, va, live_compose[va])
                )
            continue
        if vs is None and any(lo < va + len(want) and va < hi for lo, hi in skip_ranges):
            notes.append(
                "site %d (%08x) lies under a later toggle manifest's site -- compared by that manifest's ON-state check"
                % (j, va)
            )
            continue
        if vs is not None:
            # OFF state of a toggle manifest: the live bytes must be the VANILLA ones, exactly (no refs) -- except the dwords the
            # manifest's `expect` wildcards (`xx`): those are fields an always-on layer re-bases at run time (an address into a
            # relocated section), so the live value is the baked one shifted by that section's (64 KiB aligned) move.
            voff = vs_map(va)
            vanilla = bytes(vs[voff : voff + len(want)]) if voff is not None else b""
            toks = str(p.get("expect", "")).split()
            if live["bytes"] != vanilla and len(toks) == len(vanilla) == len(live["bytes"]):
                lv, vv = bytearray(live["bytes"]), bytearray(vanilla)
                for k0 in range(0, len(toks) - 3):
                    if all(t.lower() in ("xx", "??") for t in toks[k0 : k0 + 4]):
                        dl = int.from_bytes(lv[k0 : k0 + 4], "little")
                        dv = int.from_bytes(vv[k0 : k0 + 4], "little")
                        if (dl - dv) % 0x10000 == 0:
                            lv[k0 : k0 + 4] = vv[k0 : k0 + 4]
                if bytes(lv) == bytes(vv):
                    continue
            if live["bytes"] != vanilla:
                fail.append(
                    "site %d (%08x): OFF state holds %s, the vanilla bytes are %s"
                    % (j, va, live["bytes"].hex(), vanilla.hex())
                )
            continue
        # A site never moves, so BOTH sides sit at the same VA; only the sections it points INTO
        # differ between them.
        live_b = live["bytes"]
        if (
            live_b != file_bytes
            and not _refs_for(refs, 0, j)
            and len(live_b) == len(file_bytes) >= 5
            and live_b[0] == file_bytes[0] == 0xE9
            and live_b[5:] == file_bytes[5:]
        ):
            # PT-CAP9-U4: a toggle site's JMP rel32 into ANOTHER manifest's section (the reloc layer's .mhcapq): mh.dll re-aims it by that
            # section's move (caps_install), so the live rel32 is the baked one shifted by a 64 KiB aligned amount.
            if (
                int.from_bytes(live_b[1:5], "little") - int.from_bytes(file_bytes[1:5], "little")
            ) % 0x10000 == 0:
                live_b = bytes(file_bytes[:5]) + bytes(live_b[5:])
        n_refs += _cmp_blob(
            "site %d (%08x)" % (j, va),
            _refs_for(refs, 0, j),
            live_b,
            va,
            live_bases,
            file_bytes,
            va,
            file_bases,
            fail,
        )
    n_sites = len(m.get("patches", []))
    if len(fail) == site_fail_before:
        if vs is not None:
            say("  [ok] %d site(s): all hold the VANILLA bytes (limits OFF)" % n_sites)
        else:
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
    base, _rep, _w = build_reference_chain(clean, name, ref)
    dump = scratch("synth_%s.dump" % name)
    synth_dump(path, ref, dump, relocate_by=relocate_by, mutate=mutate)
    fail, notes, n_ext, n_b = compare(dump, path, base, ref, say=say)
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

    for name in names + caps_names():
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
    for name in names + caps_names():
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
    for name in names + caps_names():
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
    other = [n for n in names + caps_names() if n != sub][0]
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
    #  * `[caps] relocate=0` since PT-CAP8: the v0.2.2 relocation layer is applied at EVERY boot now (it is not a
    #    setting), and it rewrites the very sites these older manifests guard -- their expected-bytes guard would
    #    (correctly) refuse. The older manifests are parity subjects on the unrelocated layout.
    #
    # G181: this writes the whole file rather than appending a section, so there is no
    # first-match-wins hazard -- and it is read back below anyway, because a lane that was meant to
    # be configured a certain way and is not has not been configured, whatever the writer intended.
    ini = os.path.join(LANE, "mh_net.ini")
    text = (
        "[dev]\nunlock=1\n\n[config]\nmode=original\n\n[net]\nenable=0\n\n[caps]\nrelocate=0\n\n[patch]\ninmem=1\nmanifest=%s\ndump=%s\ndump_exit=1\n"
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


def compose_sites():
    """{site VA -> patch id}: the cap-manifest sites a shipped DLL byte patch deliberately rewrites AFTER the
    manifest (tools/data/cap_compose_allow.json). In a LIVE dump their bytes are the hook's, not the manifest's;
    the offline arm (no DLL) never sees them differ."""
    p = os.path.join(HERE, "data", "cap_compose_allow.json")
    if not os.path.isfile(p):
        return {}
    return {int(a["site"], 16): a["patch"] for a in json.load(open(p, encoding="utf-8"))["allowed"]}


CAPS_APPLIED_RE = re.compile(
    r"\[caps\] relocation APPLIED \((\d+) sites, (\d+) sections, base \.mhcapd=(?:0x)?([0-9A-Fa-f]+)\) in ([0-9.]+) ms"
)
CAPS_MS_WARN = 30.0  # the spec's budget (reloc + limits, dev box)
CAPS_MS_FAIL = 100.0  # a timing gate must not flake under rig load: fail only when 3x over


def run_lane_caps(dump_out, raise_):
    """One launch with `[caps] raise=<raise_>`. The caps path writes <dump_out> (reloc) and <dump_out>.limits.
    -> (seconds, the 'relocation APPLIED' log line or None)."""
    import make_lane  # noqa: F401  -- imported for its boot lock

    for p in (
        dump_out,
        dump_out + ".limits",
        dump_out + ".units",
        dump_out + ".units.limits",
        dump_out + ".ai",
    ):
        if os.path.exists(p):
            os.remove(p)
    ini = os.path.join(LANE, "mh_net.ini")
    # [config] mode=original: nothing promoted, so the reloc manifest's C1 interlock has nothing to refuse
    # (brokered+libmh refuses it by design). No [patch] inmem: the caps path owns the dump keys.
    text = (
        "[dev]\nunlock=1\n\n[config]\nmode=original\n\n[net]\nenable=0\n\n[caps]\nraise=%d\n\n"
        "[patch]\ndump=%s\ndump_exit=1\n" % (raise_, dump_out.replace("/", "\\"))
    )
    with open(ini, "w", encoding="ascii") as f:
        f.write(text)
    if open(ini, encoding="ascii").read() != text:
        raise SystemExit("the lane ini did not read back as written (%s)" % ini)
    stale = os.path.join(LANE, "mh_harness.ini")
    if os.path.exists(stale):
        os.remove(stale)
    exe = os.path.join(LANE, "mh.focus.exe")
    t0 = time.time()
    with make_lane.boot_lock("parity run caps raise=%d" % raise_):
        subprocess.run([exe], cwd=LANE, timeout=120)
    secs = time.time() - t0
    if not all(
        os.path.exists(dump_out + x) for x in ("", ".limits", ".units", ".units.limits", ".ai")
    ):
        raise SystemExit(
            "the lane ran but wrote no caps dump -- did mh.dll load and reach MH_Caps_DumpIfAsked? see %s\\logs"
            % LANE
        )
    line = None
    logs = sorted(
        glob.glob(os.path.join(LANE, "logs", "**", "mh_net.log"), recursive=True),
        key=os.path.getmtime,
    )
    if logs:
        for ln in open(logs[-1], encoding="utf-8", errors="replace"):
            if "[caps] relocation APPLIED" in ln:
                line = ln.strip()
    return secs, line


def caps_live(clean, run, say=print, dump_arg=None):
    """PT-CAP8: both cap manifests against live dumps, raise=0 then raise=1. -> rc."""
    rc = 0
    for raise_ in (0, 1):
        say("")
        say("##### caps lane: [caps] raise=%d #####" % raise_)
        dump = dump_arg or scratch("caps_raise%d.dump" % raise_)
        lane_secs = 0.0
        if run:
            lane_secs, line = run_lane_caps(dump, raise_)
            m = CAPS_APPLIED_RE.search(line or "")
            if not m:
                say(
                    "  FAIL: the boot log has no '[caps] relocation APPLIED (...) in N ms' line: %r"
                    % line
                )
                rc = 1
            else:
                ms = float(m.group(4))
                say(
                    "  [caps] APPLIED: %s sites, %s sections, .mhcapd=%s, %.2f ms (budget %.0f)"
                    % (m.group(1), m.group(2), m.group(3), ms, CAPS_MS_WARN)
                )
                if ms > CAPS_MS_FAIL:
                    say(
                        "  FAIL: apply took %.2f ms, over the %.0f ms hard limit"
                        % (ms, CAPS_MS_FAIL)
                    )
                    rc = 1
                elif ms > CAPS_MS_WARN:
                    say("  WARN: apply took %.2f ms, over the %.0f ms budget" % (ms, CAPS_MS_WARN))
        for name in caps_names():
            say("")
            say("=== %s (live, raise=%d) ===" % (name, raise_))
            path = manifest_path(name)
            ref = scratch("ref_%s.exe" % name)
            base, report, warns = build_reference_chain(clean, name, ref)
            dpath = dump_for(dump, name)
            sites_vs = base if (is_toggle(name) and raise_ == 0) else None
            skip = []
            if raise_ == 1 and not is_toggle(name):
                for later in caps_names():
                    if is_toggle(later) and later.replace("limits", "reloc") == name:
                        for q in json.load(open(manifest_path(later), encoding="utf-8")).get(
                            "patches", []
                        ):
                            qa = int(str(q["va"]), 16)
                            skip.append(
                                (qa, qa + len(bytes.fromhex(re.sub(r"\s", "", q["bytes"]))))
                            )
            fail, notes, n_ext, n_b = compare(
                dpath, path, base, ref, say=say, sites_vs=sites_vs, skip_ranges=skip
            )
            for nte in notes:
                say("  [note] %s" % nte)
            say("compared %d extent(s), %d byte(s)  [lane %.1fs]" % (n_ext, n_b, lane_secs))
            for f in fail[:10]:
                say("  MISMATCH: %s" % f)
            say(
                "PARITY OK -- raise=%d: the in-memory result is the mhpatch result%s"
                % (raise_, " (limit sites vanilla)" if sites_vs else "")
                if not fail
                else "PARITY FAILED (raise=%d)" % raise_
            )
            rc = rc or (1 if fail else 0)
    return rc


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
    ap.add_argument(
        "--caps",
        action="store_true",
        help="PT-CAP8: the two cap manifests (with --run: boot the raise=0 and raise=1 lanes)",
    )
    ap.add_argument("--clean", default=CLEAN)
    args = ap.parse_args()

    os.makedirs(TMP, exist_ok=True)
    sweep_stale_scratch()
    if args.selftest:
        return selftest(args.clean)

    names = args.manifest or (
        [GATE_MANIFEST]
        if args.gate
        else (caps_names() if args.caps else shipped() + (caps_names() if args.offline else []))
    )
    names = [os.path.basename(n).replace(".mh.patch.json", "") for n in names]
    unknown = [n for n in names if not os.path.exists(manifest_path(n))]
    if unknown:
        sys.exit("no such manifest: %s" % ", ".join(unknown))

    if args.offline:
        return offline(names, args.clean)

    if any(n in caps_names() for n in names):
        if not all(n in caps_names() for n in names) or sorted(names) != sorted(caps_names()):
            sys.exit("the cap manifests are checked together (reloc then limits): use --caps")
        if args.run:
            provision()
        elif not args.dump:
            sys.exit(
                "a live cap check needs --run (or --dump <reloc dump>, with <dump>.limits beside it)"
            )
        return caps_live(args.clean, args.run, dump_arg=args.dump)

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
