r"""Video stills: one frame from each ``Res\<INFO_FLC>`` Indeo-5 AVI via ffmpeg (optional)."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
from pathlib import Path

import yaml
from PIL import Image, ImageDraw

from .common import ArtSource

DEFAULT_POSITION = 0.5  # fraction of the clip duration (frame 0 is often black)
STILLS_YAML = Path(__file__).resolve().parent / "stills.yaml"
# Unit clips are turntables (one full rotation per clip) that START and END on the front view;
# the 50% frame is the rear. Default for them: just after the start (frame 0 can be black).
UNIT_POSITION = 0.03
CANDIDATE_FRACTIONS = (0.02, 0.05, 0.08, 0.12, 0.2, 0.35, 0.5, 0.95)  # review-sheet columns
CANDIDATES = len(CANDIDATE_FRACTIONS)
_WINGET = Path.home() / "AppData" / "Local" / "Microsoft" / "WinGet" / "Links"


def find_tool(name):
    return shutil.which(name) or next(
        (str(p) for p in (_WINGET / f"{name}.exe",) if p.is_file()), None
    )


def video_refs(src: ArtSource) -> list[str]:
    r"""Distinct INFO_FLC video paths (``obcy_b\L_ATOM.avi``) of units, buildings and projects."""
    refs = set()
    for cls in ("UNIT", "BUILDING", "PROJECT"):
        for rec in src.objects(cls):
            v = rec.get("info_flc") or ""
            if v.lower().endswith(".avi"):
                refs.add(v.lower().replace("/", "\\"))
    return sorted(refs)


def _duration(ffprobe, path):
    r = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
        capture_output=True,
        text=True,
        check=False,
    )
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def still_key(ref: str) -> str:
    return ref.lower().replace("\\", "/")


def load_overrides(path=None) -> dict[str, float]:
    """{clip ref ('/' separators, lower-case): seconds} from stills.yaml (missing -> {})."""
    p = Path(path) if path else STILLS_YAML
    data = (yaml.safe_load(p.read_text(encoding="utf-8")) or {}) if p.is_file() else {}
    return {still_key(k): float(v) for k, v in (data.get("times") or {}).items()}


def _grab(ffmpeg, path, t, dest):
    """One frame at ``t`` seconds -> RGB JPEG ``dest`` (PNG detour: ffmpeg's mjpeg refuses full-range YUV)."""
    tmp = dest.with_suffix(".png")
    r = subprocess.run(
        [
            ffmpeg,
            "-v",
            "error",
            "-y",
            "-ss",
            f"{t:.3f}",
            "-i",
            str(path),
            "-frames:v",
            "1",
            str(tmp),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if r.returncode != 0 or not tmp.is_file():
        return False
    Image.open(tmp).convert("RGB").save(dest, quality=88)
    tmp.unlink()
    return True


def unit_video_refs(src: ArtSource) -> list[str]:
    r"""Distinct UNIT INFO_FLC clips (the ones whose 50% frame often shows the unit from behind)."""
    return sorted(
        {
            v.lower().replace("/", "\\")
            for rec in src.objects("UNIT")
            if (v := rec.get("info_flc") or "").lower().endswith(".avi")
        }
    )


def stills_review_sheets(
    src: ArtSource, review_dir, refs=None, per_sheet=7, cell=(176, 132)
) -> list[str]:
    """Contact sheets stills_units_N.png: one row per clip, CANDIDATES frames,
    each labelled with its time in seconds; the clip's current pick (stills.yaml, else UNIT_POSITION) is printed in red."""
    ffmpeg, ffprobe = find_tool("ffmpeg"), find_tool("ffprobe")
    if not ffmpeg or not ffprobe:
        src.note("ffmpeg/ffprobe not found: stills review sheets skipped")
        return []
    refs = list(refs) if refs is not None else unit_video_refs(src)
    over = load_overrides()
    cache = src.cache / "stills_cand"
    cache.mkdir(parents=True, exist_ok=True)
    paths = []
    for part, i in enumerate(range(0, len(refs), per_sheet)):
        chunk = refs[i : i + per_sheet]
        lab = 150
        sheet = Image.new(
            "RGB", (lab + CANDIDATES * cell[0], len(chunk) * (cell[1] + 14)), (40, 40, 40)
        )
        d = ImageDraw.Draw(sheet)
        for r, ref in enumerate(chunk):
            key = still_key(ref)
            path = src.loose_file("Res/" + key)
            y = r * (cell[1] + 14)
            d.text((3, y + 4), key, fill=(255, 255, 255))
            if path is None:
                continue
            md5 = hashlib.md5(path.read_bytes()).hexdigest()
            dur = _duration(ffprobe, path)
            pick = over.get(key, dur * UNIT_POSITION)
            d.text((3, y + 18), f"dur {dur:.2f}s", fill=(200, 255, 200))
            d.text((3, y + 32), f"pick {pick:.2f}s", fill=(255, 140, 140))
            for n in range(CANDIDATES):
                t = dur * CANDIDATE_FRACTIONS[n] if dur > 0.2 else 0.0
                cp = cache / f"{md5}_{t:.2f}.jpg"
                if not cp.is_file() and not _grab(ffmpeg, path, t, cp):
                    continue
                im = Image.open(cp)
                im.thumbnail(cell)
                x = lab + n * cell[0]
                sheet.paste(im, (x, y))
                d.text((x + 2, y + cell[1]), f"{t:.2f}s", fill=(255, 255, 160))
        dest = Path(review_dir) / f"stills_units_{part + 1}.png"
        dest.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(dest)
        paths.append(str(dest))
    return paths


def extract_stills(
    src: ArtSource, out_dir, refs=None, overrides=None, subdir="stills"
) -> dict[str, str]:
    """{video ref (lower-case, '/' separators): 'stills/<ref>.jpg'}.

    ``overrides`` maps a ref to the frame time in seconds (stills.yaml, then this argument; default 50% of
    the clip, 3% for unit turntable clips). Cached by
    file MD5 + time under ``<cache>/stills``. Returns {} with a note when ffmpeg is missing.
    """
    ffmpeg, ffprobe = find_tool("ffmpeg"), find_tool("ffprobe")
    if not ffmpeg or not ffprobe:
        src.note("ffmpeg/ffprobe not found: video stills skipped")
        return {}
    refs = list(refs) if refs is not None else video_refs(src)
    unit_refs = {still_key(r) for r in unit_video_refs(src)}
    overrides = {still_key(k): v for k, v in {**load_overrides(), **(overrides or {})}.items()}
    cache = src.cache / "stills"
    cache.mkdir(parents=True, exist_ok=True)
    out, failed = {}, []
    for ref in refs:
        key = still_key(ref)
        path = src.loose_file("Res/" + key)
        if path is None:
            failed.append(key)
            continue
        md5 = hashlib.md5(path.read_bytes()).hexdigest()
        t = overrides.get(key)
        if t is None:
            dur = _duration(ffprobe, path)
            pos = UNIT_POSITION if key in unit_refs else DEFAULT_POSITION
            t = dur * pos if dur > 0.2 else 0.0  # one-frame clips: frame 0
        cp = cache / f"{md5}_{float(t):.2f}.jpg"
        if not cp.is_file():
            tmp = cp.with_suffix(".png")  # PNG sidesteps ffmpeg's mjpeg full-range-YUV refusal
            r = subprocess.run(
                [ffmpeg, "-v", "error", "-y", "-ss", f"{t:.3f}", "-i", str(path)]
                + ["-frames:v", "1", str(tmp)],
                capture_output=True,
                text=True,
                check=False,
            )
            if r.returncode != 0 or not tmp.is_file():
                failed.append(key)
                continue
            Image.open(tmp).convert("RGB").save(cp, quality=88)
            tmp.unlink()
        rel = f"{subdir}/{key.replace('/', '_')[: -len('.avi')]}.jpg"
        dest = Path(out_dir) / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cp, dest)
        out[key] = rel
    src.stats["stills_tried"] = len(refs)
    src.stats["stills_ok"] = len(out)
    src.stats["stills_failed"] = failed
    return out
