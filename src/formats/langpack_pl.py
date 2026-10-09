#!/usr/bin/env python3
r"""langpack_pl.py -- the POLISH language pack of the EN exe, built from the *Extermination* install (RL18).

Extermination (Techland, 1999) is the Polish-language ancestor of Mission: Humanity: the same engine
family, the same resource formats, and 698+ of MH's 721 initlang keys under the same names. Its
Polish text and voices are retail data, so, like every pack, nothing this module writes is committed.
What is committed is this recipe plus `pl_extra.txt` (the strings WE wrote).

    lang\pl\mh_ex.rsr/.nam   the EN mh_ex (73 members, EN order) with these members replaced:
      fnt\FontLay.txt + fnt\PFMENU0..4.FNT   the COMMON font set (fnt.merge: EN + Cyrillic lifted from RU
                                             retail + Polish + box); EN + Polish only without a RU source
      init\initlang.cfg                      Extermination's Polish text on MH's 721 keys (UTF-16LE)
      info\INFO.TXT                          the 174 Extermination .INF files in MH's <FILE> block format
      info\TUTORIAL.TXT, info\CREDITS.TXT    MH's scripts/credits with Extermination's Polish text
      so\human\*.SAM, so\alien\*.SAM         Extermination's so\m\l, so\m\o voices, resampled to 22000 Hz
      menu\*                                 Polish main-menu art (langpack_pl_art.py)
    lang\pl\pack.ini, mh_strings.txt, BUILD.txt   (codepage 1250; mh.dll's own strings; provenance)

WHY initlang IS REBUILT FROM MH'S KEY LIST AND NOT COPIED: Extermination's file has keys MH does not
know (a different unit roster) and lacks 21 that MH does. The pack carries exactly MH's 721 keys, in
MH's order, each with a Polish value: Extermination's where it has the key, ours (pl_extra.txt) where
it does not. Fixes applied to the retail value, all mechanical:
  * a leading 0x84 (a FONTY08 spacer glyph in Extermination, 31 values) becomes MH's own leading
    whitespace for that key;
  * "Exterminacja" -> "Mission: Humanity";
  * the printf shape (%s %d ...) must equal MH English's; K_MENU_MsgMove / K_MENU_MovingControl
    lack their %s in Extermination and are reworded in pl_extra.txt.
"""

import math
import os
import re
import wave

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
EXTRA_PATH = os.path.join(_HERE, "pl_extra.txt")
BOM = b"\xff\xfe"
CRLF = "\r\n"
BOX_AND_POLISH = None  # filled lazily from fnt (import order)

TEXT_RE = re.compile(r'TEXT\s+"([^"]+)"\s+"((?:[^"\\]|\\.)*)"')
FMT_RE = re.compile(r"%[-0-9.]*l?[a-zA-Z]")

# Extermination's NOTE -> playback rate, measured by the previous session from its sound.cfg and mixer:
# the 15 phrase samples play at NOTE 80 (~44336 Hz), OK / TAKJEST / ROZKAZ at NOTE 321 (~11050 Hz).
# Mission: Humanity plays every voice at SPEED 22000.
SRC_RATE_PHRASE = 44336
SRC_RATE_SHORT = 11050
SHORT_VOICES = ("ok.sam", "takjest.sam", "rozkaz.sam")
DST_RATE = 22000
VOICE_DIRS = {"so\\human\\": "so\\m\\l\\", "so\\alien\\": "so\\m\\o\\"}


# ---------------------------------------------------------------------------------- pl_extra.txt
def read_extra(path=EXTRA_PATH):
    """{section: {key: value}} from pl_extra.txt (UTF-8; `"..."` keeps its inner whitespace)."""
    out, sec = {}, None
    for n, line in enumerate(open(path, encoding="utf-8").read().splitlines(), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("[") and s.endswith("]"):
            sec = s[1:-1]
            out[sec] = {}
            continue
        key, eq, val = line.partition("=")
        if not eq or sec is None:
            raise SystemExit(f"{path}:{n}: expected `key = value` inside a [section]")
        val = val.strip()
        if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
            val = val[1:-1]
        key = key.strip()
        if key in out[sec]:
            raise SystemExit(f"{path}:{n}: {key} given twice in [{sec}]")
        out[sec][key] = val
    return out


# ---------------------------------------------------------------------------------- initlang
def fmt_shape(s):
    return FMT_RE.findall(s)


def build_initlang(en_blob, ex_blob, extra):
    """(utf-16le blob with BOM, report dict). `en_blob` = MH EN initlang (UTF-16LE), `ex_blob` =
    Extermination's initlang (CP1250 bytes). See the module docstring for the rules."""
    en_text = en_blob[2:].decode("utf-16-le")
    en_pairs = TEXT_RE.findall(en_text)
    ex = dict(TEXT_RE.findall(ex_blob.decode("cp1250")))
    add = extra.get("initlang", {})
    rep = {
        "keys": len(en_pairs),
        "from_ex": 0,
        "from_extra": 0,
        "spacer": [],
        "game_name": [],
        "reworded": [],
    }
    lines = [
        "// Mission: Humanity -- Polish text (language pack `pl`, built by src/formats/langpack_pl.py).",
        "// The text is Extermination's (Techland 1999) on Mission: Humanity's key list, plus the strings",
        "// of src/formats/pl_extra.txt. TEXT is bound by NAME, so the order here is irrelevant.",
        "// \\n - newline   \\t - tab   \\2 - hilite colour   & - hotkey",
        "",
    ]
    width = max(len(k) for k, _ in en_pairs) + 2
    for key, en_val in en_pairs:
        if key in add:
            val, src = add[key], "extra"
            if key in ex:
                rep["reworded"].append(key)
        elif key in ex:
            val, src = ex[key], "ex"
            if val.startswith(
                "„"
            ):  # CP1250 0x84, Extermination's FONTY08 spacer -> MH's own leading blanks
                lead = en_val[: len(en_val) - len(en_val.lstrip(" "))]
                val = lead + val[1:].lstrip(" ")
                rep["spacer"].append(key)
            if "Exterminacja" in val:
                val = val.replace("Exterminacja", "Mission: Humanity")
                rep["game_name"].append(key)
        else:
            raise SystemExit(
                f"initlang: {key} is in neither Extermination nor pl_extra.txt [initlang]"
            )
        if fmt_shape(val) != fmt_shape(en_val):
            raise SystemExit(
                f"initlang: {key} printf shape {fmt_shape(val)} != MH English {fmt_shape(en_val)}: {val!r}"
            )
        rep["from_ex" if src == "ex" else "from_extra"] += 1
        lines.append(f'   TEXT "{key}"'.ljust(width + 9) + f' "{val}"')
    unknown = [k for k in add if k not in dict(en_pairs)]
    if unknown:
        raise SystemExit(f"pl_extra.txt [initlang] names keys MH does not have: {unknown}")
    text = CRLF.join(lines) + CRLF
    return BOM + text.encode("utf-16-le"), rep


# ---------------------------------------------------------------------------------- info
FILE_RE = re.compile(r"<FILE ([^>]+)>")


def unwrap_inf(raw):
    """One Extermination .INF (CP1250, hard-wrapped at ~80 columns with a 3-space indent) -> paragraphs
    joined onto single lines, which is the shape MH's own blocks have (the game wraps them itself)."""
    text = raw.decode("cp1250").replace("\r\n", "\n")
    paras = [p for p in re.split(r"\n\s*\n", text) if p.strip()]
    out = []
    for p in paras:
        s = " ".join(line.strip() for line in p.split("\n") if line.strip())
        out.append(re.sub(r"\s+", " ", s).strip())
    return out


def build_info(en_info_blob, ex_files, extra):
    """info\\INFO.TXT: MH's block list and order, Extermination's Polish bodies (UTF-16LE)."""
    t = en_info_blob[2:].decode("utf-16-le")
    names = FILE_RE.findall(
        t
    )  # two MH blocks (O_ELEKT, O_WIEZ1) carry their text on the header line
    add = extra.get("info", {})
    ex_by = {k.lower(): v for k, v in ex_files.items()}
    out, rep = [], {"blocks": len(names), "from_ex": 0, "from_extra": 0}
    for name in names:
        k = name.lower()
        k = k.replace("/", "\\")
        if k in ex_by:
            paras = unwrap_inf(ex_by[k])
            rep["from_ex"] += 1
        else:
            hit = [v for kk, v in add.items() if kk.lower() == k]
            if not hit:
                raise SystemExit(
                    f"info: {name} is in neither Extermination nor pl_extra.txt [info]"
                )
            paras = [hit[0]]
            rep["from_extra"] += 1
        out.append(f"<FILE {name}>")
        out.extend(paras)
    return BOM + (CRLF.join(out) + CRLF + CRLF).encode("utf-16-le"), rep


# ---------------------------------------------------------------------------------- tutorial
QUOTED = re.compile(r'"(.*?)"', re.S)


def build_tutorial(en_blob, ex_blob):
    """MH's tutorial script with the quoted message texts replaced, in order, by Extermination's.
    The script structure (Colors, Panel, Reakcje, Komenda) is MH's; the quoted texts are the only
    language. Extermination's colour escapes (\\1..\\4) are dropped: MH's own English has none."""
    en = en_blob[2:].decode("utf-16-le")
    ex = ex_blob.decode("cp1250")
    en_q = QUOTED.findall(en)
    ex_q = QUOTED.findall(ex)
    if len(en_q) != len(ex_q):
        raise SystemExit(f"tutorial: MH has {len(en_q)} messages, Extermination {len(ex_q)}")
    it = iter(ex_q)

    def sub(_m):
        s = next(it)
        s = re.sub(r"\\[1-5]", "", s)
        s = s.replace("w Eksterminację", "Mission: Humanity").replace(
            "Exterminacja", "Mission: Humanity"
        )
        s = re.sub(r"[ \t]+\n", "\n", s)
        return '"' + s + '"'

    out = QUOTED.sub(sub, en)
    if re.search(r"xterminac|ksterminac", out):
        raise SystemExit("tutorial: an Extermination mention is left")
    return BOM + out.encode("utf-16-le"), {"messages": len(en_q)}


# ---------------------------------------------------------------------------------- credits
CREDIT_HEADINGS = [  # MH heading order -> index of Extermination's heading list (None = no counterpart)
    # PRODUCTION, PROJECT MANAGEMENT, SCREENPLAY, PROGRAMMING, Lead Programmer, GRAPHICS, 2D graphics,
    # 3D graphics, Film sequences, SOUND, Soundtrack, Special effects, MAP DESIGN, QA, Lead Tester,
    # PACKING DESIGN, MARKETING, SALES DEPARTMENT, INTERNATIONAL SALES, Special thanks
    0,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    20,
]


def _fold(s):
    import unicodedata

    s = s.replace("ł", "l").replace("Ł", "L")
    return (
        "".join(c for c in unicodedata.normalize("NFD", s) if not unicodedata.combining(c))
        .lower()
        .strip()
    )


def build_credits(en_blob, ex_blob):
    """MH's credits (its team, its structure) with Extermination's Polish section headings and
    accented names. A name MH lists that Extermination does not keeps MH's spelling. The copyright
    block is MH's, with Extermination's Polish for the legal lines."""
    en = en_blob[2:].decode("utf-16-le").split(CRLF)
    ex = ex_blob.decode("cp1250").split(CRLF)
    # Extermination's headings in order; its title line (EXTERMINACJA, Copyright ...) follows the 21st
    ex_heads = [ln for ln in ex if ln.startswith("\\6")][:21]
    if len(ex_heads) < 21 or "podzi" not in ex_heads[20]:
        raise SystemExit(
            f"credits: Extermination's heading list is not the expected 21 ({len(ex_heads)})"
        )
    names = {
        _fold(ln): ln.strip()
        for ln in ex
        if ln.strip() and not ln.startswith("\\") and re.search(r"[A-Za-z]", ln)
    }
    out, head_i, in_tail = [], 0, False
    for ln in en:
        if ln.startswith("\\6MISSION: HUMANITY"):
            in_tail = True
        if ln.startswith("\\6") and not in_tail:
            if head_i >= len(CREDIT_HEADINGS):
                raise SystemExit("credits: more MH headings than the mapping knows")
            ex_h = ex_heads[CREDIT_HEADINGS[head_i]]
            head_i += 1
            # keep MH's own closing marker convention (some headings end \7, some do not)
            out.append(
                ex_h
                if ex_h.endswith("\\7") == ln.endswith("\\7")
                else (ex_h + "\\7" if ln.endswith("\\7") else ex_h[:-2])
            )
        elif in_tail:
            if "protected" in ln:
                out.append("\\6Program ten chroniony jest\\7")
            elif "by international" in ln:
                out.append("\\6przez prawa autorskie\\7")
            elif "applicable international" in ln:
                out.append("\\6i umowy międzynarodowe.\\7")
            elif "All rights" in ln:
                out.append("\\6Wszelkie prawa zastrzeżone.\\7")
            else:
                out.append(ln)  # title, MH's own copyright line, MH's URL -- verbatim
        elif ln.strip() and not ln.startswith("\\"):
            out.append(names.get(_fold(ln), ln.strip()))
        else:
            out.append(ln)
    if head_i != len(CREDIT_HEADINGS):
        raise SystemExit(f"credits: {head_i} MH headings, the mapping has {len(CREDIT_HEADINGS)}")
    text = CRLF.join(out)
    if re.search(r"xterminac|ksterminac", text):
        raise SystemExit("credits: an Extermination mention is left")
    return BOM + text.encode("utf-16-le"), {"headings": head_i}


# ---------------------------------------------------------------------------------- voices
def _kaiser_win(u, beta):
    u = np.clip(u, -1.0, 1.0)
    return np.i0(beta * np.sqrt(1.0 - u * u)) / np.i0(beta)


def resample(x, src_rate, dst_rate, half_width=24, beta=8.0, block=512):
    """Band-limited windowed-sinc resampler (float64 in/out). The cutoff is 0.45 * min(rates), so
    downsampling low-passes first; deterministic (fixed block size, fixed summation order)."""
    x = np.asarray(x, dtype=np.float64)
    ratio = src_rate / dst_rate  # input samples per output sample
    fc = 0.5 * min(1.0, dst_rate / src_rate) * 0.90  # cycles per input sample
    hw = int(math.ceil(half_width * max(1.0, ratio)))
    n_out = int(len(x) / ratio)
    pad = np.concatenate([np.zeros(hw + 1), x, np.zeros(hw + 1)])
    taps = np.arange(-hw, hw + 1)
    out = np.zeros(n_out)
    for b0 in range(0, n_out, block):
        j = np.arange(b0, min(n_out, b0 + block))
        t = j * ratio
        i0 = np.floor(t).astype(np.int64)
        k = taps[None, :] - (t - i0)[:, None]
        h = 2 * fc * np.sinc(2 * fc * k) * _kaiser_win(k / hw, beta)
        seg = pad[(i0 + hw + 1)[:, None] + taps[None, :]]
        out[b0 : b0 + len(j)] = np.einsum("ij,ij->i", h, seg)
    return out


def build_voice(raw, src_rate, gain_target_rms=None):
    """signed-8-bit PCM at src_rate -> signed-8-bit PCM at DST_RATE. Optional loudness normalisation
    to `gain_target_rms` (in 8-bit counts), capped so the peak never exceeds 127."""
    x = np.frombuffer(raw, dtype=np.int8).astype(np.float64)
    y = resample(x, src_rate, DST_RATE)
    gain = 1.0
    if gain_target_rms and y.std() > 0:
        gain = min(gain_target_rms / y.std(), 127.0 / max(1.0, np.abs(y).max()))
    y = np.clip(np.rint(y * gain), -128, 127).astype(np.int8)
    return y.tobytes(), gain


def voice_source_rate(name):
    return SRC_RATE_SHORT if name.split("\\")[-1].lower() in SHORT_VOICES else SRC_RATE_PHRASE


def write_wav(path, pcm_s8, rate=DST_RATE):
    """16-bit mono WAV of a signed-8-bit buffer (for listening review)."""
    s16 = (np.frombuffer(pcm_s8, dtype=np.int8).astype(np.int16) * 256).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(s16.tobytes())


# ---------------------------------------------------------------------------------- the build
def _put(members, key, blob, report, why):
    """Replace the member whose lower-cased full name is `key` (stored uncompressed: the loader only
    inflates on the `LZW ` magic)."""
    hits = [m for m in members if m.key() == key]
    if len(hits) != 1:
        raise SystemExit(f"expected exactly one {key} in the EN pack, found {len(hits)}")
    m = hits[0]
    m.stored = blob
    m.final_size = len(blob)
    report.append(f"  {m.name:28} {len(blob):9} bytes  {why}")


def build_fonts_pl(fnt, en_src, common_ru=None):
    """The pack's fonts. With a RU retail source (`common_ru`): the COMMON set every pack carries
    (fnt.merge: EN + Cyrillic lifted from RU + the derivation table). Without: EN + the 18 Polish
    letters and the box only (fnt.merge_onto, no donor)."""
    en = en_src.font_layer("mh_ex")
    if common_ru is not None:
        merged, rep = fnt.merge(en, common_ru.font_layer("mh_ex"))
    else:
        merged, rep = fnt.merge_onto(
            en,
            en,
            lambda _cp: False,
            "n/a",
            derive=lambda cp: cp in fnt.POLISH or cp == fnt.BOX_CODEPOINT,
        )
    for name in fnt.PFMENU:
        if not fnt.verify_prefix(en.fonts[name], merged.fonts[name], len(en.layout)):
            raise SystemExit(
                f"{name}: the Polish derivation moved or changed an EN record -- refusing"
            )
    if merged.layout[: len(en.layout)] != en.layout:
        raise SystemExit("the Polish derivation reordered the EN FONTLAY -- refusing")
    blobs = {"fnt\\fontlay.txt": fnt.write_fontlay(merged.layout)}
    for name in fnt.PFMENU:
        blobs["fnt\\" + name.lower()] = merged.fonts[name].to_bytes()
    return blobs, merged, rep, en


def build_voices(L, members_en, ext_by):
    """{pack member key: (bytes, gain, src bytes)} for the 36 voices, loudness-matched to MH English's."""
    rms = []
    for m in members_en:
        k = m.key()
        if k.startswith("so\\human\\") or k.startswith("so\\alien\\"):
            rms.append(float(np.frombuffer(L.payload(m), dtype=np.int8).astype(np.float64).std()))
    target = float(np.median(rms))
    out = {}
    for m in members_en:
        k = m.key()
        for dst, src in VOICE_DIRS.items():
            if k.startswith(dst):
                sk = src + k[len(dst) :]
                if sk not in ext_by:
                    raise SystemExit(f"voices: {sk} is not in Extermination")
                raw = ext_by[sk]
                pcm, gain = build_voice(raw, voice_source_rate(k), gain_target_rms=target)
                out[k] = (pcm, gain, raw)
    if len(out) != 36:
        raise SystemExit(f"voices: expected 36, built {len(out)}")
    return out, target


def build_pl(L, fnt, ext_src, en_src, out_dir, quiet=False, art_donors=None, lang_id="pl", common_ru=None):
    """Write <out_dir> (a lang\\pl folder). `L` = the langpack module, `ext_src` = the Extermination
    install, `en_src` = the EN install; `art_donors` = {"de": Source, "fr": Source} for the main-menu
    art letters (None = keep the EN art)."""
    spec = L.PACKS["pl"]
    report = []
    members = L.read_pack(en_src)
    en_by = {m.key(): m for m in members}
    xmin = {m.key(): m for m in L.read_pack(ext_src, "Extermin")}
    xex = {m.key(): m for m in L.read_pack(ext_src, "Extermex")}
    extra = read_extra()

    # 1. fonts
    blobs, merged, frep, en_layer = build_fonts_pl(fnt, en_src, common_ru)
    if common_ru is None and not quiet:
        print(L.no_common_warning("pl", "EN+Polish"))
    for key, blob in blobs.items():
        _put(
            members,
            key,
            blob,
            report,
            "fonts: %s, EN %d -> %d FONTLAY entries (%d lifted, %d derived)"
            % ("common set" if common_ru is not None else "EN+Polish", len(en_layer.layout),
               len(merged.layout), len(frep.lifted), len(frep.derived)),
        )
    # 2. initlang, info, tutorial, credits
    il, irep = build_initlang(
        L.payload(en_by["init\\initlang.cfg"]), L.payload(xex["init\\initlang.cfg"]), extra
    )
    _put(
        members,
        "init\\initlang.cfg",
        il,
        report,
        "initlang: %d keys (%d Extermination, %d ours; %d spacers, %d game-name)"
        % (
            irep["keys"],
            irep["from_ex"],
            irep["from_extra"],
            len(irep["spacer"]),
            len(irep["game_name"]),
        ),
    )
    ex_inf = {k: L.payload(m) for k, m in xmin.items() if k.endswith(".inf")}
    info, nrep = build_info(L.payload(en_by["info\\info.txt"]), ex_inf, extra)
    _put(
        members,
        "info\\info.txt",
        info,
        report,
        "info: %d blocks (%d Extermination, %d ours)"
        % (nrep["blocks"], nrep["from_ex"], nrep["from_extra"]),
    )
    tut, _ = build_tutorial(
        L.payload(en_by["info\\tutorial.txt"]), L.payload(xmin["menu\\tutorial.txt"])
    )
    _put(
        members,
        "info\\tutorial.txt",
        tut,
        report,
        "tutorial: MH script, Extermination's Polish messages",
    )
    cr, _ = build_credits(
        L.payload(en_by["info\\credits.txt"]), L.payload(xmin["menu\\credits.txt"])
    )
    _put(members, "info\\credits.txt", cr, report, "credits: MH's team, Polish headings and names")
    # 3. voices
    ext_raw = {k: L.payload(m) for k, m in xmin.items() if k.startswith("so\\m\\")}
    voices, vtarget = build_voices(L, members, ext_raw)
    for key, (pcm, gain, _raw) in voices.items():
        _put(members, key, pcm, report, "voice: resampled to %d Hz, gain x%.2f" % (DST_RATE, gain))
    # 4. menu art
    art_lines = []
    if art_donors is not None:
        import langpack_pl_art as A

        ext_menu = {m.name: L.payload(m) for m in xmin.values() if m.key().startswith("menu\\")}
        art, art_lines = A.build_art(L, members, art_donors, ext_menu)
        for key, blob in art.items():
            _put(members, key, blob, report, "menu art")
    L.write_pack(members, out_dir)
    L.write_pack_ini(out_dir, spec["codepage"])
    lines = [
        f"language pack built by {L.TOOL} + src/formats/langpack_pl.py (format {L.FORMAT_VERSION}); retail game data -- never commit",
        f"pl source: {os.path.basename(ext_src.path)} (Extermin + Extermex)  Extermin.rsr md5 {L.md5(ext_src._read('Extermin', '.rsr'))}",
        f"en source: {os.path.basename(en_src.path)}  mh_ex.rsr md5 {L.md5(en_src._read('mh_ex', '.rsr'))}",
        "recipe: EN mh_ex + common fonts (Polish letters, Cyrillic when a RU source exists), Extermination text/info/voices, MH tutorial+credits structure",
        "Msgs.dat: none for pl (the stock one is used)",
        f"pack.ini: codepage {spec['codepage']}",
        L.install_strings(lang_id, out_dir),
        f"voices: loudness target rms {vtarget:.1f} (median of MH English's 36)",
    ]
    for ext in (".nam", ".rsr"):
        lines.append(
            f"mh_ex{ext} md5 {L.md5(open(os.path.join(out_dir, 'mh_ex' + ext), 'rb').read())}"
        )
    lines += [r.strip() for r in report] + art_lines
    with open(os.path.join(out_dir, "BUILD.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if not quiet:
        for line in lines:
            print(line)
    return report
