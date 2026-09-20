#!/usr/bin/env python3
"""gen_inmem_manifest.py -- compile an mhpatch manifest into a C++ header mh.dll applies IN MEMORY.

F1E, the in-DLL static-patch spike (the fork plan supplementary ruling: the src/patcher
manifests stop being an on-disk exe surgery and become compiled-in data that mh.dll applies to the
LOADED image at DllMain, with VirtualAlloc standing in for the appended section cave).
F4C built the `abs32_to_section` ref kind on top of it -- the spec is the "abs32_to_section ref
kind" section of docs/inmem-patching.md, and it was written before this code.

WHAT IS AND IS NOT DERIVABLE FROM A MANIFEST, because the whole feasibility question lives here.

  * A `bytes` patch is a literal blob and carries over unchanged.
  * An `asm` patch does NOT: `[import:dll!func]` resolves against the TARGET FILE's IAT, which the
    on-disk pipeline builds with `addimport`. In the fork there is no added import (the proxy
    injector replaces it), so an `asm` patch has no meaning until its import reference is re-pointed
    at a runtime GetProcAddress. This generator REFUSES such a manifest rather than emitting
    something that would assemble to a wrong slot.
  * A cave section is raw bytes plus ONE thing the JSON does not state: which dwords inside it are
    RELATIVE and therefore invalid at any VA but the one it was baked for. That is recovered here
    with capstone -- every branch whose target leaves the cave becomes an `inmem_ref` the applier
    rewrites for the actual placement -- and completeness is PROVEN by decoding every instruction
    rather than scanning for byte patterns.
  * AN ABSOLUTE POINTER INTO A SECTION IS THE OTHER HALF, and it is the one the relocate/caphike
    family is made of: there a "patch" is four operand bytes carved out of an instruction that
    starts before the patch VA -- `bytes: "79 00 0f 01"` is the dword 0x010f0079, the section base
    plus 0x79, with no opcode in the blob to decode at all. That is `abs32_to_section`, and it is
    why the 23 manifests F1E had to call NON-relocatable are representable now.

THE TWO PROOFS, AND WHY A SITE IS NOT PROVED THE WAY A SECTION IS. Placement-sensitivity is
relative to what MOVES. A section blob moves as a unit, is generator-assembled code with a known
entry at offset 0, and is therefore proved by a FULL DECODE: every byte consumed, every operand
enumerated, so the sensitive set is exact. A SITE blob does not move; it overwrites a fixed image
VA and MAY BEGIN MID-INSTRUCTION, so decoding it is unsound -- disassembling a bare relocation
dword yields a plausible-looking wrong branch. A site is proved instead by a decode-free VALUE
SCAN: at every byte offset, read the dword as an absolute AND as a rel32 and call it sensitive iff
either reading lands in a declared section. That scan is TOTAL by construction (every offset is
examined) and conservative (it can over-report, never under-report); measured over the corpus it
over-reports zero times on 46,294 site blobs. Where a multi-byte site blob does decode cleanly the
decode runs as a CORROBORATOR that may only disagree (a refusal) or add a refusal of its own --
never invent a ref, which is the unsound direction.

SILENT ACCEPTANCE IS THE BUG CLASS THIS FILE EXISTS TO KILL. Every manifest / patch / section key
is either consumed here or is documentation (`_`-prefixed); anything else REFUSES as an unknown
key. `recovered.mh.patch.json` used to be emitted `refs_complete=true` while its per-patch
`fixups` (read only by src/patcher/mhpatch.py) and its top-level `imports` were dropped on the
floor, and while its `call dword ptr [0x010f0123]` named a cave VA it declared no section for.

`refs_complete` IS A MANIFEST PROPERTY, NOT A STATEMENT ABOUT THE APPLIER. What today's
mh/patch/inmem_patch.h can actually consume is a separate axis, emitted separately as the
per-manifest APPLIER PREREQUISITES and enforced by an interlock: the compile list refuses an entry
whose prerequisites that header does not satisfy, so it cannot run ahead of the applier.

AND THE COMPILE LIST ITSELF IS DERIVED (fork ruling Q7, F4C-GATE): every manifest declares a
`class`, and the headers emitted here are every `shipped` one plus the index mh.dll selects from by
name. A hand list here would have been the same silent-acceptance bug one level up -- "which
manifests ship" living in a Python constant while the manifests said nothing.

The emitted header is data only -- no addresses are spelled by hand anywhere in src/mh_dll.

Run:  python tools/gen_inmem_manifest.py              # regenerate the committed header(s)
      python tools/gen_inmem_manifest.py --check      # header drift gate (lint_repo)
      python tools/gen_inmem_manifest.py --audit      # classify the WHOLE corpus; write the extract
      python tools/gen_inmem_manifest.py --check-audit # offline: re-derive vs the committed extract
      python tools/gen_inmem_manifest.py --selftest   # the refusal arms, on planted negatives
"""

import argparse
import bisect
import glob
import json
import os
import tempfile
import re
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PATCHER = os.path.join(REPO, "src", "patcher")
OUTDIR = os.path.join(REPO, "src", "mh_dll", "mh", "patch")
APPLIER_HEADER = os.path.join(OUTDIR, "inmem_patch.h")
# The generated INDEX of compiled-in manifests -- what mh.dll selects from by name.
INDEX_HEADER = "manifests.gen.h"

# TEST FIXTURES: manifests emitted for the OFFLINE selftest only, never for the DLL.
#
# This one IS a hand-written list, and the distinction from the compile list matters. `class` says
# what a manifest is FOR in the product; this says what a TEST needs, which is a different question
# with a different answer. `net_selftest patchtest` drives the applier's whole negative half -- a
# moved guarded byte, a taken cave VA, a site inside a promoted body, a contested entry window --
# and it needs a manifest small enough to reason about completely: ONE site, ONE body, and an
# initialised cave whose trailing `jmp` back into the game is a known address. no_cd_EN is that
# manifest, and it stays that manifest after Q7 reclassified it `reimplemented` (its behaviour ships
# as seams/standalone.cpp's trampoline, so it does not compile in). The alternative -- porting the
# arms onto a 7,770-site manifest spread over 530 bodies -- would trade a test whose every
# assertion names a known fact for one that asserts self-consistency.
#
# The fixture lands in mh_nettest/, not beside the shipped headers, so nothing can include it into
# the DLL by reflex; it is drift-gated by the same --check.
FIXTURE_DIR = os.path.join(REPO, "src", "mh_dll", "mh_nettest")
FIXTURES = [("no_cd_EN.mh.patch.json", "no_cd_en")]
AUDIT_EXTRACT = os.path.join(REPO, "tools", "data", "inmem_manifest_audit.json")
# The EN function extents. The SAME committed dump gen_dll_patches.py joins the promotable set
# against, used here for the F4C-COMP body table: which ORIGINAL FUNCTION each site lands in.
EN_FUNCTIONS = os.path.join(REPO, "tools", "data", "en_functions.json")

# The EN image's own extent, used only to classify a branch target as "back into the game" rather
# than "into the cave". ImageBase 0x400000; the first appended section of a stock EN exe lands at
# 0x10f0000 (gen_no_cd_manifest LAYOUT), so that is where the original image ends.
IMAGE_LO = 0x00400000
IMAGE_HI = 0x010F0000

# CAVE SPACE: where an appended section can live. An absolute naming this range that NO declared
# section covers is a manifest that references a page nothing will allocate -- un-appliable, not
# merely non-relocatable, so the bound decides a REFUSAL and has to be defensible.
#
# F4C set it at a flat IMAGE_HI + 64 MB and said so: a round number chosen to keep a misaligned
# `0xff32...` byte window from reading as a cave pointer, with the note that the largest declared
# section was 2.9 MB. F4C-GATE DERIVES IT instead, because F4C's own hand-off said to the moment
# MANIFESTS widened: the top of cave space is the highest VA any manifest in the corpus declares a
# section at, rounded up to the 64 KB VirtualAlloc granularity. That is a measurement of the
# layout the corpus actually uses rather than a guess with margin -- it TIGHTENS the window (today
# 0x13d0000 against the old 0x50f0000, a 44x smaller false-positive surface) and it moves on its own
# if a future manifest places a section higher. The derived value is written into the audit extract,
# so a change to it is a reviewable diff rather than an invisible reclassification.
CAVE_GRANULARITY = 0x10000

# ---- the key allow-lists ------------------------------------------------------------------------
# A key is CONSUMED or it is DOCUMENTATION (`_`-prefixed). There is no third category, because the
# third category is how `fixups` got dropped on the floor.
MANIFEST_KEYS = {"target", "sha256", "sections", "patches", "class"}

# WHICH MANIFESTS ARE COMPILED IN -- a MANIFEST FIELD, not a list here (fork ruling Q7, built at
# F4C-GATE). F1E hardcoded one entry and F4C left widening it to this item; a hand-kept list would
# have re-created the bug this file exists to kill one level up, since "which manifests ship" would
# then be folklore in a Python constant while the manifests themselves said nothing. `class` is a
# consumed key like any other: an unknown value refuses, a MISSING one refuses, and the compile list
# is derived by selecting class == shipped.
MANIFEST_CLASSES = {
    # compiled into mh.dll and parity-gated (tools/check_inmem_patch_parity.py).
    "shipped",
    # the behaviour already ships as DLL code -- compiling the manifest in would be a second carrier
    # for one fix (Q7: no_cd, run_without_focus, and by the same argument their CD-audio and
    # resync-wait siblings).
    "reimplemented",
    # the whole content is an `imports` edit: the addimport path the fork retires.
    "retired-addimport",
    # appliable only AFTER another manifest has run (storage_cap_variable_grand's cave names arrays
    # only grand_all_* relocates). Refused standalone; kept because it is still a working link in an
    # on-disk mhpatch SEQUENCE.
    "layered",
    # kept in the tree, never compiled in: the RU siblings of the EN family, and the
    # archaeological/sample manifests.
    "reference",
}
SHIPPED = "shipped"
PATCH_KEYS = {"va", "expect", "bytes"}
SECTION_KEYS = {"name", "vaddr", "vsize", "data", "chars"}

# Keys whose refusal deserves a reason better than "unknown": these are real mhpatch mechanisms the
# in-memory path cannot reproduce, and saying so is the difference between a gate and a mystery.
KEY_REASONS = {
    "imports": (
        "`imports` is an addimport-time instruction: it adds an IMPORT DESCRIPTOR to the target "
        "FILE, which the fork retires (the proxy injector replaces it). Accepting it would mean "
        "claiming parity with an mhpatch output that differs by a whole import section"
    ),
    "fixups": (
        "`fixups` overwrites 4 bytes with a resolved IAT slot VA (src/patcher/mhpatch.py:429) and "
        "is read by NOTHING on the in-memory path. A manifest whose relocation-critical key is "
        "dropped cannot be claimed relocatable"
    ),
}


def _manifest_paths(patcher=PATCHER):
    return sorted(glob.glob(os.path.join(patcher, "*.mh.patch.json")))


def manifest_name(path):
    return os.path.basename(path).replace(".mh.patch.json", "")


def cname_of(name):
    """The C++ namespace suffix for a manifest. Lower-cased and identifier-safe, so the header name
    and the namespace are both derivable from the manifest name and nothing is hand-mapped."""
    return re.sub(r"[^a-z0-9_]", "_", name.lower())


_CAVE_HI = None


def cave_hi(patcher=PATCHER):
    """The top of CAVE SPACE, derived from every section the corpus declares (see the constant
    block above). Cached: reading all 40 manifests costs ~40 ms and `_classify` asks per blob."""
    global _CAVE_HI
    if _CAVE_HI is None:
        top = IMAGE_HI
        for path in _manifest_paths(patcher):
            doc = json.load(open(path, encoding="utf-8"))
            for s in doc.get("sections") or []:
                top = max(top, int(str(s["vaddr"]), 16) + int(str(s["vsize"]), 0))
        _CAVE_HI = (top + CAVE_GRANULARITY - 1) & ~(CAVE_GRANULARITY - 1)
    return _CAVE_HI


def classes(patcher=PATCHER):
    """-> {manifest name: class}. Read from the RAW json, so a manifest the classifier refuses still
    reports the class it declares."""
    out = {}
    for path in _manifest_paths(patcher):
        out[manifest_name(path)] = json.load(open(path, encoding="utf-8")).get("class")
    return out


def shipped_manifests(patcher=PATCHER):
    """The compile list, DERIVED. -> [(filename, cname)] sorted by manifest name."""
    return [
        ("%s.mh.patch.json" % n, cname_of(n))
        for n, c in sorted(classes(patcher).items())
        if c == SHIPPED
    ]


class Refused(Exception):
    """The manifest is structurally unrepresentable in memory -- no header is emitted at all.

    Distinct from `refs_complete=false`, which still emits: that manifest is APPLIABLE at its baked
    VA and merely cannot be moved. A Refused one cannot be applied at any placement.
    """


def _hexbytes(s):
    return bytes.fromhex(re.sub(r"\s", "", s))


def _parse_expect(expect):
    """mhpatch's expect grammar -> (bytes, mask). 'xx'/'??' are wildcards (mask byte 0)."""
    out, mask = bytearray(), bytearray()
    for t in expect.split():
        if t.lower() in ("xx", "??"):
            out.append(0)
            mask.append(0)
        else:
            out.append(int(t, 16))
            mask.append(0xFF)
    return bytes(out), bytes(mask)


def _in_sections(value, sec_blobs):
    """Which declared section (if any) `value` points into, as (index, offset)."""
    for i, (sva, svsz, _b) in enumerate(sec_blobs):
        if sva <= value < sva + svsz:
            return i, value - sva
    return None


def _unread_keys(d, allowed, what):
    """-> [(where, key)] for every key that is neither consumed nor documentation (`_`-prefixed)."""
    return [(what, k) for k in d if not (k.startswith("_") or k in allowed)]


def _refuse_unread(name, found):
    """Refuse naming EVERY unread key, not just the first.

    `recovered.mh.patch.json` carries two of them at different levels -- top-level `imports` and
    per-patch `fixups` -- and reporting only the outermost would leave the one this item is
    actually about invisible in the verdict.
    """
    if not found:
        return
    parts = []
    for where, k in found:
        why = KEY_REASONS.get(k)
        parts.append("`%s` (%s)%s" % (k, where, (": " + why) if why else ""))
    raise Refused(
        "%s carries %d key(s) this generator does not read, and an unread key is refused rather "
        "than ignored: %s" % (name, len(found), "; ".join(parts))
    )


# ---- the decoder --------------------------------------------------------------------------------


def _md():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def _decode_all(blob, base_va):
    """Fully decode `blob` -> [(offset, insn)], or None if any byte will not decode.

    "Fully" means every byte is consumed by an instruction that FITS: a trailing partial
    instruction is not a decode, it is a coincidence that ran out of bytes.
    """
    md = _md()
    pos, out = 0, []
    while pos < len(blob):
        ins = next(md.disasm(blob[pos:], base_va + pos, count=1), None)
        if ins is None or pos + ins.size > len(blob):
            return None
        out.append((pos, ins))
        pos += ins.size
    return out


def _is_branch(ins):
    return capstone.x86.X86_GRP_JUMP in ins.groups or capstone.x86.X86_GRP_CALL in ins.groups


def _operand_dwords(ins, at):
    """Every 4-byte-wide operand of `ins` that could name an address.

    -> [(blob_offset, value, is_branch_target, width)]. A branch immediate is the target VA
    capstone already resolved; a non-branch immediate and a memory DISPLACEMENT are the value itself.

    A disp32 counts WHATEVER the base and index registers are. This is a 32-bit non-PIC image, so
    `mov [edx + 0x0127ccd0], eax` is an absolute array base plus a register INDEX -- exactly as
    placement-sensitive as a bare `[disp32]`, and exactly the form the caphike manifests patch at
    their own sites (`movzx eax, word ptr [eax + 0xc3d2a2]`). F1E's scanner required
    `base == 0 and index == 0`, which silently skipped all 15 relocated-array references inside
    `grand_all_caphike_storagecap`'s `.mhcap` cave; the totality assertion is what surfaced them.
    """
    out = []
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_IMM:
            out.append(
                (
                    at + ins.encoding.imm_offset,
                    op.imm & 0xFFFFFFFF,
                    _is_branch(ins),
                    ins.encoding.imm_size,
                )
            )
        elif op.type == capstone.x86.X86_OP_MEM and ins.encoding.disp_size:
            out.append(
                (
                    at + ins.encoding.disp_offset,
                    op.mem.disp & 0xFFFFFFFF,
                    False,
                    ins.encoding.disp_size,
                )
            )
    return out


# ---- the site proof: a decode-free value scan, plus a corroborating decode -----------------------


def _value_scan(blob, base_va, sec_blobs, internal=None):
    """Every placement-sensitive dword in `blob`, found WITHOUT decoding.

    -> ({offset: (kind, target_section, section_offset)}, [ambiguous offsets]).

    At each byte offset the dword is read both ways. `va + o + 4` is the exact end-of-instruction
    for every x86 rel32 encoding (E8 / E9 / 0F 8x all carry the immediate last), so the rel reading
    needs no decode to be correct. `internal` is a (lo, hi) VA range whose rel targets are NOT
    sensitive -- a section's own extent, which a uniform move keeps valid.
    """
    found, ambiguous = {}, []
    for o in range(0, max(0, len(blob) - 3)):
        raw = blob[o : o + 4]
        cand = []
        hit = _in_sections(int.from_bytes(raw, "little"), sec_blobs)
        if hit:
            cand.append(("abs32_to_section", hit[0], hit[1]))
        target = (base_va + o + 4 + int.from_bytes(raw, "little", signed=True)) & 0xFFFFFFFF
        if not (internal and internal[0] <= target < internal[1]):
            hit = _in_sections(target, sec_blobs)
            if hit:
                cand.append(("rel32_to_section", hit[0], hit[1]))
        if len(cand) > 1:
            ambiguous.append(o)
        elif cand:
            found[o] = cand[0]
    return found, ambiguous


def _corroborate_site(blob, base_va, sec_blobs):
    """Decode a multi-byte site blob, if it decodes at all, as a CHECK on the value scan.

    -> (section_targeting_map, hard_refusal_or_None) or (None, hard) when the blob does not decode
    (a mid-instruction fragment -- expected, and not itself a problem).
    """
    ins_list = _decode_all(blob, base_va)
    if ins_list is None:
        return None, None
    seen = {}
    for at, ins in ins_list:
        for off, value, is_branch, width in _operand_dwords(ins, at):
            hit = _in_sections(value, sec_blobs)
            if hit:
                if width == 4:
                    seen[off] = (
                        "rel32_to_section" if is_branch else "abs32_to_section",
                        hit[0],
                        hit[1],
                    )
                continue
            if not is_branch and IMAGE_HI <= value < cave_hi():
                return None, (
                    "holds an absolute %#x at +%#x naming CAVE SPACE that no declared section "
                    "covers -- nothing will allocate that page, so the patch cannot be applied at "
                    "ANY placement" % (value, off)
                )
    return seen, None


def _site_refs(blob, base_va, sec_blobs, what):
    """The complete ref set for one site blob. -> (refs_by_offset, soft_reason_or_None).

    Raises Refused for the un-appliable shapes. `soft_reason` makes the manifest
    refs_complete=false (appliable, not movable).
    """
    if len(blob) < 4:
        return {}, None  # no dword fits; nothing here can be placement-sensitive

    found, ambiguous = _value_scan(blob, base_va, sec_blobs)

    if len(blob) == 4 and not found:
        # The whole blob IS the dword, so this reading is exact rather than a guess.
        value = int.from_bytes(blob, "little")
        if IMAGE_HI <= value < cave_hi():
            raise Refused(
                "%s is the bare dword %#x, naming CAVE SPACE that no declared section covers"
                % (what, value)
            )

    if ambiguous:
        return (
            found,
            "%s: offset +%#x reads as BOTH an absolute and a rel32 into a declared "
            "section; which one moves is undecidable" % (what, ambiguous[0]),
        )

    offs = sorted(found)
    for a, b in zip(offs, offs[1:]):
        if b - a < 4:
            return (
                found,
                "%s: sensitive dwords at +%#x and +%#x overlap; only one rewrite can win"
                % (
                    what,
                    a,
                    b,
                ),
            )

    if len(blob) > 4:
        seen, hard = _corroborate_site(blob, base_va, sec_blobs)
        if hard:
            raise Refused("%s %s" % (what, hard))
        if seen is not None and seen != found:
            return found, (
                "%s: the decode and the value scan disagree about which dwords move "
                "(decode=%s value-scan=%s)" % (what, sorted(seen), offs)
            )
    return found, None


# ---- the section proof: a full decode --------------------------------------------------------


def _section_refs(blob, base_va, vsize, sec_blobs, idx):
    """The complete ref set for one section blob. -> (refs_by_offset, soft_reason_or_None).

    A section blob is assembled code with a known entry, so the decode IS the proof: every byte
    consumed, every operand enumerated. A byte that will not decode is F1E's refs_complete=false.
    """
    ins_list = _decode_all(blob, base_va)
    if ins_list is None:
        md = _md()
        pos = 0
        while pos < len(blob):
            ins = next(md.disasm(blob[pos:], base_va + pos, count=1), None)
            if ins is None or pos + ins.size > len(blob):
                break
            pos += ins.size
        return {}, "section %d: undecodable byte at +0x%x" % (idx, pos)

    found = {}
    for at, ins in ins_list:
        for off, value, is_branch, width in _operand_dwords(ins, at):
            if is_branch:
                if base_va <= value < base_va + vsize:
                    continue  # self-relative inside the section; a uniform move keeps it valid
                if width != 4:
                    return (
                        found,
                        "section %d: rel%d branch to %#x at +0x%x leaves the blob and cannot be relocated"
                        % (
                            idx,
                            width * 8,
                            value,
                            at,
                        ),
                    )
                hit = _in_sections(value, sec_blobs)
                if hit:
                    found[off] = ("rel32_to_section", hit[0], hit[1])
                elif IMAGE_LO <= value < IMAGE_HI:
                    found[off] = ("rel32_to_image", 0, value)
                else:
                    return (
                        found,
                        "section %d: branch to %#x is neither cave-local nor in the image"
                        % (
                            idx,
                            value,
                        ),
                    )
                continue
            hit = _in_sections(value, sec_blobs)
            if hit:
                if width != 4:
                    return (
                        found,
                        "section %d: a %d-bit absolute at +0x%x names section %d and cannot be relocated"
                        % (
                            idx,
                            width * 8,
                            off,
                            hit[0],
                        ),
                    )
                found[off] = ("abs32_to_section", hit[0], hit[1])
            elif IMAGE_HI <= value < cave_hi():
                raise Refused(
                    "section %d holds an absolute %#x at +%#x naming CAVE SPACE that no declared "
                    "section covers" % (idx, value, off)
                )
    return found, None


# ---- the totality assertion ---------------------------------------------------------------------


def _assert_total(blob, base_va, sec_blobs, emitted_offsets, what, internal=None):
    """Every placement-sensitive offset must be covered by an emitted ref. Independently re-derived.

    This cannot fire on correct code, which is exactly why --selftest fires it by SUPPRESSING a ref:
    a totality check nobody has ever seen go red is not evidence that it works.
    """
    found, _ambiguous = _value_scan(blob, base_va, sec_blobs, internal=internal)
    missing = sorted(set(found) - set(emitted_offsets))
    if missing:
        return "%s: +%#x is placement-sensitive (%s) and no emitted ref covers it" % (
            what,
            missing[0],
            found[missing[0]][0],
        )
    return None


# ---- F4C-COMP: which ORIGINAL BODY each site lands in --------------------------------------------
#
# The applier has to be able to NAME the function a site sits in (fork ruling Q6: collisions are
# refused by name), and it cannot work that out at runtime: the only extent table mh.dll carries is
# mh::addr::promotable_ranges -- 401 bodies, against the 2,892 en_functions.json knows and the ~619
# distinct ones a single caphike manifest lands in. So the join happens HERE, from the same committed
# dump gen_dll_patches.py already uses, and the answer is compiled in beside the sites.


def function_extents(path=EN_FUNCTIONS):
    """-> [(entry, end, name)] sorted by entry. `end` is INCLUSIVE, as the dump records it."""
    doc = json.load(open(path, encoding="utf-8"))
    return sorted((int(r["entry"], 16), int(r["end"], 16), r["name"]) for r in doc["functions"])


def containing_bodies(site_vas, extents=None):
    """-> ([(entry, end, name)] sorted, orphan_count).

    A site whose VA no function body covers contributes NO row and is counted instead: patches into
    data, and into the gaps Watcom leaves between bodies, are legitimate and must not be attributed
    to the nearest neighbour -- that is exactly the loose-containment reading that makes an overlap
    census unreproducible (see tools/check_inmem_composition.py).
    """
    if extents is None:
        extents = function_extents()
    starts = [e[0] for e in extents]
    hit, orphans = {}, 0
    for va in site_vas:
        i = bisect.bisect_right(starts, va) - 1
        if i < 0 or va > extents[i][1]:
            orphans += 1
            continue
        hit[extents[i][0]] = extents[i]
    return [hit[k] for k in sorted(hit)], orphans


# ---- the applier interlock ----------------------------------------------------------------------


def applier_caps(path=APPLIER_HEADER):
    """What mh/patch/inmem_patch.h can actually consume, PARSED rather than assumed.

    -> {"ref_kinds": {...}, "owner_bits": N, "off_bits": N, "target_section_bits": N}.
    """
    text = open(path, encoding="utf-8").read()
    body = re.search(r"enum class ref_kind\s*:\s*\w+\s*\{(.*?)\}", text, re.S)
    kinds = set()
    if body:
        stripped = re.sub(r"//.*", "", body.group(1))
        kinds = set(re.findall(r"\b([a-z][a-z0-9_]*)\b\s*(?:,|$)", stripped, re.M))
    caps = {"ref_kinds": kinds}
    struct = re.search(r"struct inmem_ref\s*\{(.*?)\};", text, re.S)
    fields = struct.group(1) if struct else ""
    for field in ("owner", "off", "target_section"):
        m = re.search(r"uint(\d+)_t\s+%s\s*;" % field, fields)
        caps["%s_bits" % field] = int(m.group(1)) if m else 0
    return caps


PROMOTED_HEADER = os.path.join(REPO, "src", "mh_dll", "mh", "hook", "promoted.h")


def _max_patched_bodies(path=PROMOTED_HEADER):
    """mh::hook::MAX_PATCHED_BODIES, PARSED -- the same discipline as applier_caps() above.

    The generated header carries a static_assert against this constant, so the build is the real
    gate; this is so `--selftest` can say WHICH manifest would not fit and by how much, offline,
    before anyone waits for a compiler error.
    """
    m = re.search(r"MAX_PATCHED_BODIES\s*=\s*(\d+)", open(path, encoding="utf-8").read())
    if not m:
        raise SystemExit("no MAX_PATCHED_BODIES in %s" % path)
    return int(m.group(1))


def _prereqs(refs, caps):
    """What this manifest needs that the applier header does not provide yet. Sorted, so the audit
    extract is a stable artifact.

    A field width is reported ONCE, against the largest value that overflows it -- the question is
    "how wide must this field be", not "which of 4,000 rows noticed".
    """
    need, widen = set(), {}
    for _in_sec, owner, off, kind, tsec, _target in refs:
        if kind not in caps["ref_kinds"]:
            need.add("ref_kind::%s" % kind)
        for field, value in (("owner", owner), ("off", off), ("target_section", tsec)):
            bits = caps["%s_bits" % field]
            if bits and value >= (1 << bits):
                widen[field] = max(widen.get(field, 0), value)
    for field, value in widen.items():
        need.add(
            "inmem_ref::%s must hold %d (today uint%d_t)" % (field, value, caps["%s_bits" % field])
        )
    return sorted(need)


# ---- emission -----------------------------------------------------------------------------------


def _classify(manifest_path, _suppress_ref=None):
    """Read + classify one manifest. -> dict with refs, completeness and the reason.

    Raises Refused for a structurally unrepresentable manifest. `_suppress_ref` is the --selftest
    hook that drops one emitted ref so the totality assertion can be SEEN to fire.
    """
    m = json.load(open(manifest_path, encoding="utf-8"))
    name = os.path.basename(manifest_path).replace(".mh.patch.json", "")

    # The class is checked BEFORE anything else about the content, because it is the key that says
    # what the rest of this file is for. A manifest with no class is refused rather than defaulted:
    # defaulting would let a new manifest join the corpus silently, which is the same silence the
    # unread-key rule below exists to end -- one level up.
    cls = m.get("class")
    if cls is None:
        raise Refused(
            "%s has no `class` field. Every manifest declares what it is for (one of %s) -- the "
            "compile list is DERIVED from it (fork ruling Q7), so an unclassed manifest would be "
            "a file nobody can say ships or does not" % (name, ", ".join(sorted(MANIFEST_CLASSES)))
        )
    if cls not in MANIFEST_CLASSES:
        raise Refused(
            "%s declares class `%s`, which is not one of %s"
            % (name, cls, ", ".join(sorted(MANIFEST_CLASSES)))
        )

    secs = m.get("sections") or []
    unread = _unread_keys(m, MANIFEST_KEYS, "manifest")
    for j, p in enumerate(m.get("patches", [])):
        unread += _unread_keys(p, PATCH_KEYS | {"asm"}, "patch %d" % j)
    for i, s in enumerate(secs):
        unread += _unread_keys(s, SECTION_KEYS, "section %d" % i)
    _refuse_unread(name, unread)

    for p in m.get("patches", []):
        if "asm" in p:
            raise Refused(
                "%s: patch at %s is `asm`, which resolves [import:...] against the target file's "
                "IAT. The in-memory path has no added import -- it needs the reference re-pointed "
                "at a runtime GetProcAddress, which is an applier-side import table this generator "
                "cannot emit into." % (name, p["va"])
            )

    sec_blobs = []
    for s in secs:
        blob = _hexbytes(s["data"]) if s.get("data") else b""
        sec_blobs.append((int(str(s["vaddr"]), 16), int(str(s["vsize"]), 0), blob))

    complete, why = True, ""

    def soften(reason):
        """Keep the FIRST reason. A relocate manifest has hundreds of unrepresentable patches and
        the last one is never the informative one."""
        nonlocal complete, why
        if complete and reason:
            complete, why = False, reason

    refs = []  # (in_section, owner_idx, offset, kind, target_section, target)

    for i, (va, vsz, blob) in enumerate(sec_blobs):
        if not blob:
            continue
        found, reason = _section_refs(blob, va, vsz, sec_blobs, i)
        soften(reason)
        if reason is None:
            soften(
                _assert_total(blob, va, sec_blobs, found, "section %d" % i, internal=(va, va + vsz))
            )
        for off in sorted(found):
            kind, tsec, target = found[off]
            refs.append((1, i, off, kind, tsec, target))

    for j, p in enumerate(m.get("patches", [])):
        va = int(str(p["va"]), 16)
        blob = _hexbytes(p["bytes"])
        found, reason = _site_refs(blob, va, sec_blobs, "patch %d" % j)
        soften(reason)
        if reason is None:
            soften(_assert_total(blob, va, sec_blobs, found, "patch %d" % j))
        for off in sorted(found):
            kind, tsec, target = found[off]
            refs.append((0, j, off, kind, tsec, target))

    if _suppress_ref is not None and refs:
        dropped = refs.pop(_suppress_ref % len(refs))
        blob = (
            sec_blobs[dropped[1]][2] if dropped[0] else _hexbytes(m["patches"][dropped[1]]["bytes"])
        )
        base = (
            sec_blobs[dropped[1]][0] if dropped[0] else int(str(m["patches"][dropped[1]]["va"]), 16)
        )
        owned = [r[2] for r in refs if r[0] == dropped[0] and r[1] == dropped[1]]
        internal = (
            (sec_blobs[dropped[1]][0], sec_blobs[dropped[1]][0] + sec_blobs[dropped[1]][1])
            if dropped[0]
            else None
        )
        complete, why = True, ""
        soften(_assert_total(blob, base, sec_blobs, owned, "suppressed", internal=internal))

    return {
        "name": name,
        "class": cls,
        "manifest": m,
        "sections": secs,
        "sec_blobs": sec_blobs,
        "refs": refs,
        "complete": complete,
        "why": why,
    }


def _emit(manifest_path, cname, fixture=False):
    c = _classify(manifest_path)
    m, name, secs, sec_blobs, refs = (
        c["manifest"],
        c["name"],
        c["sections"],
        c["sec_blobs"],
        c["refs"],
    )
    complete, why = c["complete"], c["why"]

    L = []
    a = L.append
    a(
        "// GENERATED by tools/gen_inmem_manifest.py from src/patcher/%s -- do not hand-edit."
        % os.path.basename(manifest_path)
    )
    a("//")
    a("// %s" % (m.get("target") or name))
    a("//")
    a(
        "// The manifest as COMPILED-IN DATA: mh.dll applies it to the loaded image instead of mhpatch"
    )
    a(
        "// applying it to a file. `refs` are the dwords inside the cave and the patch bytes that are"
    )
    a(
        "// RELATIVE and therefore only valid at one placement -- recovered by decoding, not scanning,"
    )
    a("// so `refs_complete` is a proof and not a hope. See mh/patch/inmem_patch.h.")
    if fixture:
        a("//")
        a(
            "// TEST FIXTURE (fork F4C-GATE). This manifest is NOT compiled into mh.dll -- its class is"
        )
        a(
            "// not `shipped`. It exists here because net_selftest's `patchtest` drives the applier's"
        )
        a(
            "// negative half over a manifest small enough to reason about completely, and it must be"
        )
        a("// the GENERATED bytes rather than a hand-typed stand-in or the arms stop asserting the")
        a(
            "// generator. Do not include this from the DLL; the shipped set is patch/manifests.gen.h."
        )
    a("#pragma once")
    a('#include "hook/promoted.h"')
    a('#include "patch/inmem_patch.h"')
    a("")
    a("namespace mh::patch::manifest_%s {" % cname)
    a("")
    for i, (va, vsz, blob) in enumerate(sec_blobs):
        if blob:
            rows = [
                "    " + ", ".join("0x%02x" % b for b in blob[k : k + 12])
                for k in range(0, len(blob), 12)
            ]
            a("inline constexpr uint8_t SEC%d_DATA[] = {" % i)
            L.extend([r + "," for r in rows])
            a("};")
    a("inline constexpr inmem_section SECTIONS[] = {")
    for i, (va, vsz, blob) in enumerate(sec_blobs):
        a(
            '    {"%s", 0x%08xu, 0x%xu, %s, %du, 0x%08xu},'
            % (
                secs[i]["name"],
                va,
                vsz,
                ("SEC%d_DATA" % i) if blob else "nullptr",
                len(blob),
                int(str(secs[i].get("chars", "0xE0000040")), 0),
            )
        )
    a("};")
    a("")
    for j, p in enumerate(m.get("patches", [])):
        exp, mask = _parse_expect(p.get("expect", ""))
        rep = _hexbytes(p["bytes"])
        if len(exp) != len(rep):
            raise SystemExit(
                "%s: patch %d expect/bytes length mismatch (%d vs %d)"
                % (name, j, len(exp), len(rep))
            )
        a(
            "inline constexpr uint8_t P%d_EXPECT[] = {%s};"
            % (j, ", ".join("0x%02x" % b for b in exp))
        )
        a(
            "inline constexpr uint8_t P%d_MASK[]   = {%s};"
            % (j, ", ".join("0x%02x" % b for b in mask))
        )
        a(
            "inline constexpr uint8_t P%d_REPL[]   = {%s};"
            % (j, ", ".join("0x%02x" % b for b in rep))
        )
    a("inline constexpr inmem_site SITES[] = {")
    for j, p in enumerate(m.get("patches", [])):
        a(
            "    {0x%08xu, P%d_EXPECT, P%d_MASK, P%d_REPL, %du},"
            % (int(str(p["va"]), 16), j, j, j, len(_hexbytes(p["bytes"])))
        )
    a("};")
    a("")
    # F4C-COMP: the original bodies these sites land in, so the applier can register them with the
    # C1 interlock BY NAME. Sorted by entry, one row per distinct body.
    bodies, orphans = containing_bodies(int(str(p["va"]), 16) for p in m.get("patches", []))
    if bodies:
        a("// F4C-COMP: the ORIGINAL bodies these sites land in (tools/data/en_functions.json).")
        if orphans:
            a(
                "// %d site(s) lie in no function body at all (data, or an inter-body gap) and are"
                % orphans
            )
            a("// deliberately attributed to nothing -- see containing_bodies().")
        a("inline constexpr inmem_body BODIES[] = {")
        for entry, end, fname in bodies:
            a('    {"%s", 0x%08xu, 0x%08xu},' % (fname, entry, end))
        a("};")
    else:
        a("// F4C-COMP: no site lands inside a known function body (%d orphan site(s))." % orphans)
        a("inline constexpr inmem_body *BODIES = nullptr;")
    a("")
    if refs:
        a("inline constexpr inmem_ref REFS[] = {")
        for in_sec, owner, off, kind, tsec, target in refs:
            a(
                "    {%d, %d, %du, ref_kind::%s, %d, 0x%08xu},"
                % (in_sec, owner, off, kind, tsec, target)
            )
        a("};")
    else:
        a("inline constexpr inmem_ref *REFS = nullptr;")
    a("")
    if not complete:
        a("// refs_complete = FALSE: %s" % why)
    a("inline constexpr inmem_manifest MANIFEST = {")
    a('    "%s",' % name)
    a('    "%s",' % (m.get("sha256") or ""))
    a("    SECTIONS, %d," % len(sec_blobs))
    a("    SITES, %d," % len(m.get("patches", [])))
    a("    %s, %d," % ("REFS" if refs else "nullptr", len(refs)))
    a("    %s, %d," % ("BODIES" if bodies else "nullptr", len(bodies)))
    a("    /*refs_complete=*/%s," % ("true" if complete else "false"))
    a("};")
    a("")
    # THE REGISTRY BOUND, AS A COMPILE ERROR (F4C-GATE). mh::hook::note_patched_body drops a body
    # once the table is full and says so loudly at runtime -- but a manifest whose body count
    # exceeds the table can be KNOWN not to fit before it is ever applied, and a build that cannot
    # register everything it patches has an incomplete interlock. The worst shipped manifest today
    # is 530 bodies against 1024 slots; this is what tells the next person who widens the compile
    # list, at build time rather than in a lane.
    a("static_assert(%d <= mh::hook::MAX_PATCHED_BODIES," % len(bodies))
    a(
        '              "%s registers more bodies than mh::hook\'s patched-body table holds -- '
        'raise MAX_PATCHED_BODIES");' % name
    )
    a("")
    a("} // namespace mh::patch::manifest_%s" % cname)
    a("")
    return "\n".join(L)


# ---- the corpus audit ---------------------------------------------------------------------------


def corpus_rows(patcher=PATCHER, caps=None):
    """Classify EVERY manifest in src/patcher. Deterministic, name-sorted, no paths, no clocks."""
    if caps is None:
        caps = applier_caps()
    rows = []
    for path in sorted(glob.glob(os.path.join(patcher, "*.mh.patch.json"))):
        name = manifest_name(path)
        raw = json.load(open(path, encoding="utf-8"))
        # `class` comes off the RAW json: a manifest the classifier refuses still declares what it
        # is for, and the audit's job is to show both answers side by side.
        row = {"name": name, "class": raw.get("class"), "sites": len(raw.get("patches", []))}
        try:
            c = _classify(path)
        except Refused as e:
            row.update(
                verdict="refused", reason=str(e), sections=0, refs=0, ref_kinds={}, prereqs=[]
            )
            rows.append(row)
            continue
        kinds = {}
        for r in c["refs"]:
            kinds[r[3]] = kinds.get(r[3], 0) + 1
        row.update(
            verdict="complete" if c["complete"] else "incomplete",
            reason=c["why"],
            sections=len(c["sec_blobs"]),
            refs=len(c["refs"]),
            ref_kinds=dict(sorted(kinds.items())),
            prereqs=_prereqs(c["refs"], caps),
        )
        rows.append(row)
    return rows


def audit_doc(rows):
    totals = {"complete": 0, "incomplete": 0, "refused": 0}
    site_totals = {"complete": 0, "incomplete": 0, "refused": 0}
    by_class = {}
    for r in rows:
        totals[r["verdict"]] += 1
        site_totals[r["verdict"]] += r["sites"]
        by_class[r["class"]] = by_class.get(r["class"], 0) + 1
    return {
        "_generated_by": "tools/gen_inmem_manifest.py --audit",
        "_what": (
            "Every src/patcher manifest classified for IN-MEMORY application. `verdict` is a "
            "MANIFEST property: complete = its relocation metadata is total, so the cave may be "
            "placed anywhere; incomplete = appliable only at the baked VA, with `reason`; refused "
            "= structurally unrepresentable, no header is emitted. `prereqs` is the SEPARATE axis "
            "-- what mh/patch/inmem_patch.h must gain before that manifest can be compiled in. "
            "`class` is a THIRD axis and the manifest's own declaration (fork ruling Q7): what the "
            "file is FOR. The compile list is every class `shipped` manifest and nothing else, so "
            "this extract is where a change to what mh.dll carries becomes visible."
        ),
        "manifest_count": len(rows),
        "site_count": sum(r["sites"] for r in rows),
        "by_verdict": totals,
        "sites_by_verdict": site_totals,
        "by_class": dict(sorted(by_class.items())),
        "shipped": [r["name"] for r in rows if r["class"] == SHIPPED],
        # The derived top of cave space (see the CAVE_GRANULARITY block): an absolute in
        # [image end, this) that no declared section covers is a refusal, so the bound is part of
        # the classification and belongs in the extract that gates it.
        "cave_space_hi": "0x%08x" % cave_hi(),
        "manifests": rows,
    }


def _audit_text(doc):
    return json.dumps(doc, indent=2, sort_keys=False) + "\n"


def print_audit(doc, say=print):
    say("%-42s %-18s %7s %-11s %s" % ("manifest", "class", "sites", "verdict", "refs / reason"))
    for r in doc["manifests"]:
        detail = (
            r["reason"]
            if r["verdict"] != "complete"
            else (", ".join("%s x%d" % (k, v) for k, v in r["ref_kinds"].items()) or "-")
        )
        say(
            "%-42s %-18s %7d %-11s %s"
            % (r["name"], r["class"], r["sites"], r["verdict"], detail[:80])
        )
    say("")
    say(
        "by class: %s -- COMPILED IN: %s"
        % (
            ", ".join("%s %d" % (k, v) for k, v in doc["by_class"].items()),
            ", ".join(doc["shipped"]) or "(none)",
        )
    )
    say(
        "cave space: [%08x, %s) -- derived from the corpus's declared sections"
        % (IMAGE_HI, doc["cave_space_hi"])
    )
    say(
        "%d manifests / %d sites -- %s"
        % (
            doc["manifest_count"],
            doc["site_count"],
            ", ".join(
                "%s %d (%d sites)" % (k, v, doc["sites_by_verdict"][k])
                for k, v in doc["by_verdict"].items()
            ),
        )
    )
    # Collapse the per-manifest prerequisites into the corpus answer: a field is reported once, at
    # the widest value ANY manifest needs, because the decision it feeds is a single struct edit.
    kinds, widen = set(), {}
    for r in doc["manifests"]:
        for p in r["prereqs"]:
            m = re.match(r"(inmem_ref::\w+) must hold (\d+) \(today (\w+)\)", p)
            if m:
                key = (m.group(1), m.group(3))
                widen[key] = max(widen.get(key, 0), int(m.group(2)))
            else:
                kinds.add(p)
    need = sorted(kinds) + [
        "%s must hold %d (today %s)" % (f, v, t) for (f, t), v in sorted(widen.items())
    ]
    if need:
        say("applier prerequisites still open (mh/patch/inmem_patch.h): %s" % "; ".join(need))


# ---- selftest -----------------------------------------------------------------------------------


def _synth(tmp, name, doc):
    path = os.path.join(tmp, "%s.mh.patch.json" % name)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)
    return path


def _verdict(path, **kw):
    try:
        c = _classify(path, **kw)
    except Refused as e:
        return "refused", str(e)
    return ("complete" if c["complete"] else "incomplete"), c["why"], c["refs"]


def _selftest_root(prefix):
    """One root for every temp tree the selftest makes, removed at interpreter exit.

    The selftest used to mkdtemp per synthetic tree and never remove any of them: measured
    2026-09-18 at 21,462 leaked f2drop_* dirs in %TEMP% (with d8_*, d5hooks_*, inmem_selftest_*
    and narration_selftest_* alongside) -- one lint run leaks a few, and the lint runs every
    session. mkdtemp(dir=root) keeps every tree under one directory that atexit removes.
    """
    import atexit
    import shutil

    root = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    return root


def selftest():
    import tempfile

    ok = True

    def expect(name, cond, detail=""):
        nonlocal ok
        print(
            "  [%s] %s%s"
            % ("ok" if cond else "FAIL", name, ("  <- %s" % detail) if not cond and detail else "")
        )
        ok = ok and cond

    tmp = _selftest_root("inmem_selftest_")
    SEC = {"name": ".mhx", "vaddr": "0x10f0000", "vsize": "0x1000"}

    def doc(patches, sections=(SEC,), **extra):
        d = {
            "target": "mh.exe",
            "sha256": "0" * 64,
            "class": "reference",
            "patches": list(patches),
        }
        if sections:
            d["sections"] = [dict(s) for s in sections]
        d.update(extra)
        return d

    # ---- the real corpus is the liveness anchor. A selftest whose planted negatives all fire
    # against an EMPTY corpus proves nothing about the tree it gates.
    rows = corpus_rows()
    expect("the corpus scan actually visited manifests", len(rows) >= 30, "saw %d" % len(rows))
    expect(
        "every site in the corpus is classified (none silently skipped)",
        sum(r["sites"] for r in rows) >= 46000,
        "saw %d" % sum(r["sites"] for r in rows),
    )
    expect(
        "no manifest is left refs_complete=false",
        [r["name"] for r in rows if r["verdict"] == "incomplete"] == [],
        [r["name"] for r in rows if r["verdict"] == "incomplete"],
    )
    # The two REAL refusals in the corpus, pinned BY NAME so a future loosening that quietly
    # re-accepts either of them goes red here rather than in a live lane.
    by_name = {r["name"]: r for r in rows}
    rec = by_name.get("recovered", {})
    expect(
        "recovered is REFUSED and the reason names `fixups`",
        rec.get("verdict") == "refused" and "`fixups`" in rec.get("reason", ""),
        rec.get("reason", "")[:120],
    )
    grand = by_name.get("storage_cap_variable_grand", {})
    expect(
        "storage_cap_variable_grand is REFUSED: its cave names arrays another manifest relocates",
        grand.get("verdict") == "refused" and "CAVE SPACE" in grand.get("reason", ""),
        grand.get("reason", "")[:120],
    )

    # ---- 0b. THE CLASSIFICATION FIELD (fork ruling Q7, F4C-GATE) ---------------------------------
    expect(
        "every manifest in the corpus declares a class",
        [r["name"] for r in rows if r["class"] not in MANIFEST_CLASSES] == [],
        [(r["name"], r["class"]) for r in rows if r["class"] not in MANIFEST_CLASSES],
    )
    # The compile list is DERIVED. This arm is the one that would catch a re-hardcoding.
    derived = [s.replace(".mh.patch.json", "") for s, _c in shipped_manifests()]
    expect(
        "the compile list is exactly the class `shipped` manifests",
        derived == [r["name"] for r in rows if r["class"] == SHIPPED],
        derived,
    )
    expect(
        "...and it is the EN caphike/relocate family, which is what Q7 ruled in",
        derived
        == [
            "grand_all_caphike_storagecap_EN",
            "projectile_pool_caphike_EN",
            "projectile_pool_relocate_EN",
        ],
        derived,
    )
    # Q7's DROP, pinned as a negative: these carry a DLL twin, so a manifest re-entering the compile
    # list would mean two carriers for one fix with nothing to say so.
    for n in ("no_cd_EN", "run_without_focus_EN", "net_resync_wait_fix_EN"):
        expect(
            "%s is class `reimplemented` (Q7: it has a DLL twin, so it does not compile in)" % n,
            by_name.get(n, {}).get("class") == "reimplemented",
            by_name.get(n, {}).get("class"),
        )
    # A class value nobody defined must refuse, or the field is decoration.
    v = _verdict(
        _synth(
            tmp,
            "badclass",
            doc([{"va": "0x00401000", "expect": "90", "bytes": "90"}], **{"class": "wibble"}),
        )
    )
    expect(
        "an unknown `class` value -> REFUSED by name", v[0] == "refused" and "wibble" in v[1], v[1]
    )
    noclass = doc([{"va": "0x00401000", "expect": "90", "bytes": "90"}])
    noclass.pop("class")
    v = _verdict(_synth(tmp, "noclass", noclass))
    expect(
        "a MISSING `class` -> REFUSED (not defaulted)", v[0] == "refused" and "class" in v[1], v[1]
    )

    # ---- 0c. THE DERIVED CAVE BOUND --------------------------------------------------------------
    # F4C's flat 64 MB is now derived from the corpus (F4C's own hand-off). Two things must hold:
    # it must still COVER every declared section, and it must still be above the one real refusal
    # that depends on it -- storage_cap_variable_grand's 0x0127ccd0, which is in cave space and in
    # no section the manifest declares.
    tops = []
    for path in _manifest_paths():
        for s in json.load(open(path, encoding="utf-8")).get("sections") or []:
            tops.append(int(str(s["vaddr"]), 16) + int(str(s["vsize"]), 0))
    expect(
        "the derived cave bound covers every declared section",
        cave_hi() >= max(tops),
        "%08x vs %08x" % (cave_hi(), max(tops)),
    )
    expect(
        "...and still covers the 0x0127ccd0 the layered manifest refuses on",
        IMAGE_HI <= 0x0127CCD0 < cave_hi(),
        "%08x" % cave_hi(),
    )

    # ---- 0d. THE LAYERED MANIFEST'S SUBSUMPTION, kept checkable rather than asserted in prose ----
    # storage_cap_variable_grand is class `layered` and NOT deleted, on the measured ground that
    # grand_all_caphike_storagecap carries it whole. If that ever stops being true the file is a
    # genuine orphan and the disposition has to be revisited -- so the claim is re-derived here.
    def _site_set(n):
        d = json.load(open(os.path.join(PATCHER, "%s.mh.patch.json" % n), encoding="utf-8"))
        return {
            (
                int(str(p["va"]), 16),
                re.sub(r"\s", "", p["bytes"]).lower(),
                re.sub(r"\s", "", p.get("expect", "")).lower(),
            )
            for p in d.get("patches", [])
        }

    def _cap_section(n):
        d = json.load(open(os.path.join(PATCHER, "%s.mh.patch.json" % n), encoding="utf-8"))
        for s in d.get("sections") or []:
            if s["name"] == ".mhcap":
                return (s["vaddr"], str(s["vsize"]), re.sub(r"\s", "", s.get("data") or "").lower())
        return None

    expect(
        "grand_all_caphike_storagecap SUBSUMES storage_cap_variable_grand's sites",
        _site_set("storage_cap_variable_grand") <= _site_set("grand_all_caphike_storagecap"),
        sorted(
            "%08x" % v[0]
            for v in _site_set("storage_cap_variable_grand")
            - _site_set("grand_all_caphike_storagecap")
        ),
    )
    expect(
        "...and their .mhcap caves are byte-identical at the same vaddr/vsize",
        _cap_section("storage_cap_variable_grand") == _cap_section("grand_all_caphike_storagecap"),
    )

    # ---- 1. the whole-blob abs32 -- the caphike shape, GREEN with a typed ref
    v = _verdict(
        _synth(
            tmp,
            "whole",
            doc([{"va": "0x00401000", "expect": "11 dc e1 00", "bytes": "79 00 0f 01"}]),
        )
    )
    expect("whole-blob absolute -> complete", v[0] == "complete", v[1])
    expect(
        "whole-blob absolute -> one abs32_to_section ref at +0",
        v[0] == "complete" and v[2] == [(0, 0, 0, "abs32_to_section", 0, 0x79)],
        v[2] if v[0] == "complete" else v[1],
    )

    # ---- 2. the operand-embedded abs32 (mov dword ptr [0x00e1538c], 0x010f0079)
    v = _verdict(
        _synth(
            tmp,
            "embed",
            doc(
                [
                    {
                        "va": "0x00401000",
                        "expect": "xx xx xx xx xx xx xx xx xx xx",
                        "bytes": "c7 05 8c 53 e1 00 79 00 0f 01",
                    }
                ]
            ),
        )
    )
    expect("operand-embedded absolute -> complete", v[0] == "complete", v[1])
    expect(
        "operand-embedded absolute -> abs32_to_section at +6",
        v[0] == "complete" and v[2] == [(0, 0, 6, "abs32_to_section", 0, 0x79)],
        v[2] if v[0] == "complete" else v[1],
    )

    # ---- 3. the recovered shape: an absolute naming cave space NO section declares
    v = _verdict(
        _synth(
            tmp,
            "uncovered",
            {
                "target": "mh.exe",
                "class": "reference",
                "patches": [
                    {
                        "va": "0x004a62a6",
                        "expect": "xx xx xx xx xx xx",
                        "bytes": "ff 15 23 01 0f 01",
                    }
                ],
            },
        )
    )
    expect("call through an UNDECLARED cave pointer -> REFUSED", v[0] == "refused", v[1])
    expect("...and the refusal names the VA", v[0] == "refused" and "0x10f0123" in v[1], v[1])

    # ---- 4. the bare dword naming undeclared cave space
    v = _verdict(
        _synth(
            tmp,
            "barecave",
            {
                "target": "mh.exe",
                "class": "reference",
                "patches": [{"va": "0x00401000", "expect": "11 dc e1 00", "bytes": "23 01 0f 01"}],
            },
        )
    )
    expect("bare dword into undeclared cave space -> REFUSED", v[0] == "refused", v[1])

    # ---- 5/6. unread keys, top-level and per-patch
    v = _verdict(
        _synth(
            tmp,
            "imports",
            doc([{"va": "0x00401000", "expect": "90", "bytes": "90"}], imports={"mh.dll": ["X"]}),
        )
    )
    expect("top-level `imports` -> REFUSED by name", v[0] == "refused" and "imports" in v[1], v[1])
    v = _verdict(
        _synth(
            tmp,
            "fixups",
            doc(
                [
                    {
                        "va": "0x00401000",
                        "expect": "90",
                        "bytes": "90",
                        "fixups": [{"offset": 0, "import": "mh.dll!X"}],
                    }
                ]
            ),
        )
    )
    expect("per-patch `fixups` -> REFUSED by name", v[0] == "refused" and "fixups" in v[1], v[1])
    v = _verdict(
        _synth(
            tmp, "unknown", doc([{"va": "0x00401000", "expect": "90", "bytes": "90", "wibble": 1}])
        )
    )
    expect("an unrecognised patch key -> REFUSED", v[0] == "refused" and "wibble" in v[1], v[1])
    v = _verdict(
        _synth(
            tmp,
            "unknownsec",
            doc(
                [{"va": "0x00401000", "expect": "90", "bytes": "90"}],
                sections=[dict(SEC, wobble=1)],
            ),
        )
    )
    expect("an unrecognised section key -> REFUSED", v[0] == "refused" and "wobble" in v[1], v[1])
    v = _verdict(
        _synth(
            tmp,
            "docd",
            doc(
                [{"va": "0x00401000", "expect": "90", "bytes": "90", "_note": "fine"}],
                _purpose="fine",
            ),
        )
    )
    expect("a `_`-prefixed documentation key stays GREEN", v[0] == "complete", v[1])

    # ---- 7. the asm manifest
    v = _verdict(
        _synth(
            tmp,
            "asm",
            doc([{"va": "0x00401000", "expect": "90", "asm": "call dword ptr [import:mh.dll!F]"}]),
        )
    )
    expect("an `asm` patch -> REFUSED", v[0] == "refused" and "asm" in v[1], v[1])

    # ---- 8. the placement-invariant shapes the corpus is full of
    v = _verdict(_synth(tmp, "onebyte", doc([{"va": "0x004190d2", "expect": "02", "bytes": "01"}])))
    expect(
        "a mid-instruction 1-byte immediate -> complete, 0 refs",
        v[0] == "complete" and v[2] == [],
        v[1],
    )
    v = _verdict(
        _synth(
            tmp,
            "capimm",
            doc([{"va": "0x00401000", "expect": "dc 05 00 00", "bytes": "a0 0f 00 00"}]),
        )
    )
    expect(
        "a 4-byte cap immediate (1500->4000) -> complete, 0 refs",
        v[0] == "complete" and v[2] == [],
        v[1],
    )

    # ---- 9. the site rel32 into the cave (the trampoline splice)
    v = _verdict(
        _synth(
            tmp,
            "splice",
            doc([{"va": "0x004c374a", "expect": "xx xx xx xx xx", "bytes": "e9 b1 c8 c2 00"}]),
        )
    )
    expect(
        "a rel32 splice into the cave -> rel32_to_section at +1",
        v[0] == "complete" and v[2] == [(0, 0, 1, "rel32_to_section", 0, 0)],
        v[2] if v[0] == "complete" else v[1],
    )

    # ---- 10. section proofs
    v = _verdict(
        _synth(
            tmp,
            "secjmp",
            doc(
                [{"va": "0x00401000", "expect": "90", "bytes": "90"}],
                sections=[dict(SEC, data="e9 43 38 3d ff")],
            ),
        )
    )
    expect(
        "a cave rel32 back into the image -> rel32_to_image",
        v[0] == "complete" and v[2] and v[2][0][3] == "rel32_to_image",
        v[2] if v[0] == "complete" else v[1],
    )
    v = _verdict(
        _synth(
            tmp,
            "secbad",
            doc(
                [{"va": "0x00401000", "expect": "90", "bytes": "90"}],
                sections=[dict(SEC, data="ff ff ff ff ff")],
            ),
        )
    )
    expect(
        "an undecodable cave byte -> refs_complete=false",
        v[0] == "incomplete" and "undecodable" in v[1],
        v[1],
    )
    v = _verdict(
        _synth(
            tmp,
            "seceb",
            doc(
                [{"va": "0x00401000", "expect": "90", "bytes": "90"}],
                sections=[dict(SEC, data="eb 80 90 90 90")],
            ),
        )
    )
    expect(
        "a rel8 leaving the cave -> refs_complete=false",
        v[0] == "incomplete" and "rel8" in v[1],
        v[1],
    )
    v = _verdict(
        _synth(
            tmp,
            "seccave",
            doc(
                [{"va": "0x00401000", "expect": "90", "bytes": "90"}],
                sections=[dict(SEC, data="ff 15 00 00 20 01")],
            ),
        )
    )
    expect("a cave absolute naming UNDECLARED cave space -> REFUSED", v[0] == "refused", v[1])

    # ---- 11. cross-section absolute (grand_all_caphike_storagecap's real two-section shape)
    TWO = [dict(SEC), {"name": ".mhy", "vaddr": "0x13c0000", "vsize": "0x1000"}]
    v = _verdict(
        _synth(
            tmp,
            "cross",
            doc(
                [{"va": "0x00401000", "expect": "11 dc e1 00", "bytes": "10 00 3c 01"}],
                sections=TWO,
            ),
        )
    )
    expect(
        "an absolute into the SECOND declared section -> abs32_to_section target_section=1",
        v[0] == "complete" and v[2] == [(0, 0, 0, "abs32_to_section", 1, 0x10)],
        v[2] if v[0] == "complete" else v[1],
    )

    # ---- 12. THE TOTALITY ASSERTION IS NOT VACUOUS. Drop an emitted ref and it must fire.
    p = _synth(
        tmp, "total", doc([{"va": "0x00401000", "expect": "11 dc e1 00", "bytes": "79 00 0f 01"}])
    )
    v = _verdict(p, _suppress_ref=0)
    expect(
        "suppressing an emitted ref makes the totality check RED",
        v[0] == "incomplete" and "placement-sensitive" in v[1],
        v[1],
    )
    expect("...and the same manifest is GREEN unsuppressed", _verdict(p)[0] == "complete")

    # ---- 13. the applier interlock reads the real header rather than a hardcoded list
    caps = applier_caps()
    expect(
        "the ref_kind enum parses out of mh/patch/inmem_patch.h",
        {"rel32_to_section", "rel32_to_image"} <= caps["ref_kinds"],
        caps["ref_kinds"],
    )
    expect("inmem_ref field widths parse", caps["owner_bits"] > 0 and caps["off_bits"] > 0, caps)
    expect(
        "F4C-COMP closed both prerequisites: the real header has abs32_to_section and a 16-bit owner",
        "abs32_to_section" in caps["ref_kinds"] and caps["owner_bits"] >= 16,
        caps,
    )
    # ...AND THE INTERLOCK ITSELF MUST STILL FIRE. Until F4C-COMP landed, this arm rode on the real
    # header lacking the kind -- so closing the prerequisite would have turned a live check into a
    # vacuous one with nothing to say so (the same failure the totality arm above exists to avoid).
    # It is therefore driven off a SYNTHETIC header that has neither the kind nor the width.
    narrow = os.path.join(tmp, "narrow_inmem_patch.h")
    open(narrow, "w", encoding="utf-8").write(
        "enum class ref_kind : uint8_t {\n    rel32_to_section,\n    rel32_to_image,\n};\n"
        "struct inmem_ref {\n    uint8_t in_section;\n    uint8_t owner;\n    uint16_t off;\n"
        "    ref_kind kind;\n    uint8_t target_section;\n    uint32_t target;\n};\n"
    )
    narrow_caps = applier_caps(narrow)
    narrow_need = _prereqs([(0, 300, 0, "abs32_to_section", 0, 0)], narrow_caps)
    expect(
        "against a header WITHOUT the kind and with a uint8_t owner, both prerequisites are reported",
        narrow_need
        == [
            "inmem_ref::owner must hold 300 (today uint8_t)",
            "ref_kind::abs32_to_section",
        ],
        narrow_need,
    )
    expect(
        "...and against the real header the same ref needs nothing",
        _prereqs([(0, 300, 0, "abs32_to_section", 0, 0)], caps) == [],
    )

    # ---- 13b. F4C-COMP: the body table the applier registers with the C1 interlock.
    ext = [(0x00401000, 0x0040100F, "fn_a"), (0x00402000, 0x00402FFF, "fn_b")]
    bodies, orphans = containing_bodies([0x00401004, 0x00401008, 0x00402100, 0x00500000], ext)
    expect(
        "sites collapse to their DISTINCT containing bodies, sorted by entry",
        bodies == [(0x00401000, 0x0040100F, "fn_a"), (0x00402000, 0x00402FFF, "fn_b")],
        bodies,
    )
    expect(
        "...and a site inside no body is COUNTED, not attributed to the nearest one", orphans == 1
    )
    expect(
        "a site one byte past a body's inclusive end is an orphan, not that body",
        containing_bodies([0x00401010], ext) == ([], 1),
        containing_bodies([0x00401010], ext),
    )
    expect(
        "...while the inclusive end byte itself IS inside it",
        containing_bodies([0x0040100F], ext)[0] == [(0x00401000, 0x0040100F, "fn_a")],
    )

    # ---- 14. every COMPILED-IN manifest must stay compilable: no open prerequisites, refs complete,
    # and a body count the interlock's table can actually hold (the static_assert's Python half --
    # this one says WHICH manifest and by how much, where the compiler only says it does not fit).
    caps_bodies = _max_patched_bodies()
    for src, _c in shipped_manifests():
        name = src.replace(".mh.patch.json", "")
        row = [r for r in rows if r["name"] == name]
        expect("compile-list entry %s is classified" % name, bool(row), name)
        if row:
            expect(
                "compile-list entry %s has NO open applier prereq" % name,
                row[0]["prereqs"] == [],
                row[0]["prereqs"],
            )
            expect(
                "compile-list entry %s is refs_complete (it may have to be relocated)" % name,
                row[0]["verdict"] == "complete",
                row[0].get("reason", "")[:100],
            )
        bodies, _orph = containing_bodies(
            int(str(p["va"]), 16)
            for p in json.load(open(os.path.join(PATCHER, src), encoding="utf-8")).get(
                "patches", []
            )
        )
        expect(
            "compile-list entry %s registers %d bodies, within mh::hook's %d slots"
            % (name, len(bodies), caps_bodies),
            len(bodies) <= caps_bodies,
        )

    print("gen_inmem_manifest --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---- main ---------------------------------------------------------------------------------------


def _emit_index(entries):
    """The COMPILE LIST as data: every shipped manifest, addressable by name.

    F1E's applier reached one `MANIFEST` symbol by hand. With the list derived from a manifest field
    the set is not knowable at hand-writing time, so the selection moves into data: mh.dll picks by
    `[patch] manifest=<name>` and refuses a name that is not here, listing what is.

    They are ALTERNATIVES, not a batch. Every shipped manifest bakes its cave at the image's
    next-free VA and the caphike family's site sets overlap (grand_all_* subsumes the pool
    manifests), so applying two in one process would fail the second one's expected-bytes guard --
    correctly, but for a confusing reason. One per run, named.
    """
    L = ["// GENERATED by tools/gen_inmem_manifest.py -- do not hand-edit.", "//"]
    a = L.append
    a("// The in-memory static patches COMPILED INTO mh.dll: every src/patcher manifest whose")
    a('// `class` is "shipped" (fork ruling Q7 -- the classification is a manifest field, so this')
    a("// list is derived from the corpus and cannot drift from it).")
    a("#pragma once")
    a('#include "patch/inmem_patch.h"')
    for _src, cname in entries:
        a('#include "patch/manifest_%s.gen.h"' % cname)
    a("")
    a("namespace mh::patch {")
    a("")
    a("inline constexpr compiled_manifest COMPILED[] = {")
    for src, cname in entries:
        a("    {\"%s\", &manifest_%s::MANIFEST}," % (src.replace(".mh.patch.json", ""), cname))
    a("};")
    a("inline constexpr int COMPILED_COUNT = %d;" % len(entries))
    a("")
    a("} // namespace mh::patch")
    a("")
    return "\n".join(L)


def _write(out, text, check, rc_box):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cur = open(out, encoding="utf-8").read() if os.path.exists(out) else None
    if check:
        if cur != text:
            print("STALE: %s (regenerate with python tools/gen_inmem_manifest.py)" % out)
            rc_box[0] = 1
        else:
            print("ok: %s" % os.path.relpath(out, REPO))
        return
    os.makedirs(OUTDIR, exist_ok=True)
    if cur != text:
        open(out, "w", encoding="utf-8", newline="\n").write(text)
        print("wrote %s" % os.path.relpath(out, REPO))
    else:
        print("unchanged %s" % os.path.relpath(out, REPO))


def _headers(check):
    rc = [0]
    caps = applier_caps()
    entries = shipped_manifests()
    if not entries:
        print("REFUSED: no manifest declares class `%s` -- nothing to compile in" % SHIPPED)
        return 1
    for src, cname in entries:
        path = os.path.join(PATCHER, src)
        try:
            c = _classify(path)
        except Refused as e:
            print("REFUSED: %s -- %s" % (src, e))
            return 1
        need = _prereqs(c["refs"], caps)
        if need:
            # THE INTERLOCK. Emitting a header the applier cannot consume would put the lie one
            # level up from the one this generator kills.
            print(
                "REFUSED: %s needs applier support that mh/patch/inmem_patch.h does not have: %s"
                % (src, "; ".join(need))
            )
            return 1
        _write(os.path.join(OUTDIR, "manifest_%s.gen.h" % cname), _emit(path, cname), check, rc)
    _write(os.path.join(OUTDIR, INDEX_HEADER), _emit_index(entries), check, rc)

    # The offline selftest's fixture (see FIXTURES): emitted from the same classifier, so the arms
    # in mh_nettest/inmem_patch_selftest.cpp still assert real generated bytes rather than a
    # hand-typed stand-in -- but never reachable from the DLL's own include tree.
    for src, cname in FIXTURES:
        path = os.path.join(PATCHER, src)
        try:
            _classify(path)
        except Refused as e:
            print("REFUSED fixture: %s -- %s" % (src, e))
            return 1
        _write(
            os.path.join(FIXTURE_DIR, "manifest_%s.fixture.gen.h" % cname),
            _emit(path, cname, fixture=True),
            check,
            rc,
        )

    # A HEADER FOR A MANIFEST THAT NO LONGER SHIPS IS A STALE ARTIFACT, not a harmless leftover: it
    # still compiles, so a hand include of it would arm a manifest the corpus says is retired, and
    # --check would never notice. Reclassifying no_cd_EN out of the compile list (Q7) is exactly
    # that case, and it is why this is here rather than a tidy-up.
    want = {"manifest_%s.gen.h" % c for _s, c in entries} | {INDEX_HEADER}
    for path in sorted(glob.glob(os.path.join(OUTDIR, "manifest_*.gen.h"))):
        if os.path.basename(path) in want:
            continue
        if check:
            print(
                "STALE: %s has no shipped manifest (regenerate with python "
                "tools/gen_inmem_manifest.py)" % os.path.relpath(path, REPO)
            )
            rc[0] = 1
        else:
            os.remove(path)
            print(
                "removed %s (its manifest is no longer class `%s`)"
                % (os.path.relpath(path, REPO), SHIPPED)
            )
    return rc[0]


def main():
    ap = argparse.ArgumentParser(
        description="compile an mhpatch manifest into an in-memory patch header"
    )
    ap.add_argument("--check", action="store_true", help="fail if a committed header is stale")
    ap.add_argument(
        "--audit",
        action="store_true",
        help="classify the whole corpus; rewrite the committed extract",
    )
    ap.add_argument(
        "--check-audit",
        action="store_true",
        help="offline drift gate: re-derive the corpus classification and require byte-identity "
        "with the committed extract",
    )
    ap.add_argument(
        "--selftest", action="store_true", help="the refusal arms, on planted negatives"
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.audit or args.check_audit:
        text = _audit_text(audit_doc(corpus_rows()))
        cur = (
            open(AUDIT_EXTRACT, encoding="utf-8").read() if os.path.exists(AUDIT_EXTRACT) else None
        )
        if args.check_audit:
            print_audit(json.loads(text))
            if cur != text:
                print(
                    "STALE: %s (regenerate with python tools/gen_inmem_manifest.py --audit)"
                    % os.path.relpath(AUDIT_EXTRACT, REPO)
                )
                return 1
            print("ok: %s" % os.path.relpath(AUDIT_EXTRACT, REPO))
            return 0
        print_audit(json.loads(text))
        os.makedirs(os.path.dirname(AUDIT_EXTRACT), exist_ok=True)
        if cur != text:
            open(AUDIT_EXTRACT, "w", encoding="utf-8", newline="\n").write(text)
            print("wrote %s" % os.path.relpath(AUDIT_EXTRACT, REPO))
        else:
            print("unchanged %s" % os.path.relpath(AUDIT_EXTRACT, REPO))
        return 0

    return _headers(args.check)


if __name__ == "__main__":
    sys.exit(main())
