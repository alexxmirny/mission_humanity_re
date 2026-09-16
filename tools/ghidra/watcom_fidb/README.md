# Watcom 10.6 FID database (`watcom10.fidb`)

A Ghidra **Function ID** database built from the **Watcom C/C++ 10.6** register-model runtime
libraries — **the version that actually built `mh.exe`** (see "Version detection" below) — for
auto-identifying the statically-linked CRT/library functions. Built 2026-07-18.

Ghidra's shipped FID databases are Visual-Studio-only, and no public Watcom `.fidb` exists — this
was built from scratch. It carries **905 functions** from `clib3r.lib` (C runtime) + `math387r.lib`
(387 FP math) + `plib3r.lib` (C++ runtime). On `/eng/mh.exe` it identifies **172 functions** at the
default score threshold; **129 were applied** (names tagged `fid:watcom106` + `sub:crt`).

## Version detection (why 10.6, not 11.0)
The game embeds no compiler-version string. The version was determined **empirically by FID
hit-rate**: a Watcom **11.0** db matched only 28 functions (mostly version-invariant hand-asm like
`__STOSD`/`__fpatan`), while a **10.6** db matched **149** at the same threshold and scored far
higher on the version-sensitive C code (`open` 19→152, `__filbuf` 26→138, `strdup`/`fwrite`/the heap
manager). 128 functions matched 10.6-only and **zero** matched 11.0-higher — decisive. **Lesson: to
pin a Watcom binary's exact version, build FID dbs for candidate versions and compare hit-rate;
don't assume from the release date.** (The 11.0 db even mis-named a few — e.g. it called `sscanf`
"swscanf" and `strdup` "_mbsdup" — because the real 10.6 function wasn't present to win.)

## Files
- `watcom10.fidb` — the database (attach it; do not hand-edit).
- `build_watcom_fidb.java` — the headless ingest script (create + populate a fidb from every
  program in the active project). Args: `<fidbPath> <languageID> [name] [version] [variant]`.

## Attaching it in a session (ReVA `run-script`)
```python
from java.io import File
from ghidra.feature.fid.db import FidFileManager

FidFileManager.getInstance().addUserFidFile(
    File(r"<repo>\tools\ghidra\watcom_fidb\watcom10.fidb")  # <repo> = this checkout, absolute
)
```
Then run the **Function ID** analyzer (auto-applies library names for confident unique matches), or
query without applying via `FidService.processProgram(program, queryService, threshold, monitor)`
and read `result.function` / `match.getFunctionRecord().getName()` / `match.getOverallScore()`.
The attach persists in Ghidra's user settings — keep this repo path stable.

## Rebuilding from scratch (reproducible)
Requires: Watcom 10.6 install (`C:\WATCOM10`; register-model libs `clib3r.lib` in `lib386\nt`,
`math387r.lib`/`plib3r.lib` in `lib386`) + the Ghidra install carrying the custom
`x86:LE:32:watcom` language (`ghidrawatcall` processor module — here `ghidra_12.0_PUBLIC_20251205`,
NOT a stock Ghidra install, which does not carry it).

1. **Extract the OMF `.obj` modules** with `wlib`. **10.6's `wlib` treats `/` as an option
   delimiter** — use **backslash** lib paths and keep the `@command-file` path bare (cwd), or it
   errors "Expected 'option'":
   ```sh
   WL='C:\WATCOM10\binnt\wlib.exe'
   "$WL" 'C:\WATCOM10\lib386\nt\clib3r.lib' > toc.txt            # symbol->module map
   grep -E '\.\.\.\.' toc.txt | sed -E 's/.*\.\.+//' | sort -u | sed -E 's/^/*/' > ex.cmd
   ( cd objs && "$WL" 'C:\WATCOM10\lib386\nt\clib3r.lib' @ex.cmd )   # -> 399 .obj
   # repeat for math387r.lib (64) and plib3r.lib (236)
   ```
2. **Headless import** into a throwaway project, forcing the Watcom language (FID only matches
   same-LanguageID libraries):
   ```sh
   GH=<ghidra>   # the install carrying ghidrawatcall, e.g. .../ghidra_12.0_PUBLIC_20251205
   "$GH/support/analyzeHeadless.bat" <scratch>/proj w10 \
     -import <clib_objs> <math_objs> <cpp_objs> -processor x86:LE:32:watcom -cspec watcomcpp
   ```
   (Ghidra 12.0's OMF loader imports Watcom 32-bit OMF cleanly — no patch. The "EXTERNAL block"
   decompile WARNs on standalone objs are harmless.)
3. **Build the fidb** — run the ingest once over the whole project:
   ```sh
   "$GH/support/analyzeHeadless.bat" <scratch>/proj w10 \
     -process memcpy.obj -recursive -noanalysis \
     -scriptPath <dir> -postScript build_watcom_fidb.java \
     "<out>/watcom10.fidb" "x86:LE:32:watcom" "Watcom" "10.6" "clib3r+math387r+plib3r"
   ```

## Notes / gotchas
- Library names carry Watcom's **trailing-`_`** register-model marker (`memcpy_`, `sscanf_`); on
  apply we strip the trailing `_` where it doesn't collide (kept, e.g., for a name that clashes).
- FID cannot split **byte-identical siblings** (`itoa`≡`ltoa`, `_uitoa`≡`_ultoa`, `toupper`/
  `_utoupper` on 32-bit) — the collision handler keeps one clean + one raw name; both are correct
  within the family.
- Low FID score = small function, **not** wrong — `toupper`/`tolower` score ~8 but the decompile
  confirms them exactly. Verify tiny matches by reading, don't gate purely on score.
- 8 functions matched an earlier **11.0** db but nothing in 10.6 (some printf float-format math
  internals + `time`/`localtime`) — reverted to `FUN_` rather than assert a wrong-version name;
  they're recoverable later if the right 10.6 module is fingerprinted or by manual analysis.
