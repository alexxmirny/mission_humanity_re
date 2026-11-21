"""Judge the real DATAEXT1 full-resource/map fixture from lane files and runtime logs."""

import argparse
import hashlib
import os
import re
import sys

import _rundir
import check_module_bind


def lane_for(target):
    path = os.path.abspath(target)
    while True:
        if os.path.isfile(os.path.join(path, "mods", "fixture", "mod.ini")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            raise ValueError("no fixture lane above " + target)
        path = parent


def logs(target, name):
    result = []
    for directory in _rundir.process_and_sessions(target):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as fh:
                result.append(fh.read())
    return "\n".join(result)


def check(targets, network, libmh):
    lanes = [lane_for(t) for t in targets]
    if len(set(lanes)) != len(lanes):
        raise ValueError("peers share a lane rather than independent fixture copies")
    maps = []
    fixture_sets = []
    for i, (lane, target) in enumerate(zip(lanes, targets)):
        process = _rundir.process_and_sessions(target)[0]
        if check_module_bind.check(process, libmh, module="libmh"):
            raise ValueError("peer %d did not bind libmh as %s" % (i, libmh))
        data = os.path.join(lane, "mods", "fixture", "data")
        path = os.path.join(data, "maps", "fixture.mpm")
        with open(path, "rb") as fh:
            m = fh.read()
        maps.append(m)
        tlo = m[13:25].split(b"\0", 1)[0].decode("ascii").lower()
        files = {}
        for directory, _, names in os.walk(data):
            for name in names:
                path = os.path.join(directory, name)
                with open(path, "rb") as fh:
                    files[os.path.relpath(path, data).lower()] = fh.read()
        fixture_sets.append(files)
        for resource in ("banki.dat", "bank_00.bnk", tlo):
            if resource not in files:
                raise ValueError("missing fixture file " + resource)
        video = logs(target, "mh_video.log").lower()
        net = logs(target, "mh_net.log")
        if "; [mods] overlay armed: ids=fixture" not in video:
            raise ValueError("fixture was not mounted on peer %d" % i)
        for kind, resource in (("banki", "banki.dat"), ("bank", "bank_00.bnk"), ("tileset", tlo)):
            if not re.search(
                r"; \[mods\] overlay " + kind + r" copy .*" + re.escape(resource) + r" ", video
            ):
                raise ValueError("no actual %s copy on peer %d" % (resource, i))
        # Only the host visits the picker. The client may load via resource overlay rather than
        # utils_open_file, so either actual successful load path is evidence; never demand both.
        if i == 0:
            preview = re.search(
                r"; \[map\] mod preview [^\r\n]*fixture\.mpm -> ([^\r\n]+)", net, re.I
            )
            if not preview:
                raise ValueError("no preview of fixture map")
            with open(preview.group(1), "rb") as fh:
                if fh.read() != m:
                    raise ValueError("preview cache differs from mounted fixture")
        opened = re.search(r"; \[map\] mod open [^\r\n]*fixture\.mpm -> ([^\r\n]+)", net, re.I)
        copied = re.search(r"; \[mods\] overlay map copy [^\r\n]*fixture\.mpm ", video, re.I)
        if opened:
            with open(opened.group(1), "rb") as fh:
                if fh.read() != m:
                    raise ValueError("open cache differs from mounted fixture")
        elif not copied:
            raise ValueError("no actual fixture map load on peer %d" % i)
        if i == 0 and "; [map] picker:" not in net:
            raise ValueError("picker did not add the mod-only map row")
        if network and i == 0:
            sha = hashlib.sha256(m).hexdigest()[:16]
            if not re.search(r"; \[map\] host claim fixture\.mpm sha=" + sha + r"\b", net, re.I):
                raise ValueError("host map claim does not equal fixture bytes")
        if network:
            harness = logs(target, "mh_harness.log") + logs(target, "mh_match_harness.log")

            if "; SYNTH ok " not in harness or "; SYNTH FAIL " in harness:
                raise ValueError("movement workload did not demonstrate motion cleanly")
        if "snapshot SEND armed" in net:
            raise ValueError("unexpected map distribution")
    if any(files != fixture_sets[0] for files in fixture_sets[1:]):
        raise ValueError("peer mod payload bytes differ")
    print(
        "full mod fixture: mounted, runtime resource hits, picker, preview/open cache and map hash verified"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--network", action="store_true")
    ap.add_argument("--libmh", choices=("bound", "absent"), required=True)
    ap.add_argument("targets", nargs="+")
    a = ap.parse_args()
    if len(a.targets) != (2 if a.network else 1):
        ap.error("expected one lane for solo, two for network")
    try:
        check(a.targets, a.network, a.libmh)
    except (OSError, ValueError, UnicodeError, check_module_bind.Refusal) as ex:
        print("full mod fixture FAIL:", ex)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
