"""Judge DATAEXT2 gameplay using live UI/simulation observations, not config text."""

import argparse
import json
import os
import re
import sys

import _rundir
import check_cfg_io
import check_mod_assets
import check_module_bind
import check_storage_qol

PROD = re.compile(
    r"; \[cfgprod\] (\S+) side=(\d+) selected=(\d+) academy=(\d+) online=(-?\d+) "
    r"active=(\d+) queued=(-?\d+) count=(\d+) unit=(\d+) "
    r"pos=(\d+),(\d+) goal=(\d+),(\d+) res=(-?\d+),(-?\d+),(-?\d+),(-?\d+)"
)
AVAIL = re.compile(r"; \[avail\] (\S+) side=(\d+) ui_stride=\d+ (.*)")
REACTOR = re.compile(r";\s+\[a1\] reactor_ready slot=(\d+) id=27 .*?state=(\d+) .*?online=(-?\d+)")


def logs(target, name):
    chunks = []
    for directory in _rundir.process_and_sessions(target):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                chunks.append(f.read())
    return "\n".join(chunks)


def check_campaign(target):
    directories = _rundir.process_and_sessions(target)
    sessions = []
    for directory in directories:
        path = os.path.join(directory, "session.json")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                sessions.append(json.load(f))
    if (
        len(sessions) != 1
        or sessions[0].get("mode") != "campaign"
        or sessions[0].get("role") != "solo"
    ):
        raise ValueError("expected one real solo campaign session")
    video = logs(target, "mh_video.log")
    if "; [mods] overlay armed: ids=fixture,cfgcampaign " not in video:
        raise ValueError("campaign fixture mod set did not mount")
    path = os.path.join(
        check_mod_assets.lane_for(target), "mods", "cfgcampaign", "data", "107_ksie.mp"
    )
    size = os.path.getsize(path)
    if not re.search(r"; \[mods\] overlay hit 107_ksie\.mp " + str(size) + r"B\b", video, re.I):
        raise ValueError("actual campaign map was not read through the fixture resource overlay")
    if not re.search(r"; \[cfgproof\] before_academy sys=1 planet=2\b", video):
        raise ValueError("actual New Game was not SYSTEM 1 / planet 2")


def check(target, variant, storage_qol=False):
    video = logs(target, "mh_video.log")
    states = {}
    for m in PROD.finditer(video):
        label = m.group(1)
        if label in states:
            raise ValueError("duplicate production observation " + label)
        nums = [int(x) for x in m.groups()[1:]]
        states[label] = {
            "side": nums[0],
            "selected": nums[1],
            "academy": nums[2],
            "online": nums[3],
            "active": nums[4],
            "queued": nums[5],
            "count": nums[6],
            "unit": nums[7],
            "pos": tuple(nums[8:10]),
            "goal": tuple(nums[10:12]),
            "res": tuple(nums[12:16]),
        }
    required = (
        "before_academy",
        "before_production",
        "production_started",
        "production_finished",
        "production_moved",
    )
    for label in required:
        if label not in states:
            raise ValueError("missing live production observation " + label)
    if states["before_academy"]["count"] != 0:
        raise ValueError("soldier already existed before the workload")
    before, started = states["before_production"], states["production_started"]
    if not before["academy"] or before["selected"] != before["academy"] or before["online"] <= 0:
        raise ValueError("selected Academy was not operational")
    if started["active"] != 26 or started["academy"] != before["academy"]:
        raise ValueError("the selected Academy did not start producing the Soldier")
    expected = (137, 61, 43, 0) if variant == "a" else (211, 67, 47, 0)
    actual = tuple(a - b for a, b in zip(before["res"], started["res"]))
    if actual != expected:
        raise ValueError("actual production debit %r, expected %r" % (actual, expected))
    made, moved = states["production_finished"], states["production_moved"]
    if made["count"] < 1 or not made["unit"] or moved["unit"] != made["unit"]:
        raise ValueError("no surviving newly produced Soldier observed")
    if storage_qol:
        moves = re.findall(
            r"\[cfgproof\] move unit=(\d+) type=26 from=(\d+),(\d+) to=(\d+),(\d+)",
            logs(target, "mh_uidrive.log"),
        )
        if len(moves) != 1:
            raise ValueError("expected one independent ordinary Soldier move command")
        unit, x, y, gx, gy = map(int, moves[0])
        if unit != made["unit"] or moved["goal"] != (gx, gy) or moved["pos"] == (x, y):
            raise ValueError("produced Soldier did not obey the subsequent manual move")
    elif moved["pos"] == made["pos"]:
        raise ValueError("produced Soldier did not move")
    lists = {}
    for m in AVAIL.finditer(video):
        label, side, rest = m.groups()
        row = re.search(r"(?:^| )s" + side + r"=([^ ]+)", rest)
        if row:
            lists[label] = (
                set() if row.group(1) == "-" else {int(x) for x in row.group(1).split(",")}
            )
    if "blocked" not in lists or "unlocked" not in lists:
        raise ValueError("missing real build-offer observations")
    if 27 in lists["blocked"] or 28 not in lists["blocked"] or 27 not in lists["unlocked"]:
        raise ValueError("Reactor was not blocked before Academy and unlocked after it")
    if re.search(r";\s+\[a1\] before_academy slot=\d+ id=27\b", video):
        raise ValueError("Reactor already existed before the workload")
    barracks = re.finditer(
        r";\s+\[a1\] barracks_ready slot=(\d+) id=36 .*?state=(\d+) .*?online=(-?\d+)", video
    )
    if not any(int(m.group(2)) != 100 and int(m.group(3)) > 0 for m in barracks):
        raise ValueError("no normally placed and online Barracks in the live building roster")
    if not any(int(m.group(2)) != 100 and int(m.group(3)) > 0 for m in REACTOR.finditer(video)):
        raise ValueError("no normally placed and online Reactor in the live building roster")
    if "[script] LOG: cfg gameplay complete" not in logs(target, "mh_uidrive.log"):
        raise ValueError("UI workload did not complete")
    if storage_qol:
        check_storage_qol.check_production(target, states)
    print("cfg gameplay %s: PASS" % variant)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("target", nargs="+")
    p.add_argument("--libmh", required=True, choices=("absent", "bound"))
    p.add_argument("--mixed", action="store_true", help="EN host and RU client")
    p.add_argument(
        "--campaign", action="store_true", help="require actual solo campaign and fixture map load"
    )
    p.add_argument("--variant", required=True, choices=("a", "b"))
    p.add_argument(
        "--storage-qol", action="store_true", help="require actual production Auto-exit/rally"
    )
    args = p.parse_args()
    try:
        if args.mixed and len(args.target) != 2:
            raise ValueError("mixed locale proof requires host and client")
        if len(args.target) > 1:
            check_cfg_io.same_inputs(args.target)
        for index, target in enumerate(args.target):
            process = _rundir.process_and_sessions(target)[0]
            if check_module_bind.check(process, args.libmh, module="libmh"):
                raise ValueError("wrong libmh binding on peer %d" % index)
            check_cfg_io.check(
                target,
                "before_academy" if index == 0 else "client_locale",
                "ru" if args.mixed and index == 1 else "en",
            )
        if args.campaign:
            if len(args.target) != 1 or args.mixed:
                raise ValueError("campaign proof requires exactly one solo process")
            check_campaign(args.target[0])
        check(args.target[0], args.variant, args.storage_qol)
    except (OSError, ValueError) as e:
        print("cfg gameplay: FAIL: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
