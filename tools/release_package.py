#!/usr/bin/env python3
"""release_package.py -- assemble THE release zip from a built `src/mh_dll/Release` tree, and
refuse to publish anything mislabelled.

WHY A TOOL AND NOT A WORKFLOW STEP (tracker TL-CI1). A zip whose binaries were never stamped with
the tag it is named after is a release nobody can trace a bug report back to, and a zip that quietly
gains or loses a module is a different product under the same name. Both failures are silent at
packaging time and expensive afterwards. So the rules live in a script with a `--selftest` that runs
in CI on a runner with no game, and the workflow just calls it.

    python tools/release_package.py --version 0.2.0
    python tools/release_package.py --version 0.2.0 --release-dir src/mh_dll/Release --out dist
    python tools/release_package.py --version 0.2.0 --allow-dev   # local dry run, stamp not checked
    python tools/release_package.py --version 0.2.0 --tester --out dist_tester   # NEVER in CI
    python tools/release_package.py --selftest                    # hermetic; a lint_repo row

---- ONE ZIP (v0.2.0 plan decision D1; tracker RL7) ------------------------------------------------

    <base>-<version>-net.zip    msvfw32.dll  mh.dll  mh_net.dll  mh_net_udp.dll
                                -> configuration (1): all-original game + the restored multiplayer.

Unzip it next to the game executable. That is the whole install, and it is exactly FOUR files:

  * NO mh_net.ini (RL3). An mh_net.ini beside the exe means PORTABLE MODE; the DLL otherwise runs on
    its own code defaults and the launcher writes the player's ini into the per-user config dir. A
    packaged ini would therefore silently switch every install into portable mode.
  * NO README.txt, LICENSE, THIRD_PARTY.md: the launcher embeds them (About page).
  * NO mh_harness.dll, NO libmh.dll: the harness is a developer/tester instrument (dev lanes get it
    from make_lane), and the hosted spine is not a shipped configuration. The `net-debug` and
    `brokered-debug` zips of v0.1.x are gone; diagnostics are `[log] level=debug` in the user's ini.

SHA256SUMS covers the zip, in `sha256sum -c` format. gen_update_manifest.py reads it.

THE STAMP. The packager gates the build stamp (`check_stamp`): every shipped module's VERSIONINFO
FileVersion must carry --version. The same string is what mh_net.log's first line reports at run
time (`; [build] mh <version>+<commit>`), which is how a bug report is traced to a tag.

---- THE TESTER ZIP (explicit flag only) ----------------------------------------------------------

`--tester` builds `<base>-<version>-tester.zip` INSTEAD of the ship zip: the four modules plus
mh_harness.dll and an `mh_net.ini` that is the reference ini with `[dev] unlock=1`, `[log]
level=debug`, the three HARNESS_KEYS flipped (the harness armed, clock not pinned) -- the
harness keys are dev keys, hence the unlock. The ini is
there on purpose -- the tester wants portable mode and the dev keys. release.yml never passes
`--tester`; the selftest asserts the ship zip has no ini and no harness.

---- THE REFUSALS ---------------------------------------------------------------------------------

Both exit non-zero with a named reason, because a packager that guesses is worse than one that
stops.

  MISSING ARTIFACT    a file the zip declares is not in the release dir. Named individually.

  STAMP MISMATCH      a module's VERSIONINFO FileVersion does not carry `--version`. This is the
                      check that makes the zip's NAME evidence rather than decoration. `--allow-dev`
                      bypasses it for a local dry run against a tree built with no properties, and
                      it is the only thing that bypasses it.
"""

import argparse
import hashlib
import io
import os
import re
import shutil
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_RELEASE_DIR = os.path.join(REPO, "src", "mh_dll", "Release")
DEFAULT_OUT_DIR = os.path.join(REPO, "dist")
EXAMPLE_INI = os.path.join(REPO, "src", "mh_dll", "mh_net.example.ini")

BASE_NAME = "mission_humanity_re"
SUMS_NAME = "SHA256SUMS"

# The modules whose VERSIONINFO must carry --version: the ones that import
# src/mh_dll/mh_version.props; nothing else in the release dir is stamped, so nothing else can be
# checked.
STAMPED = ("msvfw32.dll", "mh.dll", "mh_net.dll", "mh_net_udp.dll", "mh_harness.dll")


class Refusal(Exception):
    """A named reason to stop. Never a traceback: a packaging refusal is an ANSWER."""


# --------------------------------------------------------------------------- the ini variants

# (section, key, value, why). Flipped in the tester ini (the ship zip has no ini). Since dist RL6 this is ONE key:
# `[log] level=debug`. The five observer keys the debug ini used to flip one by one (v0.1.x) (OBSERVERS_AT_DEBUG
# below) are now supplied as DEFAULTS by that level inside mh.dll (src/mh_dll/mh_common/include/
# mh_ini_gate.h, LEVEL table) -- and they HAD to move: since dist RL2 the shipped DLL ignores a dev key
# written in an ini that does not carry `[dev] unlock=1`, so flipping them here would be a no-op.
DEBUG_KEYS = (
    (
        "log",
        "level",
        "debug",
        "turn on the observer logs a bug report wants -- sp_clock_log, temporal_sp, desync verbose, "
        "mouse_trace and the whole-match state record (mh_match_state.bin). All observers: they write "
        "lines and change nothing the game does.",
    ),
)

# The (section, key, value) set `[log] level=debug` must supply -- the former DEBUG_KEYS, verbatim.
# NOT used to write anything: --selftest cross-checks it against the C++ level table, so the two
# statements of "what debug means" cannot drift apart. `quiet` is the other half of that check.
OBSERVERS_AT_DEBUG = (
    ("net", "sp_clock_log", "1"),
    ("trace", "temporal_sp", "1"),
    ("desync", "verbose", "1"),
    ("input", "mouse_trace", "1"),
    ("desync", "state_record", "1"),
)
OBSERVERS_AT_QUIET = (
    ("net", "lockstep_log", "0"),
    ("net", "frametime_log", "0"),
)
LEVEL_TABLE_SRC = os.path.join(REPO, "src", "mh_dll", "mh_common", "include", "mh_ini_gate.h")

# (section, key, value, why). The determinism/replay INSTRUMENT, armed in the tester ini (the zip that
# carries mh_harness.dll). Applied AFTER DEBUG_KEYS, same _flip, same selftest invariant.
HARNESS_KEYS = (
    (
        "harness",
        "enable",
        "1",
        "install the determinism trampolines and hash EVERY sim step into mh_harness.log -- the "
        "per-step record tools/mp_analyze.py joins across peers to name the first diverging step.",
    ),
    (
        "harness",
        "fixed_step",
        "0",
        "do NOT pin the game clock. The harness's compiled default (1) makes the sim run one step "
        "per present, which is a different game; 0 is what the determinism gate runs with.",
    ),
    (
        "harness",
        "order_mode",
        "1",
        "RECORD every dispatched order to mh_orders.bin in the run folder, so a desynced match can "
        "be replayed offline against the hashes.",
    ),
)


# (section, key, value, why). A zip that arms the harness must also lift the dev gate: every [harness]
# key is a dev key and the shipped DLL ignores it (logging IGNORED) without `[dev] unlock=1`.
UNLOCK_KEYS = (
    (
        "dev",
        "unlock",
        "1",
        "the harness keys are dev keys, ignored by the shipped DLL unless the ini unlocks them. This "
        "also honours every other dev key the reference ini lists. Only a zip that arms the harness "
        "carries it; it is a test build, not a player one.",
    ),
)


def read_example_ini():
    if not os.path.isfile(EXAMPLE_INI):
        raise Refusal(
            "no reference ini at %s -- it is the source of the tester variant" % rel(EXAMPLE_INI)
        )
    with io.open(EXAMPLE_INI, encoding="utf-8") as fh:
        return fh.read()


def _flip(text, section, key, value):
    """Set `key` inside `[section]` to `value`, keeping the rest of the line. Returns the new text.

    Scoped to the section on purpose: `verbose` and `overlay` are not unique key names in this file,
    and a global regex would edit whichever one came first. Refuses rather than no-ops when the
    section or the (uncommented) key is not there -- a debug ini silently missing a key it claims to
    set is the failure this whole tool is written against.
    """
    lines = text.split("\n")
    in_section = False
    hit = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_section = stripped[1:-1].strip().lower() == section.lower()
            continue
        if not in_section:
            continue
        m = re.match(r"^(\s*)(%s)(\s*)=(\s*)(\S*)(.*)$" % re.escape(key), line)
        if not m:
            continue
        if hit is not None:
            raise Refusal(
                "reference ini has TWO uncommented `%s` keys in [%s] (lines %d and %d) -- "
                "GetPrivateProfile* reads the first, so the file is ambiguous and the debug "
                "variant cannot be derived from it" % (key, section, hit + 1, i + 1)
            )
        hit = i
        # Keep the column the value starts at, so the trailing comment stays aligned and the diff
        # against the ship ini is one character wide.
        pad = " " * max(0, len(m.group(5)) - len(value))
        lines[i] = "%s%s%s=%s%s%s%s" % (
            m.group(1),
            m.group(2),
            m.group(3),
            m.group(4),
            value,
            pad,
            m.group(6),
        )
    if hit is None:
        raise Refusal(
            "reference ini has no uncommented `%s` key in section [%s] -- the debug variant claims "
            "to turn it on, so either the key moved or the claim is stale" % (key, section)
        )
    return "\n".join(lines)


def tester_keys():
    """The tester ini's flip list: debug level + the harness keys + the dev unlock they need."""
    return DEBUG_KEYS + HARNESS_KEYS + UNLOCK_KEYS


TESTER_HEADER = """\
; mh_net.ini -- TESTER configuration, packaged with {zipname}.
;
; This file is src/mh_dll/mh_net.example.ini with {n} key(s) changed, and nothing else:
;
{keylist}
;
; An mh_net.ini beside the exe means PORTABLE MODE: logs, key file and state live in this folder.
; ================================================================================================

"""


def _keylist(keys):
    return "\n".join(";   [%s] %s=%s -- %s" % (s, k, v, why) for s, k, v, why in keys)


def tester_header(zipname):
    keys = tester_keys()
    return TESTER_HEADER.format(zipname=zipname, n=len(keys), keylist=_keylist(keys))


def make_tester_ini(text, zipname):
    body = text
    for section, key, value, _why in tester_keys():
        body = _flip(body, section, key, value)
    return tester_header(zipname) + body


def ini_diff_lines(ship_body, debug_body):
    """The per-line difference between the two ini BODIES (headers excluded). Used by the selftest
    and by the `--explain` print; returns [(lineno, ship, debug)]."""
    a, b = ship_body.split("\n"), debug_body.split("\n")
    if len(a) != len(b):
        return [(0, "line count %d" % len(a), "line count %d" % len(b))]
    return [(i + 1, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]


# --------------------------------------------------------------------------- the stamp


def read_file_version(path):
    """The FileVersion STRING out of a PE's VERSIONINFO resource, or None when there is none.

    The string field rather than the numeric one, because the numeric quad cannot hold a
    pre-release suffix: 0.1.0 and 0.1.0-rc1 are the same four integers and a packager that compared
    those would happily label an rc as its release.
    """
    import lief

    binary = lief.parse(path)
    if binary is None:
        raise Refusal("%s is not a file LIEF can parse as a PE" % rel(path))
    rm = binary.resources_manager
    if rm is None or not rm.has_version:
        return None
    for version in rm.version:
        sfi = version.string_file_info
        if sfi is None:
            continue
        for table in sfi.children:
            for entry in table.entries:
                if entry.key == "FileVersion":
                    return entry.value
    return None


# A seam, and the only one in this file. `--selftest` replaces it so the REFUSAL LOGIC can be
# exercised over fake artifacts in a temp directory with no toolchain and no PE writer; the reader
# itself is exercised by every real run, and the selftest also probes it against the real
# Release\\mh.dll when that tree happens to exist (printing a note when it does not, rather than
# skipping silently).
_STAMP_READER = read_file_version


def check_stamp(release_dir, version, names, say):
    bad = []
    for name in names:
        if name not in STAMPED:
            continue
        got = _STAMP_READER(os.path.join(release_dir, name))
        if got is None:
            bad.append((name, "<no VERSIONINFO resource>"))
        elif not got.split("+")[0] == version:
            bad.append((name, got))
        else:
            say("  stamp ok   %-16s FileVersion=%s" % (name, got))
    if bad:
        raise Refusal(
            "VERSION STAMP MISMATCH: --version %s, but %d module(s) carry something else:\n%s\n"
            "The binaries were built without /p:MhVersion=%s (or from another tag). Rebuild with "
            "the properties, or pass --allow-dev for a local dry run that does not claim a version."
            % (
                version,
                len(bad),
                "\n".join("    %-16s %s" % (n, v) for n, v in bad),
                version,
            )
        )


# --------------------------------------------------------------------------- the zips

SHIP_MODULES = ("msvfw32.dll", "mh.dll", "mh_net.dll", "mh_net_udp.dll")
TESTER_MODULES = SHIP_MODULES + ("mh_harness.dll",)


def rel(path):
    try:
        return os.path.relpath(path, REPO).replace("\\", "/")
    except ValueError:
        return path


def zip_name(version, tag):
    return "%s-%s-%s.zip" % (BASE_NAME, version, tag)


def expected_entries(tag):
    """The EXACT file list of one zip, as entry names. One function, used by the builder and by the
    selftest's assertion, so the two cannot describe different zips."""
    if tag == "net":
        return sorted(SHIP_MODULES)
    if tag == "tester":
        return sorted(list(TESTER_MODULES) + ["mh_net.ini"])
    raise KeyError(tag)


def build(version, release_dir, out_dir, allow_dev=False, tester=False, say=print):
    release_dir = os.path.abspath(release_dir)
    out_dir = os.path.abspath(out_dir)
    tag = "tester" if tester else "net"
    modules = TESTER_MODULES if tester else SHIP_MODULES

    if not os.path.isdir(release_dir):
        raise Refusal(
            "no release directory at %s -- build first:\n"
            "    msbuild src\\mh_dll\\mh.sln /t:Build /p:Configuration=Release /p:Platform=x86 "
            "/m /nodeReuse:false" % rel(release_dir)
        )

    # MISSING ARTIFACT, named individually and all at once.
    missing = [m for m in modules if not os.path.isfile(os.path.join(release_dir, m))]
    if missing:
        raise Refusal(
            "MISSING ARTIFACT: %d module(s) are not in %s:\n%s\n"
            "A green build line is not proof the set is complete; check the artifacts by name."
            % (len(missing), rel(release_dir), "\n".join("    " + m for m in missing))
        )

    if allow_dev:
        say(
            "  stamp check SKIPPED (--allow-dev): these zips do not claim to be version %s"
            % version
        )
    else:
        check_stamp(release_dir, version, modules, say)

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    name = zip_name(version, tag)
    path = os.path.join(out_dir, name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for m in modules:
            zf.write(os.path.join(release_dir, m), m)
        if tester:
            _writestr(zf, "mh_net.ini", make_tester_ini(read_example_ini(), name))
    got = sorted(zipfile.ZipFile(path).namelist())
    want = expected_entries(tag)
    if got != want:
        raise Refusal("%s holds %r, expected %r" % (name, got, want))
    say("  packaged   %-46s %d entries, %d B" % (name, len(got), os.path.getsize(path)))

    sums = os.path.join(out_dir, SUMS_NAME)
    with io.open(sums, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("%s  %s\n" % (sha256(path), name))
    say("  packaged   %-46s over 1 zip" % SUMS_NAME)
    say("release_package: %s + %s in %s" % (name, SUMS_NAME, rel(out_dir)))
    return [name]


def _writestr(zf, name, text):
    """CRLF for the two text files we author. They are read in Notepad on a Windows machine that has
    just unzipped them, and a lone-LF README is the first thing that says 'this was not made for
    you'."""
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    zf.writestr(info, text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_sums(out_dir, say=print):
    """Re-read SHA256SUMS and re-hash what it names. Cheap, and it is the only thing that proves the
    file covers the zips that are actually there rather than a set from an earlier run."""
    path = os.path.join(out_dir, SUMS_NAME)
    if not os.path.isfile(path):
        raise Refusal("no %s in %s" % (SUMS_NAME, rel(out_dir)))
    bad = []
    n = 0
    with io.open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            digest, name = line.split("  ", 1)
            n += 1
            target = os.path.join(out_dir, name)
            if not os.path.isfile(target):
                bad.append("%s: named in %s, not on disk" % (name, SUMS_NAME))
            elif sha256(target) != digest:
                bad.append("%s: digest does not match" % name)
    if bad:
        raise Refusal("%s does not verify:\n%s" % (SUMS_NAME, "\n".join("    " + b for b in bad)))
    say("  %s verifies (%d file(s))" % (SUMS_NAME, n))
    return n


# --------------------------------------------------------------------------- selftest


def selftest():
    """Hermetic: a temp tree, fake artifacts, no toolchain, no game. Asserts the exact file lists
    (ship: four modules, no ini; tester: + harness + ini), the ini-diff invariant, SHA256SUMS and
    every refusal. A lint_repo row, and it runs under --ci.

    THE ONE SEAM: `_STAMP_READER` is replaced, because a fake artifact has no VERSIONINFO to read.
    The reader is probed against the real Release\\mh.dll when that build exists, and SAYS SO when not.
    """
    global _STAMP_READER
    fails = []
    checked = [0]

    def expect(name, cond):
        checked[0] += 1
        if not cond:
            fails.append(name)
            print("  [FAIL] %s" % name)
        else:
            print("  [ok]   %s" % name)

    def refuses(name, fn):
        try:
            fn()
        except Refusal as e:
            expect("%s -> REFUSAL: %s" % (name, str(e).split("\n")[0][:96]), True)
            return
        except Exception as e:  # noqa: BLE001 -- a traceback here IS the failure
            expect("%s -> refused with %s, not a Refusal" % (name, type(e).__name__), False)
            return
        expect("%s -> refused" % name, False)

    example = read_example_ini()

    # ---- the tester ini: differs from the reference in exactly its declared keys -----------
    keys = tester_keys()
    tester = make_tester_ini(example, "x.zip")
    body = tester[len(tester_header("x.zip")) :]
    diffs = ini_diff_lines(example, body)
    expect(
        "tester ini differs from the reference in exactly %d line(s), got %d"
        % (len(keys), len(diffs)),
        len(diffs) == len(keys),
    )
    flipped = []
    for _ln, before, after in diffs:
        m = re.match(r"^\s*(\w+)\s*=\s*(\S*)", after)
        mb = re.match(r"^\s*(\w+)\s*=\s*(\S*)", before or "")
        flipped.append(m.group(1) if m else None)
        expect(
            "tester: changed line keeps its key and trailing comment (%r)" % after[:48],
            bool(m)
            and bool(mb)
            and m.group(1) == mb.group(1)
            and after.split(";", 1)[1:] == before.split(";", 1)[1:],
        )
    expect(
        "tester: the changed keys are exactly its key list",
        sorted(x for x in flipped if x) == sorted(k for _s, k, _v, _w in keys),
    )
    expect(
        "tester: unlock=1 and level=debug are declared",
        {("dev", "unlock", "1"), ("log", "level", "debug")}
        <= {(s, k, v) for s, k, v, _w in tester_keys()},
    )
    expect(
        "HARNESS_KEYS arms the harness WITHOUT the clock pin (enable=1, fixed_step=0, order_mode=1)",
        {(s, k, v) for s, k, v, _w in HARNESS_KEYS}
        == {
            ("harness", "enable", "1"),
            ("harness", "fixed_step", "0"),
            ("harness", "order_mode", "1"),
        },
    )
    # RL6: the DEBUG and QUIET sets exist twice -- here (documentation of the former DEBUG_KEYS) and in the
    # C++ level table -- so each is cross-checked against the other. The C++ rows are
    # `{"net", "sp_clock_log", <quiet>, <debug>},` (-1 = the level leaves it alone).
    try:
        with io.open(LEVEL_TABLE_SRC, encoding="utf-8") as fh:
            src_txt = fh.read()
    except OSError:
        src_txt = ""
    cpp_debug, cpp_quiet = set(), set()
    for m in re.finditer(r'\{"(\w+)",\s*"(\w+)",\s*(-?\d+),\s*(-?\d+)\}', src_txt):
        sec, key, q, d = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        if q >= 0:
            cpp_quiet.add((sec, key, str(q)))
        if d >= 0:
            cpp_debug.add((sec, key, str(d)))
    expect(
        "the C++ level table's DEBUG set is exactly the former DEBUG_KEYS (%d keys)"
        % len(OBSERVERS_AT_DEBUG),
        cpp_debug == set(OBSERVERS_AT_DEBUG),
    )
    expect(
        "the C++ level table's QUIET set is exactly lockstep_log=0 + frametime_log=0 (detection keys untouched)",
        cpp_quiet == set(OBSERVERS_AT_QUIET),
    )
    refuses(
        "a key that is not in the reference ini",
        lambda: _flip(example, "net", "no_such_key", "1"),
    )
    refuses(
        "a section that is not in the reference ini",
        lambda: _flip(example, "no_such", "log", "1"),
    )

    # ---- declared lists --------------------------------------------------------------------
    expect(
        "ship list is exactly the four modules",
        expected_entries("net") == ["mh.dll", "mh_net.dll", "mh_net_udp.dll", "msvfw32.dll"],
    )
    banned = (
        "mh_net.ini",
        "README.txt",
        "LICENSE",
        "THIRD_PARTY.md",
        "mh_harness.dll",
        "libmh.dll",
    )
    expect(
        "ship list has no ini, README, LICENSE, THIRD_PARTY, harness or libmh",
        not any(e in banned for e in expected_entries("net")),
    )
    expect(
        "tester list = ship list + mh_harness.dll + mh_net.ini, nothing else",
        sorted(set(expected_entries("tester")) - set(expected_entries("net")))
        == ["mh_harness.dll", "mh_net.ini"]
        and set(expected_entries("net")) <= set(expected_entries("tester")),
    )

    # ---- packaging over fake artifacts ----------------------------------------------------
    real_reader = _STAMP_READER
    tmp = tempfile.mkdtemp(prefix="mh_relpkg_")
    try:
        relsrc = os.path.join(tmp, "Release")
        out = os.path.join(tmp, "dist")
        os.makedirs(relsrc)
        for m in TESTER_MODULES:
            with open(os.path.join(relsrc, m), "wb") as fh:
                fh.write(b"MZ fake " + m.encode())

        _STAMP_READER = lambda path: "9.9.9"  # noqa: E731 -- a one-line stub is the point
        try:
            quiet = lambda *a, **k: None  # noqa: E731
            refuses(
                "a version the binaries do not carry",
                lambda: build("1.2.3", relsrc, out, say=quiet),
            )
            _STAMP_READER = lambda path: "1.2.3+deadbeef"  # noqa: E731
            built = build("1.2.3", relsrc, out, say=quiet)
            expect("exactly one zip built", built == [zip_name("1.2.3", "net")])
            name = built[0]
            with zipfile.ZipFile(os.path.join(out, name)) as zf:
                got = sorted(zf.namelist())
            expect("%s holds exactly its declared file list" % name, got == expected_entries("net"))
            expect(
                "%s is flat and has no ini" % name,
                all("/" not in e for e in got) and "mh_net.ini" not in got,
            )
            expect(
                "out dir holds only the zip and SHA256SUMS",
                sorted(os.listdir(out)) == sorted([name, SUMS_NAME]),
            )
            expect("SHA256SUMS verifies", verify_sums(out, say=quiet) == 1)
            with open(os.path.join(out, name), "ab") as fh:
                fh.write(b"tamper")
            refuses("a tampered zip against SHA256SUMS", lambda: verify_sums(out, say=quiet))

            tout = os.path.join(tmp, "dist_tester")
            tb = build("1.2.3", relsrc, tout, tester=True, say=quiet)
            tname = tb[0]
            with zipfile.ZipFile(os.path.join(tout, tname)) as zf:
                tgot = sorted(zf.namelist())
                tini = zf.read("mh_net.ini").decode("utf-8")
            expect(
                "%s holds exactly its declared file list" % tname,
                tgot == expected_entries("tester"),
            )
            expect(
                "tester ini in the zip carries unlock=1 and level=debug",
                bool(re.search(r"^unlock\s*=\s*1", tini, re.M))
                and bool(re.search(r"^level\s*=\s*debug", tini, re.M)),
            )

            # MISSING ARTIFACT
            os.remove(os.path.join(relsrc, "mh_net.dll"))
            refuses(
                "a release dir missing a module", lambda: build("1.2.3", relsrc, out, say=quiet)
            )
            refuses(
                "a release dir that does not exist",
                lambda: build("1.2.3", os.path.join(tmp, "nope"), out, say=quiet),
            )
        finally:
            _STAMP_READER = real_reader

        # ---- the stamp READER, against a real stamped binary when there is one --------------
        real_mh = os.path.join(DEFAULT_RELEASE_DIR, "mh.dll")
        if os.path.isfile(real_mh):
            got = read_file_version(real_mh)
            expect(
                "the VERSIONINFO reader returns a semver-shaped FileVersion off the real mh.dll "
                "(%r)" % got,
                bool(got) and re.match(r"^\d+\.\d+\.\d+", got or ""),
            )
        else:
            print(
                "  [note] no built Release\\mh.dll -- the VERSIONINFO reader was not exercised "
                "against a real binary (build first to exercise it)"
            )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("release_package selftest: %d check(s), %d failure(s)" % (checked[0], len(fails)))
    return 1 if fails else 0


# --------------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--version", help="the release version, WITHOUT a leading v (e.g. 0.1.0)")
    ap.add_argument("--release-dir", default=DEFAULT_RELEASE_DIR)
    ap.add_argument("--out", default=DEFAULT_OUT_DIR)
    ap.add_argument(
        "--allow-dev",
        action="store_true",
        help="do not require the binaries to carry --version (local dry run only)",
    )
    ap.add_argument(
        "--tester",
        action="store_true",
        help="build the TESTER zip (+ mh_harness.dll, dev ini) INSTEAD of the ship zip. Never CI.",
    )
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.version:
        ap.error("--version is required (or --selftest)")
    version = args.version.lstrip("v")
    try:
        build(version, args.release_dir, args.out, allow_dev=args.allow_dev, tester=args.tester)
        verify_sums(os.path.abspath(args.out))
    except Refusal as e:
        print("release_package: REFUSED -- %s" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
