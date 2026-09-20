#!/usr/bin/env python3
# tools/gen_no_cd_manifest.py -- bake the "run without the game CD" mhpatch manifest for a given exe.
#
# Used two ways:
#   * as a CLI, to (re)generate the committed src/patcher/no_cd_EN.mh.patch.json;
#   * as a library, by tools/mp_run.py, which bakes a fresh manifest for the run exe on every build
#     (see LAYOUT below -- a cave manifest is only valid for ONE input layout, so it cannot be a
#     single committed file that applies everywhere in a chain).
#
# WHY (RE, 2026-07-25; full write-up in the disc-check RE): the disc requirement is a PRESENCE check,
#   not a data dependency. llm_cd_locate_and_open_audio (EN 0x004c371b) scans drive indices 0..0x1f
#   for a DRIVE_CDROM whose volume label is 'MH'; on a hit it builds G_CD_DATA_PATH (0x00603f78, a
#   MAX_PATH buffer) = "<cdroot><SrcPath>\" (SrcPath = the HKLM\Software\Techland\Mission Humanity
#   value read into 0x0060407c, default "Setup") and opens MCI cdaudio. llm_cd_ensure_present
#   (EN 0x004c3921) then loops win::ShowErrorMessage ("Insert 'Mission: Humanity' CD") until that
#   BUFFER is non-empty -- the function returns 0 for BOTH "found" and "scan exhausted", so the side
#   effect is the signal -- and a cancel calls llm_game_shutdown_cleanup. Game data (mh.rsr /
#   mh_ex.rsr / Res\) is read CWD-FIRST by rsr::TryReadRsrFile, so an installed copy needs no disc.
#
# WHAT it emits (the "fallback only" variant): the drive scan is left INTACT -- a real disc still
#   sets the real CD path and opens CD audio exactly like retail. Only the scan-exhausted exit (the
#   `JMP <epilogue>` reached when the drive index hits 0x20) is diverted into an appended .mhnocd
#   cave that fills G_CD_DATA_PATH with the EXE'S OWN DIRECTORY (GetModuleFileNameA, truncated after
#   the last '\'; ".\" if that fails) and rejoins the original "return 0" epilogue. Module-derived
#   rather than CWD-derived on purpose: a launcher with the wrong working directory (the 2026-07-10
#   schtasks trap) must not be able to resurrect the dialog.
#
#   NOT the same thing as the RETIRED cd_audio_nonfatal (user, 2026-09-17), which only defused a FAILED MCI open while a disc IS
#   present -- that branch is unreachable when no disc is in the drive.
#
# LAYOUT: mhpatch's add_data_section demands `vaddr == the input's next-free VA`, so a manifest with
#   a cave is baked for ONE chain position; the cave's own `jmp` back is assembled relative to that
#   VA too. bake() therefore defaults cave_va to the input's next-free VA. The committed EN manifest
#   is baked against PRISTINE EN and is the FIRST patch of a chain -- which is also where the
#   whole-file sha256 belongs (it identifies the build for everything that follows), hence --pin.
#
# Both builds are supported and auto-detected from the site bytes (RU/EN share the data layout; only
# the code VAs differ), so this also covers mp_run's frozen-RU fallback path.
#
# Run: python tools/gen_no_cd_manifest.py                      # -> src/patcher/no_cd_EN.mh.patch.json
#      python tools/gen_no_cd_manifest.py --input <exe> --out <path> [--cave-va 0x...] [--pin]

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PATCHER = os.path.join(REPO, "src", "patcher")
if PATCHER not in sys.path:
    sys.path.insert(0, PATCHER)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import lief  # noqa: E402

import machine_config as machine  # noqa: E402
import mhpatch  # noqa: E402

CLEAN_EN = machine.POLYGON_CLEAN + "/mh.exe"

CD_PATH = 0x00603F78  # G_CD_DATA_PATH, MAX_PATH(0x104) (next symbol = the SrcPath buffer @0x60407c)
SITE_EXPECT = "e9 f9 00 00 00"  # the scan-exhausted `JMP <return-0 epilogue>`
CAVE_VSIZE = 0x1000

# site = the JMP inside llm_cd_locate_and_open_audio taken when the drive scan runs off the end;
# rejoin = its original target (MOV [EBP-0x18],0 -> the shared "return 0" epilogue).
BUILDS = {
    "en": {
        "site": 0x004C374A,
        "rejoin": 0x004C3848,
        "sha256": "1cd5810c5c0b845f0b7c8359a494243d9bec51397c8becfe149841be09c2562d",
        "target": "mh.exe (English retail, md5 b3b389e8...) -- /eng/mh.exe",
    },
    "ru": {
        "site": 0x004C3246,
        "rejoin": 0x004C3344,
        "sha256": None,  # no pristine-RU baseline on this box; RU is the frozen reference
        "target": "mh.exe (Russian retail, md5 a9bf89a6...) -- /mh.exe (frozen reference)",
    },
}


def _cave_asm(rejoin):
    # EBP/ESP are untouched (pushad/popad) and the epilogue at `rejoin` restores every callee-saved
    # register from the frame anyway, so the cave only has to leave the stack balanced.
    return f"""
    pushad
    push  0x104
    push  {CD_PATH:#x}
    push  0
    call  dword ptr [import:KERNEL32.dll!GetModuleFileNameA]
    test  eax, eax
    je    dot
    mov   ecx, eax
    mov   edx, {CD_PATH:#x}
scan:
    cmp   byte ptr [edx + ecx - 1], 0x5c
    je    cut
    dec   ecx
    jne   scan
    jmp   dot
cut:
    mov   byte ptr [edx + ecx], 0
    jmp   done
dot:
    mov   word ptr [edx], 0x5c2e
    mov   byte ptr [edx + 2], 0
done:
    popad
    jmp   {rejoin:#x}
"""


def detect_build(pe, data):
    """Which build is this exe? Decided by the site bytes, so it works on an already-patched
    (net_load'd / caphiked) copy too -- the site itself is untouched by those."""
    for name, b in BUILDS.items():
        off = mhpatch._va_to_offset(pe, b["site"])
        if off is None:
            continue
        if mhpatch._match_expect(bytes(data[off : off + 5]), SITE_EXPECT):
            return name, b
    return None, None


def bake(input_exe, out_path, cave_va=None, pin=False):
    """Write a no-CD manifest for `input_exe`. Raises ValueError if the exe isn't a recognised build
    (or the site is already patched). Returns a dict describing what was emitted."""
    pe = lief.PE.parse(input_exe)
    if pe is None:
        raise ValueError("lief could not parse %r" % input_exe)
    data = open(input_exe, "rb").read()

    name, b = detect_build(pe, data)
    if name is None:
        raise ValueError(
            "no recognised no-CD site in %r (not an RU/EN mh.exe, or already patched)" % input_exe
        )

    free = mhpatch.next_free_va(pe)
    if cave_va is None:
        cave_va = free
    elif cave_va != free:
        raise ValueError("cave VA %#x != %s's next-free %#x" % (cave_va, input_exe, free))

    slots = mhpatch.iat_slots(pe)
    cave = mhpatch.assemble(_cave_asm(b["rejoin"]), cave_va, slots)
    if len(cave) > CAVE_VSIZE:
        raise ValueError("cave overflows its section (%d bytes)" % len(cave))
    jmp = mhpatch.assemble(f"jmp {cave_va:#x}", b["site"], slots)
    assert len(jmp) == 5, len(jmp)

    man = {
        "_comment": (
            "Run WITHOUT the game CD. The disc requirement is a presence check, not a data "
            "dependency: rsr::TryReadRsrFile reads mh.rsr/mh_ex.rsr CWD-first and only falls back to "
            "the CD, so a local install already has everything. llm_cd_locate_and_open_audio scans "
            "for a DRIVE_CDROM labelled 'MH' and, on a hit, sets G_CD_DATA_PATH (0x00603f78) = "
            "'<cdroot><SrcPath>\\'; llm_cd_ensure_present then loops the blocking \"Insert 'Mission: "
            'Humanity\' CD" dialog until that buffer is non-empty, and a cancel calls '
            "llm_game_shutdown_cleanup. This patch leaves the scan INTACT -- a real disc still sets "
            "the real CD path and opens CD audio exactly as before -- and only diverts the "
            "scan-exhausted exit into an appended cave that fills G_CD_DATA_PATH with the EXE'S OWN "
            "DIRECTORY (GetModuleFileNameA, truncated after the last '\\'; \".\\\" if that fails) "
            "before rejoining the original 'return 0' epilogue. Module-derived, not CWD-derived, so "
            "a launcher with the wrong working directory cannot resurrect the dialog. NOT the same "
            "as the retired cd_audio_nonfatal (which only defused a FAILED MCI open while a disc IS present). "
            "The cave is an APPENDED section, so this manifest is baked for ONE chain position -- "
            "regenerate it for another with tools/gen_no_cd_manifest.py --input/--out. "
            "GENERATED FILE -- do not hand-edit."
        ),
        "target": b["target"],
        "sections": [
            {
                "name": ".mhnocd",
                "vaddr": hex(cave_va),
                "vsize": hex(CAVE_VSIZE),
                "data": cave.hex(" "),
                "_note": "no-disc fallback: G_CD_DATA_PATH <- exe directory, then rejoin %#x"
                % b["rejoin"],
            }
        ],
        "patches": [
            {
                "va": hex(b["site"]),
                "expect": SITE_EXPECT,
                "bytes": jmp.hex(" "),
                "_note": (
                    "llm_cd_locate_and_open_audio: the drive scan ran off the end (no CD-ROM "
                    "labelled 'MH') -- divert to the cave instead of returning with G_CD_DATA_PATH "
                    "empty (which is what makes llm_cd_ensure_present show the insert-CD dialog)"
                ),
            }
        ],
    }
    if pin:
        if not b["sha256"]:
            raise ValueError("no baseline sha256 known for the %s build -- cannot --pin" % name)
        man["sha256"] = b["sha256"]  # only the FIRST manifest of a chain identifies the build

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1)
        f.write("\n")
    return {"build": name, "cave_va": cave_va, "cave_len": len(cave), "site": b["site"]}


def main():
    ap = argparse.ArgumentParser(description="bake the no-CD manifest for one exe/chain position")
    ap.add_argument("--input", default=CLEAN_EN, help="exe the manifest will be applied to")
    ap.add_argument("--out", default=os.path.join(PATCHER, "no_cd_EN.mh.patch.json"))
    ap.add_argument("--cave-va", default=None, help="override; must equal the input's next-free VA")
    ap.add_argument(
        "--pin",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="embed the whole-file sha256 (first manifest of a chain only)",
    )
    a = ap.parse_args()
    try:
        info = bake(
            a.input,
            a.out,
            cave_va=int(str(a.cave_va), 16) if a.cave_va else None,
            pin=a.pin,
        )
    except ValueError as e:
        sys.exit(str(e))
    print("wrote %s" % a.out)
    print(
        "  build=%s  site=%#x  cave=%d bytes @ %#x  pinned=%s"
        % (info["build"], info["site"], info["cave_len"], info["cave_va"], a.pin)
    )


if __name__ == "__main__":
    main()
