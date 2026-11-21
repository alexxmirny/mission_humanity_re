#!/usr/bin/env python3
"""Judge real menu/save walks. The paths are the lane's own files, never planted metadata."""

import argparse
import hashlib
from pathlib import Path
import re
import struct
import sys

from _rundir import process_and_sessions

TABLES = {"unit", "building", "weapon", "system", "planets", "projects", "upgrades"}


def carrier(path):
    raw = path.read_bytes()
    if len(raw) < 8 or raw[-4:] != b"MHM1":
        raise ValueError(f"{path}: no MHM1 footer")
    size = struct.unpack_from("<I", raw, len(raw) - 8)[0]
    start = len(raw) - 8 - size
    if size < 36 or size > 36 + 64 * 98 or start < 40:
        raise ValueError(f"{path}: bad footer length {size}")
    data = raw[start:-8]
    ver, flags, count = struct.unpack_from("<BBH", data)
    if (ver, flags) != (1, 1) or count > 64:
        raise ValueError(f"{path}: invalid header")
    digest = data[4:36]
    pos, items = 36, []
    for _ in range(count):
        if pos + 2 > len(data):
            raise ValueError(f"{path}: truncated entry header")
        ni, nv = data[pos : pos + 2]
        pos += 2
        if not 1 <= ni <= 32 or nv > 64 or pos + ni + nv > len(data):
            raise ValueError(f"{path}: invalid entry length")
        ident = data[pos : pos + ni].decode("ascii")
        version = data[pos + ni : pos + ni + nv].decode("utf-8")
        if not re.fullmatch(r"[a-z0-9_-]+", ident):
            raise ValueError(f"{path}: invalid id")
        items.append((ident, version))
        pos += ni + nv
    if pos != len(data) or len(set(x[0] for x in items)) != len(items):
        raise ValueError(f"{path}: malformed entry list")
    return digest, items


def process_logs(run):
    dirs = [Path(d) for d in process_and_sessions(run)]

    def read(name):
        out = []
        for d in dirs:
            f = d / name
            if f.is_file():
                out.extend(f.read_text(encoding="utf-8", errors="replace").splitlines())
        return out

    return (
        dirs[0].parent.parent,
        read("mh_net.log"),
        read("mh_uidrive.log"),
        read("mh_launch.log"),
        read("mh_video.log"),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--step", required=True, choices=("a", "b", "legacy", "refuse"))
    ap.add_argument("--config", choices=("absent", "bound"), required=True)
    ap.add_argument("run")
    args = ap.parse_args()
    try:
        lane, net, ui, launch, video = process_logs(args.run)
        errors = []
        bind = "[modules] libmh: BOUND" if args.config == "bound" else "[modules] libmh: NOT BOUND"
        if not any(bind in line for line in net):
            errors.append(f"expected libmh {args.config} arm missing")
        active = "fixture" if args.step == "a" else "fixture,other"
        if not any(f"[mods] overlay armed: ids={active} " in line for line in video):
            errors.append(f"actual mod set {active} did not mount")
        if any("[mods]" in s and "REFUSED" in s for s in video):
            errors.append("mod mount refused")
        notices = [s for s in net if "[mods] load notice shown (" in s]
        want = {"a": [], "b": ["mismatch"], "legacy": ["legacy"], "refuse": []}[args.step]
        got = [s.split("[mods] load notice shown (", 1)[1].split(")", 1)[0] for s in notices]
        if got != want:
            errors.append(f"notice sequence {got!r}, wanted {want!r}")
        for name in {"a": ["dxsavea"], "b": ["dxsaveb", "dxfresh"], "legacy": [], "refuse": []}[
            args.step
        ]:
            if not any(f"[script] savegame {name} -> 1" in s for s in ui):
                errors.append(f"game_SaveGame({name}) did not return success")
        saves = lane / "save"
        if args.step == "a":
            digest, ids = carrier(saves / "dxsavea.sav")
            if ids != [("fixture", "test")] or not any(digest):
                errors.append(f"A carrier is not the mounted A set: {ids!r}")
        elif args.step == "b":
            a = carrier(saves / "dxsavea.sav")
            b = carrier(saves / "dxsaveb.sav")
            fresh = carrier(saves / "dxfresh.sav")
            if b != a:
                errors.append("resave after load did not retain A's exact digest and identity")
            if (
                a[1] != [("fixture", "test")]
                or fresh[1] != [("fixture", "test"), ("other", "test")]
                or a[0] == fresh[0]
            ):
                errors.append("fresh B origin not the distinct mounted B set")
            restore = [s for s in launch if "; G17: new-game cfg tables restored" in s]
            if not restore:
                errors.append("no G17 new-game restore")
            else:
                fields = {
                    name: (int(before), int(after))
                    for name, before, after in re.findall(r"(\w+)=(\d+)/(\d+)", restore[-1])
                }
                if set(fields) != TABLES or any(after for _, after in fields.values()):
                    errors.append(f"G17 incomplete restore: {fields!r}")
        elif args.step == "legacy":
            raw = (saves / "2-almost.sav").read_bytes()
            if raw[-4:] == b"MHM1":
                errors.append("legacy fixture unexpectedly has a mod footer")
        elif args.step == "refuse":
            if not any(
                "[caps] load REFUSED: the save is a raised-caps save and [caps] raise is OFF" in s
                for s in net
            ):
                errors.append("raised structural refusal missing")
            if not any("[caps] load REFUSED: showing the raised-limits message" in s for s in net):
                errors.append("raised structural refusal notice missing")
            if not any("[desync] BOUNDARY save_load (capovf)" in s for s in net):
                errors.append("menu did not attempt the specific capovf save")
            raw = (saves / "capovf.sav").read_bytes()
            expected_sha = "8a251d4126b62eecdf83307ff43935bc39e132aa66587164fbab165215d79537"
            if hashlib.sha256(raw).hexdigest() != expected_sha or raw[
                :40
            ] != b" 'Mission: Humanity' ver 1.00MHC1 " + bytes(6):
                errors.append("capovf is not the known raised-header fixture")
            if len(process_and_sessions(args.run)) != 1:
                errors.append("refused load unexpectedly started a game session")
            if not any(re.search(r"\[script\] \d+ ok .*: gamemode 3$", line) for line in ui):
                errors.append(
                    "after dismissing the native error the game did not remain in the menu"
                )
            if any("[caps] load: raised save accepted" in s for s in net):
                errors.append("raised save accepted with limits off")
        for msg in errors:
            print("FAIL:", msg)
        if not errors:
            print(f"PASS: actual save UI {args.step}")
        return bool(errors)
    except (OSError, ValueError, KeyError, struct.error, UnicodeError) as exc:
        print("FAIL:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
