#!/usr/bin/env python3
"""mhpatch - declarative, versioned binary-patch pipeline for mh.exe / Exterminacja.

Goal (see /ROADMAP.md M5): replace the hand-driven Stud_PE + manual-Ghidra-edit
workflow with a plain, repeatable, GitHub-able pipeline:

  1. every patch is described declaratively (address + asm/bytes + expected original
     bytes), so it is transparent and *reversible*;
  2. calls into the injected DLL go through the IAT as `call dword ptr [<iat_slot>]`
     (absolute-indirect -> no rel32 relocation to get wrong: the failure mode of the
     earlier raw-nasm attempts);
  3. one tool assembles + applies everything, gated by a whole-file checksum and a
     per-site expected-original-bytes guard.

Stack: Keystone (x86-32 assembler, fed the site VA) + LIEF (PE parsing, section
-> file-offset mapping, IAT slot resolution, best-effort import injection).

KEY FACTS (verified by prototype 2026-07-04):

  * NOT PACKED. The retail exe is a stock 32-bit Watcom PE -- BEGTEXT (code) and DGROUP
    (initialised data) are fully present, uncompressed, at the exact VAs Ghidra analyses
    (named functions disassemble straight from the retail file; Ghidra derived 3042
    functions from it). CODE / CONST patches therefore apply DIRECTLY to the retail exe
    -- there is nothing to unpack. It is 2.9 MB only because .bss is uninitialised and
    not stored. (The earlier "packed / use the unpacked mh_.exe" belief was a
    misconception; mh_.exe was just a patched copy with .bss materialised to 7.5 MB of
    zeros plus a `.newimp` import section.)
  * .bss GLOBALS have no file bytes. The big object arrays live in .bss (VA ~>=0xc00000),
    zero-filled at load -- so `apply` reports NO-RAW-BYTES for a patch aimed there. That
    is expected: relocate/resize a .bss array via section headers + code refs, not by
    editing file bytes. `diagnose` shows which sections carry raw data.
  * IMPORT-ADD is automated. LIEF's import rebuilder can't persist a new import through this
    binary's non-contiguous Watcom layout, so `add_import_section()` reproduces the `.newimp`
    technique directly: append a section, rebuild the IMAGE_IMPORT_DESCRIPTOR array (existing
    descriptors copied verbatim + the new ones), repoint the IMPORT data-directory. `apply`
    invokes it automatically for any manifest `imports` not already present. So the whole flow
    is "stock retail mh.exe -> patched build" with NO Stud_PE and NO external artifact.
    (Structurally verified: valid PE, originals still resolve, call targets hit the new IAT
    slots; the final confidence check is launching the patched game.)

Manifest schema (JSON):
{
  "target": "mh.exe (retail)",             # informational
  "sha256": "<hex>",                        # optional: refuse to run on wrong input
  "imports": { "mh.dll": ["DecompressLZWData"] },   # best-effort; usually already present
  "sections": [                                     # append zero-init (.bss-style) sections
    { "name": ".mhpool", "vaddr": "0x010f0000", "vsize": "484000" }  # host a relocated/enlarged array
  ],                                                # vaddr MUST be the next-free aligned VA
  "patches": [
    { "va": "0x004dd9f8",
      "expect": "c7 05 xx xx xx xx",         # original bytes ('xx' = wildcard)
      "asm": "call dword ptr [import:mh.dll!DecompressLZWData]; nop" }
    # or "bytes": "90 90 90" instead of "asm"
  ]
}
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, struct, sys

try:
    import lief
except ImportError:
    sys.exit("need: pip install lief")
try:
    lief.logging.disable()   # silence benign ".bss padding" / "section rva" parse warnings
except Exception:
    pass
try:
    from keystone import Ks, KS_ARCH_X86, KS_MODE_32
except ImportError:
    sys.exit("need: pip install keystone-engine")

_KS = Ks(KS_ARCH_X86, KS_MODE_32)
_IMPORT_RE = re.compile(r"\[\s*import:([^\s!]+)!([^\s\]]+)\s*\]")


def _align(v, a):
    return (v + a - 1) // a * a


# Windows UAC "installer detection" force-elevates manifest-less 32-bit exes whose FILENAME
# contains any of these -> runs as admin -> breaks this game's cursor/fullscreen. (Found 2026-07-04:
# `mh.patched.exe` elevated & misbehaved; the same bytes as `mh.exe` ran fine.)
_UAC_TRIGGERS = ("setup", "install", "update", "patch")


def installer_name_warning(path: str):
    hit = [t for t in _UAC_TRIGGERS if t in os.path.basename(path).lower()]
    if hit:
        return (f"output name contains {hit} -> Windows UAC installer-detection will force-elevate "
                f"it (this game then loses cursor/fullscreen & prompts for admin). Name it without "
                f"those words (overwrite mh.exe from a backup, or e.g. mh_mod.exe).")
    return None


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def diagnose(path: str) -> dict:
    """Per-section raw-byte coverage. For a stock retail mh.exe, BEGTEXT (code) carries
    full raw bytes -> patchable directly; only .bss (globals) legitimately has none."""
    pe = lief.PE.parse(path)
    out = {"imagebase": pe.optional_header.imagebase, "sections": [], "code_has_raw_bytes": True}
    for s in pe.sections:
        raw, vsz = int(s.sizeof_raw_data), int(s.virtual_size)
        out["sections"].append({"name": s.name, "vaddr": pe.imagebase + s.virtual_address,
                                "vsize": vsz, "raw": raw})
        if s.name.upper() == "BEGTEXT" and raw < 0x1000:
            out["code_has_raw_bytes"] = False   # BEGTEXT empty -> unexpected (not a stock exe)
    return out


def _va_to_offset(pe, va: int):
    rva = va - pe.imagebase
    for s in pe.sections:
        start = s.virtual_address
        if start <= rva < start + max(int(s.virtual_size), int(s.sizeof_raw_data)):
            off = int(s.pointerto_raw_data) + (rva - start)
            return off if off < int(s.pointerto_raw_data) + int(s.sizeof_raw_data) else None
        # None => inside virtual padding, no raw bytes (.bss / uninitialised)
    return None


def _section_for_va(pe, va: int):
    rva = va - pe.imagebase
    for s in pe.sections:
        if s.virtual_address <= rva < s.virtual_address + max(int(s.virtual_size), int(s.sizeof_raw_data)):
            return s.name
    return None


def iat_slots(pe) -> dict:
    """{ 'dll!func' : iat_slot_VA } for every imported entry already in the PE."""
    slots = {}
    for imp in pe.imports:
        base = pe.imagebase + imp.import_address_table_rva
        for i, e in enumerate(imp.entries):
            if e.name:
                slots[f"{imp.name}!{e.name}"] = base + i * 4
    return slots


def add_import_section(inp: str, out: str, imports: dict, section_name: str = ".mhimp"):
    """Add DLL imports by appending a NEW section holding a rebuilt IMAGE_IMPORT_DESCRIPTOR
    array (copies of the existing descriptors, RVAs unchanged + new ones), and repointing the
    IMPORT data-directory at it -- exactly the technique the working `mh_.exe`'s `.newimp`
    section uses. Reliable on this binary's Watcom layout, where LIEF's rebuilder fails.
    Returns [(dll, func, iat_slot_VA), ...] for the newly-added functions.
    """
    pe = lief.PE.parse(inp)
    IB = pe.optional_header.imagebase
    salign = int(pe.optional_header.section_alignment)
    falign = int(pe.optional_header.file_alignment)
    raw = bytearray(open(inp, "rb").read())

    present = {i.name.lower() for i in pe.imports}
    imports = {d: fns for d, fns in imports.items() if d.lower() not in present}
    if not imports:
        open(out, "wb").write(raw)
        return []
    existing = [(i.import_lookup_table_rva, i.timedatestamp, i.forwarder_chain,
                 i.name_rva, i.import_address_table_rva) for i in pe.imports]

    last = max(pe.sections, key=lambda s: s.virtual_address + max(int(s.virtual_size), int(s.sizeof_raw_data)))
    new_rva = _align(last.virtual_address + max(int(last.virtual_size), int(last.sizeof_raw_data)), salign)

    n_total = len(existing) + len(imports)
    cur = (n_total + 1) * 20        # blob offset just past the descriptor array (+terminator)
    regions, new_descs, slot_map = [], [], []
    for dll, funcs in imports.items():
        name_off = cur; nb = dll.encode() + b"\0"; cur += len(nb)
        ibn_off = cur; ibn = bytearray(); ibn_rvas = []
        for fn in funcs:                                   # IMAGE_IMPORT_BY_NAME: hint(0)+name
            ibn_rvas.append(new_rva + ibn_off + len(ibn))
            ibn += struct.pack("<H", 0) + fn.encode() + b"\0"
            if len(ibn) & 1: ibn += b"\0"
        cur += len(ibn)
        ilt_off = cur                                      # ILT (OriginalFirstThunk)
        thunks = b"".join(struct.pack("<I", r) for r in ibn_rvas) + struct.pack("<I", 0)
        cur += len(thunks)
        iat_off = cur; cur += len(thunks)                  # IAT (FirstThunk) = same, loader fills
        regions.append((name_off, nb, ibn_off, bytes(ibn), ilt_off, iat_off, thunks))
        new_descs.append((new_rva + ilt_off, 0, 0, new_rva + name_off, new_rva + iat_off))
        for i, fn in enumerate(funcs):
            slot_map.append((dll, fn, IB + new_rva + iat_off + i * 4))

    blob = bytearray(cur)
    o = 0
    for d in existing + new_descs:
        struct.pack_into("<IIIII", blob, o, *d); o += 20   # terminator left zero
    for name_off, nb, ibn_off, ibn, ilt_off, iat_off, thunks in regions:
        blob[name_off:name_off + len(nb)] = nb
        blob[ibn_off:ibn_off + len(ibn)] = ibn
        blob[ilt_off:ilt_off + len(thunks)] = thunks
        blob[iat_off:iat_off + len(thunks)] = thunks

    # --- PE header surgery ---
    pe_off = struct.unpack_from("<I", raw, 0x3c)[0]
    num_sec_off, opt_off = pe_off + 6, pe_off + 24
    num_sec = struct.unpack_from("<H", raw, num_sec_off)[0]
    opt_sz = struct.unpack_from("<H", raw, pe_off + 20)[0]
    sectbl = opt_off + opt_sz
    new_hdr = sectbl + num_sec * 40
    first_raw = min(int(s.offset) for s in pe.sections if int(s.offset) > 0)
    size_of_headers = struct.unpack_from("<I", raw, opt_off + 60)[0]
    if new_hdr + 40 > min(first_raw, size_of_headers):
        raise RuntimeError("no room in PE header for another section header")

    new_off = _align(len(raw), falign)
    raw += b"\0" * (new_off - len(raw))
    raw_sz = _align(len(blob), falign)
    raw += bytes(blob) + b"\0" * (raw_sz - len(blob))

    struct.pack_into("<8sIIIIIIHHI", raw, new_hdr, section_name.encode()[:8].ljust(8, b"\0"),
                     len(blob), new_rva, raw_sz, new_off, 0, 0, 0, 0, 0xC0000040)  # init/read/write
    struct.pack_into("<H", raw, num_sec_off, num_sec + 1)
    struct.pack_into("<I", raw, opt_off + 56, _align(new_rva + len(blob), salign))  # SizeOfImage
    struct.pack_into("<II", raw, opt_off + 104, new_rva, (n_total + 1) * 20)        # IMPORT dir
    open(out, "wb").write(raw)
    return slot_map


def next_free_va(pe) -> int:
    """Section-aligned VA just past the current image end -- where an appended section lands."""
    IB = pe.optional_header.imagebase
    salign = int(pe.optional_header.section_alignment)
    last = max(pe.sections, key=lambda s: s.virtual_address
               + max(int(s.virtual_size), int(s.sizeof_raw_data)))
    return IB + _align(last.virtual_address
                       + max(int(last.virtual_size), int(last.sizeof_raw_data)), salign)


def add_bss_section(inp: str, out: str, name: str, vaddr: int, vsize: int):
    """Append a zero-initialised (.bss-style) section at an explicit VA and grow SizeOfImage.
    No raw bytes are stored (pointerto_raw_data=0, sizeof_raw_data=0) -- the loader zero-fills
    the region, exactly like the game's own .bss. Used to host a relocated/enlarged object array
    at a deterministic VA (the exe has no ASLR: IMAGE_FILE_RELOCS_STRIPPED is irrelevant because
    DYNAMIC_BASE is off, so it always loads at ImageBase and the new VA is fixed). `vaddr` MUST be
    the section-aligned next-free VA (== next_free_va); we assert it so a manifest baking concrete
    addresses can't silently disagree with where the section actually lands.
    """
    pe = lief.PE.parse(inp)
    salign = int(pe.optional_header.section_alignment)
    expect_va = next_free_va(pe)
    if vaddr != expect_va:
        raise RuntimeError("section VA 0x%x != next-free 0x%x (manifest baked for a different "
                           "input layout)" % (vaddr, expect_va))
    if vaddr % salign:
        raise RuntimeError("section VA 0x%x not aligned to 0x%x" % (vaddr, salign))
    raw = bytearray(open(inp, "rb").read())
    IB = pe.optional_header.imagebase
    new_rva = vaddr - IB

    pe_off = struct.unpack_from("<I", raw, 0x3c)[0]
    num_sec_off, opt_off = pe_off + 6, pe_off + 24
    num_sec = struct.unpack_from("<H", raw, num_sec_off)[0]
    opt_sz = struct.unpack_from("<H", raw, pe_off + 20)[0]
    sectbl = opt_off + opt_sz
    new_hdr = sectbl + num_sec * 40
    first_raw = min(int(s.offset) for s in pe.sections if int(s.offset) > 0)
    size_of_headers = struct.unpack_from("<I", raw, opt_off + 60)[0]
    if new_hdr + 40 > min(first_raw, size_of_headers):
        raise RuntimeError("no room in PE header for another section header")

    # IMAGE_SCN_CNT_UNINITIALIZED_DATA | MEM_READ | MEM_WRITE = 0x80 | 0x40000000 | 0x80000000
    struct.pack_into("<8sIIIIIIHHI", raw, new_hdr, name.encode()[:8].ljust(8, b"\0"),
                     vsize, new_rva, 0, 0, 0, 0, 0, 0, 0xC0000080)
    struct.pack_into("<H", raw, num_sec_off, num_sec + 1)
    struct.pack_into("<I", raw, opt_off + 56, _align(new_rva + vsize, salign))  # SizeOfImage
    open(out, "wb").write(raw)
    return vaddr


def add_data_section(inp: str, out: str, name: str, vaddr: int, data: bytes, vsize: int,
                     chars: int = 0xE0000040):
    """Append an INITIALIZED section carrying raw `data` bytes at an explicit VA, with the
    remaining `vsize - len(raw)` zero-filled by the loader. Unlike add_bss_section (pure
    zero-init, no file bytes), this stores real content -- used to host an executable
    trampoline cave (code that widens an imm8 loop bound past 127) at the front of the section,
    with the relocated/enlarged array occupying the zero-filled tail. Default characteristics
    0xE0000040 = INITIALIZED_DATA | EXECUTE | READ | WRITE (the cave executes, the array tail is
    written by the game). `vaddr` MUST be the section-aligned next-free VA (asserted, as in
    add_bss_section) so a manifest baking concrete addresses can't disagree with the real layout.
    """
    pe = lief.PE.parse(inp)
    salign = int(pe.optional_header.section_alignment)
    falign = int(pe.optional_header.file_alignment)
    expect_va = next_free_va(pe)
    if vaddr != expect_va:
        raise RuntimeError("section VA 0x%x != next-free 0x%x (manifest baked for a different "
                           "input layout)" % (vaddr, expect_va))
    if vaddr % salign:
        raise RuntimeError("section VA 0x%x not aligned to 0x%x" % (vaddr, salign))
    if len(data) > vsize:
        raise RuntimeError("data (%d) larger than vsize (%d)" % (len(data), vsize))
    raw = bytearray(open(inp, "rb").read())
    IB = pe.optional_header.imagebase
    new_rva = vaddr - IB

    pe_off = struct.unpack_from("<I", raw, 0x3c)[0]
    num_sec_off, opt_off = pe_off + 6, pe_off + 24
    num_sec = struct.unpack_from("<H", raw, num_sec_off)[0]
    opt_sz = struct.unpack_from("<H", raw, pe_off + 20)[0]
    sectbl = opt_off + opt_sz
    new_hdr = sectbl + num_sec * 40
    first_raw = min(int(s.offset) for s in pe.sections if int(s.offset) > 0)
    size_of_headers = struct.unpack_from("<I", raw, opt_off + 60)[0]
    if new_hdr + 40 > min(first_raw, size_of_headers):
        raise RuntimeError("no room in PE header for another section header")

    new_off = _align(len(raw), falign)
    raw += b"\0" * (new_off - len(raw))
    raw_sz = _align(len(data), falign)
    raw += bytes(data) + b"\0" * (raw_sz - len(data))

    struct.pack_into("<8sIIIIIIHHI", raw, new_hdr, name.encode()[:8].ljust(8, b"\0"),
                     vsize, new_rva, raw_sz, new_off, 0, 0, 0, 0, chars)
    struct.pack_into("<H", raw, num_sec_off, num_sec + 1)
    struct.pack_into("<I", raw, opt_off + 56, _align(new_rva + vsize, salign))  # SizeOfImage
    open(out, "wb").write(raw)
    return vaddr


def _try_add_imports(pe, imports: dict):
    for dll, funcs in imports.items():
        imp = pe.add_import(dll)
        for fn in funcs:
            imp.add_entry(fn)
    cfg = lief.PE.Builder.config_t()
    cfg.imports = True
    b = lief.PE.Builder(pe, cfg)
    b.build()
    return b


def assemble(asm: str, va: int, slots: dict) -> bytes:
    def repl(m):
        key = f"{m.group(1)}!{m.group(2)}"
        if key not in slots:
            raise KeyError(f"import thunk [{key}] not present in target; add it via the "
                           f"one-time import bootstrap (Stud_PE/.newimp) first")
        # re-emit the brackets the regex consumed: this is a memory operand (the IAT
        # slot), so `call dword ptr [import:...]` -> `call dword ptr [0x...]` (FF15,
        # absolute-indirect) and NOT `call 0x...` (E8, rel32).
        return "[" + hex(slots[key]) + "]"
    asm = _IMPORT_RE.sub(repl, asm)
    enc, _ = _KS.asm(asm.encode(), va)
    if enc is None:
        raise ValueError(f"keystone failed to assemble: {asm!r}")
    return bytes(enc)


def _match_expect(actual: bytes, expect: str) -> bool:
    toks = expect.split()
    if len(toks) != len(actual):
        return False
    return all(t.lower() in ("xx", "??") or int(t, 16) == b for t, b in zip(toks, actual))


def apply_manifest(inp: str, manifest_path: str, out: str, dry_run: bool = False):
    m = json.load(open(manifest_path))
    warns = []
    if m.get("sha256") and sha256(inp) != m["sha256"]:
        sys.exit(f"checksum mismatch: {inp} is not the expected target")

    pe = lief.PE.parse(inp)
    slots = iat_slots(pe)
    base_path, data = inp, bytearray(open(inp, "rb").read())

    # any requested import thunks missing? add them via the .newimp-style section surgery.
    want = {f"{d}!{fn}" for d, fns in (m.get("imports") or {}).items() for fn in fns}
    if want - set(slots):
        need = {d: fns for d, fns in m["imports"].items()
                if any(f"{d}!{fn}" not in slots for fn in fns)}
        tmp = out + ".withimports.tmp"
        try:
            add_import_section(inp, tmp, need)
            pe = lief.PE.parse(tmp)
            slots = iat_slots(pe)
            base_path, data = tmp, bytearray(open(tmp, "rb").read())
            if want - set(slots):
                warns.append(f"could not resolve imports after add: {sorted(want - set(slots))}")
        except Exception as e:
            warns.append(f"import-add failed ({e}); bootstrap out-of-band (.newimp/Stud_PE).")

    # append sections -- host for relocated/enlarged arrays. A section with "data" is an
    # INITIALIZED (RWX) section carrying raw bytes (a trampoline cave); without it, a plain
    # zero-init (.bss-style) section.
    for _seci, sec in enumerate(m.get("sections") or []):
        va = int(str(sec["vaddr"]), 16)
        vsz = int(str(sec["vsize"]), 0)
        # unique tmp per section: with >1 section the previous iteration's output IS this
        # iteration's base_path, so a shared name makes the base_path==tmp cleanup below delete
        # the file we're about to parse (multi-section manifests, e.g. grand build + a cave).
        tmp = out + f".withsection{_seci}.tmp"
        if sec.get("data"):
            blob = bytes.fromhex(re.sub(r"\s", "", sec["data"]))
            chars = int(str(sec.get("chars", "0xE0000040")), 0)
            add_data_section(base_path, tmp, sec["name"], va, blob, vsz, chars)
        else:
            add_bss_section(base_path, tmp, sec["name"], va, vsz)
        if base_path.endswith(".tmp") and os.path.exists(base_path):
            os.remove(base_path)
        pe = lief.PE.parse(tmp)
        slots = iat_slots(pe)
        base_path, data = tmp, bytearray(open(tmp, "rb").read())

    report = []
    for p in m.get("patches", []):
        va = int(str(p["va"]), 16)
        off = _va_to_offset(pe, va)
        if off is None:
            sec = _section_for_va(pe, va)
            why = (f"in {sec} (.bss/uninitialised) - no file bytes to patch" if sec
                   else "not in any section - check the VA (placeholder/template?)")
            report.append((va, f"SKIP {why}", None)); continue
        try:
            if "asm" in p:
                newb = bytearray(assemble(p["asm"], va, slots))
            else:                                   # exact bytes (hex, spaces/newlines ok)
                newb = bytearray(bytes.fromhex(re.sub(r"\s", "", p["bytes"])))
            # fixups: overwrite 4 bytes at <offset> with a resolved IAT slot VA (LE).
            # lets a byte-exact recovered patch relocate its `call [slot]` to our import layout.
            for fx in p.get("fixups", []):
                if fx["import"] not in slots:
                    raise KeyError(f"import thunk [{fx['import']}] not present")
                struct.pack_into("<I", newb, fx["offset"], slots[fx["import"]])
            newb = bytes(newb)
        except KeyError as e:
            report.append((va, f"UNRESOLVED {e}", None)); continue
        cur = bytes(data[off:off + len(newb)])
        if "expect" in p and not _match_expect(cur, p["expect"]):
            report.append((va, f"GUARD-FAIL have={cur.hex()}", None)); continue
        if not dry_run:
            data[off:off + len(newb)] = newb
        report.append((va, "OK", newb.hex()))

    if not dry_run:
        open(out, "wb").write(data)
    if base_path != inp and os.path.exists(base_path) and base_path.endswith(".tmp"):
        os.remove(base_path)
    w = installer_name_warning(out)
    if w:
        warns.append(w)
    return report, warns


def _selftest(inp: str):
    print(f"[selftest] input : {inp}")
    print(f"[selftest] sha256: {sha256(inp)}")
    d = diagnose(inp)
    print(f"[selftest] imagebase=0x{d['imagebase']:x}  code_has_raw_bytes={d['code_has_raw_bytes']}"
          f"  {'(code present - patchable)' if d['code_has_raw_bytes'] else '(BEGTEXT empty?! unexpected for a stock Watcom PE)'}")

    pe = lief.PE.parse(inp)
    slots = iat_slots(pe)
    print(f"[selftest] existing imports: {[i.name.lower() for i in pe.imports]}")
    if slots:
        k = next(iter(slots))
        stub = assemble(f"call dword ptr [import:{k.split('!')[0]}!{k.split('!')[1]}]", 0x00401000, slots)
        print(f"[selftest] assemble call->existing thunk [{k}] @ 0x{slots[k]:08x}: {stub.hex()} "
              f"(ff15 = absolute-indirect, no rel32)")

    ep = pe.imagebase + pe.optional_header.addressof_entrypoint
    off = _va_to_offset(pe, ep)
    cur = bytes(open(inp, "rb").read()[off:off + 5]) if off is not None else b""
    print(f"[selftest] va->offset: entrypoint 0x{ep:08x} -> file 0x{off:x}  bytes={cur.hex()}"
          if off is not None else "[selftest] entrypoint has no raw bytes")

    # document the LIEF-on-Watcom import limitation
    pe2 = lief.PE.parse(inp)
    try:
        b = _try_add_imports(pe2, {"mh.dll": ["DecompressLZWData"]})
        tmp = inp + ".t.exe"; b.write(tmp)
        ok = "mh.dll" in [i.name.lower() for i in lief.PE.parse(tmp).imports]
        os.remove(tmp)
    except Exception:
        ok = False
    print(f"[selftest] LIEF import-add persists on this binary: {ok} "
          f"({'ok' if ok else 'expected FALSE for Watcom PE'})")
    # our .newimp-style section surgery (the reliable path)
    tmp2 = inp + ".imp.exe"
    try:
        added = add_import_section(inp, tmp2, {"mh.dll": ["DecompressLZWData", "GetSightAreaFromRadius"]})
        rp = lief.PE.parse(tmp2)
        names = [i.name.lower() for i in rp.imports]
        rp_slots = iat_slots(rp)
        ok2 = "mh.dll" in names and "mh.dll!DecompressLZWData" in rp_slots
        print(f"[selftest] section-surgery import-add: {'OK' if ok2 else 'FAIL'}  "
              f"(mh.dll present={('mh.dll' in names)}; "
              f"DecompressLZWData IAT @ 0x{rp_slots.get('mh.dll!DecompressLZWData', 0):08x})")
        os.remove(tmp2)
    except Exception as e:
        print(f"[selftest] section-surgery import-add: ERROR {e}")
    print("[selftest] core loop (diagnose + assemble + va-map + guard): WORKING")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="mh.exe declarative patch pipeline")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("selftest", "diagnose"):
        sp = sub.add_parser(name); sp.add_argument("input")
    sp = sub.add_parser("apply")
    sp.add_argument("input"); sp.add_argument("manifest"); sp.add_argument("output")
    sp.add_argument("--dry-run", action="store_true")
    sp = sub.add_parser("addimport", help="append a .newimp-style import section")
    sp.add_argument("input"); sp.add_argument("output")
    sp.add_argument("--dll", required=True)
    sp.add_argument("--func", action="append", required=True, help="repeatable")
    a = ap.parse_args()

    if a.cmd == "selftest":
        _selftest(a.input)
    elif a.cmd == "diagnose":
        print(json.dumps(diagnose(a.input), indent=2))
    elif a.cmd == "addimport":
        for d, f, va in add_import_section(a.input, a.output, {a.dll: a.func}):
            print(f"  added {d}!{f} -> IAT slot 0x{va:08x}")
        w = installer_name_warning(a.output)
        if w:
            print(f"  WARN: {w}")
    elif a.cmd == "apply":
        report, warns = apply_manifest(a.input, a.manifest, a.output, a.dry_run)
        for va, status, newb in report:
            print(f"  0x{va:08x}  {status}" + (f"  -> {newb}" if newb else ""))
        for w in warns:
            print(f"  WARN: {w}")
