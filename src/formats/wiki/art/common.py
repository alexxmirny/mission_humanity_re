"""Shared plumbing for the wiki art extractors: the game source, the disk cache, the cfg model."""

from __future__ import annotations

import os
import struct
import sys
import zipfile
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
_FORMATS = os.path.dirname(os.path.dirname(_HERE))  # src/formats
REPO = Path(_FORMATS).parent.parent

# fnt.py, bnk_names.py and cfgkit/ are flat modules under src/formats (same style as source.py).
if _FORMATS not in sys.path:
    sys.path.insert(0, _FORMATS)

import fnt
from cfgkit.parse import parse_files

DEFAULT_CACHE = REPO / "tmp" / "wiki_art"
PACKS = ("mh_ex", "mh")  # overlay order: mh_ex overrides mh


def _machine():
    import machine_config

    return machine_config


class ArtSource:
    """One retail install (dir or zip) plus a cache dir.

    Pack members are decompressed lazily (the LZW decoder is pure Python, ~1.5 s/MB) and cached
    on disk under ``<cache>/pack/``, so a second run touches no decompressor.
    """

    def __init__(self, game=None, mh_exe=None, cache=None, mh_tools=None):
        mc = _machine()
        self.game = str(game or getattr(mc, "POLYGON_CLEAN", None) or mc.POLYGON)
        self._mh_exe = str(mh_exe) if mh_exe else None
        self.cache = Path(cache or DEFAULT_CACHE)
        self.mh_tools = str(mh_tools or REPO / "src" / "mh_dll" / "Release" / "mh_tools.exe")
        self.src = fnt.Source(self.game)
        self.stats: dict = {}
        self.notes: list[str] = []
        self._index: dict[str, dict[str, tuple[str, int, int]]] = {}
        self._model = None
        (self.cache / "pack").mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------ pack access
    def _pack_index(self, pack):
        """upper-case member name -> (real name, offset, size) of one pack, or {}."""
        if pack not in self._index:
            nam = self.src._read(pack, ".nam")
            idx = {}
            for i in range((len(nam) if nam else 0) // 64):
                name, _t, off, size, _fin = struct.unpack("<47s5s3I", nam[i * 64 : (i + 1) * 64])
                real = name.split(b"\0")[0].decode("ascii")
                idx[real.upper()] = (real, off, size)
            self._index[pack] = idx
        return self._index[pack]

    def pack_names(self, pack):
        return [v[0] for v in self._pack_index(pack).values()]

    def member(self, name, packs=PACKS):
        """Decompressed bytes of the first pack member called ``name`` (case-insensitive), or None."""
        key = name.upper().replace("/", "\\")
        for pack in packs:
            hit = self._pack_index(pack).get(key)
            if hit is None:
                continue
            real, off, size = hit
            cp = self.cache / "pack" / pack / real.replace("\\", "/")
            if cp.is_file():
                return cp.read_bytes()
            rsr = self.src._read(pack, ".rsr")
            data = self.src._dec.decompress(rsr[off : off + size])
            cp.parent.mkdir(parents=True, exist_ok=True)
            cp.write_bytes(data)
            return data
        return None

    def member_to_file(self, name, packs=PACKS):
        """Same as :meth:`member` but returns the cached file path (for external tools)."""
        data = self.member(name, packs)
        if data is None:
            return None
        for pack in packs:
            hit = self._pack_index(pack).get(name.upper().replace("/", "\\"))
            if hit:
                return self.cache / "pack" / pack / hit[0].replace("\\", "/")
        return None

    # ------------------------------------------------------------------ loose files
    def loose_file(self, rel):
        """Path of a loose install file (e.g. ``Res\\Obcy_B\\L_ATOM.avi``), case-insensitive."""
        rel = rel.replace("\\", "/").strip("/")
        if self.src.zip is not None:
            want = rel.lower()
            for n in self.src.zip.namelist():
                if n.lower().rstrip().endswith(want):
                    cp = self.cache / "loose" / rel.lower()
                    if not cp.is_file():
                        cp.parent.mkdir(parents=True, exist_ok=True)
                        cp.write_bytes(self.src.zip.read(n))
                    return cp
            return None
        cur = Path(self.game)
        for part in rel.split("/"):
            if not cur.is_dir():
                return None
            nxt = None
            for e in os.listdir(cur):
                if e.lower() == part.lower():
                    nxt = cur / e
                    break
            if nxt is None:
                return None
            cur = nxt
        return cur if cur.is_file() else None

    # ------------------------------------------------------------------ mh.exe
    def mh_exe_bytes(self):
        if self._mh_exe:
            return Path(self._mh_exe).read_bytes()
        p = self.loose_file("mh.exe")
        if p is None:
            raise FileNotFoundError(f"mh.exe not found in {self.game}; pass mh_exe=")
        return p.read_bytes()

    # ------------------------------------------------------------------ cfg
    @property
    def model(self):
        """cfgkit Model of the retail INIT.CFG."""
        if self._model is None:
            path = self.member_to_file("init\\INIT.CFG")
            if path is None:
                raise FileNotFoundError("init\\INIT.CFG not in the packs")
            self._model = parse_files(str(path))
        return self._model

    def objects(self, cls):
        return self.model.classes[cls]

    def bump(self, key, n=1):
        self.stats[key] = self.stats.get(key, 0) + n

    def note(self, msg):
        if msg not in self.notes:
            self.notes.append(msg)


def open_zip_ok(path):
    return zipfile.is_zipfile(path)
