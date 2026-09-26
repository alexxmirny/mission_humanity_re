#!/usr/bin/env python3
"""fixture_replay.py -- the LIB-REF replay FIXTURE: pack it, verify it, replay it (tracker LIB-REF-REC).

WHAT THE FIXTURE IS. LIB-REF's acceptance replays a recorded session and compares a per-step lockstep
hash against what the in-binary engine produced. That needs three halves that MUST come from ONE run
or they describe different worlds:

  * the ORDER + CLOCK recording  -- mh_orders.bin (one record per dispatched order, tagged with its
    step) + mh_clock.bin (the per-step game clock, so a real-time recording replays with its own
    deltas rather than a synthetic fixed step);
  * the STEP-0 WORLD blob        -- mh_world.bin, every bound region at the instant before the first
    sim step (LIB-WORLD), so a standalone host can start without a cfg parser or a map reader;
  * the HASH STREAM              -- the per-step `<step> <clock> <combined> <state>` lines and the
    per-region `R <step> <h0..hN>` lines, which are LIB-REF's comparison target.

THE HASH STREAM IS THE *REPLAY'S*, NOT THE RECORD'S, AND THAT IS A RULING RATHER THAN A SHORTCUT.
A record run and a replay run sample `order_queue` at different instants -- the record hashes the
queue before the step's orders arrive, the replay hashes it after the injector has loaded them -- so
a record-stream-vs-replay comparison can never be clean, however correct the fixture is. Committing
the record's stream would therefore build a permanently-red instrument artifact into LIB-REF's
oracle. The committed stream is what a CORRECT REPLAYER produces, and self-consistency is
replay-vs-replay bit-identity (measured: all 61 regions, every step).

AND REPLAY-VS-REPLAY ALONE HAS A BLIND SPOT, so it is not the whole acceptance: a deterministically
BROKEN replay would also be bit-identical with itself. The record-vs-replay comparison is therefore
part of the ACCEPTANCE and it is FAIL-CLOSED -- every mismatching step must be attributable to the
ONE measured instrument mechanism, which `attribute()` below states in full. In outline: a periodic
sim event enqueues an order between the step's hash sample and dispatch entry, so the replay sees it
one step early; the queue regions disagree at those instants, and a downstream MARK appears within
one step of one. Anything else -- a region whose onset is not at an instant, a queue mismatch outside
an instant's window, or a mark nobody has localized in the manifest -- refuses the fixture, and
`verify` exits non-zero.

NOTE FOR ANYONE READING AN OLDER REPORT: an earlier draft of this file split the residue into
"mechanism (i) sampling instant" and "mechanism (ii) an AI enqueue-return blip", and predicted the
first would fire at EVERY step. Measurement retired both: the queue disagrees at 36 of 5000 steps,
not 5000, and the suppression instrument was refuted as the cause of the marks by running the same
replay with `replay_suppress_enqueue=0` (it diverges either way). That wording is gone; do not
reintroduce it.

THE REPLAY CONFIGURATION IS PART OF THE CONTRACT, not a runner detail. It lives in manifest.json as
first-class fields because LIB-REF's standalone replayer must reproduce that configuration's
SEMANTICS, not merely read the same bytes: the AI runs inside the sim (replay_ai_off=0) and its
order enqueues return 0 without appending (replay_suppress_enqueue=1), so the recorded stream is the
only thing that reaches the queue.

COMPARE `state`, NEVER `combined`. Measured at step 1 of a record and its replay:
    record   clock=3F9111276FB08000 combined=F452009FFCE2B0FF state=52976C9B8B49D2A8
    replay   clock=3F9111276FB08000 combined=792FF0413876D562 state=52976C9B8B49D2A8
`state` is identical -- and is the same value four independent launches produced -- because it drops
the `state_excluded()` wall-clock/pacing regions. `combined` folds them in and is therefore a
property of the run, not of the world. LIB-REF's hash clause compares `state` and the `R` columns.

THE SECOND FIXTURE SHAPE: `--live` (LIB-REF-LIVE, 2026-09-11). Everything above describes the REPLAY
fixture. A LIVE fixture is the same tool with the order half REMOVED, and the removal is the whole
claim: the in-sim AI is the sole order source, nothing is injected and nothing is suppressed, so the
standalone host has to run the enqueue/issue/dispatch loop the replay fixtures structurally cover
over. Its manifest carries `live_contract` INSTEAD of `replay_contract` (the two describe mutually
exclusive arrangements and `integrity` refuses a manifest holding both), `pack --live` refuses a run
directory that contains an `mh_orders.bin` at all, and `verify` swaps its middle two arms: the 0 -> n
counts guard and the record-vs-replay attribution are both n/a -- neither mechanism exists without an
injector -- and what replaces them is TWO hosted live runs each reproducing the committed stream
bit-for-bit (`--live-log` twice; one run agreeing with itself proves nothing).

Usage:
    python tools/fixture_replay.py pack   <run_dir> --out <fixture_dir> [--seed N --steps N ...]
    python tools/fixture_replay.py pack   <run_dir> --live --out <fixture_dir> ...   # no order half
    python tools/fixture_replay.py verify <fixture_dir> --live-log <log> --live-log <log>  # live
    python tools/fixture_replay.py stream <run_dir> --fixture <fixture_dir>   # add the replay stream
    python tools/fixture_replay.py unpack <fixture_dir> --lane <lane_dir>
    python tools/fixture_replay.py replay <fixture_dir> --lane <lane_dir> [--port N]
    python tools/fixture_replay.py verify <fixture_dir> [--against <run_dir>]
    python tools/fixture_replay.py check  <fixture_dir>       # integrity only (lint-able, no rig)
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

DEFAULT_FIXTURE = os.path.join(REPO, "tools", "data", "fixtures", "libref-replay-v1")

# The recording's own half-names -> the file the game reads them back from. The game resolves both
# INPUTS next to the exe (harness.cpp build_paths: g_orders_path / g_clock_path use g_dir, NOT the
# run folder), which is why `unpack` writes them into the lane directory rather than anywhere nicer.
LANE_INPUTS = {"orders": "mh_orders.bin", "clock": "mh_clock.bin"}

# THE REPLAY CONTRACT. Changing any of these changes what the committed hash stream means, so they
# are written into the manifest and re-read from it at replay time rather than duplicated here as
# the source of truth. This dict is only the default a fresh `pack` records.
REPLAY_FLAGS = {
    "order_mode": 2,
    "replay_ai_off": 0,  # the AI RUNS -- turning it off diverges the battlefield (see the README)
    "replay_suppress_enqueue": 1,  # ... but its enqueues return 0, so only the recording drives
    "pin_wallclock": 1,
    "fixed_step": 0,
    "region_hash_step": 1,
    "exit_on_stop": 1,
}

# THE LIVE CONTRACT (LIB-REF-LIVE). The pinned arrangement, recorded VERBATIM rather than summarised,
# because "a recording under any OTHER arrangement does not satisfy this item" is the tracker's own
# wording and a manifest that paraphrased it could not be checked against it. Note what is ABSENT:
# there is no order_mode=2, no injector and no suppression -- the three mechanisms the replay fixture
# leans on are exactly the ones this fixture must not have.
LIVE_FLAGS = {
    "all_ai": 1,
    "synth_move": 0,
    "order_mode": 0,
    "replay_suppress_enqueue": 0,
    "pin_wallclock": 1,
    "pin_fpu": 1,
    "fixed_step": 0,
    "region_hash_step": 1,
    "exit_on_stop": 1,
    "clock_record": 1,
    "world_capture": 1,
    "pin_strat_seed": 1,
    "strat_seed": 20260912,
}

LIVE_FIXTURE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "tools",
    "data",
    "fixtures",
    "libref-live-aisoak-v1",
)


def _relpath_or_abs(path: str) -> str:
    """Repo-relative when possible; the absolute path when the run dir is on another volume."""
    try:
        return os.path.relpath(path, REPO).replace("\\", "/")
    except ValueError:
        return os.path.abspath(path).replace("\\", "/")


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _read(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def _write_z(path: str, raw: bytes) -> int:
    comp = zlib.compress(raw, 9)
    with open(path, "wb") as fh:
        fh.write(comp)
    return len(comp)


def _read_z(path: str) -> bytes:
    return zlib.decompress(_read(path))


def _epoch_of_log(log_path):
    """(epoch, build) the run's DLL printed on its HASH FINGERPRINT line (TL-GATE8)."""
    import mp_analyze as _m

    return _m.harness_input_epoch(log_path)


def epoch_refusal(man, log_path):
    """The named refusal when `log_path`'s build epoch differs from the fixture's stamp, else None."""
    import mp_analyze as _m

    return _m.epoch_mismatch(
        (man.get("step0") or {}).get("hash_input_epoch"), _epoch_of_log(log_path)[0]
    )


def manifest_path(fixture: str) -> str:
    return os.path.join(fixture, "manifest.json")


def load_manifest(fixture: str) -> dict:
    with open(manifest_path(fixture), encoding="utf-8") as fh:
        return json.load(fh)


# ---- the hash stream ----------------------------------------------------------------------------
#
# Split out of mh_harness.log rather than committing the whole log: the log also carries arming
# banners, per-run paths and diagnostics that differ between runs by design, and a fixture whose
# comparison target contains run-specific text cannot be compared byte-for-byte.


def split_stream(log_bytes: bytes):
    """(per-step lines, per-region R lines) as bytes, in file order."""
    steps, regions = [], []
    for line in log_bytes.decode("latin1").splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "R":
            regions.append(line)
        elif len(parts) >= 4 and parts[0].isdigit():
            steps.append(line)
    enc = lambda rows: ("\n".join(rows) + "\n").encode("latin1") if rows else b""  # noqa: E731
    return enc(steps), enc(regions)


def parse_stream(step_bytes: bytes, region_bytes: bytes):
    """-> ({step: (clock, combined, state)}, {step: [h0..hN]}) from the committed form."""
    steps, regions = {}, {}
    for line in step_bytes.decode("latin1").splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit():
            steps[int(p[0])] = (p[1].upper(), p[2].upper(), p[3].upper())
    for line in region_bytes.decode("latin1").splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] == "R":
            regions[int(p[1])] = [x.upper() for x in p[2:]]
    return steps, regions


def region_names():
    """The hash manifest's column names, POSITIONALLY -- the same list mp_analyze labels with."""
    import mp_analyze as m

    return list(m.REGION_NAMES)


def excluded_names():
    """The state_excluded() set, read from the GENERATED registry header.

    Read from the header rather than listed here for the reason the header itself gives: the
    manifest order is a wire contract, and a second copy of it in a tool is how the two stop
    agreeing without anybody noticing."""
    src = open(
        os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h"), encoding="utf-8"
    ).read()
    m = re.search(r"inline constexpr hash_region HASH_REGIONS\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit("fixture_replay: HASH_REGIONS not found in mh_regions.gen.h")
    return {
        nm
        for nm, ex in re.findall(r'\{"([^"]+)",\s*RID_\w+,[^}]*?(true|false)\}', m.group(1))
        if ex == "true"
    }


# ---- pack ---------------------------------------------------------------------------------------


def cmd_pack(args):
    run = args.run_dir
    fixture = args.out or DEFAULT_FIXTURE
    os.makedirs(fixture, exist_ok=True)

    # LIB-REF-LIVE: the LIVE fixture has no order stream, and that ABSENCE IS THE POINT -- the proof
    # is that the in-sim AI is the sole order source, so a recording that carried orders would be the
    # wrong artifact. `--live` therefore drops `orders` from the required set, and REFUSES a run that
    # produced one anyway: an order stream in a live run dir means the recorder was armed, i.e. the
    # arrangement was not the pinned one, and packing it would bake a mislabelled contract.
    live = bool(getattr(args, "live", False))
    halves = {
        "clock": os.path.join(run, "mh_clock.bin"),
        "world": os.path.join(run, "mh_world.bin"),
    }
    if not live:
        halves["orders"] = os.path.join(run, "mh_orders.bin")
    missing = [k for k, p in halves.items() if not os.path.isfile(p)]
    if missing:
        sys.exit(
            "fixture_replay pack: %s missing from %s -- all halves must come from ONE run, so "
            "a partial pack is refused rather than filled in from elsewhere." % (missing, run)
        )
    if live and os.path.isfile(os.path.join(run, "mh_orders.bin")):
        sys.exit(
            "fixture_replay pack --live: %s HAS an mh_orders.bin. A live fixture's whole claim is "
            "that nothing was injected and nothing recorded orders, so a run with an order stream "
            "was made under a different arrangement -- refused rather than packed without it." % run
        )

    log = _read(os.path.join(run, "mh_harness.log"))
    epoch, build = _epoch_of_log(os.path.join(run, "mh_harness.log"))
    if epoch is None:
        sys.exit(
            "fixture_replay pack: %s/mh_harness.log carries no `input_epoch=` on its HASH "
            "FINGERPRINT line -- a pre-TL-GATE8 build; the fixture could not be stamped" % run
        )
    artifacts = {}
    for name, path in halves.items():
        raw = _read(path)
        comp = _write_z(os.path.join(fixture, name + ".bin.zz"), raw)
        artifacts[name] = {
            "file": name + ".bin.zz",
            "raw_bytes": len(raw),
            "zlib_bytes": comp,
            "sha256_raw": sha256(raw),
        }

    # The world blob's own header, read back so the manifest can state the fixture's step-0 identity
    # without anybody having to open the blob. Offsets are world_snapshot.h's blob_header.
    wraw = _read(halves["world"])
    import struct

    (schema,) = struct.unpack_from("<I", wraw, 12)  # common_header.schema (TL-FIXTURE-SCHEMA)
    (content_hash,) = struct.unpack_from("<Q", wraw, 24)
    lk_comb, lk_state, gclock = struct.unpack_from("<QQQ", wraw, 32)
    step, masks, sinkfp, manfp, slices = struct.unpack_from("<IIIII", wraw, 56)

    steps_lines, region_lines = split_stream(log)
    n_steps = len(steps_lines.decode("latin1").splitlines())

    seed = args.seed
    if seed is None:
        m = re.search(r"synth_seed=(\d+)", log.decode("latin1"))
        seed = int(m.group(1)) if m else None

    man = {
        "_what": (
            "LIB-REF-LIVE live-loop fixture: clock track, step-0 world blob, and the hosted live "
            "run's per-step lockstep hash stream -- all from ONE run, and NO ORDER STREAM. The "
            "absence is the artifact's whole claim: the in-sim AI is the sole order source, nothing "
            "is injected and nothing is suppressed, so the standalone host has to reproduce the "
            "enqueue/issue/dispatch loop the replay fixtures structurally cover over."
            if live
            else "LIB-REF replay fixture: order+clock recording, step-0 world blob, and the "
            "replay's per-step lockstep hash stream -- all from ONE run."
        ),
        "_read_the_readme": "README.md beside this file carries the re-record procedure and the "
        "contract LIB-REF inherits. Do not regenerate half of this set.",
        "version": 1,
        "steps": n_steps,
        "provenance": {
            "commit": args.commit,
            "recorded": args.recorded,
            "scenario_script": args.script,
            "synth_seed": seed,
            "host": args.host,
            "peers": args.peers,
            # relpath() raises across Windows drive letters, and a run dir on another volume is an
            # ordinary thing (a staged copy, a scratch lane). Fall back to the absolute path rather
            # than failing the pack over a cosmetic provenance string.
            "run_dir": _relpath_or_abs(run),
            "build": build,
        },
        # THE CONTRACT (see the module banner). LIB-REF's standalone replayer must reproduce these
        # SEMANTICS, not merely read the same bytes.
        "replay_contract": {
            "flags": dict(REPLAY_FLAGS),
            "record_flags": dict(args.record_flags or {}),
            "semantics": [
                "replay_ai_off=0: the AI RUNS inside the sim. Turning it off changes the "
                "battlefield -- measured: strat_players/units/tile_objects diverge from ~step 200, "
                "because the AI does more than enqueue orders and the recording only holds orders.",
                "replay_suppress_enqueue=1: the immediate-lane enqueue returns 0 and appends "
                "nothing, so the INJECTED recording is the only thing that reaches the order queue. "
                "The check sits at the SINK (mh::orders::detail::enqueue), not at the game entry's "
                "promotion wrapper where it lived until 2026-09-11 -- a wrapper gate saw only "
                "callers that crossed 0x00466094 and missed every libmh-internal caller the rebind "
                "routes straight to our body, which is how the harness's own synth_move workload "
                "appended one order per step past it. A standalone replayer must therefore suppress "
                "AT THE SINK too, or it is not reproducing this flag. SCOPE: the immediate lane is "
                "the only appender in a non-lockstep session; the mode-3 lane reaches the queue via "
                "release_due, which this flag deliberately does NOT suppress.",
                "the injector overwrites QUEUE[0..n] and sets COUNT at the TOP of each sim step, "
                "before the step is hashed.",
                "exit_on_stop=1 exits ONE STEP BOUNDARY LATE (harness.cpp, LIB-REF-REC): the stop "
                "step's sim body runs before the process leaves, so the recorder -- which hangs off "
                "llm_strat_order_queue_dispatch's entry, inside the body -- snapshots the exit step "
                "like every other step. Before that fix mh_orders.bin stopped one step short of "
                "mh_clock.bin and the replay injected k=0 on the final step.",
            ],
        },
        "compare": {
            "channel": "state",
            "why": "`state` drops the state_excluded() wall-clock/pacing regions and is identical "
            "across runs and across record/replay configurations (measured: step-1 state "
            "52976C9B8B49D2A8 from four independent launches). `combined` folds those regions in "
            "and is a property of the RUN, not of the world -- measured different at step 1 of a "
            "record and its own replay. LIB-REF compares `state` and the per-region R columns; it "
            "must never compare `combined`.",
            "region_columns": region_names(),
        },
        "step0": {
            "world_content_hash": "%016X" % content_hash,
            "world_schema_fp": "%08X" % schema,
            "lockstep_combined": "%016X" % lk_comb,
            "lockstep_state": "%016X" % lk_state,
            "game_clock": "%016X" % gclock,
            "step": step,
            "mask_flags": masks,
            "hash_sink_fp": "%08X" % sinkfp,
            "hash_manifest_fp": "%08X" % manfp,
            "hash_slice_count": slices,
            "hash_input_epoch": epoch,
        },
        "artifacts": artifacts,
    }

    if live:
        # The live fixture's contract REPLACES the replay one rather than sitting beside it: a
        # manifest carrying both would let a reader take the wrong half as authoritative, and the two
        # describe mutually exclusive arrangements.
        man.pop("replay_contract", None)
        man["live_contract"] = {
            "flags": dict(LIVE_FLAGS),
            "scenario_script": args.script,
            "semantics": [
                "NO ORDER STREAM EXISTS IN THIS FIXTURE, and that is the claim being made. The "
                "replay fixtures inject a recorded order stream and suppress the sim's own "
                "enqueues, which structurally covers over the enqueue/issue/dispatch loop; here "
                "the in-sim AI is the SOLE order source. A standalone host must run that loop for "
                "real and still track the recording bit-for-bit.",
                "all_ai=1: every player slot is an AI, including the local one (a pre-landing "
                "status_flags flip, so it spawns through llm_strat_spawn_ai_base like any computer "
                "player rather than ticking with no home tile).",
                "synth_move=0: the harness's synthetic workload is EXPLICITLY disarmed. It is the "
                "soak's own default here, but it is stated because a workload order would be a "
                "second order source and would make the sole-source claim false -- which is "
                "precisely how the replay fixture's step-5000 residual happened.",
                "replay_suppress_enqueue=0: suppression is OFF. The standalone host must therefore "
                "run WITHOUT --no-suppress-enqueue's opposite -- i.e. it must pass "
                "--no-suppress-enqueue, since that host suppresses by default for the replay "
                "contract.",
                "clock_record=1 with order_mode=0: the clock track is written WITHOUT an order "
                "stream. That combination did not exist before 2026-09-11 -- clock_record was "
                "gated on order_mode==1, so the only way to get a clock was to also produce the "
                "order stream this fixture forbids. order_mode==1 still implies clock_record, so "
                "every older recording path is byte-unchanged (proven: a 300-step sp_det record "
                "before and after the split produced byte-identical mh_clock.bin and "
                "mh_orders.bin).",
                "pin_strat_seed=1, strat_seed=20260912: the pinned scenario seed, carried "
                "literally. MEASURED INERT for this scenario -- five 200-step runs (two unpinned "
                "minutes apart, plus strat_seed 7, 52 and 20260912) are bit-identical on `state` "
                "and all 61 region columns -- because the knob's documented effect is the CAMPAIGN "
                "landing roll and this is an 8-way skirmish from the default start. The pin is "
                "kept as anti-cherry-picking provenance, not as a determinism mechanism.",
                "the clock track is a legitimate carried INPUT: time is an input, decisions are "
                "not. Nothing else about the run is fed to the standalone host.",
            ],
        }

    # THE HAND FIELDS, CARRIED EXPLICITLY (the filed pack-eats-re_record trap).
    # `pack` rewrites manifest.json wholesale, so any field a HUMAN wrote and no tool reproduces is
    # destroyed by a re-record -- which is exactly what happened to `fidelity.re_record`'s v1-v5
    # history on 2026-09-11. Carried by an explicit allow-list rather than a blanket merge: a
    # tool-derived field must NOT survive a re-record (stale `fidelity.marks` carried forward would
    # excuse marks the new recording never had), so the two classes cannot share a rule. What is
    # carried is PRINTED, because a silent carry is the other half of the same trap.
    carried = []
    if os.path.isfile(manifest_path(fixture)):
        try:
            with io.open(manifest_path(fixture), encoding="utf-8") as fh:
                old = json.load(fh)
        except (OSError, ValueError):
            old = {}
        for path in (("fidelity", "re_record"),):
            src = old
            for k in path[:-1]:
                src = src.get(k, {}) if isinstance(src, dict) else {}
            if isinstance(src, dict) and path[-1] in src:
                dst = man
                for k in path[:-1]:
                    dst = dst.setdefault(k, {})
                dst[path[-1]] = src[path[-1]]
                carried.append(".".join(path))

    with open(manifest_path(fixture), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(man, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print(
        "packed %s%s" % (os.path.relpath(fixture, REPO), " [LIVE: no order stream]" if live else "")
    )
    if carried:
        print("  carried hand field(s) through the repack: %s" % ", ".join(carried))
    elif os.path.isfile(manifest_path(fixture)):
        print(
            "  (no hand fields to carry -- fidelity.re_record is absent; write one if this is a "
            "re-record)"
        )
    for k, a in artifacts.items():
        print(
            "  %-8s %10d raw -> %8d zlib (%.1f%%)"
            % (k, a["raw_bytes"], a["zlib_bytes"], 100.0 * a["zlib_bytes"] / a["raw_bytes"])
        )
    print("  (the hash stream is added by `stream` from a REPLAY run -- see the banner)")


def cmd_stream(args):
    """Add the committed hash stream, taken from a REPLAY run (never from the record)."""
    fixture = args.fixture or DEFAULT_FIXTURE
    man = load_manifest(fixture)
    log = _read(os.path.join(args.run_dir, "mh_harness.log"))
    bad = epoch_refusal(man, os.path.join(args.run_dir, "mh_harness.log"))
    if bad:
        sys.exit("fixture_replay stream: REFUSED -- %s" % bad)
    steps_lines, region_lines = split_stream(log)
    if not steps_lines or not region_lines:
        sys.exit("fixture_replay stream: that run produced no hash stream (region_hash_step=0?)")
    n = len(steps_lines.decode("latin1").splitlines())
    if n != man["steps"]:
        sys.exit(
            "fixture_replay stream: the replay produced %d steps, the recording has %d. A stream "
            "of a different length is a different run -- refused." % (n, man["steps"])
        )
    for name, raw in (("hash_steps", steps_lines), ("hash_regions", region_lines)):
        comp = _write_z(os.path.join(fixture, name + ".txt.zz"), raw)
        man["artifacts"][name] = {
            "file": name + ".txt.zz",
            "raw_bytes": len(raw),
            "zlib_bytes": comp,
            "sha256_raw": sha256(raw),
        }
    man["provenance"]["stream_from_run"] = os.path.relpath(args.run_dir, REPO).replace("\\", "/")
    with open(manifest_path(fixture), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(man, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print("stream added from %s (%d steps)" % (args.run_dir, n))
    for k in ("hash_steps", "hash_regions"):
        a = man["artifacts"][k]
        print("  %-13s %10d raw -> %8d zlib" % (k, a["raw_bytes"], a["zlib_bytes"]))


# ---- unpack / replay -----------------------------------------------------------------------------


def cmd_unpack(args):
    fixture = args.fixture_dir or DEFAULT_FIXTURE
    man = load_manifest(fixture)
    out = []
    for name, fname in LANE_INPUTS.items():
        # A LIVE fixture has no `orders` half at all, so unpack what the manifest actually declares
        # rather than what a replay fixture happens to have. Keyed off the manifest, not off a flag,
        # so the two fixture shapes cannot disagree about which files exist.
        if name not in man["artifacts"]:
            continue
        raw = _read_z(os.path.join(fixture, man["artifacts"][name]["file"]))
        got = sha256(raw)
        if got != man["artifacts"][name]["sha256_raw"]:
            sys.exit(
                "fixture_replay unpack: %s decompresses to the wrong bytes (sha mismatch)" % name
            )
        dst = os.path.join(args.lane, fname)
        with open(dst, "wb") as fh:
            fh.write(raw)
        out.append((fname, len(raw)))
    if args.world:
        raw = _read_z(os.path.join(fixture, man["artifacts"]["world"]["file"]))
        with open(args.world, "wb") as fh:
            fh.write(raw)
        out.append((os.path.basename(args.world), len(raw)))
    for f, n in out:
        print("  unpacked %-16s %d bytes -> %s" % (f, n, args.lane))
    return man


def cmd_replay(args):
    """Drive ONE replay of the committed fixture and return its run directory."""
    fixture = args.fixture_dir or DEFAULT_FIXTURE
    man = cmd_unpack(args)
    flags = dict(man["replay_contract"]["flags"])
    rec = man["replay_contract"].get("record_flags") or {}
    # The WORLD-DEFINING knobs ride along from the RECORD's flags. The replay contract says how to
    # REPLAY; these say what world is being replayed, and a replay that reconstitutes a different
    # world is not a replay of this recording at all.
    #
    # The first four are the synthetic workload: it is an order source, so an arm that differs from
    # the record's disagrees with the stream it is replaying. The rest arrived with the all-AI soak
    # fixture (LIB-REF-SOAK, 2026-09-12) and are more fundamental than the workload, not less --
    # `all_ai` converts the local slot to an AI BEFORE landing, so a replay without it seats a human
    # player, spawns a different base and diverges at step 1 with nothing in the log saying why.
    # `pin_strat_seed`/`strat_seed` are the scenario seed (measured inert for this skirmish, carried
    # because the pin is provenance), and the three soak knobs below change what the run DOES rather
    # than merely what it logs: `gameover_step` can set stop_step and end the run early.
    #
    # setdefault, not assignment: an explicit replay_contract.flags entry still wins, because the
    # contract is the authority on the replay and this is only the fallback for what it does not say.
    for k in (
        "synth_move",
        "synth_seed",
        "synth_at",
        "synth_every",
        "pin_fpu",
        "all_ai",
        "all_ai_observer",
        "pin_strat_seed",
        "strat_seed",
        "gameover_step",
        "ai_probe_step",
    ):
        if k in rec:
            flags.setdefault(k, rec[k])
    extra = ";".join("%s=%s" % (k, v) for k, v in flags.items())
    argv = [
        sys.executable,
        os.path.join(REPO, "tools", "ui_test.py"),
        man["provenance"]["scenario_script"],
        "--harness",
        "--steps",
        str(man["steps"]),
        "--harness-extra",
        extra,
        "--host-dir",
        args.lane,
        "--timeout-frames",
        str(args.timeout_frames),
        "--timeout",
        str(args.timeout),
        "--headless",
        "--port",
        str(args.port),
    ]
    print("  $ " + " ".join(argv[1:]))
    before = (
        set(os.listdir(os.path.join(args.lane, "logs")))
        if os.path.isdir(os.path.join(args.lane, "logs"))
        else set()
    )
    r = subprocess.run(argv, cwd=REPO)
    if r.returncode != 0:
        sys.exit(
            "fixture_replay replay: the run FAILED (exit %d) -- re-run, do not massage"
            % r.returncode
        )
    after = set(os.listdir(os.path.join(args.lane, "logs")))
    # THE PROCESS DIRECTORY, not the session one. Since the per-SESSION log split a launch leaves
    # two new directories -- `<ts>_menu_solo` (per process: the harness log, the recording halves)
    # and `<ts>_<session>_0_solo` (per session: mh_net.log, session.json) -- and the session dir
    # sorts LAST. Picking `new[-1]` handed `stream` a directory with no mh_harness.log
    # (TL-GATE-D25FX re-record, 2026-09-20); the run dir this tool means is the one that holds it.
    new = sorted(
        d
        for d in after - before
        if os.path.isfile(os.path.join(args.lane, "logs", d, "mh_harness.log"))
    )
    if not new:
        sys.exit("fixture_replay replay: no new run directory carrying an mh_harness.log")
    rd = os.path.join(args.lane, "logs", new[-1])
    print("  run -> %s" % rd)
    return rd


# ---- verify --------------------------------------------------------------------------------------


def integrity(fixture: str):
    """Every committed artifact decompresses to the bytes the manifest names. Returns problems."""
    man = load_manifest(fixture)
    problems = []
    for name, a in man["artifacts"].items():
        p = os.path.join(fixture, a["file"])
        if not os.path.isfile(p):
            problems.append("%s: %s is missing" % (name, a["file"]))
            continue
        try:
            raw = _read_z(p)
        except zlib.error as e:
            problems.append("%s: will not decompress (%s)" % (name, e))
            continue
        if len(raw) != a["raw_bytes"]:
            problems.append("%s: %d raw bytes, manifest says %d" % (name, len(raw), a["raw_bytes"]))
        if sha256(raw) != a["sha256_raw"]:
            problems.append(
                "%s: sha256 of the decompressed bytes disagrees with the manifest" % name
            )
    # The required set depends on the fixture's SHAPE, read from the manifest's own contract key --
    # a LIVE fixture has no order stream by construction, and demanding one would refuse the very
    # artifact the shape exists to express. The converse is checked too: a live fixture that HAS an
    # orders half is a mislabelled replay fixture, which is the mistake worth catching.
    live = "live_contract" in man
    req_set = ("clock", "world", "hash_steps", "hash_regions")
    if not live:
        req_set = ("orders",) + req_set
    for req in req_set:
        if req not in man["artifacts"]:
            problems.append("the fixture has no `%s` half -- an incomplete set is refused" % req)
    if live and "orders" in man["artifacts"]:
        problems.append(
            "this is a LIVE fixture (live_contract) yet it carries an `orders` half -- the absence "
            "of an order stream IS the live claim, so a set with one is refused"
        )
    if live and "replay_contract" in man:
        problems.append(
            "the manifest carries BOTH live_contract and replay_contract -- they describe mutually "
            "exclusive arrangements and a reader could take either as authoritative"
        )
    return man, problems


def compare_streams(man, a_steps, a_regs, b_steps, b_regs, label_a, label_b):
    """Full per-region comparison. Returns (common, per-region first/count, per-step mismatch set)."""
    names = man["compare"]["region_columns"]
    common = sorted(set(a_regs) & set(b_regs))
    first, count, bad_steps = {}, {}, set()
    for s in common:
        ra, rb = a_regs[s], b_regs[s]
        for i, (x, y) in enumerate(zip(ra, rb)):
            if x != y:
                nm = names[i] if i < len(names) else "col%d" % i
                first.setdefault(nm, s)
                count[nm] = count.get(nm, 0) + 1
                bad_steps.add(s)
    state_bad = sorted(
        s for s in sorted(set(a_steps) & set(b_steps)) if a_steps[s][2] != b_steps[s][2]
    )
    return common, first, count, sorted(bad_steps), state_bad


# ---- the attribution model ------------------------------------------------------------------------
#
# ONE instrument mechanism explains the whole record-vs-replay residue, and it is measured rather
# than argued (LIB-REF-REC, 2026-09-11):
#
#   A periodic sim event enqueues an order BETWEEN the step's hash sample and dispatch entry. The
#   RECORD hashes the queue before that arrival; order_replay_inject() pre-loads the whole
#   dispatch-entry set at the top of on_sim_step, so the REPLAY hashes it one step early. Measured
#   with VALUES, not hashes (an rdump of the order_queue_count slice over every step): the record's
#   top-of-step count and its dispatch-entry count disagree at exactly 9 steps -- 75, 675, 1275,
#   1875, 2475, 3075, 3675, 4275, 4875 -- spaced 600 steps apart, i.e. every 60 s of game time.
#
# WHAT THAT PRODUCES, and why the two shapes are ONE mechanism:
#   * the QUEUE regions (order_queue, order_queue_count) disagree at the instant and for a few steps
#     after, while the queue re-converges;
#   * a DOWNSTREAM MARK appears within one step of an instant. Whether it heals is a property of the
#     STATE IT LANDS IN, not of the cause: the step-75 event marked `units` and healed in one step;
#     the step-675 event marked player_data[1].ai_tile_flags_grid -- a persistent AI influence map --
#     and it never heals. Same trigger, same perturbation, opposite-looking evidence.
#
# SO THE RULE IS FAIL-CLOSED ON ONSET, NOT ON PERSISTENCE. Every non-excluded mismatching region must
# either be a queue region inside an instant's window, or a mark whose ONSET is within one step of an
# instant AND which is DECLARED in the manifest with its localization. A mark nobody has localized
# cannot pass, and a new mark in a future re-record refuses until someone enumerates it -- which is
# the whole point of declaring them rather than deriving them.

QUEUE_REGIONS = ("order_queue", "order_queue_count")
# The queue re-converges within a few steps of an instant; 13 is the measured maximum over 5000
# steps. FIXED with margin rather than derived from the run under test -- a window that widens to fit
# its own data is a check that cannot fail.
QUEUE_WINDOW = 32
MARK_ONSET_SLACK = 1


def attribute(man, rec_regs, rep_regs, names, excluded):
    """The whole record-vs-replay residue, attributed. Returns (rows, problems, instants)."""
    common = sorted(set(rec_regs) & set(rep_regs))
    per = {}
    for s in common:
        for i, (x, y) in enumerate(zip(rec_regs[s], rep_regs[s])):
            if x != y:
                per.setdefault(names[i] if i < len(names) else "col%d" % i, []).append(s)

    instants = per.get("order_queue_count", [])
    declared = {m["region"]: m for m in man.get("fidelity", {}).get("marks", [])}
    rows, problems = [], []

    for nm in sorted(per, key=lambda n: per[n][0]):
        steps = per[nm]
        if nm in excluded:
            rows.append(
                (
                    nm,
                    steps,
                    "excluded: state_excluded() wall-clock/pacing, outside the state verdict",
                )
            )
            continue
        if nm in QUEUE_REGIONS:
            bad = [s for s in steps if not any(t <= s <= t + QUEUE_WINDOW for t in instants)]
            if bad:
                problems.append(
                    "%s mismatches at %s, outside every instant's %d-step window -- that residue is "
                    "NOT the known mechanism." % (nm, bad[:8], QUEUE_WINDOW)
                )
            worst = max((min([s - t for t in instants if t <= s] or [0]) for s in steps), default=0)
            rows.append(
                (
                    nm,
                    steps,
                    "instrument: the queue itself at the instants (max +%d steps before it "
                    "re-converges)" % worst,
                )
            )
            continue

        onset = steps[0]
        near = min((abs(onset - t) for t in instants), default=10**9)
        d = declared.get(nm)
        if near > MARK_ONSET_SLACK:
            problems.append(
                "%s onset at step %d is %d steps from the nearest instant -- outside the known "
                "mechanism, so it is NOT attributed." % (nm, onset, near)
            )
        if d is None:
            problems.append(
                "%s is a downstream mark the manifest does not declare. Localize it and add it to "
                "fidelity.marks (--accept-marks seeds the entry) -- an undeclared mark cannot pass."
                % nm
            )
        off = (onset - min(instants, key=lambda t: abs(onset - t))) if instants else 0
        rows.append(
            (
                nm,
                steps,
                "instrument mark: onset %d (instant %+d) -- %s"
                % (onset, off, (d or {}).get("localization", "UNDECLARED")),
            )
        )

    return rows, problems, instants


INJECTOR_EVIDENCE_RE = re.compile(
    r"injector: count SET at (\d+) of (\d+) arrival step\(s\), (\d+) append-without-set"
)


def injector_evidence(log_path):
    """-> {set_ok, arrivals, no_set} from a replay log's unconditional injector line, or None.

    The line is emitted by harness.cpp's replay stop block and by libref_host's run summary, in the
    same wording so one parser reads both. LAST match wins: a log can hold more than one run's tail
    only if something went wrong, and the last is the one that finished.
    """
    if not log_path or not os.path.isfile(log_path):
        return None
    hit = None
    for m in INJECTOR_EVIDENCE_RE.finditer(_read(log_path).decode("latin1")):
        hit = m
    if hit is None:
        return None
    return {"set_ok": int(hit.group(1)), "arrivals": int(hit.group(2)), "no_set": int(hit.group(3))}


def recording_arrival_steps(fixture, man):
    """-> how many DISTINCT steps the committed recording carries orders at."""
    raw = _read_z(os.path.join(fixture, man["artifacts"]["orders"]["file"]))
    return len({int.from_bytes(raw[o : o + 4], "little") for o in range(8, len(raw), 72)})


def cmd_verify(args):
    fixture = args.fixture_dir or DEFAULT_FIXTURE
    man, problems = integrity(fixture)
    print("=== fixture integrity ===")
    if problems:
        for p in problems:
            print("  !! " + p)
    else:
        print(
            "  ok: %d artifact(s), every one decompresses to its manifest sha256"
            % len(man["artifacts"])
        )

    # TL-GATE8: every run this verify would compare must be of the fixture's hash-input epoch.
    runs = list(getattr(args, "live_log", None) or [])
    if getattr(args, "replay_log", None):
        runs.append(args.replay_log)
    if getattr(args, "against", None):
        runs.append(os.path.join(args.against, "mh_harness.log"))
    refusals = [(p, epoch_refusal(man, p)) for p in runs]
    refusals = [(p, r) for p, r in refusals if r]
    if refusals:
        for p, r in refusals:
            print("  !! REFUSED %s: %s" % (p, r))
        print("\nfixture_replay verify: REFUSED (nothing compared)")
        return 2

    committed_step_bytes = _read_z(os.path.join(fixture, man["artifacts"]["hash_steps"]["file"]))
    committed_region_bytes = _read_z(
        os.path.join(fixture, man["artifacts"]["hash_regions"]["file"])
    )
    committed_steps, committed_regs = parse_stream(committed_step_bytes, committed_region_bytes)
    ok = not problems
    live = "live_contract" in man

    # ---- LIVE fixtures: a different set of arms, because the replay ones have no live analogue ----
    #
    # WHAT IS DELIBERATELY NOT CHECKED HERE, stated rather than silently skipped:
    #   * the 0 -> n counts guard is about the INJECTOR's count gate -- whether a step the replay
    #     injects into could have opened dispatch. Nothing is injected in a live run, so the question
    #     does not exist; running it would report a measurement of a mechanism that is absent.
    #   * the record-vs-replay fidelity attribution compares two DIFFERENT kinds of run that sample
    #     order_queue at different instants. A live fixture has only one kind of run, so there is no
    #     second sampling point to attribute and no residue to declare.
    # What replaces both is stronger for this artifact: TWO hosted live runs must each reproduce the
    # committed stream bit-for-bit. That is phase 0's self-consistency gate, and it is the thing the
    # whole standalone comparison rests on -- a hosted live run that is not deterministic makes every
    # later number meaningless.
    if live:
        logs = list(args.live_log or [])
        print("\n=== self-consistency: TWO hosted LIVE runs vs the committed stream ===")
        if len(logs) < 2:
            print(
                "  !! %d live log(s) supplied -- this arm needs TWO, from two separate hosted live "
                "runs. One run agreeing with itself proves nothing." % len(logs)
            )
            ok = False
        for p in logs:
            s2, r2 = split_stream(_read(p))
            same = (s2 == committed_step_bytes, r2 == committed_region_bytes)
            print(
                "  %-52s per-step %s  per-region %s"
                % (
                    os.path.basename(os.path.dirname(p)) or p,
                    "BIT-IDENTICAL" if same[0] else "DIFFER",
                    "BIT-IDENTICAL" if same[1] else "DIFFER",
                )
            )
            if not all(same):
                ok = False
        print("\n=== fidelity ===")
        print(
            "  n/a for a LIVE fixture: there is one kind of run, so no record-vs-replay sampling "
            "residue exists to attribute. The counts guard is n/a for the same reason -- nothing is "
            "injected, so no count gate can fail to open."
        )
        print("\nfixture_replay verify: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    # ---- the recorder-side 0 -> n guard ----------------------------------------------------------
    # A step whose top-of-step queue count is 0 but whose dispatch-entry count is not would never
    # open the replay's count gate (dispatch is count-gated, and with the enqueue suppressed nothing
    # else can raise it), so its orders would silently never execute. Measured ZERO here over 5000
    # steps; the manifest records that measurement and this refuses a fixture that lacks it.
    g = man.get("fidelity", {}).get("counts_guard")
    print("\n=== recorder guard: no 0 -> n step ===")
    if not g or not g.get("checked"):
        print(
            "  !! the manifest carries no counts_guard measurement -- run `counts-guard` against a "
            "record run carrying an rdump of the order_queue_count slice over every step"
        )
        ok = False
    elif g.get("zero_to_n_steps"):
        # ---- THE EVIDENCE-GATED EXCEPTION (conductor ruling, 2026-09-12) -------------------------
        #
        # THE REFUSED CLASS IS REAL AND STAYS REFUSED. An injector that APPENDS at the count without
        # SETTING it leaves a `0 -> n` step's count at 0; dispatch is count-gated, so it never runs
        # and those orders vanish with nothing in any log saying so. That is what this guard is for.
        #
        # WHAT WAS FALSIFIED IS THE PREMISE THAT `0 -> n` IMPLIES IT. The guard used to say "the
        # replay could not open the count gate for them", which is a claim about the REPLAYER, not
        # about the fixture -- and it is false for the injector actually in use, which writes
        # `count = k`. Measured on libref-replay-aisoak-v1 at arrival step 75 with the count ledger
        # (LIB-REF-SOAK, 2026-09-12): tag 16 `k=1, count=1` (the gate IS opened by the injector),
        # tag 2 the dispatcher walking that record, tag 14 `entry 1 -> kept 0` (executed and
        # consumed). `0 -> n` there means only that the queue is EMPTY BETWEEN BURSTS, which is what
        # a sparse all-AI order stream looks like; the dense sp_det fixture always had leftovers, so
        # its arrivals were never `0 -> n` and this assumption was never tested.
        #
        # SO THE EXCEPTION IS GATED ON PER-RUN MEASURED EVIDENCE, NOT ON THIS COMMENT. The replay
        # emits, unconditionally and independently of any trace window, a line carrying a READBACK
        # of the count it set at every arrival step. The exception applies only when that line is
        # present, reports zero append-without-set, and its arrival count MATCHES the number of
        # order-carrying steps in the committed recording. No line, no exception -- absent evidence
        # is not favourable evidence.
        ev = injector_evidence(args.replay_log) if args.replay_log else None
        want_arrivals = recording_arrival_steps(fixture, man)
        if ev is None:
            print(
                "  !! %d step(s) go 0 -> n: %s -- and this run carries NO injector evidence line, "
                "so the exception cannot apply. Pass --replay-log from a replay built after "
                "2026-09-12, or the fixture stays refused."
                % (len(g["zero_to_n_steps"]), g["zero_to_n_steps"][:8])
            )
            ok = False
        elif ev["no_set"] != 0 or ev["set_ok"] != ev["arrivals"]:
            print(
                "  !! %d step(s) go 0 -> n AND the injector reports %d append-without-set (count "
                "SET at %d of %d arrivals) -- this is the refused class itself, not the exception."
                % (len(g["zero_to_n_steps"]), ev["no_set"], ev["set_ok"], ev["arrivals"])
            )
            ok = False
        elif ev["arrivals"] != want_arrivals:
            print(
                "  !! %d step(s) go 0 -> n and the injector evidence describes %d arrival step(s), "
                "but the committed recording carries orders at %d step(s) -- the evidence is from a "
                "different run or a different fixture, so it cannot license this one."
                % (len(g["zero_to_n_steps"]), ev["arrivals"], want_arrivals)
            )
            ok = False
        else:
            print(
                "  ok (EVIDENCE-GATED): %d step(s) go 0 -> n, admitted because the replay MEASURED "
                "the count set at all %d arrival step(s) with 0 append-without-set, and that "
                "matches the committed recording's %d order-carrying step(s)."
                % (len(g["zero_to_n_steps"]), ev["arrivals"], want_arrivals)
            )
            print("     mid-step arrival instants: %s" % g.get("midstep_arrival_steps"))
    else:
        print(
            "  ok: 0 step(s) go 0 -> n, over %d step(s) measured in %s"
            % (g.get("steps_measured", 0), g.get("source_run", "?"))
        )
        print("     mid-step arrival instants: %s" % g.get("midstep_arrival_steps"))

    # ---- self-consistency: a REPLAY of the COMMITTED form reproduces the committed stream ---------
    if args.replay_log:
        s2, r2 = split_stream(_read(args.replay_log))
        print("\n=== self-consistency: replay of the COMMITTED form vs the committed stream ===")
        same = (s2 == committed_step_bytes, r2 == committed_region_bytes)
        print("  per-step lines   : %s" % ("BIT-IDENTICAL" if same[0] else "DIFFER"))
        print("  per-region lines : %s" % ("BIT-IDENTICAL" if same[1] else "DIFFER"))
        if not all(same):
            ok = False

    # ---- fidelity: record-vs-replay, fully attributed, fail-closed --------------------------------
    table = []
    if args.against:
        rs, rr = split_stream(_read(os.path.join(args.against, "mh_harness.log")))
        a_steps, a_regs = parse_stream(rs, rr)
        names = man["compare"]["region_columns"]
        excluded = excluded_names()
        rows, aproblems, instants = attribute(man, a_regs, committed_regs, names, excluded)
        common = sorted(set(a_regs) & set(committed_regs))
        print("\n=== fidelity: RECORD vs the committed REPLAY stream (fail-closed attribution) ===")
        print("  compared steps: %d" % len(common))
        print("  instants (order_queue_count disagrees): %s" % instants)
        if len(instants) > 1:
            sp = sorted({instants[i + 1] - instants[i] for i in range(len(instants) - 1)})
            print("  instant spacing: %s%s" % (sp, "  (periodic)" if len(sp) == 1 else ""))
        for nm, steps, why in rows:
            print("  %-22s %5d step(s)  %s" % (nm, len(steps), why))
            if len(steps) <= 40:
                print("      steps: %s" % steps)
            else:
                print(
                    "      onset %d, then every step to %d (a mark on persistent state does not heal)"
                    % (steps[0], steps[-1])
                )
            table.append({"region": nm, "count": len(steps), "steps": steps, "attribution": why})
        print(
            "  identical across all %d steps: %d of %d regions"
            % (len(common), len(names) - len(rows), len(names))
        )
        for p in aproblems:
            print("  !! " + p)
        if aproblems:
            ok = False
        if args.accept_marks:
            prev = {m["region"]: m for m in man.get("fidelity", {}).get("marks", [])}
            marks = []
            for nm, steps, why in rows:
                if nm in excluded or nm in QUEUE_REGIONS:
                    continue
                marks.append(
                    {
                        "region": nm,
                        "onset_step": steps[0],
                        "steps": len(steps),
                        "persists": len(steps) > 1
                        and steps == list(range(steps[0], steps[-1] + 1)),
                        "localization": prev.get(nm, {}).get("localization", "TODO: localize"),
                    }
                )
            man.setdefault("fidelity", {})["marks"] = marks
            with open(manifest_path(fixture), "w", encoding="utf-8", newline="\n") as fh:
                json.dump(man, fh, indent=1, ensure_ascii=False)
                fh.write("\n")
            print("  (--accept-marks: wrote %d mark(s) into the manifest)" % len(marks))

    if args.write_table and table:
        write_fidelity_table(fixture, man, table)

    print("\nfixture_replay verify: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def write_fidelity_table(fixture, man, table):
    """The committed fidelity statement -- the tool's OUTPUT, never hand-typed."""
    marks = {m["region"]: m for m in man.get("fidelity", {}).get("marks", [])}
    g = man.get("fidelity", {}).get("counts_guard", {})
    n_regions = len(man["compare"]["region_columns"])
    out = [
        "# Fidelity: the committed REPLAY trajectory vs the live RECORD it came from",
        "",
        "GENERATED by `python tools/fixture_replay.py verify --against <record_run> --write-table`.",
        "Do not hand-edit -- it is the acceptance's own output, and a hand-edited table is a claim",
        "nobody re-measured.",
        "",
        "## What this is, and what it is NOT",
        "",
        "LIB-REF's oracle compares a STANDALONE replay against the IN-BINARY replay of this same",
        "fixture. That comparison is bit-identical on all %d regions at every step -- replay vs"
        % n_regions,
        "replay, measured -- so it needs NO exclusions and nothing in this file takes anything away",
        "from it. In particular `p1_local` is inside LIB-REF's comparison, not outside it.",
        "",
        "This file is the separate, weaker statement: how close the committed replay trajectory is to",
        "the LIVE run it was recorded from. Its entire residue is one measured instrument mechanism.",
        "",
        "## The mechanism",
        "",
        "A periodic sim event enqueues an order BETWEEN the step's hash sample and dispatch entry.",
        "The record hashes the queue before that arrival; `order_replay_inject()` pre-loads the whole",
        "dispatch-entry set at the top of `on_sim_step`, so the replay hashes it one step early.",
        "",
        "Measured with VALUES rather than hashes (an rdump of the `order_queue_count` slice over every",
        "step of a record run whose recording is byte-identical to the committed one): the top-of-step",
        "count and the dispatch-entry count disagree at",
        "",
        "    %s" % (g.get("midstep_arrival_steps"),),
        "",
        "-- spaced 600 steps, i.e. every 60 s of game time. Steps going `0 -> n`, which the replay's",
        "count gate could never open: **%d**." % len(g.get("zero_to_n_steps") or []),
        "",
        "## The residue, enumerated",
        "",
        "| region | steps | attribution |",
        "| --- | --- | --- |",
    ]
    for row in table:
        steps = row["steps"]
        s = (
            ", ".join(str(x) for x in steps)
            if len(steps) <= 40
            else "onset %d, then every step to %d" % (steps[0], steps[-1])
        )
        out.append("| `%s` | %d | %s |" % (row["region"], row["count"], row["attribution"]))
        out.append("| | | steps: %s |" % s)
    out += ["", "## The marks, localized", ""]
    for nm, mk in marks.items():
        out += [
            "**`%s`** -- onset step %d, %d step(s)%s."
            % (nm, mk["onset_step"], mk["steps"], ", persists" if mk.get("persists") else ""),
            "",
            mk.get("localization", "TODO"),
            "",
        ]
    out += [
        "## The keeper",
        "",
        '"Blip vs persists" is a proxy for the transience of the STATE, not for the size of the',
        "cause. One perturbation of a persistent map (an AI influence grid) is indistinguishable",
        "under that test from a systematic divergence, while the same perturbation of transient state",
        "(a unit's position) self-heals in a step and reads as benign. Both shapes here come from the",
        "SAME trigger at the SAME periodic instants -- steps 75 and 675 -- and differ only in where",
        "the mark landed. Judge a divergence by its mechanism and its blast radius, never by whether",
        "it healed.",
        "",
    ]
    p = os.path.join(fixture, "FIDELITY.md")
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out))
    print("  wrote %s" % os.path.relpath(p, REPO))


def cmd_counts_guard(args):
    """Measure the mid-step arrivals and the 0 -> n guard from a record run that carried an rdump of
    the order_queue_count slice over every step, and record the measurement in the manifest."""
    import collections

    fixture = args.fixture or DEFAULT_FIXTURE
    man = load_manifest(fixture)
    top = {}
    for ln in open(os.path.join(args.run_dir, "mh_harness.log"), "r", errors="replace"):
        if not ln.startswith("RX "):
            continue
        p = ln.split()
        if p[2] != str(args.rid):
            continue
        top[int(p[1])] = int.from_bytes(bytes.fromhex(p[4]), "little")
    if not top:
        sys.exit("counts-guard: that run carries no RX rows for slice %d" % args.rid)
    d = _read(os.path.join(args.run_dir, "mh_orders.bin"))
    disp = collections.Counter(int.from_bytes(d[o : o + 4], "little") for o in range(8, len(d), 72))

    # THE RECORDING MUST BE THE FIXTURE'S. A guard measured against a different recording describes a
    # different run; recordings are deterministic here, so this is checkable rather than assumed.
    fx_orders = _read_z(os.path.join(fixture, man["artifacts"]["orders"]["file"]))
    if sha256(d) != sha256(fx_orders):
        sys.exit(
            "counts-guard: %s/mh_orders.bin is NOT byte-identical to the committed recording -- the "
            "guard would describe a different run. Refused." % args.run_dir
        )

    steps = sorted(top)
    mid = [s for s in steps if disp.get(s, 0) > 0 and top[s] != disp.get(s, 0)]
    zero_to_n = [s for s in mid if top[s] == 0]
    man.setdefault("fidelity", {})["counts_guard"] = {
        "checked": True,
        "source_run": os.path.relpath(args.run_dir, REPO).replace("\\", "/"),
        "source_orders_sha256": sha256(d),
        "steps_measured": len(steps),
        "midstep_arrival_steps": mid,
        "zero_to_n_steps": zero_to_n,
        "why": "A step whose top-of-step queue count is 0 but whose dispatch-entry count is not "
        "would never open the replay's count gate (dispatch is count-gated and the suppressed "
        "enqueue cannot raise it), so its orders would silently never execute. Measured, not assumed.",
    }
    with open(manifest_path(fixture), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(man, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print(
        "counts-guard: %d step(s) measured, %d mid-step arrival(s) %s, %d of them 0 -> n"
        % (len(steps), len(mid), mid, len(zero_to_n))
    )


def cmd_check(args):
    """Integrity only -- no rig, no game. This is the lint-able half."""
    fixture = args.fixture_dir or DEFAULT_FIXTURE
    if not os.path.isfile(manifest_path(fixture)):
        print("ok: no committed fixture at %s (nothing to check)" % os.path.relpath(fixture, REPO))
        return 0
    man, problems = integrity(fixture)
    if problems:
        for p in problems:
            print("  !! " + p)
        return 1
    tot = sum(a["zlib_bytes"] for a in man["artifacts"].values())
    print(
        "ok: LIB-REF %s fixture v%d -- %d steps, %d artifact(s), %d committed bytes, every "
        "sha256 verified"
        % (
            "LIVE-LOOP" if "live_contract" in man else "replay",
            man["version"],
            man["steps"],
            len(man["artifacts"]),
            tot,
        )
    )
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack")
    p.add_argument("run_dir")
    p.add_argument("--out")
    p.add_argument("--seed", type=int)
    p.add_argument("--commit", default="")
    p.add_argument("--recorded", default="")
    p.add_argument("--script", default="sp_det.txt")
    p.add_argument("--host", default="")
    p.add_argument("--peers", default="")
    p.add_argument("--record-flags", type=json.loads, default=None)
    p.add_argument(
        "--live",
        action="store_true",
        help="pack a LIVE-LOOP fixture (LIB-REF-LIVE): no order stream, live_contract in place "
        "of replay_contract. Refuses a run dir that HAS an mh_orders.bin -- the absence is the "
        "artifact's claim, so a run made under a recording arrangement is the wrong input.",
    )
    p.set_defaults(fn=cmd_pack)

    p = sub.add_parser("stream")
    p.add_argument("run_dir")
    p.add_argument("--fixture")
    p.set_defaults(fn=cmd_stream)

    p = sub.add_parser("unpack")
    p.add_argument("fixture_dir", nargs="?")
    p.add_argument("--lane", required=True)
    p.add_argument("--world")
    p.set_defaults(fn=cmd_unpack)

    p = sub.add_parser("replay")
    p.add_argument("fixture_dir", nargs="?")
    p.add_argument("--lane", required=True)
    p.add_argument("--port", type=int, default=6633)
    p.add_argument("--timeout-frames", type=int, default=400000)
    p.add_argument("--timeout", type=int, default=2400)
    p.add_argument("--world")
    p.set_defaults(fn=cmd_replay)

    p = sub.add_parser("verify")
    p.add_argument("fixture_dir", nargs="?")
    p.add_argument("--against", help="a RECORD run dir -- runs the fail-closed attribution")
    p.add_argument(
        "--replay-log", help="a replay's mh_harness.log -- runs the self-consistency arm"
    )
    p.add_argument(
        "--live-log",
        action="append",
        help="a hosted LIVE run's mh_harness.log. Pass it TWICE (two separate runs): a live "
        "fixture's self-consistency arm is two live runs each reproducing the committed stream, "
        "and one run agreeing with itself proves nothing.",
    )
    p.add_argument("--write-table", action="store_true", help="emit FIDELITY.md beside the fixture")
    p.add_argument(
        "--accept-marks",
        action="store_true",
        help="seed fidelity.marks from what this run observed. A DELIBERATE act, like re-recording "
        "a baseline: it makes an undeclared mark declared, and the localization still has to be "
        "written by a human who looked.",
    )
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("counts-guard")
    p.add_argument("run_dir", help="a RECORD run with rdump over the order_queue_count slice")
    p.add_argument("--fixture")
    p.add_argument("--rid", type=int, default=42, help="the order_queue_count hash-slice index")
    p.set_defaults(fn=cmd_counts_guard)

    p = sub.add_parser("check")
    p.add_argument("fixture_dir", nargs="?")
    p.set_defaults(fn=cmd_check)

    args = ap.parse_args()
    rc = args.fn(args)
    sys.exit(rc if isinstance(rc, int) else 0)


if __name__ == "__main__":
    main()
