#!/usr/bin/env python3
"""Strict MHOR v2 framing. Native 68-byte order payloads remain opaque and unchanged.

LE header: magic/version/native_record_bytes. Each sample: step/phase/count,
then count native records. Every recorded step has one HASH then one ENTRY,
including empty queues. No count-only or clock-derived reconstruction is possible.
"""

from __future__ import annotations

from dataclasses import dataclass
import gzip
from pathlib import Path
import re
import struct

MAGIC = b"MHOR"
VERSION = 2
RECORD_BYTES = 68
MAX_QUEUE = 4096
HASH, ENTRY = 1, 2
HEADER = struct.Struct("<4sII")
SAMPLE_HEADER = struct.Struct("<III")
UINT32_MAX = (1 << 32) - 1
FAITHFUL_FLAGS = {
    "order_mode": 2,
    "replay_dispatch_inject": 1,
    "replay_suppress_enqueue": 0,
    "replay_ai_off": 0,
}
CONTRACT = {
    "version": VERSION,
    "native_record_bytes": RECORD_BYTES,
    "phases": ["HASH", "ENTRY"],
}
RECEIPT = re.compile(
    r"order replay v2: HASH=(\d+) ENTRY=(\d+) missed=(\d+) ai_off=0 suppress_enqueue=0"
)


class Error(ValueError):
    """Malformed, incomplete or unsupported order recording; recapture is required."""


@dataclass(frozen=True)
class Sample:
    step: int
    phase: int
    records: tuple[bytes, ...]


def decode(raw: bytes, *, first_step=None, steps=None) -> tuple[Sample, ...]:
    if len(raw) < 8:
        raise Error("truncated MHOR header; recapture the recording")
    magic, version = struct.unpack_from("<4sI", raw)
    if magic != MAGIC:
        raise Error("not an MHOR order recording")
    if version != VERSION:
        raise Error("MHOR v%d is not faithful two-phase v2; recapture, do not restamp" % version)
    if len(raw) < HEADER.size:
        raise Error("truncated MHOR v2 header")
    if HEADER.unpack_from(raw)[2] != RECORD_BYTES:
        raise Error("MHOR v2 native_record_bytes must be 68")
    out = []
    pos = HEADER.size
    expected_step = first_step
    expected_phase = HASH
    while pos < len(raw):
        if len(raw) - pos < SAMPLE_HEADER.size:
            raise Error("partial MHOR sample header at byte %d" % pos)
        step, phase, count = SAMPLE_HEADER.unpack_from(raw, pos)
        pos += SAMPLE_HEADER.size
        if not step or phase != expected_phase:
            raise Error(
                "expected %s sample at step %s, got step %d phase %d"
                % ("HASH" if expected_phase == HASH else "ENTRY", expected_step, step, phase)
            )
        if expected_step is None:
            expected_step = step
        if step != expected_step:
            raise Error(
                "missing/duplicate/out-of-order MHOR step: expected %d, got %d"
                % (expected_step, step)
            )
        # Bound before multiplication, matching the fixed-width C++ scanner's overflow check.
        if count > MAX_QUEUE or count > (len(raw) - pos) // RECORD_BYTES:
            raise Error("MHOR sample count overflows or payload is truncated at step %d" % step)
        end = pos + count * RECORD_BYTES
        records = tuple(raw[i : i + RECORD_BYTES] for i in range(pos, end, RECORD_BYTES))
        out.append(Sample(step, phase, records))
        pos = end
        if phase == HASH:
            expected_phase = ENTRY
        else:
            expected_phase = HASH
            expected_step += 1
    if expected_phase != HASH:
        raise Error("missing ENTRY sample at step %d" % expected_step)
    if steps is not None and len(out) != 2 * steps:
        raise Error(
            "MHOR has %d complete step(s), requested %d; missing is not empty"
            % (len(out) // 2, steps)
        )
    return tuple(out)


def encode(samples) -> bytes:
    body = bytearray(HEADER.pack(MAGIC, VERSION, RECORD_BYTES))
    for sample in samples:
        if not 1 <= sample.step <= UINT32_MAX or sample.phase not in (HASH, ENTRY):
            raise Error("invalid MHOR step/phase")
        if len(sample.records) > MAX_QUEUE:
            raise Error("MHOR sample count overflows")
        body.extend(SAMPLE_HEADER.pack(sample.step, sample.phase, len(sample.records)))
        for record in sample.records:
            if len(record) != RECORD_BYTES:
                raise Error("native order record must be exactly 68 bytes")
            body.extend(record)
    raw = bytes(body)
    decode(raw)
    return raw


def load(path, **kwargs):
    path = Path(path)
    raw = path.read_bytes()
    if path.suffix == ".gz":
        raw = gzip.decompress(raw)
    return decode(raw, **kwargs)


def cut(raw, start, last=None):
    """Keep complete sample pairs on the existing step axis; rebase start to 1."""
    samples = decode(raw)
    if not samples or start < samples[0].step or start > samples[-1].step:
        raise Error("requested MHOR start step is missing")
    if last is None:
        last = samples[-1].step
    if last < start or last > samples[-1].step:
        raise Error("requested MHOR end step is missing")
    return encode(
        Sample(s.step - start + 1, s.phase, s.records) for s in samples if start <= s.step <= last
    )


def flag_refusal(flags):
    for key, expected in FAITHFUL_FLAGS.items():
        if str(flags.get(key)) != str(expected):
            return "faithful MHOR v2 requires %s=%d, got %r" % (key, expected, flags.get(key))
    return None


def contract_refusal(man):
    live, replay = "live_contract" in man, "replay_contract" in man
    if live == replay:
        return "exactly one live_contract or replay_contract is required"
    artifacts = man.get("artifacts", {})
    if live:
        if "orders" in artifacts:
            return "LIVE fixtures must not carry an order stream"
        flags = man["live_contract"].get("flags", {})
        for key, expected in (
            ("order_mode", 0),
            ("replay_suppress_enqueue", 0),
            ("replay_ai_off", 0),
        ):
            if str(flags.get(key)) != str(expected):
                return "live fixture requires %s=%d" % (key, expected)
        return None
    contract = man["replay_contract"]
    if contract.get("order_stream") != CONTRACT:
        return "missing/old MHOR two-phase v2 contract; recapture, do not restamp"
    return flag_refusal(contract.get("flags", {}))


def receipt_refusal(text, steps):
    hits = list(RECEIPT.finditer(text))
    if not hits:
        return "missing faithful MHOR v2 phase summary"
    counts = tuple(map(int, hits[-1].groups()))
    if counts != (steps, steps, 0):
        return "MHOR v2 phase summary HASH=%d ENTRY=%d missed=%d, expected %d/%d/0" % (
            *counts,
            steps,
            steps,
        )
    return None


def phase_counts(samples):
    return {
        phase: {s.step: len(s.records) for s in samples if s.phase == phase}
        for phase in (HASH, ENTRY)
    }


def selftest():
    """Portable byte/semantic behavior vectors, independent of source spelling or wiring."""

    def native(unit, owner, code, arg, time):
        raw = bytearray(RECORD_BYTES)
        struct.pack_into("<dHHHHI", raw, 0, time, unit, owner, 7, code, arg)
        raw[32:40] = b"\x00\xff\x10\x80\x11\x22\x33\x44"
        return bytes(raw)

    a = native(17, 0x81, 0x25, 1234, 1.5)
    b = native(18, 0x21, 0x31, 5678, 2.0)
    # Same count, different bytes: HASH is intentionally NOT an ENTRY prefix (overflow/loss case).
    samples = (
        Sample(7, HASH, (a,)),
        Sample(7, ENTRY, (b,)),
        Sample(8, HASH, ()),
        Sample(8, ENTRY, (a, b)),
        Sample(9, HASH, (a, b)),
        Sample(9, ENTRY, ()),
    )
    wire = (
        b"MHOR\x02\0\0\0\x44\0\0\0"
        + struct.pack("<III", 7, 1, 1)
        + a
        + struct.pack("<III", 7, 2, 1)
        + b
        + struct.pack("<III", 8, 1, 0)
        + struct.pack("<III", 8, 2, 2)
        + a
        + b
        + struct.pack("<III", 9, 1, 2)
        + a
        + b
        + struct.pack("<III", 9, 2, 0)
    )
    failures = []

    def check(label, good):
        if not good:
            failures.append(label)

    def refused(raw, **kwargs):
        try:
            decode(raw, **kwargs)
        except Error:
            return True
        return False

    check("independent LE wire equals encoder", encode(samples) == wire)
    check("payloads and explicit zero survive", decode(wire, first_step=7, steps=3) == samples)
    check(
        "HASH bytes are not inferred from ENTRY count",
        decode(wire)[0].records != decode(wire)[1].records,
    )
    check(
        "segment rebase retains both nonempty/empty phases",
        decode(cut(wire, 8)) == tuple(Sample(s.step - 7, s.phase, s.records) for s in samples[2:]),
    )
    for label, raw in (
        ("old v1", b"MHOR\x01\0\0\0"),
        ("bad magic", b"XXXX" + wire[4:]),
        ("partial header", wire[:11]),
        ("partial payload", wire[:-13]),
        ("missing ENTRY", encode(samples[:2])[: -SAMPLE_HEADER.size - RECORD_BYTES]),
        ("trailing byte", wire + b"x"),
        ("ENTRY first", HEADER.pack(MAGIC, 2, 68) + SAMPLE_HEADER.pack(1, ENTRY, 0)),
        ("wrong width", HEADER.pack(MAGIC, 2, 67)),
        ("huge count", HEADER.pack(MAGIC, 2, 68) + SAMPLE_HEADER.pack(1, HASH, UINT32_MAX)),
        (
            "missing step",
            encode(samples[:2]) + wire[HEADER.size + 4 * SAMPLE_HEADER.size + 4 * RECORD_BYTES :],
        ),
    ):
        check(label, refused(raw))
    check("requested missing step fails, not implicit empty", refused(wire, first_step=7, steps=4))
    check("wrong requested first step fails", refused(wire, first_step=1))
    check(
        "empty header cannot satisfy a requested step", refused(encode(()), first_step=1, steps=1)
    )
    check(
        "old suppression refused",
        flag_refusal(dict(FAITHFUL_FLAGS, replay_suppress_enqueue=1)) is not None,
    )
    check(
        "phase evidence accepted",
        receipt_refusal("order replay v2: HASH=3 ENTRY=3 missed=0 ai_off=0 suppress_enqueue=0", 3)
        is None,
    )
    check(
        "old count SET line is insufficient",
        receipt_refusal("injector: count SET at 3 of 3 arrival step(s), 0 append-without-set", 3)
        is not None,
    )
    check(
        "short phase run refused",
        receipt_refusal("order replay v2: HASH=2 ENTRY=2 missed=0 ai_off=0 suppress_enqueue=0", 3)
        is not None,
    )
    print("[mh_order_stream] %s" % ("PASS" if not failures else "FAIL: " + ", ".join(failures)))
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(selftest())
