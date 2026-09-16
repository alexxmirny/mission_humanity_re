# mhpatch — declarative binary-patch pipeline

Replaces the hand-driven **Stud_PE + manual-Ghidra-edit** workflow with a plain, versioned,
GitHub-able pipeline (the M5 goal). Every patch is described
declaratively — address + asm/bytes + expected original bytes — so it is transparent, checksum-
gated, and reversible.

**Stack:** [Keystone](https://www.keystone-engine.org/) (x86-32 assembler, fed the site VA so
branches resolve) + [LIEF](https://lief.re/) (PE parsing, section→file-offset mapping, IAT thunk
resolution). `pip install lief keystone-engine`.

## Commands

```
python mhpatch.py diagnose <exe>              # section raw-byte map (code present? .bss?)
python mhpatch.py selftest <exe>              # exercise the whole pipeline, no writes
python mhpatch.py addimport <in> <out> --dll mh.dll --func Foo --func Bar
python mhpatch.py apply <in> <manifest.json> <out> [--dry-run]   # add imports + patch, one shot
```

## Key facts (verified by prototype, 2026-07-04)

1. **The retail exe is NOT packed — patch it directly.** It's a stock 32-bit Watcom PE: BEGTEXT
   (code) and DGROUP (data) are fully present, uncompressed, at the exact VAs Ghidra analyzes
   (named functions disassemble straight from the retail file; Ghidra got 3042 functions from it).
   It's 2.9 MB only because `.bss` is uninitialised and not stored. So an abstract user needs only
   their own stock `mh.exe` — **there is nothing to unpack** (the earlier "packed / use `mh_.exe`"
   belief was a misconception; `mh_.exe` was a patched copy with `.bss` materialised to zeros).
2. **`.bss` globals have no file bytes.** The big object arrays live in `.bss` (VA ≳`0xc00000`),
   zero-filled at load — so `apply` reports `NO-RAW-BYTES` for a patch aimed there. Expected:
   relocate/resize a `.bss` array via section headers + code refs, not by editing file bytes.
3. **Import-add is automated** (replaces Stud_PE). LIEF's import rebuilder works on normal PEs
   (verified on `notepad.exe`) but **can't persist a new import through this binary's non-contiguous
   Watcom layout**. So `add_import_section()` reproduces the working `mh_.exe`'s `.newimp` technique:
   append a section, rebuild the descriptor array (existing descriptors copied verbatim + new ones,
   original DLLs still resolving into `.idata`), repoint the IMPORT data-directory. `apply` invokes
   it automatically for any `imports` not already present — so **"stock retail `mh.exe` → patched
   build"** needs no external artifact and no Stud_PE.

Calls into the DLL use the IAT: `call dword ptr [import:mh.dll!Func]` → `FF15 <slot>`
(absolute-indirect, **no rel32** — the failure mode of the earlier hand-nasm attempts).

## Manifest schema

```json
{
  "target": "mh.exe (retail)",
  "sha256": "9c4451cf...",
  "imports": { "mh.dll": ["DecompressLZWData"] },
  "patches": [
    { "va": "0x004xxxxx",
      "expect": "e8 xx xx xx xx",
      "asm": "call dword ptr [import:mh.dll!DecompressLZWData]" }
  ]
}
```

- `expect` — original bytes with `xx` wildcards; the patch is **rejected** unless they match
  (per-site guard, stronger than the whole-file `sha256`, and the basis for revert).
- `asm` (Keystone) or `bytes` (literal hex). `[import:dll!func]` in `asm` resolves to the IAT slot.
- `fixups` (with `bytes`): `[{ "offset": N, "import": "dll!func" }]` — overwrite the 4 bytes at
  `offset` with the resolved IAT slot VA (LE). Use this to keep a **byte-exact** patch (e.g. one
  recovered by diffing an existing patched build) while still relocating its `call [slot]` to
  wherever the pipeline places the import.

Examples: `example.mh.patch.json` (template) · `sight_redirect.mh.patch.json` (a minimal `asm`
redirect + the EBP-return gotcha) · `recovered.mh.patch.json` (a full mod recovered byte-exact from a
manually-patched build via `bytes`+`fixups`: sight-radius extension + LZW-decompress replacement).

## Status

**Full flow validated** against the **retail** `mh.exe` (2026-07-04): stock exe → auto-add `mh.dll`
import (`.mhimp` section) → resolve IAT slot → keystone-assemble → guarded byte-apply → the patched
site disassembles to `call dword ptr [<new slot>]` hitting the added import; originals still resolve;
valid PE. **Verified structurally — the remaining confidence check is launching the patched game.**
Open work — runtime validation, a `revert` command, a Tier-A strangler PoC — is tracked privately.
