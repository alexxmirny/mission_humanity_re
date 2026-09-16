# Watcom 10.5 FID database (`watcom105.fidb`)

A Ghidra **Function ID** database built from the **Watcom C/C++ 10.5** register-model runtime
libraries (a local Watcom 10.5 install, `<WATCOM105>`; libs dated **11 Jul 1995** — matching the `1995` string in `mh.exe`).
Built 2026-07-19 to test whether the game is a **10.5** build (the 1995 release) vs the previously
assumed **10.6** (see `README.md`). Same LanguageID as the binary: `x86:LE:32:watcom` / `watcomcpp`.

Carries **1249 functions** from `clib3r.lib` (lib386\nt, C runtime) + `math387r.lib` + `plib3r.lib`
(lib386, 387 math + C++ runtime). `attempted=1483 added=1249 excluded=234`.

## ⚠️ VERDICT REFUTED — see `README-watcom106_v2.md`. The binary is 10.6, not 10.5.
The comparison below was **confounded**: it pitted this *complete* 10.5 db against the *under-extracted*
original `watcom10.fidb`. Rebuilding 10.6 with the same complete extraction (`watcom106_v2.fidb`, 1262 fns)
and re-querying flips the result decisively — **10.6 v2 matches 208 vs 185, has 23 exclusive matches vs
0 for 10.5, and wins 35 of 185 common functions vs 0.** 10.5 is a strict subset that wins nothing. The
apparent 10.5 win below was purely missing modules in the old 10.6 db. Keep `watcom105.fidb` as the
comparison control only; **do not apply it.** (The `1995` string is a copyright/source year, not a
compiler-version signal.)

## Version verdict (SUPERSEDED — artifact): "10.5 fits better than 10.6"
Ran `FidService.processProgram` over `/eng/mh.exe` at the default score threshold (14.6) with each db
active in turn:

| metric | 10.6 (`watcom10.fidb`) | 10.5 (`watcom105.fidb`) |
| --- | --- | --- |
| functions matched | 160 | **185** |
| exclusive matches | 17 | **42** |
| higher score (of 143 common) | 19 | **32** (92 equal) |

Largest **10.5-favored** score gaps are on core version-sensitive CRT: `__set_errno_` 478→**1117**,
`__flush_` 231→**602**, `__fill_buffer_` 124→**308**, `__IOMode_` 64→**264**, `__STKOVERFLOW_` 16→**216**,
`__ZBuf2F` 42→**227**, `vsscanf_` 28→**199**, `__set_errno_nt_` 304→**485**. A minority favor 10.6
(`floor_` 257 vs 42, `__CHP` 238 vs 23, `__cvt_` 523 vs 409, the `__Init*`/thread-init trio) — so the
per-function signal is **mixed**, not the clean 5× sweep that separated 10.6 from 11.0, but the net
weight (more matches, 2.5× more exclusives, higher aggregate common-score, biggest gaps all on core
stdio/errno internals) points to **10.5**.

**Caveat / confound:** this 10.5 db was extracted more completely (1249 fns) than the existing 10.6 db
(905 fns — its original `wlib` TOC parse was greedy and under-captured ~half the `clib3r` modules; see
below). Total-count and exclusive-match advantages partly reflect that. The **per-common-function score
comparison is NOT affected by db completeness** (same function fingerprinted from each version's real
library bytes) and independently favors 10.5 — that is the load-bearing evidence. A fully clean
determination would rebuild the 10.6 db with the corrected complete extraction and re-compare.

## Build (reproducible)
Identical flow to `README.md` (10.6) with two fixes:
- **Libs:** `<WATCOM105>\lib386\nt\clib3r.lib`, `<WATCOM105>\lib386\math387r.lib`,
  `<WATCOM105>\lib386\plib3r.lib` (`<WATCOM105>` = your Watcom 10.5 install root). `wlib.exe` is Version 10.5.
- **TOC parse fix (matters):** the two-column `wlib` TOC needs *every* module token, not just the last
  per line. Use `grep -oE '\.\.+[A-Za-z0-9_]+' toc.txt | sed -E 's/^\.+//' | sort -u` (the 10.6
  README's `sed 's/.*\.\.+//'` is greedy and drops the first column → only ~half the modules).
- **`@command-file` must be in cwd** (run `wlib lib @ex.cmd` from *inside* the objs dir; a `../ex.cmd`
  relative path silently extracts nothing).

Then: headless `-import` the three obj dirs with `-processor x86:LE:32:watcom -cspec watcomcpp`
(1083 objs), then `-process memcpy.obj -recursive -noanalysis -postScript build_watcom_fidb.java
"<out>\watcom105.fidb" "x86:LE:32:watcom" "Watcom" "10.5" "clib3r+math387r+plib3r"`.

## Attaching / querying — same as `README.md`
`FidFileManager.getInstance().addUserFidFile(File(r"...\watcom105.fidb"))`, then the Function ID
analyzer or `FidService.openFidQueryService(language, False)` + `processProgram(program, qs, threshold,
monitor)`. To compare versions cleanly, toggle each `FidFile.setActive(...)` so only one db is active
per query, and restore `watcom10.fidb` active afterward.
