"""Game-data source for the wiki: INIT.CFG, INITLANG.CFG and INFO.TXT straight from the packs.

Nothing here needs an unpacked resource tree.  A retail install (directory or ``.zip`` of one) is
read through ``fnt.Source`` (the ``.nam``/``.rsr`` pair reader), and only the three members the
wiki needs are decompressed (a ``members`` prefix filter runs *before* decompression, so this is
about two seconds, not a full-pack unpack).

* ``init\\INIT.CFG`` comes from the ``mh`` pack (identical in every language build).
* ``init\\INITLANG.CFG`` (TEXT table) and ``info\\INFO.TXT`` (UTF-16 ``<FILE path>`` blocks, the
  descriptions) come from the ``mh_ex`` pack, one pair per language.

Machine paths are never hardcoded: pass them as arguments.  ``default_game()`` / ``default_ru()``
only consult ``tools/machine_config.py``, the ``MH_WIKI_EN`` / ``MH_WIKI_RU`` environment variables
and the gitignored ``game_data/`` / ``workdir/`` drops beside the repo.

Public API::

    data = load(game, ru=None)                # GameData: EN from `game`, RU from `ru`
    data = load(game, lang_sources={"ru": ru_path, "pl": pl_path})
    data = load_files(init_path, {"en": (initlang_path, info_path), ...})   # loose UTF-16 files
    data.init                                  # INIT.CFG text
    data.langs["en"].initlang / .info          # INITLANG text / {normalized path: description}
    parse_cfg(text)                            # cfgkit Model (cfgkit.parse.parse_text)
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field

_HERE = os.path.dirname(os.path.abspath(__file__))
_FORMATS = os.path.dirname(_HERE)  # src/formats
_REPO = os.path.dirname(os.path.dirname(_FORMATS))

# fnt.py and cfgkit/ are flat modules under src/formats (same style as build_mod.py).
if _FORMATS not in sys.path:
    sys.path.insert(0, _FORMATS)

import fnt
from cfgkit.parse import parse_text as _parse_text

_INIT_MEMBER = "init\\init.cfg"
_LANG_MEMBER = "init\\initlang.cfg"
_INFO_MEMBER = "info\\info.txt"


@dataclass
class LangData:
    """One language's text layer."""

    initlang: str = ""  # decoded INITLANG.CFG
    info: dict = field(default_factory=dict)  # normalized INF path -> description text
    origin: str = ""  # where it was read from (for the build log)


@dataclass
class GameData:
    init: str = ""  # decoded INIT.CFG
    langs: dict = field(default_factory=dict)  # lang code -> LangData
    origin: str = ""


# ------------------------------------------------------------------------------------ decoding


def decode_utf16(raw: bytes) -> str:
    """UTF-16 (BOM or LE without BOM) -> str, BOM stripped."""
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-16-le")


def norm_path(p: str) -> str:
    """INF path key: lower case, backslash separators."""
    return p.strip().replace("/", "\\").lower()


_FILE_RE = re.compile(r"^<FILE\s+([^>]+?)\s*>\s*$", re.MULTILINE)


def parse_info(text: str) -> dict:
    """INFO.TXT -> {normalized path: description}.  Blocks run to the next ``<FILE ...>`` line."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    out = {}
    marks = list(_FILE_RE.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        out[norm_path(m.group(1))] = text[m.end() : end].strip()
    return out


def parse_cfg(text: str):
    """Parse cfg text with cfgkit (INIT and INITLANG parse independently; see cfgkit.textio)."""
    return _parse_text(text)


# ------------------------------------------------------------------------------------ pack access


def _member(src, pack: str, name: str):
    """Decompressed bytes of one pack member (case-insensitive), or None."""
    hits = src.members(pack, prefix=name)
    for key, val in hits.items():
        if key.lower().replace("/", "\\") == name:
            return val
    return None


def load(game, ru=None, *, lang_sources=None) -> GameData:
    """Read the wiki's game data from retail installs.

    ``game``: EN install (dir or zip) providing INIT.CFG and the ``en`` language layer.
    ``ru``: optional install (dir/zip, e.g. a bare ``ru_rsr`` with only ``mh_ex``) for ``ru``.
    ``lang_sources``: ``{lang: path}`` for any further languages (``en`` may be overridden).
    """
    sources = {"en": game}
    if ru is not None:
        sources["ru"] = ru
    sources.update(lang_sources or {})
    opened = {}

    def open_(path):
        if path not in opened:
            opened[path] = fnt.Source(path)
        return opened[path]

    init_raw = None
    for path in [game] + [p for p in sources.values() if p != game]:
        init_raw = _member(open_(path), "mh", _INIT_MEMBER)
        if init_raw is not None:
            break
    if init_raw is None:
        raise FileNotFoundError(f"no {_INIT_MEMBER} in the `mh` pack of {game}")
    data = GameData(init=decode_utf16(init_raw), origin=str(game))
    for lang, path in sources.items():
        src = open_(path)
        lang_raw = _member(src, "mh_ex", _LANG_MEMBER)
        if lang_raw is None:
            raise FileNotFoundError(f"no {_LANG_MEMBER} in the `mh_ex` pack of {path} ({lang})")
        info_raw = _member(src, "mh_ex", _INFO_MEMBER)
        data.langs[lang] = LangData(
            initlang=decode_utf16(lang_raw),
            info=parse_info(decode_utf16(info_raw)) if info_raw else {},
            origin=str(path),
        )
    return data


def load_files(init_path, lang_files: dict) -> GameData:
    """Loose-file variant: ``{lang: (initlang_path, info_path_or_None)}`` (used by the selftest)."""
    with open(init_path, "rb") as f:
        data = GameData(init=decode_utf16(f.read()), origin=str(init_path))
    for lang, (lang_path, info_path) in lang_files.items():
        with open(lang_path, "rb") as f:
            ld = LangData(initlang=decode_utf16(f.read()), origin=str(lang_path))
        if info_path:
            with open(info_path, "rb") as f:
                ld.info = parse_info(decode_utf16(f.read()))
        data.langs[lang] = ld
    return data


# ------------------------------------------------------------------------------------ defaults


def _machine_attr(name):
    try:
        sys.path.insert(0, os.path.join(_REPO, "tools"))
        import machine_config

        return getattr(machine_config, name, None)
    except Exception:  # noqa: BLE001 -- machine_config is optional here
        return None
    finally:
        try:
            sys.path.remove(os.path.join(_REPO, "tools"))
        except ValueError:
            pass


def _first_existing(cands):
    for c in cands:
        if c and (os.path.isdir(str(c)) or os.path.isfile(str(c))):
            return str(c)
    return None


def default_game():
    """Best-effort EN install: $MH_WIKI_EN, machine_config POLYGON_CLEAN, then the repo drops."""
    return _first_existing(
        [
            os.environ.get("MH_WIKI_EN"),
            _machine_attr("POLYGON_CLEAN"),
            os.path.join(_REPO, "workdir", "mh_en_clean"),
            os.path.join(_REPO, "game_data", "mh_en_clean.zip"),
        ]
    )


def default_ru():
    """Best-effort RU layer: $MH_WIKI_RU, the bare ``ru_rsr`` mh_ex pack, then the RU clean zip."""
    return _first_existing(
        [
            os.environ.get("MH_WIKI_RU"),
            os.path.join(_REPO, "game_data", "ru_rsr"),
            os.path.join(_REPO, "game_data", "mh_ru_clean.zip"),
            _machine_attr("RU_CLEAN"),
        ]
    )
