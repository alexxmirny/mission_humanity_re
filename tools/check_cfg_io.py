"""Verify actual DATAEXT2 text pointers and both mounted/retail resource entry points."""

import os
import re

import _rundir
import check_mod_assets

ROW = re.compile(
    r"; \[cfgio\] (\S+) soldier=([0-9a-f!-]+) academy=([0-9a-f!-]+) "
    r"mounted=(\d+):(\d+):([0-9a-f!-]+) resource=(\d+):(\d+):([0-9a-f!-]+)"
)


def check(target, label, locale, valid=True):
    lane = check_mod_assets.lane_for(target)
    texts = []
    for directory in _rundir.process_and_sessions(target):
        path = os.path.join(directory, "mh_video.log")
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                texts.append(f.read())
    rows = [m.groups() for m in ROW.finditer("\n".join(texts)) if m.group(1) == label]
    if len(rows) != 1:
        raise ValueError("expected exactly one cfgio %s observation; got %d" % (label, len(rows)))
    _, soldier, academy, mhit, msize, mhex, rhit, rsize, rhex = rows[0]
    if not valid:
        if (mhit, msize, mhex, rhit, rsize, rhex) != ("0", "0", "-", "0", "0", "-"):
            raise ValueError("invalid later patch leaked a resource through snapshot or retail ABI")
        return
    with open(os.path.join(lane, "mods", "fixture", "data", "info", "cfgproof.txt"), "rb") as f:
        payload = f.read()
    expected = ("1", str(len(payload)), payload.hex())
    if (mhit, msize, mhex) != expected or (rhit, rsize, rhex) != expected:
        raise ValueError(
            "mounted/retail resource contents do not match independently provisioned bytes"
        )
    if locale == "en":
        soldier_expected = "Fixture Soldier"
    elif locale == "ru":
        soldier_expected = "Солдат фикстуры"
    else:
        raise ValueError("unsupported proof locale " + locale)
    # The English mod TEXT is overlaid before locale-specific TEXT. RU has no
    # Academy entry, so the English mod translation remains the winner.
    academy_expected = "Fixture Academy"
    if soldier != soldier_expected.encode("utf-16-le").hex():
        raise ValueError("actual Soldier TEXT pointer differs from selected translation")
    if academy != academy_expected.encode("utf-16-le").hex():
        raise ValueError("actual Academy TEXT pointer differs from expected translation/fallback")


def same_inputs(targets):
    paths = (
        "cfg/patch.cfg",
        "lang/en.cfg",
        "lang/ru.cfg",
        "data/info/cfgproof.txt",
        "data/maps/cfgfixture.mpm",
    )
    all_bytes = []
    lanes = []
    for target in targets:
        lane = check_mod_assets.lane_for(target)
        lanes.append(lane)
        root = os.path.join(lane, "mods", "fixture")
        data = []
        for path in paths:
            with open(os.path.join(root, *path.split("/")), "rb") as f:
                data.append(f.read())
        all_bytes.append(data)
    if len(set(lanes)) != len(lanes) or any(data != all_bytes[0] for data in all_bytes[1:]):
        raise ValueError("peers must have independent lanes with byte-identical config inputs")
