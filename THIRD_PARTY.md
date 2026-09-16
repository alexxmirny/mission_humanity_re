# Third-party notices

This project's own code and documentation are MIT-licensed — see [LICENSE](LICENSE). The
components below are **not ours**, and each keeps its own terms. Where the tree vendors the
upstream license text, this file says where to read it; where it does not, the license is named
with its canonical source so you can read it there.

## The retail game — no assets, no binaries, ever

The game this project reverse-engineers and re-implements is **Exterminacja**, also released as
**Mission Humanity**, by **Techland** (~2001). Techland owns it. Nothing about that changes here.

**This repository ships no game content of any kind** — no game executable (stock or patched), no
`mh.rsr` / `mh.nam` resource archives, no sprite banks, no audio, no video, no text, no map or
mission data, no installer. Everything here is either written from scratch or derived from
analysis, and you need a **legally obtained copy of the game** to run any of it against the real
binary. See [INSTALL.md](INSTALL.md).

Two consequences worth stating explicitly, because they are what keep the boundary from being a
promise:

- **The patch tooling refuses a binary it does not recognise.** Every patch site in
  `src/patcher/*.mh.patch.json` carries an `expect` field — the original bytes it is replacing,
  with wildcards where an operand is allowed to vary — and a site whose expected bytes do not match
  is rejected rather than applied. The in-memory applier in `mh.dll` reproduces the same guard
  against the loaded image and is all-or-nothing: one mismatched site leaves the image untouched
  and names what moved ([docs/inmem-patching.md](docs/inmem-patching.md)). A wrong or already-patched
  executable therefore fails loudly instead of being silently corrupted.
- **What the repository does contain of the retail binary is addresses and expected bytes, not
  content**: symbol names, virtual addresses, struct layouts, ABI tables, and the short byte runs
  those guards compare against. They exist so a patch can refuse a binary it was not built for, and
  they are useless without the game.

## Vendored source — license text is in the tree

| Component | Where | License | Text |
| --- | --- | --- | --- |
| **CLI11** 2.6.1 | `src/mh_dll/include/CLI11.hpp` | 3-clause BSD | vendored, at the head of the header |
| **stb_image** v2.30 | `src/mh_dll/include/stb_image.h` | dual: MIT **or** public domain (Unlicense) — we take the MIT alternative | vendored, at the foot of the header |

**CLI11** — Copyright (c) 2017-2025 University of Cincinnati, developed by Henry Schreiner under
NSF AWARD 1414736. All rights reserved. Upstream: <https://github.com/CLIUtils/CLI11>. The full
three-clause text, including the "neither the name of the copyright holder" clause and the warranty
disclaimer, is reproduced verbatim in the comment block at the head of the vendored header; it is
redistributed here under condition 1 of that license.

**stb_image** — Copyright (c) 2017 Sean Barrett. Upstream: <https://github.com/nothings/stb>. The
file offers ALTERNATIVE A (MIT) and ALTERNATIVE B (public domain); this project relies on
ALTERNATIVE A and retains the notice. Both texts are reproduced verbatim at the end of the vendored
header.

## Vendored source — license text is NOT in the tree

| Component | Where | License | Canonical text |
| --- | --- | --- | --- |
| **BitmapPlusPlus** | `src/mh_dll/include/BitmapPlusPlus.hpp` | MIT | <https://github.com/baderouaich/BitmapPlusPlus> |

The vendored single header carries no license or copyright block of its own. The upstream project
is MIT-licensed; read the `LICENSE` file in that repository for the authoritative text and
copyright line.

## Derived data

| Component | Where | License | Canonical text |
| --- | --- | --- | --- |
| **DejaVu Sans Mono** (derived from **Bitstream Vera**) | `src/mh_dll/mh/seams/overlay_font.h` | Bitstream Vera Fonts License + the DejaVu changes (public domain) | <https://dejavu-fonts.github.io/License.html> |
| **Open Watcom** runtime libraries | `tools/ghidra/watcom_fidb/*.fidb` and the transcribed CRT bodies in `src/mh_dll/libmh_test/crt_vendor_selftest.cpp` + `src/mh_dll/libmh_test/fp_x87_selftest.cpp` | Sybase Open Watcom Public License version 1.0 | <https://github.com/open-watcom/open-watcom-v2/blob/master/license.txt> |

**The font.** `overlay_font.h` is a generated 6x10 monochrome bitmap of ASCII `0x20`-`0x7e`,
rasterised from DejaVu Sans Mono at ppem 10 for the in-game debug overlay's text blitter. No font
file is redistributed — only the rendered cells. The Bitstream Vera license permits derivative works
provided they are not named "Bitstream" or "Vera"; this derivative is not, and the notice is carried
here and in the generated header itself.

**Open Watcom.** The retail game is a Watcom 10.6 build with the C runtime statically linked, which
touches this repository twice. (1) `tools/ghidra/watcom_fidb/` holds three Ghidra **Function ID**
databases built from Open Watcom runtime libraries, used to identify the CRT functions baked into
the game binary. A `.fidb` stores function hashes and names, not code. (2) 27 `__declspec(naked)`
bodies in `crt_vendor_selftest.cpp`, plus the x87 assembly in `fp_x87_selftest.cpp`, are
transcriptions of Open Watcom runtime routines — they are the oracle that proves this project's own
vendored CRT replacements behave identically. Both are Open Watcom's work, not Techland's and not
ours, and both are covered by the license named above.

## Documentation contributed by others

**`BNK_FORMAT.md`** — the sprite-bank format reference — was authored by **@dupan_80025** (aka PC
Vandalus), whose credit line is the first line of the file and stays there. It is republished with
that attribution intact and is **not** relicensed under this project's MIT grant; no separate
license was stated by its author. The `src/mh_dll/mh_tools` implementation of the format is this
project's own work and is MIT like the rest.
