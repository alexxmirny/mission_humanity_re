#!/usr/bin/env python3
"""Check actual Start metadata and per-step speed/quantum.

Single match: --speeds 200 --min-steps 1000 --carriers absent,absent HOST CLIENT
Rematch: --speeds 200,100 --min-steps 100 --carriers absent,bound HOST_PROC CLIENT_PROC
Hash equality remains the existing registry mp_analyze/rematch checker's verdict.
"""

import argparse
import json
import math
import struct
from pathlib import Path

import check_module_bind as modules
import check_rematch_residue as rematch
import state_record as sr


def read_json(path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def process_and_sessions(path, count):
    path = path.resolve()
    if (path / "session.json").is_file():
        leaf = read_json(path / "session.json").get("process_dir")
        if not leaf or Path(leaf).name != leaf:
            raise ValueError(f"session lacks a safe process_dir: {path}")
        process = path.parent / leaf
    else:
        process = path
    sessions = [Path(p) for p in rematch.session_runs(str(process.parent), process.name)]
    if len(sessions) != count:
        raise ValueError(f"{process}: want {count} sessions of this process, found {len(sessions)}")
    return process, sessions


def check_record(session, percent, step_ms, minimum):
    path = session / "mh_match_state.bin"
    if not path.is_file():
        path = session / "mh_match_state.bin.gz"
    rec = sr.load(str(path))
    # Capture rows terminate a still-running peer; only a raw incomplete tail may be discarded.
    # Every retained chunk remains CRC-checked, and the coverage check below still applies.
    raw_prefix = path.suffix == ".bin" and rec.stop_reason == "truncated"
    if (rec.stop_reason is not None and not raw_prefix) or rec.header.is_ring:
        raise ValueError(f"{path}: invalid whole-match stream ({rec.stop_reason})")
    offsets = {}
    for name in ("game_speed", "sim_step_interval"):
        index = rec.region_index[name]
        if rec.regions[index].len != 8:
            raise ValueError(f"{path}: {name} is not an 8-byte double")
        offsets[name] = rec.region_offset[index]
    buf = None
    samples = {}
    for chunk in rec.chunks:
        if isinstance(chunk, sr.KeyframeChunk):
            if chunk.step in samples and buf != chunk.state:
                raise ValueError(
                    f"periodic keyframe disagrees with reconstructed step {chunk.step}"
                )
            buf = bytearray(chunk.state)
        elif isinstance(chunk, sr.StepChunk):
            if buf is None:
                raise ValueError(f"step precedes keyframe: {path}")
            for rid, offset, size, data in chunk.runs:
                base = rec.region_offset[rid] + offset
                buf[base : base + size] = data
        else:
            continue
        if chunk.step in samples and not isinstance(chunk, sr.KeyframeChunk):
            raise ValueError(f"duplicate state sample at step {chunk.step}")
        speed = struct.unpack_from("<d", buf, offsets["game_speed"])[0]
        quantum = struct.unpack_from("<d", buf, offsets["sim_step_interval"])[0]
        if speed != percent / 100.0:
            raise ValueError(
                f"{path} step {chunk.step}: effective game_speed={speed!r}, want {percent / 100.0!r}"
            )
        if not math.isclose(quantum, step_ms / 1000.0, rel_tol=0.0, abs_tol=1e-12):
            raise ValueError(
                f"{path} step {chunk.step}: quantum={quantum!r}, want {step_ms / 1000.0!r}"
            )
        samples[chunk.step] = True
    if (
        len(samples) < minimum
        or min(samples, default=0) != 1
        or max(samples, default=0) != len(samples)
    ):
        raise ValueError(
            f"{path}: need >={minimum} contiguous state samples from step 1, found {len(samples)}"
        )
    return len(samples)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--speeds", required=True)
    parser.add_argument("--min-steps", type=int, required=True)
    parser.add_argument("--step-ms", type=int, default=10)
    parser.add_argument("--carriers", required=True)
    parser.add_argument("peers", nargs=2)
    args = parser.parse_args()
    speeds = [int(v) for v in args.speeds.split(",")]
    carriers = args.carriers.split(",")
    if (
        not speeds
        or any(v not in (100, 125, 150, 175, 200) for v in speeds)
        or len(carriers) != 2
        or any(v not in ("absent", "bound") for v in carriers)
        or args.min_steps < 1
        or args.step_ms < 1
    ):
        raise ValueError("invalid expected speeds/carriers/coverage/quantum")
    peer_sessions = []
    for role, peer, carrier in zip(("host", "client"), args.peers, carriers):
        process, sessions = process_and_sessions(Path(peer), len(speeds))
        outcome = modules.classify(modules.read_log(str(process)), module="libmh")[0]
        if outcome != carrier:
            raise ValueError(f"{role}: libmh {outcome}, want {carrier}")
        cfg = read_json(process / "effective_config.json")["config"]["net"]
        if int(cfg.get("game_speed_pct", "0")) != 150:
            raise ValueError(f"{role}: conflicting developer pin150 was not provisioned")
        records = []
        for index, (session, percent) in enumerate(zip(sessions, speeds), 1):
            metadata = read_json(session / "session.json")
            if (
                metadata.get("lobby_speed_pct") != percent
                or metadata.get("sim_step_ms") != args.step_ms
            ):
                raise ValueError(f"{role} match{index}: wrong frozen metadata/quantum")
            if metadata.get("gameplay_src") != ("published" if role == "host" else "adopted"):
                raise ValueError(
                    f"{role} match{index}: no authoritative published/adopted Start metadata"
                )
            if not metadata.get("match_id"):
                raise ValueError(f"{role} match{index}: missing match identity")
            samples = check_record(session, percent, args.step_ms, args.min_steps)
            records.append(metadata["match_id"])
            print(
                f"{role} match{index}: frozen={percent}% effective={percent / 100.0:g} quantum={args.step_ms / 1000.0:g} samples={samples} carrier={carrier}"
            )
        peer_sessions.append(records)
    if peer_sessions[0] != peer_sessions[1] or len(set(peer_sessions[0])) != len(speeds):
        raise ValueError("peers do not name the same distinct match identities")
    print(
        "lobby speed: PASS (scalar/quantum/Start identity; existing peer-hash verdict is separately required)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, modules.Refusal) as error:
        print(f"lobby speed: FAIL -- {error}")
        raise SystemExit(1)
