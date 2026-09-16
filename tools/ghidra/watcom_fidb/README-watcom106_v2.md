# Watcom 10.6 FID database v2 — full extraction (`watcom106_v2.fidb`)

A **rebuild** of the 10.6 FID db (`watcom10.fidb`) using the **corrected complete extraction** flow
(the original had a greedy `wlib`-TOC `sed` that dropped ~half the `clib3r` modules). Built 2026-07-19
from `C:\WATCOM10` (wlib 10.6; `clib3r.lib` in `lib386\nt`, `math387r.lib`/`plib3r.lib` in `lib386`,
dated Feb 1996). Same LanguageID `x86:LE:32:watcom` / `watcomcpp`.

**1262 functions** (`attempted=1499 added=1262 excluded=237`), from 1094 obj modules (680 clib + 90
math + 324 cpp) — vs the original `watcom10.fidb`'s 905. Now completeness-matched to `watcom105.fidb`
(1249 fns) so a 10.5-vs-10.6 comparison is confound-free.

## VERDICT: the binary is Watcom 10.6, NOT 10.5
The earlier `README-watcom105.md` reported "10.5 fits better than 10.6", but that was an **extraction
artifact** — a *complete* 10.5 db was being compared against the *under-extracted* original 10.6 db.
Rebuilding 10.6 with the same complete flow and re-querying `/eng/mh.exe` (default threshold 14.6, one
db active at a time) **reverses the result decisively**:

| metric | 10.5 (`watcom105.fidb`, 1249 fns) | 10.6 v2 (`watcom106_v2.fidb`, 1262 fns) |
| --- | --- | --- |
| functions matched | 185 | **208** |
| exclusive matches | **0** | **23** |
| per-common score wins (of 185) | **0** | **35** (150 tie) |

**10.5 is a strict subset of 10.6's matches and wins zero common functions.** Every 10.6-favored score
gap is on core version-sensitive CRT: `memset_` +290, `sprintf_`/`__fprtf_` +238, `__cvt_`/`floor_`/
`_Scale10V_` +215. The 10.5 "wins" from the first pass were all functions the old 10.6 db simply hadn't
extracted. **Conclusion: `mh.exe` is a Watcom 10.6 build** (the original `README.md` determination
stands). The `1995` string in the binary is a copyright/source year, **not** a compiler-version signal
(10.6 shipped Feb 1996; the 10.5 install's libs are Jul 1995).

**Methodology lesson (added to the ledger):** when pinning a version by FID hit-rate, both candidate
dbs must be built with the **same extraction completeness** — total-match and exclusive-match counts are
meaningless otherwise; only the **per-common-function score** (same function fingerprinted from each
version's real bytes) is confound-free, and it must corroborate before trusting a count-based verdict.

## Status / how to use
`watcom106_v2.fidb` is the **more complete 10.6 db** and should supersede `watcom10.fidb` for auto-ID
(more coverage, same version). The coordinator will handle activation + a fresh Function-ID auto-apply
pass. `watcom105.fidb` is retained only as the comparison control — **do not apply it** (wrong version).
Build/attach mechanics are identical to `README.md`.
