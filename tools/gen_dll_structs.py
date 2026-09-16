#!/usr/bin/env python3
"""
gen_dll_structs.py -- generate src/mh_dll/mh/addr/mh_structs.gen.h from
tools/data/dll_struct_layouts.json (itself dumped from /eng/mh.exe by the
Ghidra-side struct-layout dump).

The struct-side companion to gen_dll_addrs.py. The injected mh.dll imports game
struct LAYOUTS so seam code dereferences named fields (widget->x, ev->event_type)
instead of hand-coded byte offsets. This tool is Ghidra-FREE: it reads the
committed JSON and emits a C++ header of mirror structs, one field per Ghidra
component with every inter-field gap explicitly padded, guarded by a static_assert
on the total size and on every named field's offset -- so a layout drift (a Ghidra
retype/reorder that changes an offset) becomes a COMPILE error, never a silent
wrong-offset read.

Regenerate the JSON when a struct changes (mh_dump_struct_layouts.py against
/eng/mh.exe), then re-run this. `--check` is the drift gate (in tools/lint_repo.py).

Usage:
  python tools/gen_dll_structs.py          # regenerate the header in place
  python tools/gen_dll_structs.py --check  # fail if the header differs from a fresh regen
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LAYOUTS = REPO / "tools" / "data" / "dll_struct_layouts.json"
HEADER = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_structs.gen.h"

# Ghidra scalar type name -> fixed-width C type. Pointers/arrays/struct refs are handled structurally.
SCALARS = {
    "uint": "uint32_t",
    "int": "int32_t",
    "ushort": "uint16_t",
    "short": "int16_t",
    "uchar": "uint8_t",
    "byte": "uint8_t",
    "sbyte": "int8_t",
    "schar": "int8_t",
    "bool": "uint8_t",
    "char": "char",
    # Ghidra models the game's UTF-16 text as `wchar_t` (2 bytes here -- llm_tutorial_step.title is
    # wchar_t[64] over 128 bytes). char16_t is the fixed-width C++ spelling of exactly that, so the
    # emitted field keeps the real element type instead of degrading to an opaque byte blob.
    "wchar_t": "char16_t",
    "long": "int32_t",
    "ulong": "uint32_t",
    "longlong": "int64_t",
    "ulonglong": "uint64_t",
    "float": "float",
    "double": "double",
    "pointer": "void *",
    "undefined": "uint8_t",
    "undefined1": "uint8_t",
    "undefined2": "uint16_t",
    "undefined4": "uint32_t",
    "undefined8": "uint64_t",
}


def cident(name):
    """Ghidra struct name -> the C identifier we emit.

    Ghidra type names may be NAMESPACED (`map::object::unit`), which is not a C identifier, so the
    `::` are flattened exactly the way dll_addr_manifest.json flattens symbol names. Keeping the
    Ghidra name as the manifest/JSON key means the dumper still finds the type; only the emitted
    spelling changes. (Every struct exported before 2026-07-27 had a flat name, so this had never
    fired -- `map::object::unit` produced `struct mh_map::object::unit` and would not compile.)
    """
    return name.replace("::", "_")


def map_type(t, exported):
    """(ghidra type string) -> (c_type_prefix, array_suffix) or (None, '') if opaque.

    exported: set of Ghidra struct names we emit as `mh_<name>` so pointers to them stay typed."""
    t = t.strip()
    # MULTI-DIMENSIONAL arrays. Ghidra names an array-of-arrays by appending the OUTER count to the
    # element type's name, so `byte[10][10]` is 10 rows of `byte[10]` -- 100 bytes. Peeling only one
    # bracket (and discarding the recursive call's suffix, which is what this did until 2026-08-01)
    # emits a 1-D `uint8_t x[10]` and silently loses 90 bytes, shifting every later field.
    # cfg::final::struct::Building's three `area` planes were the first to hit it. Peeling
    # right-to-left collects the dimensions OUTERMOST-FIRST, which is also C's declaration order.
    dims = []
    while True:
        m = re.match(r"^(.*)\[(\d+)\]$", t)
        if not m:
            break
        dims.append(int(m.group(2)))
        t = m.group(1).strip()
    if dims:
        base_c, _ = map_type(t, exported)
        return base_c, "".join("[%d]" % d for d in dims)
    if t.endswith("*"):
        base = t
        stars = 0
        while base.endswith("*"):
            base = base[:-1].strip()
            stars += 1
        if base in exported:
            return "struct mh_%s %s" % (cident(base), "*" * stars), ""
        if base == "char":
            return "char " + "*" * stars, ""
        return "void " + "*" * stars, ""  # unknown pointee: keep it a 4-byte pointer
    if t in SCALARS:
        return SCALARS[t], ""
    if t in exported:
        return "struct mh_%s" % cident(t), ""
    return None, ""  # opaque: caller emits raw bytes of the right size


def embedded_types(sd, exported):
    """The structs `sd` contains BY VALUE (directly or as an array of them).

    Pointer fields do not count -- a forward declaration is enough for those, which is exactly why
    the forward-declaration block below is not sufficient on its own.
    """
    out = set()
    for f in sd["fields"]:
        t = f["type"].strip()
        m = re.match(r"^(.*)\[(\d+)\]$", t)
        if m:
            t = m.group(1).strip()
        if t.endswith("*"):
            continue
        if t in exported:
            out.add(t)
    return out


def dependency_order(structs):
    """Emit order such that a struct is declared AFTER everything it embeds by value.

    The manifest's order is "what the DLL asked for", which is not a compile order: adding
    `game::player_data` (which embeds `llm_strat_ai_target_entry[64]`) before its element type
    produced `error C2079: uses undefined struct` on every build (found 2026-08-01, AI0). A cycle is
    impossible for by-value containment, so a plain DFS is enough; anything that somehow forms one
    is emitted in manifest order rather than dropped.
    """
    exported = set(structs.keys())
    order, state = [], {}

    def visit(name):
        if state.get(name) == 2:
            return
        if state.get(name) == 1:  # a by-value cycle cannot exist; do not loop if the data says so
            return
        state[name] = 1
        for dep in sorted(embedded_types(structs[name], exported)):
            visit(dep)
        state[name] = 2
        order.append(name)

    for name in structs:
        visit(name)
    return order


C_SIZES = {
    "int8_t": 1,
    "uint8_t": 1,
    "char": 1,
    "char16_t": 2,
    "int16_t": 2,
    "uint16_t": 2,
    "int32_t": 4,
    "uint32_t": 4,
    "int64_t": 8,
    "uint64_t": 8,
    "float": 4,
    "double": 8,
}


def emitted_size(c_type, suffix, structs):
    """Bytes the emitted declaration occupies under #pragma pack(1), or None if not derivable.

    Exists so a type the mapper renders too small fails HERE, naming the field, instead of as a wall
    of static_assert errors in whichever translation unit happens to include the header first. Both
    2026-08-01 generator bugs (a collapsed 2-D array, a nameless component emitting nothing) reached
    the compiler because nothing checked the emitted layout against the layout it was generated from.
    """
    n = 1
    for d in re.findall(r"\[(\d+)\]", suffix):
        n *= int(d)
    base = c_type.strip()
    if base.endswith("*"):
        return 4 * n  # 32-bit target
    if base in C_SIZES:
        return C_SIZES[base] * n
    if base.startswith("struct mh_"):
        for name, sd in structs.items():
            if "mh_" + cident(name) == base[len("struct ") :]:
                return sd["size"] * n
    return None


def emit(layouts) -> str:
    src = layouts["structs"]
    structs = {name: src[name] for name in dependency_order(src)}
    exported = set(structs.keys())
    o = []
    o.append(
        "// GENERATED by tools/gen_dll_structs.py from tools/data/dll_struct_layouts.json -- DO NOT EDIT."
    )
    o.append(
        "// Regenerate: mh_dump_struct_layouts.py (Ghidra) then gen_dll_structs.py; --check is the drift gate."
    )
    o.append("//")
    o.append(
        "// C++ mirrors of /eng/mh.exe game structs the injected DLL dereferences. Every inter-field gap is"
    )
    o.append(
        "// explicitly padded and every size/offset is static_assert'd, so a Ghidra layout change that shifts"
    )
    o.append(
        "// an offset breaks the BUILD instead of silently reading the wrong bytes. 32-bit target: game"
    )
    o.append(
        "// pointers are 4 bytes (mirrored as real pointers). Field docs come from the Ghidra field comments."
    )
    o.append("#pragma once")
    o.append("#include <cstdint>")
    o.append("#include <cstddef>")
    o.append("")
    o.append("namespace mh::game {")
    o.append("")
    # forward declarations so cross-referencing pointer fields resolve regardless of emit order
    for name in structs:
        o.append("struct mh_%s;" % cident(name))
    o.append("")
    o.append("#pragma pack(push, 1)")
    o.append("")
    problems = []
    for name, sd in structs.items():
        size = sd["size"]
        o.append("// %s (size 0x%x, Ghidra category %s)" % (name, size, sd.get("category", "?")))
        o.append("struct mh_%s {" % cident(name))
        cursor = 0
        pad_n = 0
        for f in sd["fields"]:
            off = f["offset"]
            if off > cursor:  # fill the gap left by undefined bytes
                o.append("    uint8_t _pad_0x%02x[%d];" % (cursor, off - cursor))
                pad_n += 1
            # A DEFINED component may still be NAMELESS in Ghidra (a typed-but-unlabelled slot).
            # Emitting `int32_t ;` for it compiles -- as a declaration with no declarator -- and
            # contributes ZERO bytes, which silently shifts every field after it. cfg::final::struct::
            # Building has four such slots and was the first struct to expose it (2026-08-01); the
            # per-field static_asserts caught it at build time, which is exactly what they are for.
            f_name = f["name"] or ("_unnamed_0x%x" % off)
            c_type, suffix = map_type(f["type"], exported)
            if c_type is None:  # opaque -> raw bytes of the field's size
                o.append(
                    "    uint8_t %s[%d]; // %s%s"
                    % (
                        f_name,
                        f["size"],
                        f["type"],
                        (" -- " + f["comment"]) if f["comment"] else "",
                    )
                )
            else:
                esz = emitted_size(c_type, suffix, structs)
                if esz is not None and esz != f["size"]:
                    problems.append(
                        "%s.%s @+0x%x: Ghidra says %d bytes, `%s %s%s` emits %d (type %s)"
                        % (name, f_name, off, f["size"], c_type, f_name, suffix, esz, f["type"])
                    )
                cm = (" // " + f["comment"]) if f["comment"] else ""
                # keep the SEMANTIC typedef visible: the dumper resolved it to a base scalar so
                # this header can be Ghidra-free, but which quantity the field carries is the point
                # of having the typedef at all.
                if f.get("type_name"):
                    cm = (cm + " " if cm else " // ") + "[%s]" % f["type_name"]
                o.append("    %s %s%s;%s" % (c_type, f_name, suffix, cm))
            cursor = off + f["size"]
        if cursor < size:
            o.append("    uint8_t _pad_0x%02x[%d];" % (cursor, size - cursor))
        o.append("};")
        o.append("")
    o.append("#pragma pack(pop)")
    o.append("")
    o.append(
        "// ---- layout guards: a shifted offset or size below is a Ghidra drift -> regenerate the JSON ----"
    )
    for name, sd in structs.items():
        o.append(
            'static_assert(sizeof(mh_%s) == 0x%x, "mh_%s size drift vs Ghidra");'
            % (cident(name), sd["size"], name)
        )
        for f in sd["fields"]:
            f_name = f["name"] or ("_unnamed_0x%x" % f["offset"])
            o.append(
                'static_assert(offsetof(mh_%s, %s) == 0x%x, "mh_%s.%s offset drift vs Ghidra");'
                % (cident(name), f_name, f["offset"], name, f_name)
            )
        o.append("")
    o.append("} // namespace mh::game")
    o.append("")
    if problems:
        raise SystemExit(
            "REFUSING to write a header whose fields do not match the dumped layout:\n  "
            + "\n  ".join(problems)
        )
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--check", action="store_true", help="drift gate: fail if header differs from regen"
    )
    args = ap.parse_args()

    layouts = json.loads(LAYOUTS.read_text(encoding="utf-8-sig"))
    text = emit(layouts)

    if args.check:
        on_disk = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
        if on_disk != text:
            print(
                f"DRIFT: {HEADER} differs from a fresh regen -- run gen_dll_structs.py and commit."
            )
            return 1
        print(
            f"ok: {HEADER.name} matches dll_struct_layouts.json ({len(layouts['structs'])} structs)"
        )
        return 0

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {HEADER} ({len(layouts['structs'])} structs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
