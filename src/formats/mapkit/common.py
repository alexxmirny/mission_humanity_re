"""Shared primitives for mapkit: colour conversion + base64 helpers.

Kept dependency-free (rgb565_to_888 duplicated from bnk_names.py:362 rather than
imported, to avoid pulling that module's heavier deps into the codec path)."""
from __future__ import annotations

import base64
import os
import sys

# src/formats/mapkit/common.py -> repo root is four levels up; import machine_config for the machine-
# specific game-data roots (used as defaults across mapkit; also re-exported to mapkit.treebank).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import machine_config as _machine  # noqa: E402

CLEAN = _machine.RES_UNPACK  # pristine unpacked source-of-truth
BANKI = _machine.BANKI  # unpacked sprite banks (mapkit.treebank)


def rgb565_to_888(v: int) -> tuple[int, int, int]:
    r5, g6, b5 = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r5 * 255 + 15) // 31, (g6 * 255 + 31) // 63, (b5 * 255 + 15) // 31)


def rgb888_to_565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def b64e(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def b64d(s: str) -> bytes:
    return base64.b64decode(s.encode("ascii"))
