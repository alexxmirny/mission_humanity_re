#!/usr/bin/env python3
"""replay_match_segment.py -- mp:SES7b: replay ONE recorded match offline, in a fresh process, and
compare its per-step hashes with the recording's own.

WHAT IT REPLAYS. A match folder from a bug report (or a rig lane) that holds the mp:SES7 match
segment: mh_match_orders.bin + mh_match_clock.bin + mh_match_seed.bin (the replay path's three
inputs on the MATCH's own step axis) and mh_match_harness.log (the recorded per-step hashes, on the
PROCESS axis, with `step_base` on its `; [match] segment OPEN` line). A pre-SES7 report has no
segment; its PROCESS folder (mh_orders.bin / mh_clock.bin / mh_harness_seed.bin / mh_harness.log)
is replayable only when the process's FIRST sim step was this match's first step -- i.e. no earlier
match in that process ran a single step -- because the process seed is dumped at process step 1.
The tool checks that off the session.json files and refuses otherwise, by name.

HOW. A per-tree solo lane (the ui_test `solo=` machinery), the three inputs copied beside mh.exe as
mh_orders.bin / mh_clock.bin / mh_harness_seed.bin, and a generated walk: host a network game on
the recorded MAP (session.json `map`, picked by row in "Available maps"), seat the RECORDED ROSTER,
Start -- sp_det.txt's walk with the map row added.

THE ROSTER (tooling:TL-REPLAY-ROSTER). The walk seats every occupied lobby slot of the recording, read
from the SEED's `players` region (the recording's own Players[] -- session.json `roster` lists only the
humans and its slot numbers have disagreed with the seed, so it is a cross-check, never the source):
controller_flags +0x06 bit 2 = a network human, 0x0b = a computer, 0 = nothing. Slot 0 is the replay
host. Every other occupied slot gets a lobby AI row at its OWN index (rows 1.. at y=115+17*i), each
gated on `occ`: a recorded AI is the real thing, a recorded human is a placeholder the seed overwrites
(its flags are the recording's, so the replay treats it as network-controlled; the shape the 2-peer
oracle always had at slot 1). A computer's race is set with `pokerace`; colour has no lobby hook and
the seed carries it. WHY: the AI's private (unhashed) state is built at session start from the LOBBY
roster; one AI at slot 1 left a 3-human + AI-at-3 recording with player 3 not an established AI and a
divergence at match step 2. A seed that cannot be parsed falls back to the old one-computer walk. The harness then runs the replay
contract (the LIB-REF one, tools/fixture_replay.py REPLAY_FLAGS, plus the seed inject):

    seed_step=1 seed_mode=1      the match's step-1 state over every hash region, pre-body
    order_mode=2                 the recorded queue drives dispatch
    replay_dispatch_inject=1     (mp:D37b) the queue is set to the recorded snapshot at dispatch
                                 ENTRY (the recorder's point); at the top of the step (the hash
                                 point) to that snapshot less its AI-tick tail
    replay_suppress_enqueue=0    every enqueue runs for real (the suppression lost same-pass orders)
    replay_drop_net_issue=1      (mp:D37b) llm_strat_order_dispatch drops network-controlled
                                 players' orders: the recording holds their lockstep copies
    replay_session_mode=3        (mp:D37b, a lockstep recording) the sim BODY runs in the recorded
                                 SESSION_MODE; the frame around it stays single-process
    replay_seat=<slot>           (mp:D37b) PlayerSide = the recording's session.json slot (--seat)
    replay_ai_off=0              the AI runs
    synth_move=0                 no workload: the recording is the only order source
    clock track                  mh_clock.bin pins TOTAL_GAME_TIME per step (the recorded deltas)
    stop_step=N exit_on_stop=1   N = the last recorded step, on the match axis

THE CONFIGURATION FOLLOWS THE RECORDING. The recorder's mh_harness.log says `configuration (1)`
(mh.dll + mh_net.dll + mh_harness.dll, no libmh.dll -- what a player runs) or `(2)` (libmh.dll's
spine present -- the rig). The replay lane gets the same module set: libmh.dll is removed from the
lane for a (1) recording (ui_test refreshes only the satellites a lane already has, so the removal
sticks). `--config 1|2` overrides.

THE VERDICT compares the `state` column (never `combined`: it folds in the wall-clock/pacing regions
state_excluded() drops -- fixture_replay.py's ruling) and the clock column at every step both logs
carry. IDENTICAL, or the FIRST diverging step plus the non-excluded regions that differ there when
both logs carry `R` lines (the rig does; a player's region_hash_step=0 log does not, and then the
first step is all the tool can name). Exit 0 = IDENTICAL over >= --min-steps common steps.

BYTE-EXACT, IN ADDITION (mp:D42). When BOTH the recording and the replay lane carry a state
recording (`[desync] state_record=1` in `mh_net.ini` on each side -> `mh_match_state.bin` in the
match folder), `tools/state_record.py`'s `first_diff` judges them byte-exactly and the result prints
next to the hash verdict above -- extra evidence, never a second vote: it does not change `ok` / the
exit code, and it is skipped silently (no state recording on one or both sides is the common case).

Usage:
    python tools/replay_match_segment.py <match_or_process_dir> [--config auto|1|2] [--steps N]
    python tools/replay_match_segment.py <dir> --compare <replay_run_dir>    # no run, compare only
    python tools/replay_match_segment.py <client dir> --seat 1 --against <host dir>   # D37b oracle
        [--net-extra "pioneer_refill_fix=0"] [--map-file X.mpm] [--legacy-suppress] [--extra k=v]
    python tools/replay_match_segment.py --selftest                          # offline, no rig
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import _rundir  # noqa: E402 -- the run-dir name contract (SES8 + SES1)

SEG = {
    "orders": "mh_match_orders.bin",
    "clock": "mh_match_clock.bin",
    "seed": "mh_match_seed.bin",
    "log": "mh_match_harness.log",
}
PROC = {
    "orders": "mh_orders.bin",
    "clock": "mh_clock.bin",
    "seed": "mh_harness_seed.bin",
    "log": "mh_harness.log",
}
LANE_NAME = {"orders": "mh_orders.bin", "clock": "mh_clock.bin", "seed": "mh_harness_seed.bin"}
ORDERS_MAGIC_LEN = 8  # MHOR v1 header: u32 magic, u32 version
ORDER_REC = 72  # u32 step + the 68-byte llm_strat_order

REPLAY_FLAGS = {
    "seed_step": 1,
    "seed_mode": 1,
    "order_mode": 2,
    # mp:D37b: NOT the LIB-REF suppression. Every enqueue runs for real, and the queue is set to the
    # recorded snapshot at dispatch ENTRY (the recorder's own point). The suppression lost every order
    # a handler appends during the dispatch pass -- it never reaches an entry snapshot -- which left
    # the rc4 field recordings at match step 5857 (Nortus) / 6357 (Last Question). --legacy-suppress
    # restores the old pair for comparison.
    "replay_suppress_enqueue": 0,
    "replay_dispatch_inject": 1,
    # mp:D37b: a lockstep recording STAGED every order issued for a network-controlled player; the
    # released copy is in the recording. The single-process replay (SESSION_MODE 2) would enqueue it
    # at once, a step early and twice -- so those issue calls are dropped (harness issue_detour).
    "replay_drop_net_issue": 1,
    "replay_ai_off": 0,
    "synth_move": 0,
    "fixed_step": 0,
    "region_hash_step": 1,
    "exit_on_stop": 1,
    "match_segment": 0,  # the replay's own session must not write a second segment beside it
}

# rows of "Available maps" (mp_host_el2.txt, from a `dump`): widget [1] origin (14,54), row i at
# y = 98 + 14*i, listed in the game's (FindFirstFile, case-folded) order of Maps\*.mpm.
MAP_ROW_X, MAP_ROW_Y0, MAP_ROW_DY = 80, 98, 14

STEP_RE = re.compile(r"^(\d+) ([0-9A-Fa-f]{16}) ([0-9A-Fa-f]{16}) ([0-9A-Fa-f]{16})")
OPEN_RE = re.compile(r"; \[match\] segment OPEN (\S+) at process step (\d+).*?step_base=(\d+)")
CONFIG_RE = re.compile(r"; \[harness\] configuration \((\d)\)")
ARM_RE = re.compile(r"; ==== mh replay harness armed: (.*?) ====")


class Refusal(Exception):
    """A named reason this folder cannot be replayed (not a divergence)."""


# ---- reading a recording ------------------------------------------------------------------------


def _read_text(path):
    with open(path, "r", encoding="latin-1") as fh:
        return fh.read()


def parse_stream(text, base=0):
    """({match_step: (clock, combined, state)}, {match_step: [region hashes]}) from a harness log."""
    steps, regs = {}, {}
    for ln in text.splitlines():
        if ln.startswith("R "):
            p = ln.split()
            if len(p) > 2 and p[1].isdigit():
                regs[int(p[1]) - base] = [x.upper() for x in p[2:]]
            continue
        m = STEP_RE.match(ln)
        if m:
            steps[int(m.group(1)) - base] = tuple(x.upper() for x in m.groups()[1:])
    return steps, regs


def arm_flags(text):
    """The recorder's `; ==== mh replay harness armed: k=v ... ====` banner as a dict (last one)."""
    got = {}
    for m in ARM_RE.finditer(text):
        got = dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv)
    return got


def config_of(text):
    m = CONFIG_RE.findall(text)
    return int(m[-1]) if m else None


def hash_kind_of(*texts):
    """tooling:TL-HARN-INCHASH: the hash kind a recording's per-step hashes carry -- the first of
    `texts` that states one (mp_analyze.hash_kind_of_text: last kind-bearing line), else 1 (a log that
    states none predates the item, when only the FNV walk existed)."""
    import mp_analyze

    for t in texts:
        k = mp_analyze.hash_kind_of_text(t)
        if k is not None:
            return k
    return 1


def kind_refusal(judged_kind, replay_log_text, judged_label="the recording"):
    """The named refusal when the replay hashed with another kind than the stream it is judged
    against, else None. A replay run on a DLL that predates the knob reads as kind 1."""
    import mp_analyze

    return mp_analyze.kind_mismatch(
        judged_kind, mp_analyze.hash_kind_of_text(replay_log_text), judged_label, "the replay"
    )


def _session_json(d):
    try:
        with open(os.path.join(d, "session.json"), encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def sessions_of_process(logs_dir, proc_leaf):
    """[(leaf, session.json)] of the session folders whose process_dir names `proc_leaf`, by time.

    By TIME, not by raw name: SES8 (`YYYY-MM-DDTHH-MM-SSZ_...`) and SES1 (`YYYYMMDDTHHMMSSZ_...`)
    names do not sort together as strings (tools/_rundir.py sort_key)."""
    out = []
    for d in sorted(glob.glob(os.path.join(logs_dir, "*")), key=_rundir.sort_key):
        j = _session_json(d)
        if j and j.get("process_dir") == proc_leaf:
            out.append((os.path.basename(d), j))
    return out


def resolve(path):
    """The recording in `path` -> dict(kind, files{orders,clock,seed,log}, base, map, config, ...).

    Raises Refusal with the reason when the folder cannot be replayed."""
    path = os.path.abspath(path.rstrip("\\/"))
    if not os.path.isdir(path):
        raise Refusal("no such folder: %s" % path)
    logs_dir = os.path.dirname(path)
    sj = _session_json(path)
    if os.path.isfile(os.path.join(path, SEG["log"])) or os.path.isfile(
        os.path.join(path, SEG["orders"])
    ):
        files = {k: os.path.join(path, v) for k, v in SEG.items()}
        missing = [
            SEG[k] for k in ("orders", "clock", "seed", "log") if not os.path.isfile(files[k])
        ]
        if missing:
            raise Refusal(
                "segment incomplete, missing %s -- the recorder ran without order_mode=1 "
                "(no orders/clock) or with seed_mode!=0 (no seed): nothing to replay from"
                % ", ".join(missing)
            )
        log = _read_text(files["log"])
        m = OPEN_RE.search(log)
        if not m:
            raise Refusal(
                "%s has no `; [match] segment OPEN` line -- step_base unknown" % SEG["log"]
            )
        base = int(m.group(3))
        proc_leaf = (sj or {}).get("process_dir")
        proc_log = os.path.join(logs_dir, proc_leaf, PROC["log"]) if proc_leaf else None
        arm = _read_text(proc_log) if proc_log and os.path.isfile(proc_log) else ""
        return dict(
            kind="segment",
            dir=path,
            snap_dir=path,
            files=files,
            base=base,
            map=(sj or {}).get("map"),
            slot=(sj or {}).get("slot"),
            lockstep=bool((sj or {}).get("lockstep_step_ms")),
            config=config_of(arm),
            arm=arm_flags(arm),
            ref=log,
            # TL-HARN-INCHASH: the segment's OPEN line states the kind itself (no token = kind 1);
            # the process log's fingerprint line is the fallback.
            hash_kind=hash_kind_of(log, arm),
        )
    # a session folder without a segment (pre-SES7) -> its process folder
    if sj and sj.get("process_dir") and not os.path.isfile(os.path.join(path, PROC["orders"])):
        return resolve(os.path.join(logs_dir, sj["process_dir"]))
    files = {k: os.path.join(path, v) for k, v in PROC.items()}
    missing = [PROC[k] for k in ("orders", "clock", "seed", "log") if not os.path.isfile(files[k])]
    if missing:
        raise Refusal(
            "no match segment and no process recording here (missing %s)" % ", ".join(missing)
        )
    ref = _read_text(files["log"])
    arm = arm_flags(ref)
    if arm.get("seed_step") != "1" or arm.get("seed_mode") != "0":
        raise Refusal(
            "the process seed was not dumped at process step 1 (armed seed_step=%s seed_mode=%s)"
            % (arm.get("seed_step"), arm.get("seed_mode"))
        )
    played = [
        (leaf, j)
        for leaf, j in sessions_of_process(logs_dir, os.path.basename(path))
        if (j.get("final_clock_ms") or 0) > 0
    ]
    if len(played) != 1:
        raise Refusal(
            "a process recording replays only when exactly ONE match of that process ran sim steps "
            "(its seed is process step 1); found %d: %s -- use that match's mp:SES7 segment"
            % (len(played), ", ".join(leaf for leaf, _ in played) or "none")
        )
    return dict(
        kind="process",
        dir=path,
        snap_dir=os.path.join(logs_dir, played[0][0]),
        files=files,
        base=0,
        map=played[0][1].get("map"),
        slot=played[0][1].get("slot"),
        lockstep=bool(played[0][1].get("lockstep_step_ms")),
        config=config_of(ref),
        arm=arm,
        ref=ref,
        session=played[0][0],
        hash_kind=hash_kind_of(ref),
    )


def snap_steps(snap_dir):
    """{step: path} of the desync watch's snapshots in a match folder (mh_desync_snap_<step>.bin)."""
    out = {}
    for p in glob.glob(os.path.join(snap_dir or "", "mh_desync_snap_*.bin")):
        m = re.search(r"mh_desync_snap_(\d+)\.bin$", p)
        if m:
            out[int(m.group(1))] = p
    return out


def match_state_path(dir_):
    """The mh_match_state*.bin state recording (mp:D40/D42) in `dir_`, or None. Globs rather than
    the fixed name because a match folder that already holds one gets `mh_match_state_<n>.bin`
    (docs/state-record.md); the first (lexically) match is the one this match folder wrote.

    mp:D46: mh.dll gzips the file after the match and deletes the raw one, so the folder may hold
    `mh_match_state.bin.gz` instead (state_record.load reads both). A recording is named by its raw
    stem: when BOTH forms exist (a kill between the .gz rename and the raw delete) the raw file is
    returned, and a `.gz.tmp` (an unfinished compress) matches neither glob."""
    if not dir_:
        return None
    stems = set()
    for p in glob.glob(os.path.join(dir_, "mh_match_state*.bin")):
        stems.add(p)
    for p in glob.glob(os.path.join(dir_, "mh_match_state*.bin.gz")):
        stems.add(p[: -len(".gz")])
    cands = sorted(stems)
    if not cands:
        return None
    return cands[0] if os.path.exists(cands[0]) else cands[0] + ".gz"


def snap_cadence(steps):
    """(every, max) that makes the replay dump at every recorded snapshot step (their gcd)."""
    import math

    if not steps:
        return 0, 0
    g = 0
    for s in steps:
        g = math.gcd(g, s)
    return g, max(steps) // g


def diff_snaps(a_path, b_path):
    """[region names whose VERDICT streams differ] between two MHSN snapshots of one step."""
    import mp_desync_snap_diff as sd

    a, b = sd.read_snap(a_path), sd.read_snap(b_path)
    if a["step"] != b["step"] or a["fp"] != b["fp"]:
        raise Refusal(
            "snapshot pair %s / %s is not comparable (step or manifest)" % (a_path, b_path)
        )
    names = [r["name"] if isinstance(r, dict) else str(r) for r in sd.load_manifest()]
    return [
        names[i] if i < len(names) else "col%d" % i
        for i, (x, y) in enumerate(zip(a["regions"], b["regions"]))
        if x != y
    ]


def clock_steps(path):
    return os.path.getsize(path) // 8


def order_records(path):
    return max(0, (os.path.getsize(path) - ORDERS_MAGIC_LEN) // ORDER_REC)


# ---- comparing ----------------------------------------------------------------------------------


def excluded_columns():
    """Positional indexes of the state_excluded() regions, plus the column names."""
    import fixture_replay
    import mp_analyze

    names = list(mp_analyze.REGION_NAMES)
    ex = fixture_replay.excluded_names()
    return names, {i for i, n in enumerate(names) if n in ex}


def compare(ref_steps, ref_regs, rep_steps, rep_regs, names=None, excluded=frozenset()):
    """-> dict(common, first, what, regions). `first` is None when every common step agrees."""
    common = sorted(set(ref_steps) & set(rep_steps))
    first, what = None, None
    for s in common:
        a, b = ref_steps[s], rep_steps[s]
        if a[2] != b[2] or a[0] != b[0]:
            first = s
            what = "state" if a[2] != b[2] else "clock"
            break
    regions = []
    if first is not None and first in ref_regs and first in rep_regs:
        for i, (x, y) in enumerate(zip(ref_regs[first], rep_regs[first])):
            if x != y and i not in excluded:
                regions.append(names[i] if names and i < len(names) else "col%d" % i)
    return dict(common=len(common), first=first, what=what, regions=regions)


def verdict_text(rec, res, min_steps):
    if res["common"] < min_steps:
        return (
            False,
            "REFUSED: only %d common step(s) (< %d) -- the replay did not run far enough"
            % (
                res["common"],
                min_steps,
            ),
        )
    if res["first"] is None:
        return True, "IDENTICAL: state + clock agree at all %d common step(s)" % res["common"]
    where = (
        " regions: %s" % ", ".join(res["regions"])
        if res["regions"]
        else " (no R lines on both sides at that step)"
    )
    return False, "DIVERGED at match step %d (%s);%s" % (res["first"], res["what"], where)


def judge_state_recording(rec_path, rep_path):
    """mp:D42: judge two state recordings BYTE-EXACTLY via state_record.first_diff, alongside (never
    in place of) the hash verdict above -- a separate, additional oracle, not a replacement for it.
    -> (ok, [lines to print]); ok is True/False/None (None = not judged: a path is missing, or the
    files did not decode/compare -- the caller's exit code stays governed by the hash verdict only).
    """
    if not rec_path or not rep_path:
        return None, []
    import state_record

    lines = ["  state recording (byte-exact, mp:D42): %s vs %s" % (rec_path, rep_path)]
    try:
        res = state_record.first_diff(state_record.load(rec_path), state_record.load(rep_path))
    except (ValueError, OSError) as e:
        lines.append("  state recording judge: ERROR -- %s" % e)
        return None, lines
    if res["identical"]:
        lines.append(
            "  state recording: IDENTICAL over its own steps %d..%d (this file's own step axis -- "
            "see mh_net.log's STATE RECORD step axis line for the offset to the match-step axis above)"
            % (res["first_step"], res["last_step"])
        )
        return True, lines
    lines.append("  state recording: FIRST DIFF at its own step %d" % res["step"])
    for d in res["diffs"]:
        loc = ""
        if d.get("field"):
            rec_str = "record %d, " % d["record"] if d.get("record") is not None else ""
            loc = "  (%sfield=%s)" % (rec_str, d["field"])
        elif d.get("note"):
            loc = "  (%s)" % d["note"]
        lines.append("    %-20s offset 0x%x len %d%s" % (d["region"], d["offset"], d["len"], loc))
    return False, lines


# ---- running ------------------------------------------------------------------------------------


def map_row(maps_dir, map_name):
    """Row index of `map_name` in "Available maps" (case-insensitive), or None."""
    if not map_name:
        return 0, None
    files = sorted(
        (f for f in os.listdir(maps_dir) if f.lower().endswith(".mpm")), key=lambda f: f.upper()
    )
    for i, f in enumerate(files):
        if f.lower() == map_name.lower():
            return i, os.path.splitext(map_name)[0].lower()
    return None, None


SLOT_ROW_Y0, SLOT_ROW_DY = (
    115,
    17,
)  # lobby slot i's state spinner: click 70 y (row 1 = 132, row 2 = 149)
PLAYER_DESC = 0x34  # llm_strat_player_desc stride; Players[8] = the `players` hash region
FLAG_NET, FLAG_COMPUTER = 0x04, 0x0B


def seed_players(seed_path):
    """[dict(slot, flags, race, color, name)] for every Players[] entry of a seed blob, or None when the
    blob is not a seed of THIS manifest (size mismatch) -- the caller falls back to the legacy walk."""
    try:
        with open(
            os.path.join(REPO, "tools", "data", "hash_manifest.json"), encoding="utf-8"
        ) as fh:
            regions = json.load(fh)["regions"]
        off = total = 0
        for r in regions:
            if r["name"] == "players":
                off = total
            total += r["size"]
        with open(seed_path, "rb") as fh:
            blob = fh.read()
    except (OSError, ValueError, KeyError):
        return None
    if len(blob) != total:
        return None
    out = []
    for i in range(8):
        rec = blob[off + i * PLAYER_DESC : off + (i + 1) * PLAYER_DESC]
        out.append(
            dict(
                slot=i,
                flags=rec[6],
                race=rec[0],
                color=rec[1],
                name=rec[0x10:0x30].split(b"\0")[0].decode("latin-1"),
            )
        )
    return out


def roster_of(players):
    """The lobby plan of a recording's Players[]: [dict(slot, kind 'host'|'human'|'ai', race, name)] for
    the occupied slots, by slot. Slot 0 is the replay host whatever it was."""
    plan = []
    for p in players or ():
        f = p["flags"]
        if not f:
            continue
        if p["slot"] == 0:
            kind = "host"
        elif f & FLAG_NET:
            kind = "human"
        elif f == FLAG_COMPUTER:
            kind = "ai"
        else:
            continue
        plan.append(dict(slot=p["slot"], kind=kind, race=p["race"], name=p["name"]))
    return plan


def session_humans(sj):
    """{slot} of the humans session.json's `roster` ("0:HFef,1:HRizzen") names, or None."""
    ros = (sj or {}).get("roster")
    if not ros:
        return None
    return {int(m.group(1)) for m in re.finditer(r"(\d+):H", ros)}


def roster_note(plan, sj):
    """A line when session.json names a human slot the seed does not hold, else None. (session.json
    lists only the humans, and a client's lists only itself: a subset of the seed's is the normal case.)"""
    hs = session_humans(sj)
    seeded = {p["slot"] for p in plan if p["kind"] in ("host", "human")}
    if hs is None or hs <= seeded:  # a client's roster names only itself; a host's omits the AIs
        return None
    return "session.json roster humans %s != seed humans %s -- the seed is used" % (
        sorted(hs),
        sorted(seeded),
    )


def walk_script(row, label, plan=None):
    """sp_det.txt's walk with the map row picked (split click, then the right panel's label) and the
    recorded roster seated (`plan` from roster_of; None = one computer in slot 1, the legacy walk)."""
    y = MAP_ROW_Y0 + MAP_ROW_DY * row
    pick = (
        "cursor %d %d\ncursor %d %d\npress %d %d\nrelease %d %d\npresent %s\n"
        % ((MAP_ROW_X, y) * 4 + (label,))
        if label
        else ""
    )
    seat, pokes = "click 70 132\npresent Computer\n", ""
    others = [p for p in plan or () if p["slot"] >= 1]
    if others:
        seat = ""
        for n, p in enumerate(others, start=1):
            seat += "# slot %d: %s %r\nclick 70 %d\npresent Computer\nocc %d\n" % (
                p["slot"],
                "COMPUTER" if p["kind"] == "ai" else "human (placeholder)",
                p["name"],
                SLOT_ROW_Y0 + SLOT_ROW_DY * p["slot"],
                n + 1,
            )
            if p["kind"] == "ai":
                pokes += "pokerace %d %d\nrace %d %d\n" % ((p["slot"], p["race"]) * 2)
    return (
        "# GENERATED by tools/replay_match_segment.py (mp:SES7b): host alone on the recorded map,\n"
        "# seat the recorded roster (TL-REPLAY-ROSTER), Start; the harness injects the seed.\n"
        "settled value:110\nclickv 110\npresent Player name\nsettled Ok\nclickl Ok\n"
        "settled Create\nclickl Create\npresent Game name\nsettled Ok\nclickl Ok\n"
        "present Available maps\nsettled Ok\n" + pick + "settled Ok\nclickl Ok\n"
        "present Network players\nsettled Start\n" + seat + pokes + "enabled Start\n"
        "clickl Start\nabsent Network players\ngamemode 2\n"
        "log REPLAY in-game, the harness owns the run from here\nend\n"
    )


def provision_lane(config):
    import ui_test

    lane = ui_test._solo_lane_dir("mhreplay_c%d" % config)
    lib = os.path.join(lane, "libmh.dll")
    if config == 1 and os.path.isfile(lib):
        os.remove(lib)  # configuration (1): no spine; refresh_satellites will not put it back
    if config == 2 and not os.path.isfile(lib):
        shutil.copy(os.path.join(REPO, "src", "mh_dll", "Release", "libmh.dll"), lib)
    return lane


def lane_add_map(lane, map_file):
    """Put `map_file` into the lane's Maps\\ WITHOUT touching the shared install: a lane whose Maps is
    still the shared link gets its own directory first (every shared map linked in individually,
    make_lane's link_or_copy), then the extra map is copied in. For a map a player downloaded (X2),
    which the install does not ship -- the rc4 Nortus match's nortus.mpm."""
    import make_lane

    maps = os.path.join(lane, "Maps")
    if os.path.islink(maps) or (os.path.isdir(maps) and _is_junction(maps)):
        shared = os.path.realpath(maps)
        _unlink_dir_link(maps)
        os.makedirs(maps)
        for f in sorted(os.listdir(shared)):
            if os.path.isfile(os.path.join(shared, f)):
                make_lane.link_or_copy(os.path.join(shared, f), os.path.join(maps, f), False)
    os.makedirs(maps, exist_ok=True)
    shutil.copyfile(map_file, os.path.join(maps, os.path.basename(map_file)))


def _is_junction(p):
    fn = getattr(os.path, "isjunction", None)
    if fn is not None:
        return fn(p)
    try:
        return bool(os.lstat(p).st_file_attributes & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT
    except (OSError, AttributeError):
        return False


def _unlink_dir_link(p):
    try:
        os.unlink(p)
    except OSError:
        os.rmdir(p)  # a junction is removed as an (empty-looking) directory, never recursively


def replay_flags(rec, steps, seat=-1, legacy_suppress=False, extra=None, session_mode=-1):
    """The harness knobs of one replay run (REPLAY_FLAGS + the per-recording ones)."""
    flags = dict(REPLAY_FLAGS)
    if legacy_suppress:
        flags["replay_suppress_enqueue"] = 1
        flags["replay_dispatch_inject"] = 0
        flags["replay_drop_net_issue"] = 0
    if session_mode is not None and session_mode >= 0:
        flags["replay_session_mode"] = session_mode
    flags["pin_fpu"] = rec.get("arm", {}).get("pin_fpu", "1")
    # TL-HARN-INCHASH: the replay hashes with the kind of the stream it will be judged against, so an
    # rc4/rc5 recording (kind 1) is replayed in kind 1 whatever the harness default is.
    flags["hash_kind"] = rec.get("hash_kind") or 1
    flags["stop_step"] = steps
    if seat is not None and seat >= 0:
        flags["replay_seat"] = seat
    every, nmax = snap_cadence([k for k in snap_steps(rec.get("snap_dir")) if k <= steps])
    if every:
        flags["verdict_snap_every"] = every
        flags["verdict_snap_max"] = nmax
    for kv in extra or ():
        k, _, v = kv.partition("=")
        flags[k.strip()] = v.strip()
    return flags


def run_replay(
    rec,
    config,
    steps,
    timeout,
    seat=-1,
    legacy_suppress=False,
    extra=None,
    net_extra="",
    map_file=None,
    session_mode=-1,
    extra_ini=(),
):
    import make_lane

    lane = provision_lane(config)
    for k, dst in LANE_NAME.items():
        shutil.copyfile(rec["files"][k], os.path.join(lane, dst))
    if map_file:
        lane_add_map(lane, map_file)
    row, label = map_row(os.path.join(lane, "Maps"), rec.get("map"))
    if row is None:
        raise Refusal(
            "map %r is not in this lane's Maps\\ -- cannot load the recorded map (a downloaded "
            "map: pass it with --map-file)" % rec["map"]
        )
    tmpd = os.path.join(REPO, "tmp", "replay_match")
    os.makedirs(tmpd, exist_ok=True)
    script = os.path.join(tmpd, "replay_walk.txt")
    players = None if rec.get("legacy_roster") else seed_players(rec["files"]["seed"])
    plan = roster_of(players) if players else None
    if plan:
        print(
            "  roster (from the seed): "
            + ", ".join("%d=%s %r" % (p["slot"], p["kind"], p["name"]) for p in plan)
        )
        note = roster_note(plan, _session_json(rec.get("snap_dir") or rec["dir"]))
        if note:
            print("  note: " + note)
    else:
        print(
            "  roster: legacy walk (one computer in slot 1): --legacy-roster or an unreadable seed"
        )
    with open(script, "w", encoding="ascii", newline="\n") as fh:
        fh.write(walk_script(row, label, plan))
    flags = replay_flags(rec, steps, seat, legacy_suppress, extra, session_mode)
    ident = make_lane.read_identity(lane)
    argv = [
        sys.executable,
        os.path.join(REPO, "tools", "ui_test.py"),
        script,
        "--harness",
        "--steps",
        str(steps),
        "--harness-extra",
        ";".join("%s=%s" % kv for kv in flags.items()),
        "--host-dir",
        lane,
        "--port",
        str(6300 + int(ident.get("lane") or 0)),
        "--timeout",
        str(timeout),
        "--timeout-frames",
        str(max(400000, steps * 4)),
        "--stall-timeout",
        "120",
        "--headless",
    ]
    if net_extra:
        argv += ["--net-extra", net_extra]
    for frag in extra_ini:
        argv += ["--extra-ini", frag]
    before = (
        set(
            _n
            for _n in os.listdir(os.path.join(lane, "logs"))
            if os.path.isdir(os.path.join(os.path.join(lane, "logs"), _n))
        )
        if os.path.isdir(os.path.join(lane, "logs"))
        else set()
    )
    print(
        "  replay lane %s (configuration (%d)), map %r row %d, %d steps"
        % (lane, config, rec.get("map"), row, steps)
    )
    print("  $ " + " ".join(argv[1:]))
    subprocess.run(argv, cwd=REPO)
    after = set(
        _n
        for _n in os.listdir(os.path.join(lane, "logs"))
        if os.path.isdir(os.path.join(os.path.join(lane, "logs"), _n))
    )
    new = sorted(
        d for d in after - before if os.path.isfile(os.path.join(lane, "logs", d, "mh_harness.log"))
    )
    if not new:
        raise Refusal("the replay left no run folder carrying an mh_harness.log")
    return os.path.join(lane, "logs", new[-1])


def _session_of_run(run):
    """The replay's session folder (its snapshots land wherever MH_RunDir pointed at the time)."""
    logs = os.path.dirname(run)
    for leaf, _ in sessions_of_process(logs, os.path.basename(run)):
        return os.path.join(logs, leaf)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", nargs="?", help="a match (session) folder, or a process folder")
    ap.add_argument("--config", default="auto", choices=("auto", "1", "2"))
    ap.add_argument("--steps", type=int, default=0, help="replay only the first N match steps")
    ap.add_argument("--compare", help="compare against an existing replay run folder; do not run")
    ap.add_argument("--min-steps", type=int, default=50)
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument(
        "--seat",
        default="auto",
        help="the local player slot (PlayerSide) the replay plays as: auto = the recording's "
        "session.json `slot`, -1 = leave the lobby walk's seat (slot 0), N = slot N",
    )
    ap.add_argument(
        "--session-mode",
        default="auto",
        help="the SESSION_MODE the sim body runs in (harness replay_session_mode): auto = 3 for a "
        "lockstep match (session.json lockstep_step_ms), else leave the replay's own; -1 = leave",
    )
    ap.add_argument(
        "--legacy-suppress",
        action="store_true",
        help="the pre-D37b contract (replay_suppress_enqueue=1, no dispatch-entry inject)",
    )
    ap.add_argument(
        "--extra", action="append", default=[], help="an extra [harness] key=value (repeatable)"
    )
    ap.add_argument("--net-extra", default="", help='extra [net] lines, "k=v;k=v" (ui_test)')
    ap.add_argument(
        "--extra-ini",
        action="append",
        default=[],
        help="an mh_net.ini fragment merged into the replay lane (ui_test --extra-ini; repeatable), "
        "e.g. tools/uiscripts/ini/dirty_probe.ini",
    )
    ap.add_argument(
        "--map-file", help="an .mpm the install does not ship, added to the lane's Maps"
    )
    ap.add_argument(
        "--against",
        help="judge the replay against ANOTHER recording's per-step stream (a folder resolve() "
        "accepts, e.g. the other peer's match folder) instead of its own",
    )
    ap.add_argument(
        "--legacy-roster",
        action="store_true",
        help="the pre-TL-REPLAY-ROSTER walk (one computer in slot 1 whatever the recording seated) "
        "-- the negative arm of the roster seating",
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.dir:
        ap.error("a recording folder is required")
    try:
        rec = resolve(a.dir)
    except Refusal as e:
        print("replay_match_segment: REFUSED -- %s" % e)
        return 2
    ref_steps, ref_regs = parse_stream(rec["ref"], rec["base"])
    ref_steps = {s: v for s, v in ref_steps.items() if s >= 1}
    n_clock = clock_steps(rec["files"]["clock"])
    last = min(max(ref_steps) if ref_steps else 0, n_clock)
    if a.steps:
        last = min(last, a.steps)
    config = (rec["config"] or 1) if a.config == "auto" else int(a.config)
    seat = (
        (rec.get("slot") if rec.get("slot") is not None else -1)
        if a.seat == "auto"
        else int(a.seat)
    )
    smode = (3 if rec.get("lockstep") else -1) if a.session_mode == "auto" else int(a.session_mode)
    print(
        "recording: %s (%s), base=%d, %d hashed step(s), %d clock step(s), %d order record(s), "
        "map=%r, recorded in configuration (%s)"
        % (
            rec["dir"],
            rec["kind"],
            rec["base"],
            len(ref_steps),
            n_clock,
            order_records(rec["files"]["orders"]),
            rec.get("map"),
            rec["config"] or "?",
        )
    )
    if last < 1:
        print("replay_match_segment: REFUSED -- no recorded step to replay")
        return 2
    # TL-HARN-INCHASH: the stream the replay is JUDGED against decides the kind it runs with -- the
    # recording's own, or --against's. Resolved before the run for that reason.
    other = None
    if a.against:
        try:
            other = resolve(a.against)
        except Refusal as e:
            print("replay_match_segment: REFUSED -- --against: %s" % e)
            return 2
    judged = other or rec
    import mp_analyze

    print(
        "  hash kind: %s, from %s's own log -- the replay runs with it"
        % (mp_analyze.kind_label(judged["hash_kind"]), "--against" if other else "the recording")
    )
    try:
        run = a.compare or run_replay(
            dict(rec, hash_kind=judged["hash_kind"], legacy_roster=a.legacy_roster),
            config,
            last,
            a.timeout,
            seat=seat,
            legacy_suppress=a.legacy_suppress,
            extra=a.extra,
            net_extra=a.net_extra,
            map_file=a.map_file,
            session_mode=smode,
            extra_ini=a.extra_ini,
        )
    except Refusal as e:
        print("replay_match_segment: REFUSED -- %s" % e)
        return 2
    rep_text = _read_text(os.path.join(run, "mh_harness.log"))
    bad = kind_refusal(judged["hash_kind"], rep_text, "--against" if other else "the recording")
    if bad:
        print("replay_match_segment: REFUSED -- %s" % bad)
        return 2
    rep_steps, rep_regs = parse_stream(rep_text)
    if other:
        ref_steps, ref_regs = parse_stream(other["ref"], other["base"])
        ref_steps = {s: v for s, v in ref_steps.items() if s >= 1}
        rec = dict(rec, snap_dir=other.get("snap_dir"))
        print("  judged AGAINST %s (%d hashed step(s))" % (other["dir"], len(ref_steps)))
    ref_steps = {s: v for s, v in ref_steps.items() if s <= last}
    names, excluded = excluded_columns()
    res = compare(ref_steps, ref_regs, rep_steps, rep_regs, names, excluded)
    ok, text = verdict_text(rec, res, min(a.min_steps, last))
    print("  replay run: %s (seat %s, body session mode %s)" % (run, seat, smode))
    print(
        "  %s (recorded steps 1..%d, replayed %d; hash %s)"
        % (text, last, len(rep_steps), mp_analyze.kind_label(judged["hash_kind"]))
    )
    # the recording's desync snapshots (a desynced lockstep match writes them) vs the replay's own
    # dumps at the same match steps: the region names a region-less (player) log cannot give
    rec_snaps, rep_snaps = snap_steps(rec.get("snap_dir")), snap_steps(run)
    rep_snaps.update(snap_steps(_session_of_run(run)))
    ex_names = {names[i] for i in excluded}
    for k in sorted(set(rec_snaps) & set(rep_snaps)):
        d = diff_snaps(rec_snaps[k], rep_snaps[k])
        judged = [r for r in d if r not in ex_names]
        print(
            "  snapshot step %d: %s (+%d state_excluded region(s) differ, not judged)"
            % (
                k,
                ("differs in " + ", ".join(judged)) if judged else "every judged region IDENTICAL",
                len(d) - len(judged),
            )
        )
        if judged:
            print(
                "    bytes: python tools/mp_desync_snap_diff.py %s %s"
                % (rec_snaps[k], rep_snaps[k])
            )
    # mp:D42: when BOTH sides carry a state recording (mh_net.ini `[desync] state_record=1` on the
    # recorder AND the replay lane), judge them byte-exactly too -- purely additional evidence next
    # to the hash verdict above; it never changes `ok` / the exit code.
    rec_state = match_state_path(rec.get("snap_dir"))
    rep_state = match_state_path(run) or match_state_path(_session_of_run(run))
    _, state_lines = judge_state_recording(rec_state, rep_state)
    for ln in state_lines:
        print(ln)
    return 0 if ok else 1


# ---- selftest -----------------------------------------------------------------------------------


def _line(step, clock, state, regs=None):
    s = "%d %s %s %s\n" % (step, clock, "C0FFEE00C0FFEE00", state)
    if regs is not None:
        s += "R %d %s\n" % (step, " ".join(regs))
    return s


def selftest():
    fails = []

    def check(label, cond):
        print("  %-66s %s" % (label, "ok" if cond else "FAIL"))
        if not cond:
            fails.append(label)

    names = ["a", "b", "c"]
    ok_regs = ["1" * 16, "2" * 16, "3" * 16]
    clk = lambda s: "%016X" % (0x3F80000000000000 + s)  # noqa: E731
    st = lambda s: "%016X" % (0xABC0000 + s)  # noqa: E731
    ref = "".join(_line(100 + s, clk(s), st(s), ok_regs) for s in range(1, 61))
    rep = "".join(_line(s, clk(s), st(s), ok_regs) for s in range(1, 61))
    r1, rr1 = parse_stream(ref, 100)
    r2, rr2 = parse_stream(rep)
    res = compare(r1, rr1, r2, rr2, names, {2})
    check("identical streams (rebased by step_base) -> IDENTICAL", verdict_text({}, res, 50)[0])
    bad = list(ok_regs)
    bad[1] = "F" * 16
    rep2 = "".join(
        _line(s, clk(s), st(s) if s < 40 else "DEADBEEF00000000", bad if s >= 40 else ok_regs)
        for s in range(1, 61)
    )
    r3, rr3 = parse_stream(rep2)
    res = compare(r1, rr1, r3, rr3, names, {2})
    check(
        "a state mismatch names the first step and the region",
        res["first"] == 40 and res["regions"] == ["b"] and not verdict_text({}, res, 50)[0],
    )
    ex = list(ok_regs)
    ex[2] = "E" * 16
    rep3 = "".join(_line(s, clk(s), st(s), ex) for s in range(1, 61))
    r4, rr4 = parse_stream(rep3)
    check(
        "an excluded-region-only difference is not a divergence",
        compare(r1, rr1, r4, rr4, names, {2})["first"] is None,
    )
    rep4 = "".join(_line(s, clk(s) if s != 7 else clk(99), st(s)) for s in range(1, 61))
    r5, rr5 = parse_stream(rep4)
    res = compare(r1, rr1, r5, rr5, names, {2})
    check("a clock mismatch is a divergence", res["first"] == 7 and res["what"] == "clock")
    short = "".join(_line(s, clk(s), st(s)) for s in range(1, 11))
    r6, rr6 = parse_stream(short)
    check(
        "too few common steps is REFUSED, not IDENTICAL",
        not verdict_text({}, compare(r1, rr1, r6, rr6), 50)[0],
    )

    # resolve(): a segment, an incomplete segment, and the two process-recording shapes
    tmp = tempfile.mkdtemp(prefix="replay_seg_selftest_")
    try:
        logs = os.path.join(tmp, "logs")
        proc = os.path.join(logs, "20260101T000000Z_menu_solo")
        s1 = os.path.join(logs, "20260101T000100Z_aaaa_0_solo")
        s2 = os.path.join(logs, "20260101T000200Z_bbbb_0_solo")
        for d in (proc, s1, s2):
            os.makedirs(d)

        def put(d, name, data=b""):
            with open(os.path.join(d, name), "wb") as fh:
                fh.write(data if isinstance(data, bytes) else data.encode("latin-1"))

        armed = (
            "; [harness] configuration (1): spine ABSENT, registry=mh.dll\n"
            "; ==== mh replay harness armed: seed_step=1 seed_mode=0 stop_step=0 fixed_step=0 "
            "pin_fpu=1 region_hash_step=0 order_mode=1 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        put(proc, "mh_harness.log", armed + ref)
        for n in ("mh_orders.bin", "mh_clock.bin", "mh_harness_seed.bin"):
            put(proc, n, b"\0" * 16)
        put(
            s1,
            "session.json",
            json.dumps(
                {"process_dir": os.path.basename(proc), "map": "x.mpm", "final_clock_ms": 5000}
            ),
        )
        put(
            s2,
            "session.json",
            json.dumps(
                {"process_dir": os.path.basename(proc), "map": "x.mpm", "final_clock_ms": 9000}
            ),
        )
        put(
            s2,
            "mh_match_harness.log",
            "; [match] segment OPEN %s at process step 101: match step k = process step 100+k (step_base=100).\n"
            % os.path.basename(s2)
            + ref,
        )
        for k in ("orders", "clock", "seed"):
            put(s2, SEG[k], b"\0" * 16)
        r = resolve(s2)
        check(
            "a segment resolves with its step_base, map and the process's configuration",
            r["kind"] == "segment"
            and r["base"] == 100
            and r["map"] == "x.mpm"
            and r["config"] == 1,
        )
        # SES8: a raw name sort puts `20260917T...` AFTER `2026-09-29T...` (`-` < `0`).
        mixed = os.path.join(tmp, "mixed")
        leaves = ("20260917T000005Z_dddddddd_0_solo", "2026-09-29T00-00-05Z_cccccccc_m_host")
        for leaf in reversed(leaves):
            os.makedirs(os.path.join(mixed, leaf))
            put(os.path.join(mixed, leaf), "session.json", json.dumps({"process_dir": "P"}))
        check(
            "sessions_of_process orders SES1 before SES8 by time, not by raw name",
            [n for n, _ in sessions_of_process(mixed, "P")] == list(leaves),
        )
        os.remove(os.path.join(s2, SEG["seed"]))
        try:
            resolve(s2)
            check("a segment without its seed is REFUSED", False)
        except Refusal:
            check("a segment without its seed is REFUSED", True)
        try:
            resolve(s1)
            check("a pre-SES7 match that is not the process's only played one is REFUSED", False)
        except Refusal as e:
            check(
                "a pre-SES7 match that is not the process's only played one is REFUSED",
                "exactly ONE" in str(e),
            )
        put(
            s2,
            "session.json",
            json.dumps(
                {"process_dir": os.path.basename(proc), "map": "x.mpm", "final_clock_ms": 0}
            ),
        )
        for n in SEG.values():
            p = os.path.join(s2, n)
            if os.path.isfile(p):
                os.remove(p)
        r = resolve(s1)
        check(
            "the process's ONLY played match replays from the process files",
            r["kind"] == "process" and r["base"] == 0 and r["session"] == os.path.basename(s1),
        )
        put(proc, "mh_harness.log", armed.replace("seed_mode=0", "seed_mode=2") + ref)
        try:
            resolve(s1)
            check("a process recording whose seed was never dumped is REFUSED", False)
        except Refusal:
            check("a process recording whose seed was never dumped is REFUSED", True)
        # the walk picks the right row
        maps = os.path.join(tmp, "Maps")
        os.makedirs(maps)
        for f in ("Blue Monday.mpm", "BlueMonday.mpm", "Cold War.mpm", "Last Question.mpm"):
            put(maps, f)
        row, label = map_row(maps, "last question.mpm")
        check(
            "map row: case-folded file order (Last Question = row 3 here)",
            row == 3 and label == "last question",
        )
        check(
            "the walk splits the row click and gates on the map label",
            "press 80 140" in walk_script(row, label)
            and "present last question" in walk_script(row, label),
        )
        check(
            "an unknown map is None (refused by the runner)",
            map_row(maps, "nowhere.mpm")[0] is None,
        )
        # TL-REPLAY-ROSTER: a fixture seed + session.json, 3 humans (slots 0-2) + a computer in slot 3
        # (the 936e0006 shape) -> the walk seats every slot as recorded.
        regs = json.load(open(os.path.join(REPO, "tools", "data", "hash_manifest.json")))["regions"]
        blob = bytearray(sum(r["size"] for r in regs))
        base = 0
        for r in regs:
            if r["name"] == "players":
                break
            base += r["size"]
        for slot, (flags, race, name) in enumerate(
            [(7, 1, "Fef"), (7, 1, "Rizzen"), (7, 1, "Carol"), (0x0B, 2, "Computer")]
        ):
            o = base + slot * PLAYER_DESC
            blob[o], blob[o + 6] = race, flags
            blob[o + 0x10 : o + 0x10 + len(name)] = name.encode()
        put(tmp, "fixture_seed.bin", bytes(blob))
        fx_sj = {"roster": "0:HFef,1:HRizzen,2:HCarol", "slot": 0}
        plan = roster_of(seed_players(os.path.join(tmp, "fixture_seed.bin")))
        check(
            "roster: 3 humans + a computer in slot 3 read off the seed's Players[]",
            [(p["slot"], p["kind"]) for p in plan]
            == [(0, "host"), (1, "human"), (2, "human"), (3, "ai")]
            and plan[3]["race"] == 2,
        )
        w = walk_script(0, None, plan)
        clicks = re.findall(r"^click 70 (\d+)$", w, re.M)
        check(
            "the walk seats slots 1, 2, 3 at their own rows, gated on occ 2, 3, 4",
            clicks == ["132", "149", "166"]
            and re.findall(r"^occ (\d+)$", w, re.M) == ["2", "3", "4"],
        )
        check(
            "...sets only the computer's race (slot 3 = Alien) and checks it",
            w.count("pokerace") == 1 and "pokerace 3 2\nrace 3 2\n" in w,
        )
        check(
            "...and session.json's human slots agree (no note); a disagreeing one is named",
            roster_note(plan, fx_sj) is None
            and "!=" in (roster_note(plan, {"roster": "0:HFef,5:HGhost"}) or ""),
        )
        check(
            "no readable seed / a lone host -> the legacy one-computer walk, unchanged",
            seed_players(os.path.join(tmp, "nowhere.bin")) is None
            and walk_script(0, None) == walk_script(0, None, roster_of([]))
            and walk_script(0, None, [dict(slot=0, kind="host", race=1, name="x")])
            == walk_script(0, None)
            and "click 70 132\npresent Computer\nenabled Start" in walk_script(0, None),
        )
        # mp:D37b: the replay contract. Every enqueue live + the dispatch-entry inject + the net-player
        # issue drop, NOT the LIB-REF suppression (which lost same-pass orders: the rc4 field
        # recordings left their own stream at 5857 / 6357); the seat and body session mode follow
        # the recording's session.json.
        f = replay_flags({"arm": {}}, 100, seat=1, session_mode=3)
        check(
            "D37b contract: suppress 0, dispatch inject 1, net issue drop 1, seat + body mode passed",
            f["replay_suppress_enqueue"] == 0
            and f["replay_dispatch_inject"] == 1
            and f["replay_drop_net_issue"] == 1
            and f["replay_seat"] == 1
            and f["replay_session_mode"] == 3
            and f["stop_step"] == 100,
        )
        f = replay_flags({"arm": {}}, 100, legacy_suppress=True, extra=["verdict_snap_from=90"])
        check(
            "--legacy-suppress restores the SES7b pair; no seat/mode knob when not asked; --extra wins",
            f["replay_suppress_enqueue"] == 1
            and f["replay_dispatch_inject"] == 0
            and f["replay_drop_net_issue"] == 0
            and "replay_seat" not in f
            and "replay_session_mode" not in f
            and f["verdict_snap_from"] == "90",
        )
        put(
            s2,
            "session.json",
            json.dumps(
                {
                    "process_dir": os.path.basename(proc),
                    "map": "x.mpm",
                    "final_clock_ms": 9000,
                    "slot": 1,
                    "lockstep_step_ms": 100,
                }
            ),
        )
        put(
            s2,
            SEG["log"],
            "; [match] segment OPEN %s at process step 1: (step_base=0).\n" % os.path.basename(s2)
            + ref,
        )
        for k in ("orders", "clock", "seed"):
            put(s2, SEG[k], b"\0" * 16)
        r = resolve(s2)
        check(
            "a lockstep segment carries its seat (slot) and lockstep flag",
            r["slot"] == 1 and r["lockstep"] is True,
        )
        # TL-HARN-INCHASH: the kind follows the recording. A pre-item segment (no token) is kind 1
        # and replays with hash_kind=1; a kind-2 segment states it on its OPEN line and replays in 2.
        check(
            "a pre-item segment is kind 1 and its replay is pinned to hash_kind=1",
            r["hash_kind"] == 1 and replay_flags(r, 10)["hash_kind"] == 1,
        )
        put(
            s2,
            SEG["log"],
            "; [match] segment OPEN %s at process step 1: (step_base=0). x hash_kind=2\n"
            % os.path.basename(s2)
            + ref,
        )
        r2 = resolve(s2)
        check(
            "a kind-2 segment (OPEN line token) replays with hash_kind=2",
            r2["hash_kind"] == 2 and replay_flags(r2, 10)["hash_kind"] == 2,
        )
        fp2 = "; HASH FINGERPRINT 0123456789ABCDEF split=ok input_epoch=1 build=x hash_kind=2\n"
        check(
            "a kind-1 recording judged against a kind-2 replay is REFUSED by name, not DIVERGED",
            "hash kind mismatch: the recording is kind 1 (FNV VERDICT walk), the replay is kind 2"
            in (kind_refusal(1, fp2 + rep) or ""),
        )
        check(
            "...and a replay of the recording's own kind is not refused",
            kind_refusal(2, fp2 + rep) is None and kind_refusal(1, rep) is None,
        )
        check(
            "a process recording's kind comes off its fingerprint line (the last one wins)",
            hash_kind_of(fp2 + ref) == 2
            and hash_kind_of(fp2 + fp2.replace(" hash_kind=2", "") + ref) == 1
            and hash_kind_of(ref) == 1,
        )
        # the lane gets a downloaded map WITHOUT the shared install being touched
        lane = os.path.join(tmp, "lane")
        os.makedirs(os.path.join(lane, "Maps"))
        extra_map = os.path.join(tmp, "nortus.mpm")
        put(tmp, "nortus.mpm", b"MAP")
        lane_add_map(lane, extra_map)
        check(
            "--map-file lands in the lane's own Maps",
            map_row(os.path.join(lane, "Maps"), "nortus.mpm")[0] == 0,
        )

        # mp:D42: match_state_path() discovery + judge_state_recording() byte-exact judge, over
        # synthetic mh_match_state.bin files built with state_record's own (public) load()/
        # first_diff() -- not a second copy of that format's logic.
        import state_record

        check(
            "match_state_path -> None for a folder with no state recording",
            match_state_path(logs) is None,
        )
        st_a, st_b = os.path.join(tmp, "state_a"), os.path.join(tmp, "state_b")
        os.makedirs(st_a)
        os.makedirs(st_b)
        sr_defs = [("x", 4), ("y", 4)]
        sr_data, _ = state_record._build_match(
            sr_defs, {1: b"\x01\x02\x03\x04\x05\x06\x07\x08"}, keyframe_every=1
        )
        with open(os.path.join(st_a, "mh_match_state.bin"), "wb") as fh:
            fh.write(sr_data)
        with open(os.path.join(st_b, "mh_match_state.bin"), "wb") as fh:
            fh.write(sr_data)  # byte-identical copy
        check(
            "match_state_path finds mh_match_state.bin in a match folder",
            match_state_path(st_a) == os.path.join(st_a, "mh_match_state.bin"),
        )
        ok_j, lines_j = judge_state_recording(match_state_path(st_a), match_state_path(st_b))
        check(
            "judge_state_recording: byte-identical files -> IDENTICAL",
            ok_j is True and any("IDENTICAL" in ln for ln in lines_j),
        )
        sr_data_poked, _ = state_record._build_match(
            sr_defs,
            {1: b"\x01\x02\x03\x04\x05\x06\x07\xff"},
            keyframe_every=1,  # last byte (y+3) poked
        )
        with open(os.path.join(st_b, "mh_match_state.bin"), "wb") as fh:
            fh.write(sr_data_poked)
        ok_j2, lines_j2 = judge_state_recording(match_state_path(st_a), match_state_path(st_b))
        joined2 = "\n".join(lines_j2)
        check(
            "judge_state_recording: a poked byte is named (region y, offset 0x3), ok=False",
            ok_j2 is False and "FIRST DIFF" in joined2 and "offset 0x3 len 1" in joined2,
        )
        check(
            "judge_state_recording: a missing side -> not judged (None), no exception",
            judge_state_recording(None, match_state_path(st_b))[0] is None
            and judge_state_recording(match_state_path(st_a), None)[0] is None,
        )

        # mp:D46: the compressed form mh.dll leaves after a match. A side holding only
        # mh_match_state.bin.gz is found, judged against a raw side, and a .gz.tmp is invisible.
        import gzip

        st_c = os.path.join(tmp, "state_c")
        os.makedirs(st_c)
        with open(os.path.join(st_c, "mh_match_state.bin.gz"), "wb") as fh:
            fh.write(gzip.compress(sr_data, 6))
        with open(os.path.join(st_c, "mh_match_state.bin.gz.tmp"), "wb") as fh:
            fh.write(b"unfinished")
        check(
            "match_state_path finds a lone mh_match_state.bin.gz (and ignores .gz.tmp)",
            match_state_path(st_c) == os.path.join(st_c, "mh_match_state.bin.gz"),
        )
        with open(os.path.join(st_b, "mh_match_state.bin"), "wb") as fh:
            fh.write(sr_data)  # st_b is byte-identical to st_a again
        ok_j3, lines_j3 = judge_state_recording(match_state_path(st_c), match_state_path(st_b))
        check(
            "judge_state_recording: a gzip side vs a raw side, same bytes -> IDENTICAL",
            ok_j3 is True and any("IDENTICAL" in ln for ln in lines_j3),
        )
        ok_j4, lines_j4 = judge_state_recording(match_state_path(st_c), match_state_path(st_a))
        check(
            "judge_state_recording: gzip vs gzip-free raw is symmetric",
            ok_j4 is True,
        )
        with open(os.path.join(st_c, "mh_match_state.bin"), "wb") as fh:
            fh.write(sr_data)  # both forms present: a kill after the rename, before the delete
        check(
            "match_state_path: both forms present -> the raw file",
            match_state_path(st_c) == os.path.join(st_c, "mh_match_state.bin"),
        )
        st_d = os.path.join(tmp, "state_d")
        os.makedirs(st_d)
        for nm in ("mh_match_state_2.bin.gz", "mh_match_state_3.bin"):
            with open(os.path.join(st_d, nm), "wb") as fh:
                fh.write(sr_data)
        check(
            "match_state_path: the lexically first stem wins across both forms (_2.bin.gz before _3.bin)",
            match_state_path(st_d) == os.path.join(st_d, "mh_match_state_2.bin.gz"),
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("replay_match_segment selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:] or "--compare" in sys.argv[1:]:
        raise SystemExit(main())
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "replay_match_segment"))
