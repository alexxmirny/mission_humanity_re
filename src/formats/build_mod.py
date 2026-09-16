#!/usr/bin/env python3
"""One-click mod build: edited BMP tree -> BNKs -> mh.rsr/mh.nam -> installed game.

Chains the whole validated pipeline (first proven in-game 2026-07-05, striped power
plants): `mh_tools pack` encodes the edited sprite tree back into BANKI.DAT + BANK
files, they are staged over the pristine uncompressed resource base (plus an optional
overlay folder for any other modded files), `pack.pack()` rebuilds the .rsr/.nam pair
(entries stored uncompressed - the loader only LZW-decompresses on magic; confirmed
fine in-game), and the result is installed into the game dir with one-time *.vanilla
backups.

Typical use (all defaults come from tools/machine_config.py -- this machine's paths):
  python build_mod.py                      # rebuild from machine.BANKI and install
  python build_mod.py --no-install         # just build into %TEMP%\\mh_mod_build\\out
  python build_mod.py --overlay my_mod     # also copy my_mod\\mh\\** (and mh_ex\\**) over the base

Revert in-game state: copy <game>\\mh.rsr.vanilla/.nam.vanilla back over mh.rsr/.nam.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# repo root is three levels up (src/formats/build_mod.py); import machine_config for the game-data roots.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import machine_config as machine  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack import pack  # noqa: E402  (src/formats/pack.py)

VANILLA_MH_ENTRIES = 843  # entry count of the retail mh pack (sanity warning only)


def stage_tree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def overlay_tree(src, dst):
    """Copy overlay files over the staged base; returns (copied, new_files)."""
    copied, new = 0, []
    for path, _dirs, files in os.walk(src):
        rel = os.path.relpath(path, src)
        for name in files:
            target_dir = os.path.join(dst, rel) if rel != "." else dst
            os.makedirs(target_dir, exist_ok=True)
            target = os.path.join(target_dir, name)
            if not os.path.exists(target):
                new.append(os.path.normpath(os.path.join(rel, name)))
            shutil.copy2(os.path.join(path, name), target)
            copied += 1
    return copied, new


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tree", default=machine.BANKI,
                    help="edited sprite tree (mh_tools unpack layout v2)")
    ap.add_argument("--clean", default=machine.RES_UNPACK,
                    help="pristine uncompressed base (contains mh\\ and mh_ex\\)")
    ap.add_argument("--overlay", help="optional folder with mh\\ / mh_ex\\ subdirs of extra modded files")
    ap.add_argument("--cfg-src", metavar="DIR",
                    help="cfgkit YAML source tree (mod_src/cfg): compiled to INIT.CFG/initlang.cfg "
                         "and folded in as an overlay (the one-command edit-YAML -> in-game path)")
    ap.add_argument("--map-src", metavar="DIR",
                    help="mapkit Tiled tree (mod_src/map): *.tmj compiled to .MP maps "
                         "and folded in as an overlay (the one-command edit-in-Tiled -> in-game path)")
    ap.add_argument("--game", default=machine.RU_POLYGON, help="game dir to install into")
    ap.add_argument("--build-dir", default=os.path.join(tempfile.gettempdir(), "mh_mod_build"))
    ap.add_argument("--mh-tools",
                    default=os.path.normpath(os.path.join(script_dir, "..", "mh_dll", "Release", "mh_tools.exe")))
    ap.add_argument("--no-install", action="store_true", help="build only, skip the game-dir step")
    ap.add_argument("--reencode-all", action="store_true", help="force re-encoding of every sprite (debug)")
    args = ap.parse_args()

    build = args.build_dir
    banks_dir = os.path.join(build, "banks")
    packsrc = os.path.join(build, "packsrc")
    out_dir = os.path.join(build, "out")
    os.makedirs(out_dir, exist_ok=True)

    # 0. Compile the cfgkit YAML sources (if any) into an overlay. The compiled
    #    INIT.CFG/initlang.cfg land in exactly the mh\\init + mh_ex\\init overlay
    #    shape, so it joins the overlay list below like any other modded files.
    overlays = [args.overlay] if args.overlay else []
    if args.cfg_src:
        cfg_overlay = os.path.join(build, "cfg_overlay")
        repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
        cmd = [sys.executable, "-m", "src.formats.cfgkit.cfg_compile",
               "--src", args.cfg_src, "--out", cfg_overlay]
        print(f"[cfg] compiling {args.cfg_src} -> {cfg_overlay}")
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
        tail = (r.stdout.strip().splitlines() or [""])[-3:]
        for line in tail:
            print("      " + line)
        if r.returncode != 0:
            sys.exit(f"cfg compile failed (fix the YAML sources or aliases):\n{r.stdout}\n{r.stderr}")
        overlays.append(cfg_overlay)

    if args.map_src:
        map_overlay = os.path.join(build, "map_overlay")
        repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
        cmd = [sys.executable, "-m", "src.formats.mapkit.map_compile",
               "--src", args.map_src, "--out", map_overlay]
        print(f"[map] compiling {args.map_src} -> {map_overlay}")
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
        tail = (r.stdout.strip().splitlines() or [""])[-3:]
        for line in tail:
            print("      " + line)
        if r.returncode != 0:
            sys.exit(f"map compile failed (fix the Tiled sources):\n{r.stdout}\n{r.stderr}")
        overlays.append(map_overlay)

    # 1. Encode the sprite tree back into BANKI.DAT + BANK_*.BNK.
    if not os.path.isfile(args.mh_tools):
        sys.exit(f"mh_tools.exe not found at {args.mh_tools} (build the mh.sln Release first)")
    cmd = [args.mh_tools, "pack", "-i", args.tree, "-o", banks_dir]
    if args.reencode_all:
        cmd.append("--reencode-all")
    print(f"[1/4] encoding banks: {' '.join(cmd)}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"mh_tools pack failed:\n{r.stdout}\n{r.stderr}")
    print("      " + (r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "ok"))

    # 2. Stage pack sources: clean base + rebuilt banks + overlay.
    packs = ["mh"]
    print(f"[2/4] staging {args.clean}\\mh -> {packsrc}\\mh")
    stage_tree(os.path.join(args.clean, "mh"), os.path.join(packsrc, "mh"))
    nbank = 0
    for name in os.listdir(banks_dir):
        shutil.copy2(os.path.join(banks_dir, name), os.path.join(packsrc, "mh", name))
        nbank += 1
    print(f"      {nbank} bank files staged over the base")
    for overlay in overlays:
        for sub in ("mh", "mh_ex"):
            osub = os.path.join(overlay, sub)
            if not os.path.isdir(osub) or not any(os.scandir(osub)):
                continue
            if sub == "mh_ex" and "mh_ex" not in packs:
                print(f"      staging {args.clean}\\mh_ex (overlay touches it)")
                stage_tree(os.path.join(args.clean, "mh_ex"), os.path.join(packsrc, "mh_ex"))
                packs.append("mh_ex")
            copied, new = overlay_tree(osub, os.path.join(packsrc, sub))
            print(f"      overlay {sub}: {copied} files")
            for f in new:
                print(f"      WARNING: overlay adds new resource {sub}\\{f} "
                      "(the exe must actually request this name)")

    # 3. Build the .rsr/.nam pairs.
    print(f"[3/4] packing {packs} -> {out_dir}")
    pack(packsrc, out_dir, packs)
    for p in packs:
        n = os.path.getsize(os.path.join(out_dir, f"{p}.nam")) // 64
        size = os.path.getsize(os.path.join(out_dir, f"{p}.rsr"))
        note = ""
        if p == "mh" and n != VANILLA_MH_ENTRIES:
            note = f"  WARNING: vanilla has {VANILLA_MH_ENTRIES} entries"
        print(f"      {p}.rsr: {size:,} bytes, {n} entries{note}")

    # 4. Install with one-time backups.
    if args.no_install:
        print(f"[4/4] --no-install: outputs left in {out_dir}")
        return
    print(f"[4/4] installing into {args.game}")
    for p in packs:
        for ext in (".rsr", ".nam"):
            live = os.path.join(args.game, p + ext)
            backup = live + ".vanilla"
            if os.path.exists(live) and not os.path.exists(backup):
                shutil.copy2(live, backup)
                print(f"      backed up {p}{ext} -> {p}{ext}.vanilla")
            shutil.copy2(os.path.join(out_dir, p + ext), live)
            print(f"      installed {p}{ext}")
    print("done. Revert with: copy <game>\\mh.rsr.vanilla mh.rsr (and .nam)")


if __name__ == "__main__":
    main()
