"""Judge live configuration and the save-A/load-under-B/New Game transition."""

import argparse
import os
import re
import sys

import _rundir
import check_cfg_io
import check_module_bind

LINE = re.compile(
    r"; \[cfgproof\] (\S+) sys=(-?\d+) planet=(\d+) system1_first=(-?\d+) "
    r"system1_second=(-?\d+) cost=(\d+:-?\d+,\d+:-?\d+,\d+:-?\d+,\d+:-?\d+)"
)
A = {1: 137, 2: 61, 3: 43}
B = {1: 211, 2: 67, 3: 47}
STOCK = {1: 100, 2: 60, 3: 40}


def nonzero_costs(slots):
    """Sum live charges by resource id; a zero slot has no meaningful id."""
    result = {}
    for slot in slots.split(","):
        resource, value = (int(part) for part in slot.split(":"))
        if value:
            result[resource] = result.get(resource, 0) + value
    return {resource: value for resource, value in result.items() if value}


def read_logs(target, name):
    chunks = []
    for directory in _rundir.process_and_sessions(target):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                chunks.append(f.read())
    return "\n".join(chunks)


def check(target, step, libmh=None):
    if libmh is not None:
        process = _rundir.process_and_sessions(target)[0]
        if check_module_bind.check(process, libmh, module="libmh"):
            raise ValueError("wrong libmh binding: expected " + libmh)
    video = read_logs(target, "mh_video.log")
    rows = {}
    for match in LINE.finditer(video):
        label, system, planet, first, second, costs = match.groups()
        if label in rows:
            raise ValueError("duplicate cfgproof label " + label)
        rows[label] = (int(system), int(planet), int(first), int(second), costs)

    def want(label, first, second, costs, planet=None):
        if label not in rows:
            raise ValueError("missing cfgproof row " + label)
        actual = rows[label]
        if actual[2:4] != (first, second) or nonzero_costs(actual[4]) != costs:
            raise ValueError("%s: wrong SYSTEM/cost state %r" % (label, actual))
        if planet is not None and (actual[0] != 1 or actual[1] != planet):
            raise ValueError("%s: expected system 1/planet %d, got %r" % (label, planet, actual))

    if step == "a":
        want("a_boot", 2, 1, A)
        want("a_new", 2, 1, A, 2)
    elif step == "b":
        want("b_boot", 1, 2, B)
        want("b_loaded", 2, 1, A, 2)
        # This is the SYSTEM-before-restore reproduction: inspect the selected planet,
        # not merely the final restored table, which can be correct after a wrong selection.
        want("b_fresh", 1, 2, B, 1)
    else:
        want("invalid_stock", 1, 2, STOCK)
        if "CONFIG REFUSED:" not in video:
            raise ValueError("missing config refusal, so stock state alone is not proof")
    label = {"a": "a_boot", "b": "b_boot", "invalid": "invalid_stock"}[step]
    check_cfg_io.check(target, label, "en", valid=step != "invalid")
    print("cfg runtime %s: PASS" % step)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("target")
    p.add_argument("--step", required=True, choices=("a", "b", "invalid"))
    p.add_argument("--libmh", choices=("absent", "bound"))
    args = p.parse_args()
    try:
        check(args.target, args.step, args.libmh)
    except (OSError, ValueError) as e:
        print("cfg runtime %s: FAIL: %s" % (args.step, e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
