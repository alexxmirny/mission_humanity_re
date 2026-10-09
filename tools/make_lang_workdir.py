#!/usr/bin/env python3
"""
make_lang_workdir.py -- build one MANUAL-TESTING folder: the clean EN game, the tester build over
it, every language pack that has a retail source, the launcher when it is built, and a README.

    python tools/make_lang_workdir.py                              # -> workdir/lang_test
    python tools/make_lang_workdir.py --out D:\\t --allow-dev        # a dev build (stamp not checked)
    python tools/make_lang_workdir.py --zip dist_tester\\x-tester.zip  # use an existing tester zip
    python tools/make_lang_workdir.py --selftest                   # hermetic: temp dirs only

WHAT THE FOLDER HOLDS.
  * the clean EN install, COPIED from workdir/mh_en_clean (the source is never written);
  * the tester zip unpacked over it (mh.dll, mh_net.dll, mh_net_udp.dll, msvfw32.dll, mh_harness.dll,
    mh_net.ini), from release_package.py --tester against the Release tree, or from --zip;
  * lang/<id>/ for each of ru fr de it pl, built by src/formats/langpack.py from game_data/*_rsr
    (a pack whose retail source is absent is SKIPPED by name, not failed);
  * mh_launcher.exe when src/launcher's release build exists (else the cargo command is printed);
  * the COMMON fonts in the base mh_ex.rsr: `fnt.py install` merges the Cyrillic (lifted from the RU
    retail fonts) and Polish (derived) glyphs onto the EN fonts, so English -- no pack chosen -- still
    renders a Russian or Polish chat line and player name (skipped by name when no RU source is found);
  * README_testers.txt: how to switch language, and which packs were built.

COPY, NOT HARDLINK, BY DEFAULT. The game writes its log and its saves IN PLACE, so a hardlinked
folder would write through to the clean source. --link is there for a read-only tester who wants
the disk space; the tester zip is still unlinked before it is written, so a hardlinked mh.dll never
writes through either.

FONTS. Two layers. Every pack carries its own fonts inside lang/<id>/mh_ex.rsr (fnt.merge_onto with
that language as the base); the BASE install gets the common EN+Cyrillic+Polish set from
`src/formats/fnt.py install` (61 Cyrillic glyphs lifted from the RU retail fonts, 5 Cyrillic + 18
Polish + the guard's box derived; every untouched mh_ex member verified byte-identical), whose
.vanilla backups are removed from the folder.

REPRODUCIBLE. langpack builds are byte-identical; the tree digest printed at the end is the check
(a second build of the same inputs prints the same digest).

Exit: 0 built (skips allowed), 1 a pack or the tester zip failed, 2 refused (bad source, bad output).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGPACK = os.path.join(REPO, "src", "formats", "langpack.py")
FNT = os.path.join(REPO, "src", "formats", "fnt.py")
RELEASE_PACKAGE = os.path.join(REPO, "tools", "release_package.py")
README_NAME = "README_testers.txt"
LAUNCHER_NAME = "mh_launcher.exe"
PACK_ORDER = ("ru", "fr", "de", "it", "pl")
# A pack's retail source folder under game_data/, and the files that prove it is complete (the .rsr
# and its .nam are a matched pair). `ext_rsr` is Extermination, the Polish source (RL18).
PACK_SOURCE = {"ru": "ru_rsr", "fr": "fr_rsr", "de": "de_rsr", "it": "it_rsr", "pl": "ext_rsr"}
PACK_FILES = {"ext_rsr": ("Extermin.rsr", "Extermin.nam")}
DEFAULT_PACK_FILES = ("mh_ex.rsr", "mh_ex.nam")
# The Polish recipe borrows its Z and acute from the DE and FR art (langpack_pl_art.py).
PACK_EXTRA_SOURCES = {"pl": ("de", "fr")}
CODEPAGE = {"ru": 1251, "fr": 1252, "de": 1252, "it": 1252, "pl": 1250}


class Refusal(Exception):
    """A named reason the build will not proceed."""


def main_tree():
    """The main checkout, where the gitignored inputs (workdir/, game_data/) live. A worktree has
    none of them. `MH_MAIN_TREE` overrides; otherwise it is the first entry of `git worktree list`."""
    env = os.environ.get("MH_MAIN_TREE")
    if env:
        return os.path.normpath(env)
    try:
        out = subprocess.run(
            ["git", "worktree", "list", "--porcelain"],
            cwd=REPO,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        for line in out.splitlines():
            if line.startswith("worktree "):
                return os.path.normpath(line[len("worktree ") :])
    except (OSError, subprocess.CalledProcessError):
        pass
    return REPO


MAIN = main_tree()
DEFAULT_SOURCE = os.path.join(MAIN, "workdir", "mh_en_clean")
DEFAULT_OUT = os.path.join(MAIN, "workdir", "lang_test")
DEFAULT_GAME_DATA = os.path.join(MAIN, "game_data")


def rel(path):
    """A path for the log: the main checkout's own paths are printed absolute, because the inputs
    and the output live there and a worktree-relative path would point at nothing."""
    return os.path.normpath(os.path.abspath(path)).replace("\\", "/")


def _inside(path, parent):
    path, parent = (
        os.path.normcase(os.path.abspath(path)),
        os.path.normcase(os.path.abspath(parent)),
    )
    return path == parent or path.startswith(parent + os.sep)


def copy_tree(src, dst, link=False):
    """Copy (or hardlink) every file under src into dst. Returns (copied, linked)."""
    copied = linked = 0
    for root, _dirs, files in os.walk(src):
        target_dir = os.path.join(dst, os.path.relpath(root, src))
        os.makedirs(target_dir, exist_ok=True)
        for name in files:
            s = os.path.join(root, name)
            d = os.path.join(target_dir, name)
            if link:
                try:
                    os.link(s, d)
                    linked += 1
                    continue
                except OSError:
                    # WinError 1314 / cross-device / a filesystem without hardlinks: copy instead.
                    pass
            shutil.copy2(s, d)
            copied += 1
    return copied, linked


def unpack_zip(zip_path, out):
    """Extract the tester zip over out. Each destination is UNLINKED first: it may be a hardlink to
    the clean source, and writing through it would corrupt the source."""
    names = []
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            name = info.filename
            if "/" in name or "\\" in name or name in ("", ".", "..") or os.path.isabs(name):
                raise Refusal(f"zip member {name!r} is not a plain file name")
            dest = os.path.join(out, name)
            if os.path.lexists(dest):
                os.remove(dest)
            with zf.open(info) as src, open(dest, "wb") as fh:
                shutil.copyfileobj(src, fh)
            names.append(name)
    return sorted(names)


def build_tester_zip(version, release_dir, allow_dev, work_dir):
    """Run release_package.py --tester into work_dir and return the zip it wrote."""
    cmd = [
        sys.executable,
        RELEASE_PACKAGE,
        "--version",
        version,
        "--tester",
        "--release-dir",
        release_dir,
        "--out",
        work_dir,
    ]
    if allow_dev:
        cmd.append("--allow-dev")
    proc = _run(cmd)
    if proc.returncode != 0:
        raise Refusal("release_package --tester failed:\n" + (proc.stdout + proc.stderr).strip())
    zips = [n for n in os.listdir(work_dir) if n.endswith("-tester.zip")]
    if len(zips) != 1:
        raise Refusal(f"release_package wrote {len(zips)} tester zips, expected 1")
    return os.path.join(work_dir, zips[0])


def pack_inputs(lang, game_data):
    """(source dir, None) when the pack can be built, else (None, reason)."""
    src = os.path.join(game_data, PACK_SOURCE[lang])
    need = PACK_FILES.get(PACK_SOURCE[lang], DEFAULT_PACK_FILES)
    missing = [f for f in need if not os.path.isfile(os.path.join(src, f))]
    if not os.path.isdir(src):
        return None, f"no source folder {rel(src)}"
    if missing:
        return None, "source {} lacks {}".format(rel(src), ", ".join(missing))
    for extra in PACK_EXTRA_SOURCES.get(lang, ()):
        extra_src = os.path.join(game_data, PACK_SOURCE[extra])
        if not os.path.isfile(os.path.join(extra_src, "mh_ex.rsr")):
            return None, f"needs the {extra.upper()} install for its menu art ({rel(extra_src)})"
    return src, None


def run_langpack(lang, src, en_dir, out):
    cmd = [
        sys.executable,
        LANGPACK,
        "build",
        "--lang",
        lang,
        "--id",
        lang,
        "--en",
        en_dir,
        "--game",
        out,
    ]
    if lang == "ru":
        cmd += ["--ru", src]
    else:
        cmd += ["--src", src]
        ru_dir = os.path.join(DEFAULT_GAME_DATA, PACK_SOURCE["ru"])
        if os.path.isfile(os.path.join(ru_dir, "mh_ex.rsr")):  # the common fonts' Cyrillic donor
            cmd += ["--ru", ru_dir]
    proc = _run(cmd)
    text = (proc.stdout + proc.stderr).strip()
    if proc.returncode != 0:
        return proc.returncode, text
    # The builder's own check: every member present, the pair complete, the codepage pinned.
    chk = _run([sys.executable, LANGPACK, "check", "--game", out, "--id", lang])
    if chk.returncode != 0:
        return chk.returncode, "pack check failed:\n" + (chk.stdout + chk.stderr).strip()
    return 0, text


def run_fonts(en_dir, out):
    """Merge the common fonts into `out`'s base mh_ex.rsr. Returns (rc, text)."""
    build_dir = tempfile.mkdtemp(prefix="lang_workdir_fnt_")
    try:
        proc = _run(
            [
                sys.executable,
                FNT,
                "install",
                "--en",
                en_dir,
                "--game",
                out,
                "--build-dir",
                build_dir,
            ]
        )
    finally:
        shutil.rmtree(build_dir, ignore_errors=True)
    for name in ("mh_ex.rsr.vanilla", "mh_ex.nam.vanilla"):
        p = os.path.join(out, name)
        if os.path.exists(p):
            os.remove(p)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def _run(cmd):
    return subprocess.run(
        cmd,
        cwd=REPO,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def build_packs(langs, game_data, en_dir, out, runner=run_langpack, say=print):
    """Returns (built: {lang: src}, skipped: {lang: reason}, failed: {lang: text})."""
    built, skipped, failed = {}, {}, {}
    for lang in langs:
        src, reason = pack_inputs(lang, game_data)
        if src is None:
            skipped[lang] = reason
            say(f"  SKIP  pack {lang:<3} {reason}")
            continue
        rc, text = runner(lang, src, en_dir, out)
        if rc != 0:
            failed[lang] = text
            say(f"  FAIL  pack {lang:<3} langpack.py exited {rc}:\n{text}")
            continue
        built[lang] = src
        say(f"  built  pack {lang:<3} from {rel(src)}")
    return built, skipped, failed


def find_launcher(candidates):
    # The repo is a Cargo workspace (root Cargo.lock), so the exe lands in <root>/target; a crate-local
    # target/ only exists when the launcher is built from its own directory.
    for root in candidates:
        for rel in (("target",), ("src", "launcher", "target")):
            path = os.path.join(root, *rel, "release", LAUNCHER_NAME)
            if os.path.isfile(path):
                return path
    return None


def tree_digest(out):
    """sha256 over (relative path, file sha256) for every file, in sorted order."""
    h = hashlib.sha256()
    entries = []
    for root, _dirs, files in os.walk(out):
        for name in files:
            p = os.path.join(root, name)
            fh = hashlib.sha256()
            with open(p, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    fh.update(chunk)
            entries.append((os.path.relpath(p, out).replace("\\", "/"), fh.hexdigest()))
    for rp, dig in sorted(entries):
        h.update((f"{rp} {dig}\n").encode())
    return h.hexdigest(), len(entries)


def readme_text(zip_name, tester_names, built, skipped, failed, launcher, out, fonts=None):
    lines = [
        "Mission: Humanity -- language test folder",
        "",
        "START: run mh_launcher.exe (or mh.exe directly; it runs in English until a pack is chosen).",
        "",
        "CONTENTS",
        "  mh.exe, the game data      clean English install (copied from workdir\\mh_en_clean)",
        "  {}".format(", ".join(tester_names)),
        f"                             tester build ({zip_name}), unpacked over the install",
        "  lang\\<id>\\                  one language pack per folder (see BUILT PACKS)",
        "  {}  {}".format(LAUNCHER_NAME, "launcher" if launcher else "NOT BUILT -- see below"),
        "",
        "SWITCH LANGUAGE",
        "  1. Launcher: Settings > Language > \"Language - launcher and game\". Pick a built pack.",
        "     Only languages built for this folder are offered.",
        "  2. Without the launcher: open mh_net.ini in this folder, find [lang], and set:",
        "         pack=ru        (or fr, de, it, pl)",
        "     Delete the line or set pack=en for English.",
        "",
        "BUILT PACKS",
    ]
    for lang in PACK_ORDER:
        if lang in built:
            lines.append(
                f"  {lang:<3} codepage {CODEPAGE[lang]}   lang\\{lang}\\   "
                f"(from {os.path.basename(built[lang])})"
            )
    if not built:
        lines.append("  none -- no retail source was found under game_data\\")
    for lang in PACK_ORDER:
        if lang in skipped:
            lines.append(f"  SKIPPED {lang}: {skipped[lang]}")
        if lang in failed:
            lines.append(f"  FAILED  {lang}: the build did not finish (see the build log)")
    lines += [
        "",
        "FONTS",
        "  Each pack carries its own fonts, inside its mh_ex.rsr.",
        "  English (no pack): "
        + (
            "common fonts -- the EN set plus Cyrillic and Polish letters, for chat and names."
            if fonts is None
            else f"stock EN fonts only -- {fonts}"
        ),
        "",
        "REPORTING",
        "  Note the pack id, the screen, and what is wrong. Attach mh_video.log and mh_net.log.",
        "",
    ]
    if not launcher:
        lines += [
            "LAUNCHER NOT BUILT. From the repo root:",
            "  cargo build --release --manifest-path src\\launcher\\Cargo.toml",
            "then re-run tools\\make_lang_workdir.py.",
            "",
        ]
    return "\r\n".join(lines) + "\r\n"


def build(
    source=DEFAULT_SOURCE,
    out=DEFAULT_OUT,
    game_data=DEFAULT_GAME_DATA,
    zip_path=None,
    release_dir=None,
    version="0.2.0",
    allow_dev=False,
    langs=PACK_ORDER,
    link=False,
    launcher_roots=None,
    runner=run_langpack,
    fonts_runner=run_fonts,
    say=print,
):
    if not os.path.isfile(os.path.join(source, "mh.exe")) or not os.path.isfile(
        os.path.join(source, "mh_ex.rsr")
    ):
        raise Refusal(f"no clean EN install at {source} (needs mh.exe and mh_ex.rsr)")
    out = os.path.abspath(out)
    source = os.path.abspath(source)
    if out == source or _inside(out, source) or _inside(source, out):
        raise Refusal(f"--out {out} overlaps the clean source {source}")
    if _inside(out, game_data):
        raise Refusal(f"--out {out} is inside an input tree")

    if os.path.lexists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    copied, linked = copy_tree(source, out, link=link)
    say(f"copied   {rel(source)} -> {rel(out)} ({copied} files copied, {linked} hardlinked)")

    # The tester build: an existing zip, or a fresh one from the Release tree.
    tmp = None
    try:
        if zip_path is None:
            tmp = tempfile.mkdtemp(prefix="lang_workdir_")
            zip_path = build_tester_zip(version, release_dir, allow_dev, tmp)
        names = unpack_zip(zip_path, out)
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    say("unpacked {} ({})".format(os.path.basename(zip_path), ", ".join(names)))

    built, skipped, failed = build_packs(langs, game_data, source, out, runner=runner, say=say)
    # The common fonts: a failure here is a SKIP by name (no RU source on this machine), not a failed
    # folder -- English still runs, it just cannot draw Cyrillic or Polish chat.
    rc, text = fonts_runner(source, out)
    fonts_skip = None
    if rc == 0:
        say("fonts    common EN+Cyrillic+Polish set merged into mh_ex.rsr")
    else:
        fonts_skip = (text.splitlines() or ["fnt.py install failed"])[-1]
        say(f"fonts    SKIPPED: {fonts_skip}")
    launcher_src = find_launcher(launcher_roots or [REPO, MAIN])
    if launcher_src:
        shutil.copy2(launcher_src, os.path.join(out, LAUNCHER_NAME))
        say(f"copied   launcher {rel(launcher_src)}")
    else:
        say(
            "launcher: not built. Run: cargo build --release --manifest-path "
            "src/launcher/Cargo.toml   (then re-run this tool)"
        )

    with open(os.path.join(out, README_NAME), "w", encoding="ascii", newline="") as fh:
        fh.write(
            readme_text(
                os.path.basename(zip_path),
                names,
                built,
                skipped,
                failed,
                launcher_src is not None,
                out,
                fonts_skip,
            )
        )

    digest, count = tree_digest(out)
    say(f"folder   {rel(out)}: {count} files, tree digest {digest}")
    say("packs    built: %s" % (", ".join(sorted(built, key=PACK_ORDER.index)) or "none"))
    if skipped:
        say("packs    skipped: {}".format(", ".join(sorted(skipped, key=PACK_ORDER.index))))
    return 1 if failed else 0


# ---- selftest: a fake clean install, fake retail sources, a fake tester zip ------------------------


def _write(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def _read(path):
    with open(path, "rb") as fh:
        return fh.read()


def selftest():
    fails = []

    def expect(name, cond):
        if not cond:
            fails.append(name)
        print("  {} {}".format("ok  " if cond else "FAIL", name))

    quiet = lambda *_a, **_k: None
    with tempfile.TemporaryDirectory(prefix="mk_lang_selftest_") as tmp:
        src = os.path.join(tmp, "mh_en_clean")
        gd = os.path.join(tmp, "game_data")
        out = os.path.join(tmp, "lang_test")
        _write(os.path.join(src, "mh.exe"), b"EXE")
        _write(os.path.join(src, "mh_ex.rsr"), b"RSR")
        _write(os.path.join(src, "mh_ex.nam"), b"NAM")
        _write(
            os.path.join(src, "mh.dll"), b"OLD-CLEAN-DLL"
        )  # a stale file the tester zip replaces
        _write(os.path.join(src, "save", "a.sav"), b"SAVE")
        for lang in ("ru", "fr", "de"):
            _write(os.path.join(gd, PACK_SOURCE[lang], "mh_ex.rsr"), lang.encode())
            _write(os.path.join(gd, PACK_SOURCE[lang], "mh_ex.nam"), lang.encode())
        # it: source folder absent -> skipped by name. pl: Extermination absent -> skipped.
        zip_path = os.path.join(tmp, "mission_humanity_re-0.2.0-tester.zip")
        with zipfile.ZipFile(zip_path, "w") as zf:
            zf.writestr("mh.dll", b"NEW-TESTER-DLL")
            zf.writestr("mh_net.ini", b"[lang]\r\n")
        clean_before = _read(os.path.join(src, "mh.dll"))

        def fake_runner(lang, s, en, o):
            _write(os.path.join(o, "lang", lang, "mh_ex.rsr"), b"pack")
            _write(os.path.join(o, "lang", lang, "mh_ex.nam"), b"pack")
            return 0, ""

        fonts_calls = []

        def fake_fonts(en, o):
            fonts_calls.append((en, o))
            _write(os.path.join(o, "mh_ex.rsr"), b"merged")
            return 0, "installed"

        gd_before = sorted(
            (os.path.relpath(os.path.join(r, f), gd), _read(os.path.join(r, f)))
            for r, _d, fs in os.walk(gd)
            for f in fs
        )

        # 1. copy, not link: the output holds the clean files, and the tester zip overwrote mh.dll.
        rc = build(
            source=src,
            out=out,
            game_data=gd,
            zip_path=zip_path,
            link=False,
            launcher_roots=[tmp],
            runner=fake_runner,
            fonts_runner=fake_fonts,
            say=quiet,
        )
        expect("build returns 0 with two skips", rc == 0)
        expect("clean exe copied", _read(os.path.join(out, "mh.exe")) == b"EXE")
        expect("save subtree copied", _read(os.path.join(out, "save", "a.sav")) == b"SAVE")
        expect(
            "tester zip overwrote mh.dll in the output",
            _read(os.path.join(out, "mh.dll")) == b"NEW-TESTER-DLL",
        )
        expect("clean source mh.dll untouched", _read(os.path.join(src, "mh.dll")) == clean_before)
        expect(
            "packs built for ru fr de",
            all(os.path.isdir(os.path.join(out, "lang", x)) for x in ("ru", "fr", "de")),
        )
        expect(
            "skipped packs have no folder",
            not os.path.exists(os.path.join(out, "lang", "it"))
            and not os.path.exists(os.path.join(out, "lang", "pl")),
        )

        # 2. hardlink mode: a hardlinked stale mh.dll must not be written through to the source.
        out2 = os.path.join(tmp, "lang_test_link")
        build(
            source=src,
            out=out2,
            game_data=gd,
            zip_path=zip_path,
            link=True,
            launcher_roots=[tmp],
            runner=fake_runner,
            fonts_runner=fake_fonts,
            say=quiet,
        )
        expect(
            "link mode: output mh.dll is the tester build",
            _read(os.path.join(out2, "mh.dll")) == b"NEW-TESTER-DLL",
        )
        expect(
            "link mode: clean source mh.dll still clean",
            _read(os.path.join(src, "mh.dll")) == clean_before,
        )

        # 3. README: built, skipped and the launcher line; CRLF, ASCII.
        readme = _read(os.path.join(out, README_NAME))
        expect(
            "README lists the built packs",
            b"codepage 1251   lang\\ru\\" in readme and b"codepage 1252   lang\\fr\\" in readme,
        )
        expect(
            "README names the skipped packs", b"SKIPPED it" in readme and b"SKIPPED pl" in readme
        )
        expect(
            "README says no launcher when none is built",
            (b"LAUNCHER NOT BUILT" in readme)
            == (not os.path.exists(os.path.join(out, LAUNCHER_NAME))),
        )
        expect(
            "the common fonts were merged into the OUTPUT, from the clean source",
            bool(fonts_calls)
            and fonts_calls[0] == (os.path.abspath(src), os.path.abspath(out))
            and _read(os.path.join(out, "mh_ex.rsr")) == b"merged",
        )
        expect("README names the common fonts", b"common fonts" in readme)
        out_skip = os.path.join(tmp, "lang_test_nofonts")
        rc_skip = build(
            source=src,
            out=out_skip,
            game_data=gd,
            zip_path=zip_path,
            langs=("ru",),
            launcher_roots=[tmp],
            runner=fake_runner,
            fonts_runner=lambda en, o: (2, "no RU retail source found"),
            say=quiet,
        )
        expect(
            "a missing RU font source is a SKIP by name, not a failed folder",
            rc_skip == 0
            and b"stock EN fonts only -- no RU retail source found"
            in _read(os.path.join(out_skip, README_NAME)),
        )
        expect("README uses CRLF", b"\r\n" in readme and b"\n" not in readme.replace(b"\r\n", b""))

        # 4. the launcher is copied when a release exe exists.
        lroot = os.path.join(tmp, "fake_repo")
        _write(
            os.path.join(lroot, "src", "launcher", "target", "release", LAUNCHER_NAME), b"LAUNCHER"
        )
        out3 = os.path.join(tmp, "lang_test_launcher")
        build(
            source=src,
            out=out3,
            game_data=gd,
            zip_path=zip_path,
            launcher_roots=[lroot],
            runner=fake_runner,
            fonts_runner=fake_fonts,
            say=quiet,
        )
        expect(
            "launcher copied as mh_launcher.exe",
            _read(os.path.join(out3, LAUNCHER_NAME)) == b"LAUNCHER",
        )

        # 5. reproducible: a second build of the same inputs gives the same tree digest.
        d1 = tree_digest(out)
        build(
            source=src,
            out=out,
            game_data=gd,
            zip_path=zip_path,
            launcher_roots=[tmp],
            runner=fake_runner,
            fonts_runner=fake_fonts,
            say=quiet,
        )
        expect("second build: identical tree digest", tree_digest(out) == d1)

        # 6. refusals: out inside the source, a missing source, and a path-traversal zip member.
        def refuses(name, fn):
            try:
                fn()
            except Refusal:
                expect(name, True)
                return
            expect(name, False)

        refuses(
            "refuses an --out inside the clean source",
            lambda: build(
                source=src,
                out=os.path.join(src, "x"),
                game_data=gd,
                zip_path=zip_path,
                runner=fake_runner,
                fonts_runner=fake_fonts,
                say=quiet,
            ),
        )
        refuses(
            "refuses a source with no mh.exe",
            lambda: build(
                source=os.path.join(tmp, "nowhere"),
                out=os.path.join(tmp, "o4"),
                game_data=gd,
                zip_path=zip_path,
                runner=fake_runner,
                fonts_runner=fake_fonts,
                say=quiet,
            ),
        )
        bad = os.path.join(tmp, "bad.zip")
        with zipfile.ZipFile(bad, "w") as zf:
            zf.writestr("../escape.dll", b"x")
        refuses(
            "refuses a zip member that escapes the folder",
            lambda: build(
                source=src,
                out=os.path.join(tmp, "o5"),
                game_data=gd,
                zip_path=bad,
                runner=fake_runner,
                fonts_runner=fake_fonts,
                say=quiet,
            ),
        )

        # 7. the retail sources were never written.
        gd_after = sorted(
            (os.path.relpath(os.path.join(r, f), gd), _read(os.path.join(r, f)))
            for r, _d, fs in os.walk(gd)
            for f in fs
        )
        expect("game_data unchanged", gd_before == gd_after)

    if fails:
        print(f"make_lang_workdir selftest: {len(fails)} FAILED: {', '.join(fails)}")
        return 1
    print("make_lang_workdir selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--out", default=DEFAULT_OUT, help="output folder (default workdir/lang_test)")
    ap.add_argument("--source", default=DEFAULT_SOURCE, help="clean EN install (read-only)")
    ap.add_argument(
        "--game-data", default=DEFAULT_GAME_DATA, help="retail sources, <lang>_rsr folders"
    )
    ap.add_argument("--zip", help="an existing tester zip (skips release_package)")
    ap.add_argument(
        "--release-dir",
        help="Release tree for release_package (default: this tree, then the main one)",
    )
    ap.add_argument(
        "--version", default="0.2.0", help="the version the tester zip is named and stamped with"
    )
    ap.add_argument(
        "--allow-dev",
        action="store_true",
        help="pass --allow-dev to release_package (unstamped build)",
    )
    ap.add_argument("--langs", default=",".join(PACK_ORDER), help="comma list of packs to build")
    ap.add_argument(
        "--link", action="store_true", help="hardlink the clean install instead of copying it"
    )
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    langs = [x.strip() for x in args.langs.split(",") if x.strip()]
    bad = [x for x in langs if x not in PACK_ORDER]
    if bad:
        ap.error("unknown pack(s): {}".format(", ".join(bad)))
    release_dir = args.release_dir
    if release_dir is None and args.zip is None:
        for root in (REPO, MAIN):
            cand = os.path.join(root, "src", "mh_dll", "Release")
            if os.path.isfile(os.path.join(cand, "mh.dll")):
                release_dir = cand
                break
        if release_dir is None:
            print(
                "make_lang_workdir: REFUSED -- no Release build (msbuild src\\mh_dll\\mh.sln) and no --zip"
            )
            return 2
    try:
        return build(
            source=args.source,
            out=args.out,
            game_data=args.game_data,
            zip_path=args.zip,
            release_dir=release_dir,
            version=args.version.lstrip("v"),
            allow_dev=args.allow_dev,
            langs=langs,
            link=args.link,
        )
    except Refusal as e:
        print(f"make_lang_workdir: REFUSED -- {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
