#!/usr/bin/env python3
# tools/gen_proxy_stubs.py -- generate the forwarding stubs for a proxy ("shim") DLL.
#
# WHY: we want mh.dll loaded into a byte-for-byte UNTOUCHED retail mh.exe. The
#   DLL must attach BEFORE the exe's entry point (the mutex rename must beat WinMain, SetProcessDPIAware
#   must beat the first window, iat_replace assumes imports are snapped, and the hook install order
#   assumes exactly one pre-game arming pass). Only a STATICALLY-imported module gets that timing --
#   which rules out ddraw/dinput/dsound, all LoadLibrary'd at WM_CREATE.
#
#   mh.exe imports 12 DLLs; five are not KnownDLLs, so a file of that name sitting next to the exe wins
#   the search (the application directory is searched first, in safe mode and out). We drop a proxy on
#   one of those names. It re-exports everything the real system DLL exports, forwarding each call, and
#   statically imports MH_HostedPoolBase from mh.dll -- so the loader chains mh.exe -> shim -> mh.dll and
#   mh.dll's DllMain runs first, with NO LoadLibrary under the loader lock (the Windows-compat notes
#   forbids that outright).
#
# WHY NAKED STUBS rather than export forwarders: a PE forwarder entry ("msvfw32.ICClose") resolves the
#   module by NAME through the normal search order -- which finds US, not the system DLL, and spins. The
#   usual escape is to ship a renamed copy of the Microsoft DLL, which we will not do (redistribution +
#   version drift). So each export is a __declspec(naked) thunk that jumps through a slot resolved on
#   FIRST CALL from the real DLL's absolute path (GetSystemDirectoryA), never from DllMain.
#
#   The thunk clobbers only EAX, which is caller-saved in cdecl/stdcall/winapi, and it touches nothing
#   else -- the arguments stay exactly where the caller put them and the real function returns straight
#   to the original caller. That is what makes one stub shape correct for every signature.
#
# WHY EVERY EXPORT and not just the three the game calls: the module name is claimed process-wide. For
#   msvfw32 specifically, the real AVIFIL32.dll statically imports MSVFW32.dll, so avifil32 binds to our
#   proxy too. Ordinals are preserved for the same reason -- an ordinal-importing consumer must still
#   bind. A forwarder in the SOURCE DLL is a hard error here rather than a guess: forwarders need their
#   own chain resolution and none of our candidate DLLs has any.
#
# NOT a general-purpose proxy generator: it assumes 32-bit x86 (naked + inline asm), and it assumes
#   every export is CODE. A data export would need a different stub and there is no way to detect one
#   from the export table alone -- if a future candidate DLL has data exports, this will emit a thunk
#   that jumps to the data. All four live candidates (msvfw32/msacm32/avifil32/winmm) are code-only.
#
# Run: python tools/gen_proxy_stubs.py                    # -> src/mh_shim/msvfw32/*.gen.*
#      python tools/gen_proxy_stubs.py --dll msacm32.dll  # the fallback slot
#      python tools/gen_proxy_stubs.py --list             # just print the export table
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SHIM_ROOT = REPO_ROOT / "src" / "mh_shim"

# 32-bit system DLLs live in SysWOW64 on a 64-bit Windows; that is also what GetSystemDirectoryA
# returns to the WOW64 process at runtime, so the build-time table and the runtime load agree.
SYSDIR32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "SysWOW64"

IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class Export:
    __slots__ = ("ordinal", "name", "forwarder")

    def __init__(self, ordinal: int, name: str | None, forwarder: str | None):
        self.ordinal = ordinal
        self.name = name
        self.forwarder = forwarder

    @property
    def stub(self) -> str:
        """The C identifier the stub is defined under."""
        return self.name if self.name else "mh_proxy_ord_%d" % self.ordinal


def read_exports(path: Path) -> list[Export]:
    """Parse a PE export directory. Returns entries sorted by ordinal."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise SystemExit("%s: not a PE file" % path)
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    sec_off = pe + 24 + optsize

    sections = []
    for i in range(nsec):
        off = sec_off + i * 40
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, rsize), raw))

    def rva_to_off(rva: int) -> int | None:
        for vaddr, size, raw in sections:
            if vaddr <= rva < vaddr + size:
                return raw + (rva - vaddr)
        return None

    exp_rva, exp_size = struct.unpack_from("<II", data, pe + 24 + 96)
    if not exp_rva:
        raise SystemExit("%s: no export directory" % path)
    d = rva_to_off(exp_rva)

    ordinal_base = struct.unpack_from("<I", data, d + 16)[0]
    n_funcs, n_names = struct.unpack_from("<II", data, d + 20)
    a_funcs = rva_to_off(struct.unpack_from("<I", data, d + 28)[0])
    a_names = rva_to_off(struct.unpack_from("<I", data, d + 32)[0])
    a_ords = rva_to_off(struct.unpack_from("<I", data, d + 36)[0])

    def cstr(off: int) -> str:
        return data[off : data.index(b"\0", off)].decode("latin1")

    # index-into-the-function-array -> name, via the parallel name/ordinal arrays
    names: dict[int, str] = {}
    for i in range(n_names):
        idx = struct.unpack_from("<H", data, a_ords + 2 * i)[0]
        names[idx] = cstr(rva_to_off(struct.unpack_from("<I", data, a_names + 4 * i)[0]))

    out: list[Export] = []
    for i in range(n_funcs):
        fn_rva = struct.unpack_from("<I", data, a_funcs + 4 * i)[0]
        if fn_rva == 0:
            continue  # a hole in the ordinal space
        fwd = None
        if exp_rva <= fn_rva < exp_rva + exp_size:
            fwd = cstr(rva_to_off(fn_rva))
        out.append(Export(ordinal_base + i, names.get(i), fwd))
    return out


HEADER = """// GENERATED by tools/gen_proxy_stubs.py -- DO NOT EDIT BY HAND.
// Source DLL: %s (%d exports)
//
// Each stub jumps through g_mh_proxy_real[i], filled on first call by
// mh_proxy_resolve() in proxy_core.cpp. Only EAX is touched, and it is caller-saved in every
// convention these DLLs use, so one stub shape is correct for every signature.
"""


def emit_stubs(dll: str, exports: list[Export], src: Path) -> str:
    lines = [HEADER % (src, len(exports)), '#include "proxy_core.h"', ""]
    lines.append("extern \"C\" const char* const g_mh_proxy_names[MH_PROXY_COUNT] = {")
    for e in exports:
        # An ordinal-only export has no name to look up; resolve it by ordinal at runtime.
        lines.append('    %s,' % ('"%s"' % e.name if e.name else "(const char*)%d" % e.ordinal))
    lines.append("};")
    lines.append("")
    for i, e in enumerate(exports):
        lines += [
            'extern "C" __declspec(naked) void %s(void) {' % e.stub,
            "    __asm {",
            "        mov  eax, dword ptr [g_mh_proxy_real + %d]" % (i * 4),
            "        test eax, eax",
            "        jnz  ready",
            "        push %d" % i,
            "        call mh_proxy_resolve",
            "        add  esp, 4",
            "    ready:",
            "        jmp  eax",
            "    }",
            "}",
            "",
        ]
    return "\n".join(lines)


def emit_def(dll: str, exports: list[Export], src: Path) -> str:
    """The .def is what gives the exports their REAL names (and ordinals) in the output DLL.

    A cdecl `extern "C" void Foo(void)` is the object-file symbol `_Foo`; a bare `Foo` on an EXPORTS
    line matches it and exports the undecorated name, which is exactly what the importers look for.
    """
    lines = [
        "; GENERATED by tools/gen_proxy_stubs.py from %s -- DO NOT EDIT BY HAND." % src,
        "; Ordinals mirror the system DLL so an ordinal-importing consumer still binds.",
        "EXPORTS",
    ]
    for e in exports:
        if e.name:
            lines.append("    %s @%d" % (e.name, e.ordinal))
        else:
            lines.append("    %s @%d NONAME" % (e.stub, e.ordinal))
    lines.append("")
    return "\n".join(lines)


def emit_count_header(dll: str, exports: list[Export], src: Path) -> str:
    return (
        HEADER % (src, len(exports))
        + "#pragma once\n\n"
        + "#define MH_PROXY_DLL_NAME \"%s\"\n" % dll
        + "#define MH_PROXY_COUNT %d\n" % len(exports)
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--dll", default="msvfw32.dll", help="system DLL to proxy (default: msvfw32.dll)"
    )
    ap.add_argument("--sysdir", type=Path, default=SYSDIR32, help="where to read the real DLL from")
    ap.add_argument(
        "--out", type=Path, default=None, help="output dir (default src/mh_shim/<stem>)"
    )
    ap.add_argument("--list", action="store_true", help="print the export table and exit")
    args = ap.parse_args()

    src = args.sysdir / args.dll
    if not src.is_file():
        raise SystemExit("no such DLL: %s" % src)

    exports = read_exports(src)
    exports.sort(key=lambda e: e.ordinal)

    if args.list:
        for e in exports:
            print("%5d  %-40s %s" % (e.ordinal, e.name or "(noname)", e.forwarder or ""))
        print("total: %d" % len(exports))
        return 0

    # Loud failures, per the header: neither case can be guessed at.
    fwd = [e for e in exports if e.forwarder]
    if fwd:
        raise SystemExit(
            "%s exports %d forwarder(s) (e.g. %s -> %s); this generator cannot proxy those.\n"
            "Pick a different slot -- this generator needs a DLL that forwards nothing."
            % (args.dll, len(fwd), fwd[0].name, fwd[0].forwarder)
        )
    bad = [e.name for e in exports if e.name and not IDENT_RE.match(e.name)]
    if bad:
        raise SystemExit(
            "%s exports names that are not C identifiers: %s" % (args.dll, ", ".join(bad))
        )

    stem = Path(args.dll).stem
    out = args.out or (SHIM_ROOT / stem)
    out.mkdir(parents=True, exist_ok=True)
    (out / "proxy_table.gen.h").write_text(
        emit_count_header(args.dll, exports, src), encoding="utf-8"
    )
    (out / "proxy_stubs.gen.cpp").write_text(emit_stubs(args.dll, exports, src), encoding="utf-8")
    (out / "proxy_exports.gen.def").write_text(emit_def(args.dll, exports, src), encoding="utf-8")

    named = sum(1 for e in exports if e.name)
    print(
        "%s: %d exports (%d named, %d ordinal-only) -> %s"
        % (args.dll, len(exports), named, len(exports) - named, out)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
