# SLEIGH patch: `REP MOVS`→`memcpy`, `REP STOSB`→`memset`

Makes the Ghidra decompiler render x86 `REP`/`REPNE MOVS(B/W/D)` string copies as
`memcpy(dst, src, n)` and `REP`/`REPNE STOSB` fills as `memset(dst, val, n)`, instead of the ugly
unrolled `for` loop — the readability win Hex-Rays has built in. Applied 2026-07-18.

**Covers BOTH the `F3` (REP) and `F2` (REPNE) prefixes** — for `MOVS/STOS/LODS` they're semantically
identical (the "not-equal" condition only affects `SCAS/CMPS`), and **Watcom 10.6 actually emits `F2`
for the majority** of its string copies (in `mh.exe`: 228 `F2` vs 138 `F3`). A first cut that handled
only `REP`/`F3` left ~60% of the copies as loops — hence covering `REPNE` too. `REPNE STOSW/STOSD`
are left as loops (a word/dword fill isn't a byte `memset`).

This is the approach of the (unmerged) Ghidra **PR #5872**, extended to cover `MOVSW`/`MOVSD` +
`STOSB` across all address sizes, and with `EDI`/`ESI`/`ECX` advanced after the op (the decompiler
prunes those when dead → clean `memcpy(...)`; keeps them when the code uses the post-state).

## Where it lives / what it touches
- **File:** `<GHIDRA>/Ghidra/Processors/x86/data/languages/ia.sinc` (the shared base x86 SLEIGH).
  This is used by the base x86 languages **and** the custom `x86:LE:32:watcom` language
  (`slafile="x86.sla"`), so it affects `/eng/mh.exe`. It also affects **all** x86 (32/64) analysis
  in this install — by design.
- **Ghidra install:** `ghidra_12.0_PUBLIC_20251205` (the one with the `ghidrawatcall` module).

## Files here
- `rep-movs-memcpy.patch` — unified diff (stock → patched `ia.sinc`). The re-appliable artifact.
- `ia.sinc.stock` — the exact stock `ia.sinc` this was made against (for diffing / clean revert).
- In-place backups also live next to the target: `ia.sinc.stock-bak`, `x86.sla.stock-bak`,
  `x86-64.sla.stock-bak`.

## Apply / re-apply (after a Ghidra reinstall or update)
```sh
GH=/f/apps/ghidra_12.0_PUBLIC_20251205
L="$GH/Ghidra/Processors/x86/data/languages"
cp "$L/ia.sinc" "$L/ia.sinc.stock-bak"                     # backup first
patch "$L/ia.sinc" < rep-movs-memcpy.patch                 # or hand-apply the diff
"$GH/support/sleigh.bat" "$L/x86.slaspec"                  # recompile the 32-bit .sla
"$GH/support/sleigh.bat" "$L/x86-64.slaspec"               # and 64-bit (shared ia.sinc)
```
Then **restart Ghidra** (the running JVM caches the compiled language; a program reopen alone may
not reload it). Reopen the program; decompiling now shows `memcpy`/`memset`.

## Revert
```sh
cp "$L/ia.sinc.stock-bak" "$L/ia.sinc"       # or: patch -R "$L/ia.sinc" < rep-movs-memcpy.patch
"$GH/support/sleigh.bat" "$L/x86.slaspec" ; "$GH/support/sleigh.bat" "$L/x86-64.slaspec"
# restart Ghidra
```

## Validation (2026-07-18)
Headless raw-binary test (`F3 REP MOVSD`, `F2 REPNE MOVSD/MOVSB/STOSB`, plain `MOVSB`) decompiled to:
```c
memcpy(EDI, ESI, 0x40);            // F3 REP MOVSD  (0x10 dwords * 4)
memcpy(EDI+0x40, ESI+0x40, 0x20);  // F2 REPNE MOVSD
memcpy(EDI+0x60, ESI+0x60, 4);     // F2 REPNE MOVSB
memset(EDI+0x64, 0, 0x20);         // F2 REPNE STOSB
*(EDI+0x84) = *(ESI+0x64);         // plain MOVSB still a single copy (guard intact)
```
Both `x86.slaspec` and `x86-64.slaspec` compile with 0 errors. In `mh.exe`: after the restart the
F3 sites auto-refreshed (128/138 → `memcpy`), and the 228 F2 sites convert on the next restart.

**Note on applying to an already-disassembled program:** existing instructions re-resolve their
prototype from the new language when the program is (re)opened, so a **Ghidra restart + reopen** is
enough — no mass re-disassembly needed. (A handful of odd-context sites may keep the loop; clear+
re-disassemble those individually if desired.)

## Caveats (know these)
- **Forward-only (`DF=0` assumed).** A backward copy (`STD`; `DF=1`) is misrendered — but
  compiler-emitted string ops are ~always forward.
- **Emulation of `REP MOVS`/`STOSB` is lost** — `memcpy`/`memset` are opaque userops with no p-code
  body (the maintainers' objection to #5872). Fine for decompilation/reading; don't emulate these.
- **Segment overrides ignored** — uses flat `EDI`/`ESI`; a genuinely segmented `FS:`-relative string
  op would be wrong (nonexistent in this flat 32-bit game). Still decodes (no failure).
- **`REP STOSW`/`STOSD` left as loops** — a word/dword fill isn't a byte `memset` unless the value is
  byte-uniform, which can't be known at lift time. Only `STOSB`→`memset` is safe.
- Re-apply after any Ghidra update (like `tools/ghidra/x86watcom.cspec.fixed`).
