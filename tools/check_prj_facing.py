#!/usr/bin/env python3
"""Check projectile-facing frames using the session's complete MHSR recording.

The prj_facing_rl4 workload loads the Ayatus save and observes weapons 19/20.
Only mismatches already present in the first recorded state are inherited from
that save and ignored. Later new mismatches are counted, including new shots
that are wrong the first time they appear.

    python tools/check_prj_facing.py <session-or-process-dir>
    python tools/check_prj_facing.py --expect-bad <session-or-process-dir>
    python tools/check_prj_facing.py --selftest
"""

import argparse
import math
import re
import struct
import sys
import zlib
from pathlib import Path

import _rundir
import state_record as sr

POOL_STRIDE = 121
POOL_SIZE = 181500  # 1500 slots, including slot 0
FMT = "<iiHHHHHHHHBdddddddiiiiiiiHiHi"
NAMES = (
    "active weapon_id x y src_x src_y_ground src_y_vis dst_x dst_y_ground dst_y_vis owner "
    "delta_x delta_y launch_time duration anim_clock smoke_clock explo_clock scatter_x scatter_y "
    "facing anim_frame smoke_sprite explo_flag anim_loop homing_player homing_unit shooter_ref "
    "shooter_unit"
).split()
BASE = {20: 1523, 19: 1547}
EXPECT_BAD_MIN = 10
MIN_SAMPLE = 20
_RECORDING_NAME = re.compile(r"mh_match_state(?:_\d+)?\.bin(?:\.gz)?\Z")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def find_recording(target):
    """Use the explicit session, or the sole session belonging to this process."""
    target = Path(target).resolve()
    members = [Path(p).resolve() for p in _rundir.process_and_sessions(str(target))]
    sessions = members[1:]
    if _rundir.is_session_dir(str(target)):
        require(target in sessions, "session does not belong to its process")
        session = target
    else:
        require(len(sessions) == 1, "expected exactly one session for this process")
        session = sessions[0]
    paths = sorted(
        p for p in session.iterdir() if p.is_file() and _RECORDING_NAME.fullmatch(p.name)
    )
    require(len(paths) == 1, "expected exactly one session state recording in " + str(session))
    return paths[0]


def frames(rec):
    """Reconstruct one projectile pool per step; never infer an absent STEP."""
    require(rec.stop_reason is None, "damaged state recording: " + str(rec.stop_detail))
    require(rec.header.flags == 0, "expected a non-ring recording with known flags")
    require(rec.header.keyframe_every > 0, "invalid keyframe interval")
    require(rec.end is not None, "state recording lacks END")
    require(
        rec.chunks
        and isinstance(rec.chunks[0], sr.KeyframeChunk)
        and isinstance(rec.chunks[-1], sr.EndChunk)
        and sum(isinstance(c, sr.EndChunk) for c in rec.chunks) == 1,
        "expected one final END after an initial KEYF",
    )
    names = [region.name for region in rec.regions]
    require(len(names) == len(set(names)), "duplicate state-record region names")
    require(names.count("projectile_pool") == 1, "missing projectile_pool")
    rid = names.index("projectile_pool")
    require(rec.regions[rid].len == POOL_SIZE, "unexpected projectile_pool length")
    require(struct.calcsize(FMT) == POOL_STRIDE, "projectile layout width mismatch")
    expected_header = sr.HEADER_FIXED_SIZE + sum(
        5 + len(region.name.encode("utf-8")) for region in rec.regions
    )
    require(rec.header.header_len == expected_header, "invalid state-record header length")
    require(rec.first_step == 1, "recording must begin at session step 1")

    base = rec.region_offset[rid]
    end = base + POOL_SIZE
    first = rec.chunks[0]
    require(first.step == 1 and len(first.state) == rec.total_len, "invalid initial KEYF")
    pool = bytearray(first.state[base:end])
    raw_bytes = expected_header + sr.CHUNK_HDR_SIZE + 4 + len(first.state)
    pending = 1
    count = 1
    for chunk in rec.chunks[1:-1]:
        if isinstance(chunk, sr.StepChunk):
            require(chunk.step == pending + 1, "missing, duplicate or out-of-order STEP")
            yield pending, pool
            pending = chunk.step
            count += 1
            raw_bytes += sr.CHUNK_HDR_SIZE + 8
            for run_rid, offset, length, data in chunk.runs:
                require(0 <= run_rid < len(rec.regions), "invalid run region")
                require(
                    length > 0
                    and length == len(data)
                    and 0 <= offset
                    and offset + length <= rec.regions[run_rid].len,
                    "invalid state-record run",
                )
                raw_bytes += sr.RUN_HDR_SIZE + length
                if run_rid == rid:
                    pool[offset : offset + length] = data
        elif isinstance(chunk, sr.KeyframeChunk):
            require(len(chunk.state) == rec.total_len, "invalid KEYF length")
            image = chunk.state[base:end]
            if chunk.step == pending:
                # Periodic KEYFs follow their STEP. A re-prime coinciding with
                # the periodic cadence can also write two identical KEYFs.
                require(image == pool, "KEYF disagrees with reconstructed projectile pool")
            else:
                require(chunk.step == pending + 1, "missing or out-of-order KEYF")
                yield pending, pool
                pending = chunk.step
                count += 1
                pool = bytearray(image)
            raw_bytes += sr.CHUNK_HDR_SIZE + 4 + len(chunk.state)
        else:
            raise ValueError("unexpected chunk before END")
    require(
        rec.last_step == pending
        and rec.end.last_step == pending
        and rec.end.steps_recorded == count
        and count == pending,
        "state END step coverage mismatch",
    )
    require(rec.end.raw_bytes_written == raw_bytes, "state END byte count mismatch")
    yield pending, pool


def scan(path):
    """Return steps, seen shots, counted wrong shots, and inherited wrong count."""
    rec = sr.load(str(path))
    seen = {}
    bad = {}
    inherited_wrong = set()
    steps = 0
    for step, pool in frames(rec):
        steps += 1
        for slot in range(1, POOL_SIZE // POOL_STRIDE):
            values = dict(zip(NAMES, struct.unpack_from(FMT, pool, POOL_STRIDE * slot)))
            if not values["active"] or values["weapon_id"] not in BASE:
                continue
            launch = values["launch_time"]
            require(math.isfinite(launch), "non-finite projectile launch time")
            key = (slot, round(launch, 3))
            seen.setdefault(key, step)
            if values["anim_loop"] != BASE[values["weapon_id"]] + values["facing"] - 1:
                if step == 1:
                    inherited_wrong.add(key)
                elif key not in inherited_wrong:
                    bad.setdefault(key, step)
    return steps, seen, bad, len(inherited_wrong)


def verdict(steps, seen, bad, pre, expect_bad):
    msgs = [
        "  %d recorded steps, %d weapon-19/20 shots seen, "
        "%d ignored (wrong in the save), %d wrong" % (steps, len(seen), pre, len(bad))
    ]
    for key, step in sorted(bad.items(), key=lambda item: item[1])[:5]:
        msgs.append("    slot %d launched %.3f first wrong at step %d" % (*key, step))
    ok = True
    if steps < 100 or len(seen) < MIN_SAMPLE:
        ok = False
        msgs.append(
            "FAIL: sample too small (%d steps, %d shots) -- "
            "the recording or the save workload did not run" % (steps, len(seen))
        )
    elif expect_bad and len(bad) < EXPECT_BAD_MIN:
        ok = False
        msgs.append(
            "FAIL: expected-red arm shows only %d wrong shots (< %d)" % (len(bad), EXPECT_BAD_MIN)
        )
    elif not expect_bad and bad:
        ok = False
        msgs.append("FAIL: %d shot(s) have a frame inconsistent with their facing" % len(bad))
    return ok, msgs


def selftest():
    import json
    import tempfile

    def shot(slot, wid, facing, loop, launch):
        values = dict.fromkeys(NAMES, 0)
        values.update(active=1, weapon_id=wid, facing=facing, anim_loop=loop, launch_time=launch)
        return slot * POOL_STRIDE, struct.pack(FMT, *[values[name] for name in NAMES])

    def set_shot(pool, slot, wid, facing, loop, launch):
        offset, record = shot(slot, wid, facing, loop, launch)
        pool[offset : offset + len(record)] = record

    def fixture(wrong):
        pool = bytearray(POOL_SIZE)
        set_shot(pool, 1, 20, 24, 1522, 1.0)  # Already wrong in the saved state.
        states = {}
        for step in range(1, 102):
            if step == 2:
                for slot in range(2, 23):
                    loop = 1522 if wrong and slot <= 12 else 1546
                    set_shot(pool, slot, 20, 24, loop, float(slot))
            states[step] = bytes(pool)
        return sr._build_match([("projectile_pool", POOL_SIZE)], states, 25)[0]

    def fails(path):
        try:
            scan(path)
        except (ValueError, OSError, EOFError, zlib.error, UnicodeError, struct.error):
            return
        raise AssertionError("invalid recording was accepted: " + str(path))

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve()
        path = root / "mh_match_state.bin"
        good = fixture(False)
        wrong = fixture(True)
        path.write_bytes(good)
        result = scan(path)
        assert (result[0], len(result[1]), len(result[2]), result[3]) == (101, 22, 0, 1)
        assert verdict(*result, False)[0]
        assert not verdict(*result, True)[0]
        path.write_bytes(wrong)
        result = scan(path)
        assert len(result[2]) == 11 and result[3] == 1
        assert result[2][(2, 2.0)] == 2
        assert not verdict(*result, False)[0]
        assert verdict(*result, True)[0]
        for broken in (
            wrong[:-28],
            wrong[:-1],
            wrong[:-29] + bytes([wrong[-29] ^ 1]) + wrong[-28:],
        ):
            path.write_bytes(broken)
            fails(path)

        header = sr._pack_header([("projectile_pool", POOL_SIZE)], keyframe_every=2)
        empty = bytes(POOL_SIZE)
        initial = sr._pack_keyf(1, empty)

        def closed(chunks, last, count, byte_adjust=0):
            body = header + b"".join(chunks)
            return body + sr._pack_end(last, count, len(body) + byte_adjust)

        step2 = sr._pack_step(2, [])
        step3 = sr._pack_step(3, [])
        key3 = sr._pack_keyf(3, empty)
        invalid = [
            closed([initial, step3], 3, 2),
            closed([initial, step2, step2], 2, 3),
            closed([initial, step2, sr._pack_keyf(1, empty)], 2, 2),
            closed([initial, step2, step3, sr._pack_keyf(3, b"\x01" + empty[1:])], 3, 3),
            closed([initial, sr._pack_step(2, [(0, POOL_SIZE, 1, b"x")])], 2, 2),
            closed([initial, step2], 2, 1),
            closed([initial, step2], 2, 2, 1),
            closed([initial, step2], 2, 2) + sr._pack_end(2, 2, 0),
            sr._pack_header([("projectile_pool", POOL_SIZE)], flags=1)
            + initial
            + sr._pack_end(1, 1, 0),
        ]
        for data in invalid:
            path.write_bytes(data)
            fails(path)
        path.write_bytes(closed([initial, step2, step3, key3], 3, 3))
        assert scan(path)[0] == 3

        # Re-priming replaces a delta with a full next-step image. Coinciding
        # with the periodic cadence can produce another identical same-step KEYF.
        changed = bytearray(empty)
        set_shot(changed, 2, 20, 24, 1522, 2.0)
        rebase = sr._pack_keyf(2, bytes(changed))
        path.write_bytes(closed([initial, rebase, rebase, step3], 3, 3))
        steps, seen, bad, pre = scan(path)
        assert (steps, len(seen), len(bad), pre) == (3, 1, 1, 0)
        assert bad[(2, 2.0)] == 2

        process = root / "2026-11-14T12-00-00Z_menu_solo"
        session = root / "2026-11-14T12-00-01Z_1234abcd_planet8_campaign"
        other = root / "2026-11-14T12-00-02Z_abcdef12_planet8_campaign"
        process.mkdir()
        session.mkdir()
        other.mkdir()
        for directory in (session, other):
            (directory / "session.json").write_text(
                json.dumps({"process_dir": process.name}), encoding="utf-8"
            )
        (session / "mh_match_state.bin").write_bytes(good)
        assert find_recording(session) == session / "mh_match_state.bin"
        for target in (process, other):
            try:
                find_recording(target)
            except ValueError:
                pass
            else:
                raise AssertionError("ambiguous process or missing session accepted")
        (session / "mh_match_state.bin.gz").write_bytes(b"ambiguous")
        try:
            find_recording(session)
        except ValueError:
            pass
        else:
            raise AssertionError("ambiguous recording accepted")
    print("check_prj_facing selftest: PASS")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--expect-bad", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("run_dir", nargs="?")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.run_dir:
        parser.error("usage: check_prj_facing.py [--expect-bad] <session-or-process-dir>")
    try:
        path = find_recording(args.run_dir)
        ok, msgs = verdict(*scan(path), args.expect_bad)
    except (ValueError, OSError, EOFError, zlib.error, UnicodeError, struct.error) as exc:
        print("FAIL: state recording: %s" % exc)
        return 1
    print("  recording: %s" % path)
    print("\n".join(msgs))
    print(
        "check_prj_facing: %s%s"
        % ("PASS" if ok else "FAIL", " [expected-red arm]" if args.expect_bad else "")
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
