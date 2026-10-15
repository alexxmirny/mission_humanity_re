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

A FIELD REPORT (mh.dll alone, no harness) carries mh.dll's own recording instead: mh_rec_orders.bin.gz +
mh_rec_clock.bin.gz (desync/order_capture.h), mh_rec_seed.bin.gz (the step-1 seed, the harness's blob
format) and mh_rec_hash.txt (the `state` hash every N match steps, desync/replay_record.h). Those are
unpacked to tmp/replay_match/shipped/<leaf>/ and replayed the same way; the verdict is judged at the
recorded checkpoints, and a divergence names the first bad checkpoint and the last good one. A complete
harness segment wins over it; `--shipped` forces the shipped recording.

SEGMENTS (hotfix rc9.4). A session that loaded a save, crossed a campaign planet transition, returned from a
tactical mission or started a new campaign has one seed per state replacement: mh_rec_seed_<n>.bin.gz, and
mh_rec_segments.txt names each segment's first step on the session axis (desync/replay_record.h).
`--shipped --segment N` replays segment N alone: its seed, the orders/clock streams cut at its first step
and renumbered from 1, judged at the recorded checkpoints inside it (the segment's first step carries
one). The SCENE comes from a save, not the lobby: the map, the terrain and every unhashed table are
whatever the game loads, and the seed then overwrites the hashed state. A save_load segment loads the
very save it recorded (`--save NAME|PATH`, else the recorded name from the committed saves / the
polygon), and its content hash must equal the recorded one; any other kind needs `--carrier-save` (a save
on the same planet), and the verdict then also tests whether the unhashed state matters.

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
    python tools/replay_match_segment.py <session dir> --shipped --segment N [--save NAME|PATH]
        [--carrier-save NAME|PATH]                                               # rc9.4 segment N
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
# The SHIPPED recording (mh.dll alone, no harness -- what a field report carries): the order recording
# (desync/order_capture.h, gzipped at the session close), the step-1 seed and the hash checkpoints
# (desync/replay_record.h). Each .bin may be raw or .gz.
SHIP = {
    "orders": "mh_rec_orders.bin",
    "clock": "mh_rec_clock.bin",
    "seed": "mh_rec_seed.bin",
    "hash": "mh_rec_hash.txt",
    "segments": "mh_rec_segments.txt",
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


HASH_HDR_RE = re.compile(r"every (\d+) steps.*?hash_kind=(\d).*?configuration \((\d)\)")


def parse_hash_file(text):
    """mh_rec_hash.txt (desync/replay_record.h) -> (meta, {match_step: (None, None, STATE)}).

    The checkpoints carry only the state hash: the clock slot is None and compare() skips it."""
    meta, steps = {}, {}
    for ln in text.splitlines():
        if ln.startswith(";"):
            m = HASH_HDR_RE.search(ln)
            if m:
                meta = dict(every=int(m.group(1)), kind=int(m.group(2)), config=int(m.group(3)))
            continue
        p = ln.split()
        if len(p) == 2 and p[0].isdigit() and re.fullmatch(r"[0-9A-Fa-f]{16}", p[1]):
            steps[int(p[0])] = (None, None, p[1].upper())
    return meta, steps


SEG_LINE_RE = re.compile(r"^segment (\d+) (.*)$")
SEG_KV_RE = re.compile(r'(\w+)=("[^"]*"|\S+)')


def parse_segments(text):
    """mh_rec_segments.txt (desync/replay_record.h) -> ({n: dict(step, kind, ...)}, [sanity lines]).

    Values stay strings except `step` / `prev_step` (int); quoted values are unquoted."""
    segs, sanity = {}, []
    for ln in text.splitlines():
        if ln.startswith("sanity "):
            sanity.append(ln)
            continue
        m = SEG_LINE_RE.match(ln)
        if not m:
            continue
        d = {}
        for k, v in SEG_KV_RE.findall(m.group(2)):
            d[k] = v[1:-1] if v.startswith('"') else v
        for k in ("step", "prev_step"):
            if k in d and d[k].isdigit():
                d[k] = int(d[k])
        segs[int(m.group(1))] = d
    return segs, sanity


def fnv1a64_file(path):
    """The FNV-1a 64 of a file, as replay_record.cpp's file_fnv64 computes it (16 hex, upper)."""
    h = 0xCBF29CE484222325
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 16)
            if not chunk:
                break
            for c in chunk:
                h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016X" % h


def cut_streams(orders_src, clock_src, start, outdir):
    """Cut the order + clock streams at session step `start` into `outdir` (SEG names), renumbering the
    order records so recorded step `start` is replay step 1. -> (orders, clock, kept records)."""
    import struct

    os.makedirs(outdir, exist_ok=True)
    off = start - 1
    clock = os.path.join(outdir, SEG["clock"])
    with open(clock_src, "rb") as fi, open(clock, "wb") as fo:
        fi.seek(off * 8)
        shutil.copyfileobj(fi, fo, 1 << 20)
    orders = os.path.join(outdir, SEG["orders"])
    kept = 0
    with open(orders_src, "rb") as fi, open(orders, "wb") as fo:
        fo.write(fi.read(ORDERS_MAGIC_LEN))
        while True:
            chunk = fi.read(ORDER_REC * 4096)
            if not chunk:
                break
            for i in range(0, len(chunk), ORDER_REC):
                (stp,) = struct.unpack_from("<I", chunk, i)
                if stp >= start:
                    fo.write(struct.pack("<I", stp - off) + chunk[i + 4 : i + ORDER_REC])
                    kept += 1
    return orders, clock, kept


def slice_segment(rec, n, outdir):
    """rc9.4: re-point a shipped recording at its segment `n`. -> the new rec dict: seed = segment n's
    seed (unpacked), orders/clock cut at its first step, the checkpoints of [start, next start) renumbered
    so the segment's first step is 1, `segment` = its parsed line. Raises Refusal by name."""
    import gzip

    segs = rec.get("segments") or {}
    if n not in segs:
        raise Refusal(
            "no segment %d in %s (segments: %s)"
            % (
                n,
                SHIP["segments"],
                ", ".join(str(k) for k in sorted(segs)) or "none -- a pre-rc9.4 recording",
            )
        )
    seg = segs[n]
    start = seg.get("step")
    if not isinstance(start, int) or start < 1:
        raise Refusal("segment %d has no first step" % n)
    leaf = seg.get("seed", "")
    src = _ship_file(rec["dir"], leaf) if leaf.endswith(".bin") else None
    if not src:
        raise Refusal("segment %d has no seed file (seed=%s)" % (n, leaf or "?"))
    os.makedirs(outdir, exist_ok=True)
    seed = os.path.join(outdir, SEG["seed"])
    if src.endswith(".gz"):
        with gzip.open(src, "rb") as fi, open(seed, "wb") as fo:
            shutil.copyfileobj(fi, fo)
    else:
        shutil.copyfile(src, seed)
    orders, clock, kept = cut_streams(rec["files"]["orders"], rec["files"]["clock"], start, outdir)
    nxt = min(
        [s["step"] for k, s in segs.items() if k > n and isinstance(s.get("step"), int)] or [0]
    )
    ref = {
        s - (start - 1): v
        for s, v in rec["ref_steps"].items()
        if s >= start and (not nxt or s < nxt)
    }
    print(
        "  segment %d: %s at session step %d%s, seed %s; %d order record(s) kept, %d checkpoint(s) in it"
        % (
            n,
            seg.get("kind"),
            start,
            (" (next segment at %d)" % nxt) if nxt else " (the last)",
            leaf,
            kept,
            len(ref),
        )
    )
    return dict(
        rec,
        files=dict(rec["files"], seed=seed, orders=orders, clock=clock),
        base=start - 1,
        ref_steps=ref,
        segment=seg,
        segment_n=n,
        segment_len=(nxt - start) if nxt else 0,
    )


def find_save(spec, recorded_name=None):
    """A save to load in the replay lane: an existing PATH, else a NAME (no extension) from the committed
    scenario saves or the polygon's save dir (ui_test.resolve_save). -> path or None."""
    if spec and os.path.isfile(spec):
        return spec
    name = spec or recorded_name
    if not name:
        return None
    import machine_config as machine
    import ui_test

    p = ui_test.resolve_save(name, os.path.join(machine.POLYGON, "save"))
    return p if os.path.isfile(p) else None


def segment_scene(rec, save_spec=None, carrier_spec=None, force=False):
    """rc9.4: the save the replay lane loads for a segment replay. -> (path, note). A save_load segment
    replays on the save it recorded, and the content hash must match; any other kind needs a carrier."""
    seg = rec["segment"]
    kind = seg.get("kind")
    if kind == "save_load" and not carrier_spec:
        want = seg.get("file_fnv")
        path = find_save(save_spec, seg.get("source"))
        if not path:
            raise Refusal(
                "segment %s loaded save %r (%s bytes, FNV %s): pass it with --save NAME|PATH"
                % (rec["segment_n"], seg.get("source"), seg.get("file_bytes"), want)
            )
        got = fnv1a64_file(path)
        if want and got != want.upper() and not force:
            raise Refusal(
                "save %s has content hash %s, the recording loaded %s -- not the same file (--force-save "
                "replays it anyway)" % (path, got, want)
            )
        return path, "the recorded save (content hash %s %s)" % (
            got,
            "matches" if got == (want or "").upper() else "FORCED",
        )
    if not carrier_spec:
        raise Refusal(
            "segment %s is a %s boundary: there is no save that IS its scene -- pass --carrier-save NAME|PATH "
            "(a save on planet %s; the seed overwrites the hashed state, the rest is the carrier's)"
            % (rec["segment_n"], kind, seg.get("planet_after"))
        )
    path = find_save(carrier_spec)
    if not path:
        raise Refusal("--carrier-save %s: no such save" % carrier_spec)
    return path, "CARRIER %s (the unhashed state is the carrier's, not the recording's)" % path


def load_walk_script():
    """rc9.4: the segment replay's walk -- main menu -> Load game (the deployed save is the newest, i.e.
    the preselected row) -> in game; the harness injects the segment seed at its step 1."""
    return (
        "# GENERATED by tools/replay_match_segment.py (rc9.4 --segment): load the scene save, then the\n"
        "# harness injects the segment seed at step 1 and replays the cut orders/clock.\n"
        "settled value:108\nclickv 108\npresent Load game\nsettled Load game\nclickl Load game\n"
        "absent Load game\ngamemode 2\n"
        "log REPLAY in-game (segment), the harness owns the run from here\nend\n"
    )


def _ship_file(path, name):
    """The shipped recording `name` in `path`: the raw file, else its .gz, else None."""
    for cand in (name, name + ".gz"):
        f = os.path.join(path, cand)
        if os.path.isfile(f):
            return f
    return None


def stage_shipped(path, outdir):
    """Unpack a shipped recording's three replay inputs into `outdir` under the segment names.
    -> {orders, clock, seed} paths. Raises Refusal naming what is missing."""
    import gzip

    found = {k: _ship_file(path, SHIP[k]) for k in ("orders", "clock", "seed")}
    missing = [SHIP[k] + "[.gz]" for k, v in found.items() if not v]
    if missing:
        raise Refusal("shipped recording incomplete, missing %s" % ", ".join(missing))
    os.makedirs(outdir, exist_ok=True)
    out = {}
    for k, src in found.items():
        dst = os.path.join(outdir, SEG[k])
        if src.endswith(".gz"):
            with gzip.open(src, "rb") as fi, open(dst, "wb") as fo:
                shutil.copyfileobj(fi, fo)
        else:
            shutil.copyfile(src, dst)
        out[k] = dst
    return out


def resolve_shipped(path, sj=None):
    """A session folder holding mh.dll's own recording (mh_rec_*) -> a resolve() dict whose reference
    stream is the hash checkpoints (mh_rec_hash.txt), not a harness log."""
    sj = sj if sj is not None else _session_json(path)
    hp = _ship_file(path, SHIP["hash"])
    meta, steps = parse_hash_file(_read_text(hp)) if hp else ({}, {})
    sp = _ship_file(path, SHIP["segments"])
    segs, sanity = parse_segments(_read_text(sp)) if sp else ({}, [])
    files = stage_shipped(
        path, os.path.join(REPO, "tmp", "replay_match", "shipped", os.path.basename(path))
    )
    return dict(
        kind="shipped",
        dir=path,
        snap_dir=path,
        files=files,
        base=0,
        map=(sj or {}).get("map"),
        slot=(sj or {}).get("slot"),
        lockstep=bool((sj or {}).get("lockstep_step_ms")),
        role=(sj or {}).get("role"),
        config=meta.get("config") or None,
        arm={},
        ref="",
        ref_steps=steps,
        hash_every=meta.get("every"),
        hash_kind=meta.get("kind") or 1,
        segments=segs,
        sanity=sanity,
    )


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


def resolve(path, shipped=False):
    """The recording in `path` -> dict(kind, files{orders,clock,seed,log}, base, map, config, ...).

    Precedence: a complete harness segment, then mh.dll's shipped recording (mh_rec_*; `shipped=True`
    forces it), then a process recording. Raises Refusal with the reason when nothing replays."""
    path = os.path.abspath(path.rstrip("\\/"))
    if not os.path.isdir(path):
        raise Refusal("no such folder: %s" % path)
    logs_dir = os.path.dirname(path)
    sj = _session_json(path)
    has_ship = all(_ship_file(path, SHIP[k]) for k in ("orders", "clock", "seed"))
    if shipped:
        return resolve_shipped(path, sj)  # stage_shipped names what is missing
    if os.path.isfile(os.path.join(path, SEG["log"])) or os.path.isfile(
        os.path.join(path, SEG["orders"])
    ):
        files = {k: os.path.join(path, v) for k, v in SEG.items()}
        missing = [
            SEG[k] for k in ("orders", "clock", "seed", "log") if not os.path.isfile(files[k])
        ]
        if missing and has_ship:
            return resolve_shipped(path, sj)
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
            role=(sj or {}).get("role"),
            config=config_of(arm),
            arm=arm_flags(arm),
            ref=log,
            # TL-HARN-INCHASH: the segment's OPEN line states the kind itself (no token = kind 1);
            # the process log's fingerprint line is the fallback.
            hash_kind=hash_kind_of(log, arm),
        )
    if has_ship:
        return resolve_shipped(path, sj)
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
        role=played[0][1].get("role"),
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
    first, what, last_ok = None, None, None
    for s in common:
        a, b = ref_steps[s], rep_steps[s]
        # a shipped recording's checkpoints (mh_rec_hash.txt) carry no clock: None is not compared
        clock_bad = a[0] is not None and b[0] is not None and a[0] != b[0]
        if a[2] != b[2] or clock_bad:
            first = s
            what = "state" if a[2] != b[2] else "clock"
            break
        last_ok = s
    regions = []
    if first is not None and first in ref_regs and first in rep_regs:
        for i, (x, y) in enumerate(zip(ref_regs[first], rep_regs[first])):
            if x != y and i not in excluded:
                regions.append(names[i] if names and i < len(names) else "col%d" % i)
    return dict(common=len(common), first=first, what=what, regions=regions, last_ok=last_ok)


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
    ship = (rec or {}).get("kind") == "shipped"
    if res["first"] is None:
        if ship:
            return True, (
                "IDENTICAL: state agrees at all %d recorded hash checkpoint(s) (mh_rec_hash.txt)"
                % res["common"]
            )
        return True, "IDENTICAL: state + clock agree at all %d common step(s)" % res["common"]
    if ship:
        return False, (
            "DIVERGED at hash checkpoint match step %d (state); the last agreeing checkpoint is %s "
            "-- the divergence is in steps %s..%d (a field recording has no per-region hashes)"
            % (
                res["first"],
                res["last_ok"] if res["last_ok"] is not None else "none",
                (res["last_ok"] + 1) if res["last_ok"] is not None else 1,
                res["first"],
            )
        )
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
                team=rec[
                    7
                ],  # the team byte (+7); no lobby hook seats it yet -- the seed carries it
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
        plan.append(
            dict(slot=p["slot"], kind=kind, race=p["race"], name=p["name"], team=p.get("team"))
        )
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


def auto_session_mode(rec):
    """tooling:TL-REPLAY-BM: the SESSION_MODE the recorded sim BODY ran in, from session.json.
    role=solo (one process, no peer) ran mode 2 even though the config carries lockstep_step_ms (it is
    a config value, not a statement about the session); a hosted/joined match ran mode 3. A recording
    with neither signal leaves the replay's own (-1)."""
    if rec.get("role") == "solo":
        return 2
    return 3 if rec.get("lockstep") else -1


def host_sync_keys(schema_path=None):
    """PT-SET1: [(key, default_text)] of the host-synced gameplay rows (settings_schema.json `host: true`)."""
    path = schema_path or os.path.join(REPO, "src", "launcher", "settings_schema.json")
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    out = []
    for r in doc.get("settings", doc.get("rows", [])):
        if r.get("host"):
            d = r.get("default")
            out.append((r["key"], repr(d) if isinstance(d, float) else str(int(d))))
    return out


def gameplay_net_extra(sj, user_extra="", keys=None):
    """PT-SET1: the lane `--net-extra` that replays the recording with the gameplay settings it ran with.

    session.json `gameplay` is "key=value;key=value" (ints %d, floats %.17g) -- exactly the net_extra
    shape. Returns (net_extra, warning). A recording without the field (or an empty one: it predates the
    rows) replays with the registry defaults, and the warning says so. Keys the registry no longer
    knows are dropped with a note; keys the caller's own --net-extra already sets win."""
    keys = host_sync_keys() if keys is None else keys
    known = dict(keys)
    taken = {kv.split("=", 1)[0].strip() for kv in user_extra.split(";") if "=" in kv}
    raw = (sj or {}).get("gameplay")
    notes = []
    if isinstance(raw, str) and raw.strip():
        vals = {}
        for kv in raw.split(";"):
            if "=" not in kv:
                continue
            k, v = (x.strip() for x in kv.split("=", 1))
            if k in known:
                vals[k] = v
            else:
                notes.append("recorded key %r is not a gameplay row any more -- dropped" % k)
        src = "recording (%s)" % ((sj or {}).get("gameplay_src") or "?")
        for (
            k,
            d,
        ) in keys:  # a key the recording lacks gets its registry default, not the lane's whatever
            vals.setdefault(k, d)
    else:
        vals = dict(keys)
        src = "registry defaults"
        notes.append(
            "session.json has no `gameplay` field (a recording from before PT-SET1) -- replaying with "
            "the registry defaults; a desync may be the settings, not the sim"
        )
    pairs = ["%s=%s" % (k, vals[k]) for k, _ in keys if k not in taken]
    return ";".join(pairs), "; ".join(notes) if notes else None, src


def hqueue_counts(sr, first, last):
    """tooling:TL-REPLAY-BM: the recording's order_queue_count AT ITS HASH POINT for match steps
    first..last -> list of u32 (0xFFFFFFFF = the state recording does not cover that step).

    WHY. The order stream (MHOR) holds the queue at dispatch ENTRY, S(N). The hash point sits earlier
    in the same sim step, and S(N) = [what was queued by then: released orders + a human's direct
    enqueue (immediate lane, exec_time 0)] + [the AI tick's tail T, enqueued between hash and
    dispatch]. The replay's own AI regenerates T but nothing regenerates the human's click, and no
    field of the record says which is which. The state recording's order_queue_count at step N IS the
    boundary: the first that-many records of S(N) are the hash-point queue. The harness (mh_hqueue.bin,
    order_replay_inject_prefix) injects exactly that prefix."""
    import struct

    ci = sr.region_index["order_queue_count"]
    base = sr.region_offset[ci]
    rlen = sr.regions[ci].len
    out = {}
    cur = None  # bytearray(4): the count region's bytes as of the chunk being walked
    for c in sr.chunks:
        kind = type(c).__name__
        if kind == "KeyframeChunk":
            cur = bytearray(c.state[base : base + rlen])
            out[c.step] = struct.unpack_from("<I", cur, 0)[0]
        elif kind == "StepChunk":
            for ridx, off, ln, data in c.runs:
                if ridx == ci and cur is not None:
                    cur[off : off + ln] = data
            if cur is not None:
                out[c.step] = struct.unpack_from("<I", cur, 0)[0]
    return [out.get(k, 0xFFFFFFFF) for k in range(first, last + 1)]


def write_hqueue(rec, dst, first=1, last=0):
    """Write mh_hqueue.bin for match steps first..last (replay step k = recorded step first+k-1) from
    the recording's state file. -> number of steps covered, or 0 (no state recording: the harness
    falls back to its clock-window split, unchanged)."""
    import struct

    import state_record

    st_path = match_state_path(rec.get("snap_dir"))
    if not st_path:
        return 0
    sr = state_record.load(st_path)
    counts = hqueue_counts(sr, first, last)
    with open(dst, "wb") as fh:
        fh.write(struct.pack("<%dI" % len(counts), *counts))
    return sum(1 for v in counts if v != 0xFFFFFFFF)


def slice_from_step(rec, step, outdir):
    """tooling:PT-FIX1 (--from-keyframe): stage the replay inputs for a replay that STARTS at match step
    `step` instead of step 1, into `outdir`. -> dict(files=<rec files with orders/clock/seed swapped>, base).

    THE SEED. mh_match_state.bin holds the same region set, in the same manifest order, as the step-1
    seed blob (mh_match_seed.bin -- verified here by size), sampled at the same point (the step's
    PRE-BODY boundary). state_at(step) laid out flat IS therefore a seed for `step`: the harness injects
    it at replay step 1, so replay step k runs recorded step `step`+k-1. A keyframe step costs no delta
    walk; any other step is rebuilt from the previous keyframe.
    THE ORDERS are the per-step due-queue snapshots (MHOR v1: u32 step + order[0x44]); records of steps
    >= `step` are kept and renumbered by -(step-1). THE CLOCK is one double per step: dropped (step-1).
    `base` = the recording's step_base + (step-1), so parse_stream() puts recorded step `step` at 1.

    NOT IN THE SEED: whatever the hash manifest does not cover (the AI's private state, per-module
    statics). The replay's verdict says whether that matters -- identical hashes from replay step 1."""
    import struct

    import state_record

    st_path = match_state_path(rec.get("snap_dir"))
    if not st_path:
        raise Refusal(
            "--from-keyframe needs the recording's mh_match_state.bin[.gz] (state_record=1)"
        )
    sr = state_record.load(st_path)
    if sr.stop_reason:
        print("  note: state recording stops at step %s (%s)" % (sr.last_step, sr.stop_reason))
    seed_len = os.path.getsize(rec["files"]["seed"])
    if sr.total_len != seed_len:
        raise Refusal(
            "the state recording's region set (%d bytes) is not the seed's (%d bytes) -- cannot "
            "build a seed from it" % (sr.total_len, seed_len)
        )
    try:
        st = state_record.state_at(sr, step)
    except ValueError as e:
        raise Refusal(str(e))
    os.makedirs(outdir, exist_ok=True)
    seed = os.path.join(outdir, SEG["seed"])
    with open(seed, "wb") as fh:
        for r in sr.regions:
            fh.write(st[r.name])
    off = step - 1
    clock = os.path.join(outdir, SEG["clock"])
    with open(rec["files"]["clock"], "rb") as fi, open(clock, "wb") as fo:
        fi.seek(off * 8)
        shutil.copyfileobj(fi, fo, 1 << 20)
    orders = os.path.join(outdir, SEG["orders"])
    kept = 0
    with open(rec["files"]["orders"], "rb") as fi, open(orders, "wb") as fo:
        fo.write(fi.read(ORDERS_MAGIC_LEN))
        while True:
            chunk = fi.read(ORDER_REC * 4096)
            if not chunk:
                break
            for i in range(0, len(chunk), ORDER_REC):
                (stp,) = struct.unpack_from("<I", chunk, i)
                if stp >= step:
                    fo.write(struct.pack("<I", stp - off) + chunk[i + 4 : i + ORDER_REC])
                    kept += 1
    print(
        "  from step %d: seed = state_at(%d) (%d bytes), clock -%d step(s), %d order record(s) kept"
        % (step, step, seed_len, off, kept)
    )
    files = dict(rec["files"], seed=seed, clock=clock, orders=orders)
    return dict(files=files, base=rec["base"] + off)


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
    hqueue=True,
):
    import make_lane

    lane = provision_lane(config)
    for k, dst in LANE_NAME.items():
        shutil.copyfile(rec["files"][k], os.path.join(lane, dst))
    hq_dst = os.path.join(lane, "mh_hqueue.bin")
    if os.path.isfile(hq_dst):
        os.remove(
            hq_dst
        )  # a lane is reused: never let a previous recording's counts steer this one
    if hqueue:
        n_hq = write_hqueue(
            rec, hq_dst, rec.get("first_step", 1), rec.get("first_step", 1) + steps - 1
        )
        print(
            "  hash-point queue counts: %s"
            % (
                "%d step(s) from the state recording" % n_hq
                if n_hq
                else "none (no state recording)"
            )
        )
    if map_file:
        lane_add_map(lane, map_file)
    scene = rec.get("scene_save")
    if scene:  # rc9.4 --segment: the scene is a save, not the lobby
        import time

        sd = os.path.join(lane, "save")
        os.makedirs(sd, exist_ok=True)
        dst = os.path.join(sd, os.path.basename(scene))
        shutil.copyfile(scene, dst)
        now = time.time() + 60  # the newest save = the Load dialog's preselected row
        os.utime(dst, (now, now))
        row, label = 0, None
    else:
        row, label = map_row(os.path.join(lane, "Maps"), rec.get("map"))
    if row is None:
        raise Refusal(
            "map %r is not in this lane's Maps\\ -- cannot load the recorded map (a downloaded "
            "map: pass it with --map-file)" % rec["map"]
        )
    tmpd = os.path.join(REPO, "tmp", "replay_match")
    os.makedirs(tmpd, exist_ok=True)
    script = os.path.join(tmpd, "replay_walk.txt")
    players = None if (rec.get("legacy_roster") or scene) else seed_players(rec["files"]["seed"])
    plan = roster_of(players) if players else None
    if scene:
        print("  scene: %s" % rec.get("scene_note"))
    elif plan:
        print(
            "  roster (from the seed): "
            + ", ".join(
                "%d=%s %r team %s" % (p["slot"], p["kind"], p["name"], p.get("team")) for p in plan
            )
        )
        note = roster_note(plan, _session_json(rec.get("snap_dir") or rec["dir"]))
        if note:
            print("  note: " + note)
    else:
        print(
            "  roster: legacy walk (one computer in slot 1): --legacy-roster or an unreadable seed"
        )
    with open(script, "w", encoding="ascii", newline="\n") as fh:
        fh.write(load_walk_script() if scene else walk_script(row, label, plan))
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
    gp_extra, gp_note, gp_src = gameplay_net_extra(
        _session_json(rec.get("snap_dir") or rec["dir"]), net_extra
    )
    print("  gameplay: %s from %s" % (gp_extra or "(none)", gp_src))
    if gp_note:
        print("  WARNING: " + gp_note)
    net_extra = ";".join(x for x in (net_extra, gp_extra) if x)
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
        help="the SESSION_MODE the sim body runs in (harness replay_session_mode): auto = 2 for a "
        "solo recording (session.json role=solo), 3 for a lockstep match (lockstep_step_ms), else "
        "leave the replay's own; -1 = leave",
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
    ap.add_argument(
        "--from-keyframe",
        type=int,
        default=0,
        metavar="N",
        help="start the replay AT match step N: the seed is the state recording's state at N "
        "(mh_match_state.bin), orders/clock are sliced + renumbered from N (replay step k = recorded "
        "step N+k-1). --steps then counts from N. Saves replaying a long match from step 1",
    )
    ap.add_argument(
        "--no-hqueue",
        action="store_true",
        help="do not hand the harness the recording's hash-point queue counts (the pre-"
        "TL-REPLAY-BM clock-window split of the dispatch snapshot) -- the negative arm",
    )
    ap.add_argument(
        "--shipped",
        action="store_true",
        help="replay mh.dll's own recording (mh_rec_orders/clock/seed + the mh_rec_hash.txt "
        "checkpoints -- what a field report carries) even when a harness segment is present",
    )
    ap.add_argument(
        "--segment",
        type=int,
        default=-1,
        metavar="N",
        help="rc9.4: replay segment N of a shipped recording (mh_rec_segments.txt): its seed, the "
        "orders/clock cut at its first step, judged at the checkpoints inside it. Implies --shipped",
    )
    ap.add_argument(
        "--save",
        help="the save a save_load segment loaded (NAME or PATH; default: the recorded name); its "
        "content hash must equal the recorded one",
    )
    ap.add_argument(
        "--carrier-save",
        help="a save that stands in for the scene of a non-save segment (planet transition, tactical "
        "return, new game) -- or of a save_load one whose save is lost",
    )
    ap.add_argument("--force-save", action="store_true", help="replay on a save whose hash differs")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.dir:
        ap.error("a recording folder is required")
    try:
        rec = resolve(a.dir, shipped=a.shipped or a.segment >= 0)
        if a.segment >= 0:
            rec = slice_segment(
                rec, a.segment, os.path.join(REPO, "tmp", "replay_match", "segment_%d" % a.segment)
            )
            scene, note = segment_scene(rec, a.save, a.carrier_save, a.force_save)
            rec = dict(rec, scene_save=scene, scene_note=note)
            for ln in rec.get("sanity") or ():
                print("  recorded " + ln)
    except Refusal as e:
        print("replay_match_segment: REFUSED -- %s" % e)
        return 2
    if a.from_keyframe > 1:
        try:
            sl = slice_from_step(
                rec,
                a.from_keyframe,
                os.path.join(REPO, "tmp", "replay_match", "from_%d" % a.from_keyframe),
            )
        except Refusal as e:
            print("replay_match_segment: REFUSED -- %s" % e)
            return 2
        rec = dict(rec, files=sl["files"], base=sl["base"], first_step=a.from_keyframe)
    if rec["kind"] == "shipped":
        ref_steps, ref_regs = dict(rec["ref_steps"]), {}
    else:
        ref_steps, ref_regs = parse_stream(rec["ref"], rec["base"])
    ref_steps = {s: v for s, v in ref_steps.items() if s >= 1}
    n_clock = clock_steps(rec["files"]["clock"])
    if rec["kind"] == "shipped":
        # checkpoints are sparse: replay every recorded step, judge at the checkpoints inside it
        last = n_clock
        if rec.get("segment_len"):
            last = min(last, rec["segment_len"])
        if not ref_steps:
            print("  note: no mh_rec_hash.txt checkpoints -- the replay runs but cannot be judged")
    else:
        last = min(max(ref_steps) if ref_steps else 0, n_clock)
    if a.steps:
        last = min(last, a.steps)
    config = (rec["config"] or 1) if a.config == "auto" else int(a.config)
    seat = (
        (rec.get("slot") if rec.get("slot") is not None else -1)
        if a.seat == "auto"
        else int(a.seat)
    )
    smode = auto_session_mode(rec) if a.session_mode == "auto" else int(a.session_mode)
    if rec.get("scene_save") and a.session_mode == "auto":
        smode = -1  # a loaded save runs in its own SESSION_MODE (the campaign's 1)
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
            hqueue=not a.no_hqueue,
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
        if other["kind"] == "shipped":
            ref_steps, ref_regs = dict(other["ref_steps"]), {}
        else:
            ref_steps, ref_regs = parse_stream(other["ref"], other["base"])
        ref_steps = {s: v for s, v in ref_steps.items() if s >= 1}
        rec = dict(rec, snap_dir=other.get("snap_dir"))
        print("  judged AGAINST %s (%d hashed step(s))" % (other["dir"], len(ref_steps)))
    ref_steps = {s: v for s, v in ref_steps.items() if s <= last}
    names, excluded = excluded_columns()
    res = compare(ref_steps, ref_regs, rep_steps, rep_regs, names, excluded)
    # a shipped recording is judged at EVERY checkpoint the replay reached (at least one)
    judged_rec = other or rec
    need = max(1, len(ref_steps)) if judged_rec["kind"] == "shipped" else min(a.min_steps, last)
    ok, text = verdict_text(judged_rec, res, need)
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


def _refused(fn):
    try:
        fn()
    except Refusal:
        return True
    return False


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

    # the shipped recording's checkpoints (mh_rec_hash.txt): state only, sparse, first bad named
    hdr = (
        "; mh_rec_hash v1: <match step> <state hash> every 20 steps, hash_kind=1 (the harness "
        "`state` column), configuration (1)\n"
    )
    htxt = hdr + "".join("%d %s\n" % (s, st(s)) for s in (1, 20, 40, 60)) + "; end at step 60\n"
    meta, hs = parse_hash_file(htxt)
    check(
        "mh_rec_hash.txt parses: header + 4 checkpoints, no clock",
        meta == dict(every=20, kind=1, config=1)
        and sorted(hs) == [1, 20, 40, 60]
        and hs[20] == (None, None, st(20)),
    )
    ship = {"kind": "shipped"}
    res = compare(hs, {}, r2, rr2)
    check(
        "checkpoints vs a matching replay -> IDENTICAL (clock not compared)",
        res["common"] == 4 and verdict_text(ship, res, 4)[0],
    )
    res = compare(hs, {}, r3, rr3)
    ok_s, txt_s = verdict_text(ship, res, 4)
    check(
        "a diverging replay names the first bad checkpoint and the last good one",
        not ok_s and res["first"] == 40 and res["last_ok"] == 20 and "21..40" in txt_s,
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
        # tooling:PT-FIX1 --from-keyframe: slice a 6-step synthetic match (4-byte regions x,y) at step 4.
        import struct

        sl_dir = os.path.join(tmp, "slice_src")
        os.makedirs(sl_dir)
        sl_states = {s: bytes([s] * 4 + [100 + s] * 4) for s in range(1, 7)}
        sl_data, _ = state_record._build_match(sr_defs, sl_states, keyframe_every=3)
        put(sl_dir, "mh_match_state.bin", sl_data)
        put(sl_dir, "mh_harness_seed.bin", sl_states[1])
        put(sl_dir, "mh_match_clock.bin", struct.pack("<6d", 1, 2, 3, 4, 5, 6))
        recs = b"MHOR" + struct.pack("<I", 1)
        for stp in (1, 3, 4, 4, 6):
            recs += struct.pack("<I", stp) + bytes([stp]) * (ORDER_REC - 4)
        put(sl_dir, "mh_match_orders.bin", recs)
        sl_rec = dict(
            snap_dir=sl_dir,
            base=10,
            files=dict(
                seed=os.path.join(sl_dir, "mh_harness_seed.bin"),
                clock=os.path.join(sl_dir, "mh_match_clock.bin"),
                orders=os.path.join(sl_dir, "mh_match_orders.bin"),
            ),
        )
        got = slice_from_step(sl_rec, 4, os.path.join(tmp, "slice_out"))
        with open(got["files"]["seed"], "rb") as fh:
            sl_seed = fh.read()
        with open(got["files"]["clock"], "rb") as fh:
            sl_clock = struct.unpack("<3d", fh.read())
        with open(got["files"]["orders"], "rb") as fh:
            sl_ord = fh.read()
        sl_steps = [
            struct.unpack_from("<I", sl_ord, ORDERS_MAGIC_LEN + i * ORDER_REC)[0]
            for i in range(order_records(got["files"]["orders"]))
        ]
        check(
            "from-keyframe: the seed is the state recording's state at the start step",
            sl_seed == sl_states[4],
        )
        check(
            "from-keyframe: the clock track starts at the start step (step 4 -> first double)",
            sl_clock == (4.0, 5.0, 6.0) and sl_ord[:4] == b"MHOR",
        )
        check(
            "from-keyframe: orders before the start step are dropped, the rest renumbered from 1",
            sl_steps == [1, 1, 3] and got["base"] == 13,
        )
        check(
            "from-keyframe: a start step the state recording does not hold is REFUSED",
            _refused(lambda: slice_from_step(sl_rec, 9, os.path.join(tmp, "slice_bad"))),
        )
        # a SHIPPED recording (mh.dll alone): gzipped inputs + checkpoints; preferred over an
        # incomplete harness segment (the rig's non-recording peer writes mh_match_harness.log only)
        import gzip

        sh = os.path.join(logs, "20260101T000500Z_ffff_1_join")
        os.makedirs(sh)
        put(sh, "session.json", json.dumps({"map": "y.mpm", "slot": 1, "lockstep_step_ms": 20}))
        put(sh, "mh_match_harness.log", "; [match] segment OPEN x at process step 3: step_base=2\n")
        for name, data in (
            ("mh_rec_orders.bin.gz", b"MHOR\x01\0\0\0"),
            ("mh_rec_clock.bin.gz", b"\0" * 24),
            ("mh_rec_seed.bin.gz", b"S" * 32),
        ):
            with gzip.open(os.path.join(sh, name), "wb") as fh:
                fh.write(data)
        put(sh, "mh_rec_hash.txt", htxt)
        got = resolve(sh)
        with open(got["files"]["seed"], "rb") as fh:
            seed_ok = fh.read() == b"S" * 32
        check(
            "resolve: a shipped recording (gz) beats an incomplete segment; inputs unpacked",
            got["kind"] == "shipped"
            and seed_ok
            and clock_steps(got["files"]["clock"]) == 3
            and got["config"] == 1
            and sorted(got["ref_steps"]) == [1, 20, 40, 60]
            and got["slot"] == 1,
        )
        # rc9.4 segments: a save_load segment at step 30 with its own seed; the cut + the renumbering
        import struct

        put(sh, "mh_rec_seed_1.bin", b"T" * 32)
        recs = b"MHOR" + struct.pack("<I", 1)
        for stp in (5, 29, 30, 31, 45):
            recs += struct.pack("<I", stp) + bytes([stp]) * (ORDER_REC - 4)
        with gzip.open(os.path.join(sh, "mh_rec_orders.bin.gz"), "wb") as fh:
            fh.write(recs)
        with gzip.open(os.path.join(sh, "mh_rec_clock.bin.gz"), "wb") as fh:
            fh.write(struct.pack("<50d", *range(1, 51)))
        put(
            sh,
            "mh_rec_hash.txt",
            hdr + "".join("%d %s\n" % (s, st(s)) for s in (1, 20, 30, 40)),
        )
        put(
            sh,
            "mh_rec_segments.txt",
            "; mh_rec_segments v1: x\nsegment 0 step=1 kind=session_start clock_after=0.000 planet_after=3 "
            "seed=mh_rec_seed.bin\nsegment 1 step=30 kind=save_load prev_step=29 clock_before=9.000 "
            'clock_after=4.000 planet_before=3 planet_after=5 planet=-1 hash_before=AAAA hash_after=BBBB '
            'source="2-almost" file="save\\2-almost.sav" file_bytes=3 file_fnv=E71FA2190541574B '
            "seed=mh_rec_seed_1.bin\nsanity NEW seg=1 step=30 boundary=save_load: buildings p0 i10\n",
        )
        got = resolve(sh, shipped=True)
        check(
            "segments: parsed (0 and 1, step/kind/source/fnv) + the sanity line",
            sorted(got["segments"]) == [0, 1]
            and got["segments"][1]["step"] == 30
            and got["segments"][1]["source"] == "2-almost"
            and got["segments"][1]["file_fnv"] == "E71FA2190541574B"
            and len(got["sanity"]) == 1,
        )
        sg = slice_segment(got, 1, os.path.join(tmp, "seg_out"))
        with open(sg["files"]["seed"], "rb") as fh:
            seg_seed = fh.read()
        with open(sg["files"]["clock"], "rb") as fh:
            seg_clock = struct.unpack("<21d", fh.read())
        seg_steps = [
            struct.unpack_from(
                "<I", open(sg["files"]["orders"], "rb").read(), ORDERS_MAGIC_LEN + i * ORDER_REC
            )[0]
            for i in range(order_records(sg["files"]["orders"]))
        ]
        check(
            "segment 1: its own seed, clock from step 30, orders >= 30 renumbered from 1",
            seg_seed == b"T" * 32
            and seg_clock[0] == 30.0
            and seg_steps == [1, 2, 16]
            and sg["base"] == 29,
        )
        check(
            "segment 1: checkpoints 30 and 40 become replay steps 1 and 11",
            sorted(sg["ref_steps"]) == [1, 11] and sg["ref_steps"][11][2] == st(40),
        )
        check(
            "a segment the table does not hold is REFUSED",
            _refused(lambda: slice_segment(got, 7, tmp)),
        )
        abc = os.path.join(tmp, "abc.sav")
        put(tmp, "abc.sav", b"abc")
        check(
            "fnv1a64_file matches mh.dll's file_fnv64 ('abc')",
            fnv1a64_file(abc) == "E71FA2190541574B",
        )
        check(
            "the scene of a save_load segment is the recorded save when the hash matches",
            segment_scene(sg, abc)[0] == abc,
        )
        put(tmp, "abd.sav", b"abd")
        check(
            "...and a different save is REFUSED by its hash",
            _refused(lambda: segment_scene(sg, os.path.join(tmp, "abd.sav"))),
        )
        sg2 = dict(sg, segment=dict(sg["segment"], kind="planet_transition"))
        check(
            "a planet_transition segment needs --carrier-save",
            _refused(lambda: segment_scene(sg2)) and segment_scene(sg2, None, abc)[0] == abc,
        )
        check(
            "the segment walk loads the save from the main menu",
            "clickl Load game" in load_walk_script() and "gamemode 2" in load_walk_script(),
        )
        os.remove(os.path.join(sh, "mh_rec_seed.bin.gz"))
        check(
            "resolve: a shipped recording without its seed is REFUSED, by name",
            _refused(lambda: resolve(sh, shipped=True)),
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # tooling:TL-REPLAY-BM: session mode from session.json; hash-point queue counts from a state file
    check(
        "auto session mode: role=solo -> 2 even with lockstep_step_ms",
        auto_session_mode({"role": "solo", "lockstep": True}) == 2,
    )
    check(
        "auto session mode: hosted lockstep -> 3",
        auto_session_mode({"role": "host", "lockstep": True}) == 3,
    )
    check("auto session mode: neither signal -> -1 (leave)", auto_session_mode({}) == -1)
    # PT-SET1: the recording's gameplay settings reach the replay lane
    gk = [
        ("turret_reload_realtime_fix", "1"),
        ("turret_fire_rate_mult", "2.0"),
        ("turret_clip_fix", "0"),
    ]
    ex, warn, src = gameplay_net_extra(
        {
            "gameplay": "turret_reload_realtime_fix=0;turret_fire_rate_mult=3;turret_clip_fix=1",
            "gameplay_src": "adopted",
        },
        "",
        gk,
    )
    check(
        "gameplay: a recorded set is passed through verbatim",
        ex == "turret_reload_realtime_fix=0;turret_fire_rate_mult=3;turret_clip_fix=1"
        and warn is None
        and "adopted" in src,
    )
    ex, warn, _ = gameplay_net_extra(
        {"gameplay": "turret_fire_rate_mult=3;gone_key=1"}, "turret_clip_fix=1", gk
    )
    check(
        "gameplay: a missing key = registry default, an unknown key dropped + noted, the caller's --net-extra wins",
        ex == "turret_reload_realtime_fix=1;turret_fire_rate_mult=3"
        and warn
        and "gone_key" in warn,
    )
    for sj in ({}, None, {"gameplay": ""}, {"gameplay": 5}):
        ex, warn, src = gameplay_net_extra(sj, "", gk)
        check(
            "gameplay: no usable field %r -> registry defaults + a warning" % (sj,),
            ex == "turret_reload_realtime_fix=1;turret_fire_rate_mult=2.0;turret_clip_fix=0"
            and warn
            and "no `gameplay` field" in warn
            and src == "registry defaults",
        )
    live = host_sync_keys()
    check(
        "gameplay: the live schema yields the four turret rows",
        [k for k, _ in live][:4]
        == [
            "turret_reload_realtime_fix",
            "turret_fire_rate_mult",
            "turret_clip_fix",
            "turret_fire_poll_fix",
        ],
    )
    import state_record

    rdefs = [("order_queue", 8), ("order_queue_count", 4)]
    sts = {}
    for stp, c in ((10, 0), (11, 0), (12, 1), (13, 1), (14, 3), (15, 0)):
        sts[stp] = bytes(8) + struct.pack("<I", c)
    blob, _ = state_record._build_match(rdefs, sts, 4)
    hq_path = os.path.join(tempfile.mkdtemp(prefix="hq_"), "mh_match_state.bin")
    with open(hq_path, "wb") as fh:
        fh.write(blob)
    got = hqueue_counts(state_record.load(hq_path), 9, 16)
    check(
        "hqueue_counts: per-step counts, uncovered steps unknown",
        got == [0xFFFFFFFF, 0, 0, 1, 1, 3, 0, 0xFFFFFFFF],
    )
    print("replay_match_segment selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:] or "--compare" in sys.argv[1:]:
        raise SystemExit(main())
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "replay_match_segment"))
