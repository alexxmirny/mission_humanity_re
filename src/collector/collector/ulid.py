"""Minimal ULID minting (no third-party dependency needed for this).

A ULID is 48 bits of millisecond timestamp followed by 80 bits of randomness,
Crockford base32 encoded to 26 characters. We mint it server-side, in the
handler, right before creating a report directory -- never accept one from a
client, which is what keeps `REPORTS_DIR/<YYYY-MM>/<ulid>/` free of
client-controlled path components.

Collision risk with 80 random bits is negligible for this service's volume;
we do not attempt strict per-millisecond monotonicity (the reference ULID spec
allows either).
"""

from __future__ import annotations

import os
import time

_CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def new_ulid() -> str:
    ts_ms = int(time.time() * 1000) & 0xFFFFFFFFFFFF  # 48 bits
    rand = int.from_bytes(os.urandom(10), "big")  # 80 bits
    value = (ts_ms << 80) | rand
    return _encode(value, 26)


def _encode(value: int, length: int) -> str:
    # Python ints are arbitrary precision, so shifting past the value's real bit
    # width just yields zero bits -- no explicit padding needed for the top of
    # the 130-bit (26 * 5) encoding space that a 128-bit value doesn't fill.
    chars = [_CROCKFORD[(value >> (i * 5)) & 0x1F] for i in range(length - 1, -1, -1)]
    return "".join(chars)
