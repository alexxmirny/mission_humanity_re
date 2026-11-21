#!/usr/bin/env python3
"""Decode + compare the two peers' per-step ORDER RECORDINGS (mh_orders.bin).

WHAT THIS IS FOR, and how it differs from mp_analyze. mp_analyze compares region HASHES and answers
"do the peers agree?" -- a binary verdict per step. When the answer is no, it cannot say WHICH order
differs, because a hash of order_queue is opaque. This decodes the recording that harness.cpp's
order_record() writes alongside it, so a red run becomes "peer A scheduled THIS order on step N and
peer B scheduled it on step N+2".

IT IS NOT AN INDEPENDENT ORACLE. order_record() serialises the very region the hash compares, so a
clean diff here does not add confidence to a clean mp_analyze. Its value is entirely diagnostic: it
is DECODABLE, which is what turned "order_queue differs" into MP D14 (the CTL_RESYNC_BEGIN synthetic
order carrying a hardcoded past exec_time and so landing on whatever step each peer drained its
socket). See D14.

FORMAT: MHOR v2 (tools/mh_order_stream.py), complete exact HASH and ENTRY samples.
Diagnostics default to ENTRY, the actual post-AI/pre-count-gate queue. HASH can
be requested explicitly; it is never inferred as an ENTRY prefix. Empty samples
remain present in by_step(), unlike missing steps. Old v1 and partial files refuse.

    usage: python tools/mp_order_diff.py <host_dir_or_bin> <client_dir_or_bin> [--kind 0xf0]
"""

import argparse
import struct
import sys
from pathlib import Path

import mh_order_stream as order_stream

HEADER_LEN = order_stream.HEADER.size
ORDER_LEN = order_stream.RECORD_BYTES


class Order:
    """One 0x44-byte llm_strat_order as it sat in order_queue on a given step."""

    __slots__ = ("step", "exec_time", "unit_index", "owner_and_kind", "param0", "order_code", "raw")

    def __init__(self, step, raw):
        self.step = step
        self.raw = raw
        (self.exec_time,) = struct.unpack_from("<d", raw, 0x00)
        (self.unit_index,) = struct.unpack_from("<H", raw, 0x08)
        (self.owner_and_kind,) = struct.unpack_from("<H", raw, 0x0A)
        (self.param0,) = struct.unpack_from("<H", raw, 0x0C)
        (self.order_code,) = struct.unpack_from("<H", raw, 0x0E)

    @property
    def kind(self):
        # The kind switch at 0x004668f1 masks with 0xf0; the low nibble is the owner.
        return self.owner_and_kind & 0xF0

    def label(self):
        return "kind=0x%02x owner=%d unit=%d param0=%d code=%d exec_time=%r" % (
            self.kind,
            self.owner_and_kind & 0x0F,
            self.unit_index,
            self.param0,
            self.order_code,
            self.exec_time,
        )


def resolve(p):
    """Accept either the .bin itself or a peer artifact directory containing it."""
    p = Path(p)
    if p.is_dir():
        hits = sorted(p.rglob("mh_orders.bin"))
        if not hits:
            sys.exit("no mh_orders.bin under %s (was the run made with --record 1?)" % p)
        return hits[0]
    return p


class Orders(list):
    """Records plus coverage: a zero-count sample is not a missing sample."""

    def __init__(self):
        super().__init__()
        self.sample_steps = set()


def load(path, phase=order_stream.ENTRY):
    """Decode native records at one exact phase; preserve explicit empty steps."""
    try:
        samples = order_stream.load(path)
    except (OSError, ValueError) as e:
        sys.exit("%s: %s" % (path, e))
    if phase not in (order_stream.HASH, order_stream.ENTRY):
        raise ValueError("unknown order phase")
    out = Orders()
    for sample in samples:
        if sample.phase == phase:
            out.sample_steps.add(sample.step)
            out.extend(Order(sample.step, raw) for raw in sample.records)
    return out


def by_step(orders):
    d = {step: [] for step in getattr(orders, "sample_steps", ())}
    for o in orders:
        d.setdefault(o.step, []).append(o)
    return d


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("host")
    ap.add_argument("client")
    ap.add_argument(
        "--kind",
        default="0xf0",
        help="highlight where this order kind was scheduled on each peer (default 0xf0, the CTL_RESYNC_BEGIN synthetic order); 'none' to skip",
    )
    ap.add_argument(
        "--max-report", type=int, default=5, help="how many differing steps to print in full"
    )
    ap.add_argument("--phase", choices=("HASH", "ENTRY"), default="ENTRY")
    a = ap.parse_args()

    hp, cp = resolve(a.host), resolve(a.client)
    phase = order_stream.HASH if a.phase == "HASH" else order_stream.ENTRY
    h, c = load(hp, phase), load(cp, phase)
    print("host   %s: %d records over %d steps" % (hp, len(h), len(by_step(h))))
    print("client %s: %d records over %d steps" % (cp, len(c), len(by_step(c))))

    # The tracked kind, per peer: the step it FIRST appears on and the exec_time it carries. This is
    # the D14 readout -- the two peers must agree on both.
    rc = 0
    if a.kind.lower() != "none":
        want = int(a.kind, 0)
        first = {}
        for name, stream in (("host", h), ("client", c)):
            hits = [o for o in stream if o.kind == want]
            if hits:
                first[name] = (hits[0].step, hits[0].exec_time)
                steps = sorted({o.step for o in hits})
                print(
                    "kind 0x%02x on %-6s: first step %d, present on %d steps (%d..%d), exec_time %r"
                    % (want, name, hits[0].step, len(steps), steps[0], steps[-1], hits[0].exec_time)
                )
            else:
                print("kind 0x%02x on %-6s: NEVER SCHEDULED in this run" % (want, name))
        if len(first) == 2:
            (hs, ht), (cs, ct) = first["host"], first["client"]
            if hs == cs and ht == ct:
                print("  => AGREE: both peers scheduled it on step %d at exec_time %r" % (hs, ht))
            else:
                print("  => SKEW: host step %d (t=%r) vs client step %d (t=%r)" % (hs, ht, cs, ct))
                rc = 1
        elif len(first) == 1:
            # One peer saw it and the other did not: strictly worse than a step skew.
            print("  => SKEW: only one peer scheduled it at all")
            rc = 1

    # Full stream comparison, step by step.
    hs, cs = by_step(h), by_step(c)
    common = sorted(set(hs) & set(cs))
    diffs = []
    for s in common:
        a_ = [o.raw for o in hs[s]]
        b_ = [o.raw for o in cs[s]]
        if a_ != b_:
            diffs.append(s)
    print("compared %d common steps; %d differ" % (len(common), len(diffs)))
    if diffs:
        rc = 1
        print(
            "first differing step: %d   (contiguous run: %d..%d)" % (diffs[0], diffs[0], diffs[-1])
        )
        for s in diffs[: a.max_report]:
            print("  step %d:" % s)
            for o in hs[s]:
                print("    host  : %s" % o.label())
            for o in cs[s]:
                print("    client: %s" % o.label())
    else:
        print("ORDER STREAMS IDENTICAL on every common step")
    return rc


if __name__ == "__main__":
    sys.exit(main())
