#!/usr/bin/env python3
"""fixture_replay.py -- the LIB-REF replay FIXTURE: pack it, verify it, replay it (tracker LIB-REF-REC).

The fixture carries ONE run's clock track, step-0 world and complete hash stream.
Replay fixtures additionally carry MHOR v2: exact HASH and post-AI/pre-count-gate
ENTRY queue snapshots at every recorded step, including explicit empty queues.
Replay keeps AI and real enqueues ON, replaces HASH before hashing and ENTRY
before the conditional dispatch gate. In-pass generated orders execute naturally.
Neither a count-only prefix guess nor old v1 data can certify faithful replay.

The committed stream is independently compared against the recording with no
sampling-instant allowances. All non-excluded regions and state hashes must
match at every declared step. Live fixtures have no order stream and no injection;
two independent hosted live runs must reproduce their committed stream.

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

import mh_order_stream as order_stream

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
    "replay_suppress_enqueue": 0,
    "replay_dispatch_inject": 1,
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
    "replay_ai_off": 0,
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


def parse_kv(text: str) -> dict:
    """`k=v;k=v` (the --net-extra syntax) -> {k: int-or-str}. Ints stay ints so the manifest reads
    as the other flag dicts do."""
    out = {}
    for kv in (text or "").split(";"):
        kv = kv.strip()
        if not kv:
            continue
        if "=" not in kv:
            sys.exit("fixture_replay: %r is not k=v" % kv)
        k, v = (x.strip() for x in kv.split("=", 1))
        out[k] = int(v) if re.fullmatch(r"-?\d+", v) else v
    return out


def run_config_mismatches(run_dir: str, net: dict, harness: dict) -> list:
    """What the run's own effective_config.json says differs from the settings a manifest is about
    to claim for it. The pack states what the recording ran under; this is where that statement is
    checked against the run instead of taken from the command line. [] when they agree."""
    p = os.path.join(run_dir, "effective_config.json")
    if not os.path.isfile(p):
        return [
            "%s has no effective_config.json -- the claimed settings cannot be checked" % run_dir
        ]
    with open(p, encoding="utf-8") as fh:
        cfg = json.load(fh).get("config", {})
    bad = []
    for section, want in (("net", net), ("harness", harness)):
        got = cfg.get(section, {})
        for k, v in want.items():
            if str(got.get(k)) != str(v):
                bad.append("[%s] %s: run has %r, claimed %r" % (section, k, got.get(k), v))
    return bad


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


def kind_refusal(man, log_path):
    """tooling:TL-HARN-INCHASH: the named refusal when `log_path` was hashed with another hash KIND than
    the fixture's stream (an unstamped fixture is kind 1 -- nothing else existed before the item)."""
    import mp_analyze as _m

    return _m.kind_mismatch(
        (man.get("step0") or {}).get("hash_kind"),
        _m.harness_hash_kind(log_path),
        "the fixture",
        os.path.basename(os.path.dirname(log_path) or log_path) or log_path,
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
    # tooling:TL-HARN-INCHASH: a fixture's stream is judged by libref_host, which re-derives kind 1
    # (the FNV VERDICT walk) and nothing else -- a kind-2 stream would fail there at every step.
    import mp_analyze as _m

    kind = _m.kind_of(_m.harness_hash_kind(os.path.join(run, "mh_harness.log")))
    if kind != 1:
        sys.exit(
            "fixture_replay pack: REFUSED -- %s/mh_harness.log is %s; a fixture's stream is re-derived "
            "by libref_host, which hashes kind 1 only. Re-record with `[harness] hash_kind=1`."
            % (run, _m.kind_label(kind))
        )
    clock_raw = _read(halves["clock"])
    if len(clock_raw) % 8 or len(clock_raw) // 8 == 0:
        sys.exit("fixture_replay pack: truncated or empty clock track")
    if not live:
        order_stream.decode(_read(halves["orders"]), first_step=1, steps=len(clock_raw) // 8)
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
    if n_steps != len(clock_raw) // 8:
        sys.exit("fixture_replay pack: hash/clock step counts disagree")

    # THE STEP REGIME is part of the contract too (2026-09-30, the -v2 fixtures): a recording made
    # with the harness's fixed_step pin and a [net] sim_step_ms / rig_fixed_step_loop other than the
    # rig's pinned defaults integrates at a different step, so a replay under the defaults is a
    # different run. Both are checked against the run's own effective_config.json before they are
    # written, and `replay` passes the [net] half back.
    net = parse_kv(getattr(args, "net", "") or "")
    fixed_step = getattr(args, "fixed_step", None)
    claimed_h = {} if fixed_step is None else {"fixed_step": fixed_step}
    if net or claimed_h:
        bad = run_config_mismatches(run, net, claimed_h)
        if bad:
            sys.exit("fixture_replay pack: REFUSED -- %s" % "; ".join(bad))

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
            "order_stream": dict(order_stream.CONTRACT),
            "record_flags": dict(args.record_flags or {}),
            "semantics": [
                "AI runs and every real source enqueue executes, including overflow/loss effects.",
                "MHOR v2 HASH is the exact queue at the existing hash sample.",
                "MHOR v2 ENTRY replaces the queue after AI and BEFORE the count gate, even when empty.",
                "Only nonempty ENTRY dispatches; handler-generated orders execute once naturally.",
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
            "hash_kind": kind,  # TL-HARN-INCHASH: 1 = FNV walk (the only kind libref_host knows)
        },
        "artifacts": artifacts,
    }

    if fixed_step is not None:
        man["replay_contract"]["flags"]["fixed_step"] = fixed_step
    if net:
        man["replay_contract"]["net"] = dict(net)
        man["replay_contract"]["semantics"].append(
            "net: the [net] keys the recording ran under (checked against its effective_config.json "
            "at pack). They set the step regime -- sim_step_ms is SIM_STEP_INTERVAL, and "
            "rig_fixed_step_loop=1 runs sim_tick's mode-3 fixed-step loop in single-player (one "
            "step per frame, delta = interval exactly). `replay` passes them back as --net-extra."
        )

    if live:
        # The live fixture's contract REPLACES the replay one rather than sitting beside it: a
        # manifest carrying both would let a reader take the wrong half as authoritative, and the two
        # describe mutually exclusive arrangements.
        rc = man.pop("replay_contract", None) or {}
        man["live_contract"] = {
            "flags": dict(LIVE_FLAGS, **({} if fixed_step is None else {"fixed_step": fixed_step})),
            "scenario_script": args.script,
            "semantics": [
                "NO ORDER STREAM EXISTS IN THIS FIXTURE. The in-sim AI is the SOLE order source; "
                "no HASH or ENTRY replacement runs. The standalone host must reproduce this live "
                "enqueue/issue/dispatch loop bit-for-bit.",
                "all_ai=1: every player slot is an AI, including the local one (a pre-landing "
                "status_flags flip, so it spawns through llm_strat_spawn_ai_base like any computer "
                "player rather than ticking with no home tile).",
                "synth_move=0: the harness's synthetic workload is EXPLICITLY disarmed. It is the "
                "soak's own default here, but it is stated because a workload order would be a "
                "second order source and would make the sole-source claim false -- which is "
                "precisely how the replay fixture's step-5000 residual happened.",
                "replay_suppress_enqueue=0: suppression is OFF, as in faithful MHOR v2 replay.",
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
        if args.record_flags:
            man["live_contract"]["record_flags"] = dict(args.record_flags)
        if net:
            man["live_contract"]["net"] = dict(net)
            man["live_contract"]["semantics"].append(rc["semantics"][-1])

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
    """Add a complete, phase-validated replay or live stream; independently verify against record."""
    fixture = args.fixture or DEFAULT_FIXTURE
    man = load_manifest(fixture)
    refusal = order_stream.contract_refusal(man)
    if refusal:
        sys.exit("fixture_replay stream: REFUSED -- " + refusal)
    log = _read(os.path.join(args.run_dir, "mh_harness.log"))
    bad = epoch_refusal(man, os.path.join(args.run_dir, "mh_harness.log")) or kind_refusal(
        man, os.path.join(args.run_dir, "mh_harness.log")
    )
    if bad:
        sys.exit("fixture_replay stream: REFUSED -- %s" % bad)
    if "replay_contract" in man:
        refusal = order_stream.receipt_refusal(log.decode("latin1"), man["steps"])
        if refusal:
            sys.exit("fixture_replay stream: REFUSED -- " + refusal)
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
    refusal = order_stream.contract_refusal(man)
    if refusal:
        sys.exit("fixture_replay unpack: REFUSED -- " + refusal)
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
        if name == "orders":
            order_stream.decode(raw, first_step=1, steps=man["steps"])
        if name == "clock" and len(raw) != man["steps"] * 8:
            sys.exit("fixture_replay unpack: clock length does not match declared steps")
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
    # tooling:TL-HARN-INCHASH: the replay hashes with the FIXTURE's kind, whatever the harness default
    # is -- unstamped fixtures are kind 1.
    flags.setdefault("hash_kind", (man.get("step0") or {}).get("hash_kind") or 1)
    extra = ";".join("%s=%s" % (k, v) for k, v in flags.items())
    # The recording's [net] step regime rides back (see `pack --net`); absent on older fixtures.
    net = man["replay_contract"].get("net") or {}
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
    if net:
        argv += ["--net-extra", ";".join("%s=%s" % (k, v) for k, v in net.items())]
    print("  $ " + " ".join(argv[1:]))
    before = (
        set(
            _n
            for _n in os.listdir(os.path.join(args.lane, "logs"))
            if os.path.isdir(os.path.join(os.path.join(args.lane, "logs"), _n))
        )
        if os.path.isdir(os.path.join(args.lane, "logs"))
        else set()
    )
    r = subprocess.run(argv, cwd=REPO)
    if r.returncode != 0:
        sys.exit(
            "fixture_replay replay: the run FAILED (exit %d) -- re-run, do not massage"
            % r.returncode
        )
    after = set(
        _n
        for _n in os.listdir(os.path.join(args.lane, "logs"))
        if os.path.isdir(os.path.join(os.path.join(args.lane, "logs"), _n))
    )
    # THE PROCESS DIRECTORY, not the session one. Since the per-SESSION log split a launch leaves
    # two new directories -- `<ts>_menu_solo` (per process: the harness log, the recording halves)
    # and `<ts>_<mid8>_<map>_<mode>` (per session: mh_net.log, session.json; SES1 spelled it
    # `<ts>_<mid8>_0_solo`) -- and the session dir sorts LAST. Picking `new[-1]` handed `stream` a directory with no mh_harness.log
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
    bad = run_config_mismatches(rd, net, {}) if net else []
    if bad:
        sys.exit(
            "fixture_replay replay: the run did not get the fixture's [net]: %s" % "; ".join(bad)
        )
    return rd


# ---- verify --------------------------------------------------------------------------------------


def integrity(fixture: str):
    """Every committed artifact decompresses to the bytes the manifest names. Returns problems."""
    man = load_manifest(fixture)
    problems = []
    refusal = order_stream.contract_refusal(man)
    if refusal:
        problems.append(refusal)
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
        if name == "orders":
            try:
                order_stream.decode(raw, first_step=1, steps=man["steps"])
            except order_stream.Error as e:
                problems.append("orders: " + str(e))
        if name == "clock" and len(raw) != man["steps"] * 8:
            problems.append("clock: length does not match declared step count")
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


# ---- exact record/replay comparison --------------------------------------------------------------

QUEUE_REGIONS = ("order_queue", "order_queue_count")


def attribute(man, rec_regs, rep_regs, names, excluded):
    """No v1 sampling-instant allowances survive the exact two-phase contract."""
    expected = set(range(1, man["steps"] + 1))
    problems = []
    for label, regs in (("record", rec_regs), ("replay", rep_regs)):
        if set(regs) != expected:
            problems.append("%s does not cover every declared step" % label)
        if any(len(row) != len(names) for row in regs.values()):
            problems.append("%s region row width differs from manifest" % label)
    per = {}
    for step in sorted(expected & set(rec_regs) & set(rep_regs)):
        for name, a, b in zip(names, rec_regs[step], rep_regs[step]):
            if a != b:
                per.setdefault(name, []).append(step)
    rows = []
    for name, steps in per.items():
        why = "excluded pacing region" if name in excluded else "UNEXPLAINED divergence"
        rows.append((name, steps, why))
        if name not in excluded:
            problems.append("%s differs at %d step(s), first %d" % (name, len(steps), steps[0]))
    return rows, problems, per.get("order_queue_count", [])


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
    # TL-HARN-INCHASH: ...and of the fixture's hash KIND.
    refusals = [(p, epoch_refusal(man, p) or kind_refusal(man, p)) for p in runs]
    refusals = [(p, r) for p, r in refusals if r]
    if refusals:
        for p, r in refusals:
            print("  !! REFUSED %s: %s" % (p, r))
        print("\nfixture_replay verify: REFUSED (nothing compared)")
        return 2
    if problems:
        print("fixture_replay verify: REFUSED (artifact integrity/phase contract)")
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

    print("\n=== exact HASH/ENTRY coverage and pre-count-gate replacement ===")
    g = man.get("fidelity", {}).get("counts_guard", {})
    if not g.get("checked") or g.get("steps_measured") != man["steps"]:
        print("  !! missing complete two-phase counts-guard measurement")
        ok = False
    if not args.replay_log:
        print("  !! --replay-log is required: hashes alone do not establish actual phase execution")
        ok = False
    else:
        refusal = order_stream.receipt_refusal(
            _read(args.replay_log).decode("latin1"), man["steps"]
        )
        if refusal:
            print("  !! " + refusal)
            ok = False
    if not args.against:
        print("  !! --against RECORD_RUN is required for independent record/replay fidelity")
        ok = False

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
        expected = set(range(1, man["steps"] + 1))
        if set(a_steps) != expected or set(committed_steps) != expected:
            aproblems.append("record/replay step hash coverage is incomplete")
        state_bad = [
            s
            for s in expected & set(a_steps) & set(committed_steps)
            if a_steps[s][0] != committed_steps[s][0] or a_steps[s][2] != committed_steps[s][2]
        ]
        if state_bad:
            aproblems.append(
                "clock/state differs at %d step(s), first %d" % (len(state_bad), min(state_bad))
            )
        print("\n=== fidelity: exact RECORD vs committed REPLAY (no sampling allowances) ===")
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

    if args.write_table and args.against and ok:
        write_fidelity_table(fixture, man, table)

    print("\nfixture_replay verify: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def write_fidelity_table(fixture, man, table):
    """Generated only after complete independent exact record/replay proof."""
    guard = man.get("fidelity", {}).get("counts_guard", {})
    out = [
        "# Fidelity: exact MHOR v2 record/replay",
        "",
        "GENERATED by fixture_replay.py verify --against RECORD_RUN --replay-log LOG --write-table.",
        "All declared steps, clocks, state hashes and non-excluded region columns matched.",
        "HASH and ENTRY are distinct exact input snapshots, including explicit empty queues.",
        "AI and real enqueue paths ran; ENTRY replacement occurred before the conditional count gate.",
        "No prefix inference, synthetic empty dispatch or sampling-instant residue allowance applies.",
        "",
        "Measured steps: %d; HASH-to-ENTRY count changes: %s; zero-to-n: %s."
        % (
            guard.get("steps_measured", 0),
            guard.get("midstep_arrival_steps", []),
            guard.get("zero_to_n_steps", []),
        ),
        "",
        "| region | differing steps | disposition |",
        "| --- | --- | --- |",
    ]
    out.extend("| `%s` | %d | %s |" % (r["region"], r["count"], r["attribution"]) for r in table)
    path = os.path.join(fixture, "FIDELITY.md")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")
    print("  wrote %s" % os.path.relpath(path, REPO))


def cmd_counts_guard(args):
    """Measure both exact phase counts; zero-to-n is valid, missing ENTRY is not."""
    fixture = args.fixture or DEFAULT_FIXTURE
    man = load_manifest(fixture)
    raw = _read(os.path.join(args.run_dir, "mh_orders.bin"))
    samples = order_stream.decode(raw, first_step=1, steps=man["steps"])
    fixture_raw = _read_z(os.path.join(fixture, man["artifacts"]["orders"]["file"]))
    if raw != fixture_raw:
        sys.exit("counts-guard: recording is not byte-identical to the committed order stream")
    counts = order_stream.phase_counts(samples)
    h, e = counts[order_stream.HASH], counts[order_stream.ENTRY]
    mid = [s for s in h if h[s] != e[s]]
    zero_to_n = [s for s in h if h[s] == 0 and e[s] != 0]
    guard = {
        "checked": True,
        "source_run": _relpath_or_abs(args.run_dir),
        "source_orders_sha256": sha256(raw),
        "steps_measured": len(h),
        "phase_samples": {"HASH": len(h), "ENTRY": len(e)},
        "midstep_arrival_steps": mid,
        "zero_to_n_steps": zero_to_n,
        "empty_entry_steps": [s for s in e if e[s] == 0],
        "why": "Every step has explicit HASH and ENTRY bytes; ENTRY sets the queue before the count gate.",
    }
    man.setdefault("fidelity", {})["counts_guard"] = guard
    with open(manifest_path(fixture), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(man, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print(
        "counts-guard: %d exact HASH/ENTRY pairs, %d count changes, %d zero-to-n"
        % (len(h), len(mid), len(zero_to_n))
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
        "--fixed-step",
        type=int,
        default=None,
        help="the contract's fixed_step (default: the built-in 0). Checked against the run's "
        "effective_config.json [harness].",
    )
    p.add_argument(
        "--net",
        default="",
        help="k=v;k=v -- the [net] keys the recording ran with (e.g. sim_step_ms=20;"
        "rig_fixed_step_loop=1). Checked against the run's effective_config.json, written into the "
        "contract, and passed back by `replay`.",
    )
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
    p.add_argument(
        "--against", help="a RECORD run dir -- required independent exact fidelity proof"
    )
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
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("counts-guard")
    p.add_argument("run_dir", help="a RECORD run with complete MHOR v2 HASH/ENTRY snapshots")
    p.add_argument("--fixture")
    p.set_defaults(fn=cmd_counts_guard)

    p = sub.add_parser("check")
    p.add_argument("fixture_dir", nargs="?")
    p.set_defaults(fn=cmd_check)

    args = ap.parse_args()
    rc = args.fn(args)
    sys.exit(rc if isinstance(rc, int) else 0)


if __name__ == "__main__":
    main()
