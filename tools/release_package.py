#!/usr/bin/env python3
"""release_package.py -- assemble the three drop-in zips a release publishes, from a built
`src/mh_dll/Release` tree, and refuse to publish anything mislabelled.

WHY A TOOL AND NOT A WORKFLOW STEP (tracker TL-CI1). The packaging rules are the part of a release
that is easy to get quietly wrong: a zip with the WRONG libmh.dll in it produces a configuration
the user did not ask for and cannot see (see docs/dll-split.md -- the hosted and standalone builds
share a file name and cannot do each other's job), and a zip whose binaries were never stamped with
the tag it is named after is a release nobody can trace a bug report back to. Both failures are
silent at packaging time and expensive afterwards. So the rules live in a script with a `--selftest`
that runs in CI on a runner with no game, and the workflow just calls it.

    python tools/release_package.py --version 0.1.0
    python tools/release_package.py --version 0.1.0 --release-dir src/mh_dll/Release --out dist
    python tools/release_package.py --version 0.1.0 --allow-dev   # local dry run, stamp not checked
    python tools/release_package.py --selftest                    # hermetic; a lint_repo row

---- THE THREE ZIPS (the user's ruling, 2026-09-17) ----------------------------------------------

Each is a DROP-IN: unzip it next to the game executable and that is the whole install. They are
nested, so the difference between two of them is exactly one file plus the ini.

  <base>-net.zip              msvfw32.dll  mh.dll  mh_net.dll  mh_net_udp.dll  mh_net.ini  LICENSE
                              THIRD_PARTY.md  README.txt
                              -> configuration (1): all-original game + the restored multiplayer.

  <base>-net-debug.zip        the above + mh_harness.dll, and an mh_net.ini with the diagnostic
                              logging keys turned on (the harness itself cannot arm here -- Q4,
                              below).
                              -> still configuration (1), but it talks. This is what a bug report
                                 should be produced with.

  <base>-brokered-debug.zip   the above + libmh.dll, THE HOSTED BUILD, AT THE ZIP ROOT, and an
                              mh_net.ini that ALSO arms the harness (per-step hashes + the order
                              record, clock not pinned -- user ruling 2026-09-20).
                              -> configuration (2): the re-implemented spine serves the domains it
                                 owns. The file must be at the root and not in a subfolder, because
                                 mh.dll composes it beside itself: a `Release/libmh.dll` entry would
                                 unzip into a subdirectory and the drop-in would silently be
                                 configuration (1) again.

  SHA256SUMS                  one file over all three, in `sha256sum -c` format.

WHAT IS NOT IN ANY OF THEM, and this is a ruling rather than an oversight: no selftest executable
(net_selftest.exe / libmh_selftest.exe are the offline gate, not a product), no standalone binaries
(`Release\\standalone\\` is configuration (3), which is not shipped), and no byte of the game. The
zips are useless without a legally obtained copy, which is the project's standing position.

---- THE TWO ini VARIANTS, AND WHY THE SHIP ONE IS THE EXAMPLE FILE VERBATIM ----------------------

`src/mh_dll/mh_net.example.ini` documents every key the DLL reads WITH ITS REAL DEFAULT, and says
so in its own header: an all-commented copy behaves exactly like no file at all, and every value it
shows uncommented is the DLL's own shipping default. So "the ini with SHIP settings" is that file,
copied, with a short provenance header prepended. That was chosen over authoring a second,
minimal, all-commented ini for one measured reason: a hand-written ship ini is a SECOND statement
of the defaults, and the moment a default changes in the source the two disagree with nothing to
notice. Copying the reference means the ship ini cannot drift from the documented defaults, because
it IS them; and a user who wants to change something finds every key already documented in front of
them rather than having to fetch a second file.

The DEBUG ini is that same file with a named, short list of keys flipped -- see DEBUG_KEYS and
HARNESS_KEYS below for the lists and, per key, why it is in one. Two properties are asserted by
`--selftest` rather than promised here: each debug ini differs from the ship ini in EXACTLY the
lines its list names, and each flipped line keeps its trailing comment, so the file still documents
itself.

THE DEBUG ini RECORDS ORDERS AND PER-STEP HASHES (user ruling 2026-09-20). Until that ruling the
rule was "debug keys are pure observers" and `[harness] enable=1` was kept out on the grounds that
it installs trampolines and, with the harness's own default `fixed_step=1`, pins the game clock. The
ruling puts the instrument INTO the debug configuration -- a bug report with no per-step hash and no
order stream is a report the desync tooling (tools/mp_analyze.py, mp_desync_snap_diff.py) cannot
read -- and HARNESS_KEYS is how it goes in without becoming a different game:

  [harness] enable=1       the trampolines install and EVERY sim step is hashed into mh_harness.log.
  [harness] fixed_step=0   THE LINE THAT KEEPS THE GAME THE GAME. The harness's compiled default is
                           1, which writes TOTAL_GAME_TIME = GAME_CLOCK + interval at every sim_tick
                           -- one sim step per present, i.e. a game whose speed is its frame rate,
                           and a lockstep peer whose clock fights the horizon. The determinism gate
                           itself runs the harness with fixed_step=0 (tools/mp_run.py HARNESS, which
                           tools/ui_test.py make_harness_ini feeds every --determinism peer), so
                           this is the gate's configuration, not a new one.
  [harness] order_mode=1   every dispatched order is RECORDED to mh_orders.bin in the run folder,
                           which is what makes a desynced match replayable offline.

WHAT AN ARMED HARNESS DOES NOT DO: change what the sim computes. The harness "only observes"
(mh/seams/harness_bind.cpp: the un-armed bodies answer what the armed ones do; the ini's own
[harness] prose: `1 = install the sim/console trampolines and hash every step`), and the standing
evidence is the determinism gate: `python tools/test_ui.py --determinism` boots BOTH rig peers with
`[harness] enable=1 fixed_step=0 pin_fpu=1 region_hash_step=1` through the real menu -> lobby ->
Start and requires ALL PAIRS IDENTICAL over the lockstep hash stream -- an instrument that altered the
sim would desync the pair it is measuring. The remaining harness defaults an armed run inherits are
observers too: `pin_fpu=1` sets the x87 control word to the Win32 default it already has (53-bit,
round-nearest), `seed_step=1 seed_mode=0` DUMPS the region set to mh_harness_seed.bin at step 1
(a ~2.8 MB read + write, never an inject), `stop_step=0` never stops. The COST is what the ruling
buys the report with: one walk of the 56-region manifest per sim step (~4-5 ms on the rig, the
`[desync] COST PROBE` figure -- which a run with `[desync] enabled=1` was already paying every 50th
step), plus mh_harness.log, mh_orders.bin and the seed blob on disk.

WHERE THE HARNESS KEYS GO -- ONLY THE ZIP THAT CARRIES libmh.dll. Ruling Q4 (mh_harness/
mh_harness_dllmain.cpp spine_refuse): the harness reads the sim's state regions THROUGH the spine,
so in configuration (1) -- no libmh.dll in the process -- `[harness] enable=1` is REFUSED on three
channels (mh_harness_refused.log beside the exe, OutputDebugString, stderr) and the run is
uninstrumented. A net-debug ini that armed it would ship a refusal file on every launch and no
hashes. So HARNESS_KEYS is applied to a debug zip IF AND ONLY IF that zip's module list carries
libmh.dll (`brokered-debug` today); `net-debug` gets DEBUG_KEYS alone and its README says why. The
condition is derived from the module list, not declared per zip, so a zip that gains libmh.dll
gains the harness keys with it.

WHAT IS STILL NOT IN EITHER LIST. `[debug] overlay` LEFT DEBUG_KEYS on 2026-09-20 (user ruling): the
debug ini keeps `overlay=0`, i.e. installed-but-hidden, so the Ctrl+Alt+D toggle works and nothing
paints over the game until asked -- proven by test_ui.py's `debug_overlay` scenario (absent on the
first frame, present after the chord on two screens, absent after a second). `[net] log_gamemode` stays out: it
watches a global using the DEBUG REGISTERS, which is a real intervention in the process, not a log
level. `[harness] fixed_step=1` (the compiled default) is exactly the thing HARNESS_KEYS turns OFF.

---- THE TWO REFUSALS ----------------------------------------------------------------------------

Both exit non-zero with a named reason, because a packager that guesses is worse than one that
stops.

  MISSING ARTIFACT    a file the zip declares is not in the release dir. Named individually; a
                      build that quietly stopped producing one module is exactly the failure
                      ci.yml's eight-artifact step exists for, one step later.

  STAMP MISMATCH      a module's VERSIONINFO FileVersion does not carry `--version`. This is the
                      check that makes the zip's NAME evidence rather than decoration. `--allow-dev`
                      bypasses it for a local dry run against a tree built with no properties, and
                      it is the only thing that bypasses it: there is no flag that packages a
                      release-named zip from mismatched binaries.

  (A third refusal has no flag at all: THE WRONG libmh.dll. The hosted and standalone builds share
  a name, so the candidate is checked the way mh.dll checks it at bind time -- it must export every
  row of the generated contract in src/mh_dll/mh/seams/libmh_contract.gen.cpp. Measured on the
  current build: the hosted DLL resolves 81 of 81 and the standalone one 12 of 81.)
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
CONTRACT_SRC = os.path.join(REPO, "src", "mh_dll", "mh", "seams", "libmh_contract.gen.cpp")

BASE_NAME = "mission_humanity_re"
SUMS_NAME = "SHA256SUMS"

# The modules whose VERSIONINFO must carry --version. Exactly the five that import
# src/mh_dll/mh_version.props; nothing else in the release dir is stamped, so nothing else can be
# checked and nothing else is shipped.
STAMPED = ("msvfw32.dll", "mh.dll", "mh_net.dll", "mh_net_udp.dll", "mh_harness.dll", "libmh.dll")


class Refusal(Exception):
    """A named reason to stop. Never a traceback: a packaging refusal is an ANSWER."""


# --------------------------------------------------------------------------- the ini variants

# (section, key, value, why). Flipped in EVERY debug ini and nowhere else. Every one of these is an
# OBSERVER -- it writes a line somewhere and changes nothing the game does. See the module docstring
# for the keys that were considered and left out, and why. `[debug] overlay` was here until
# 2026-09-20: the debug ini now ships the overlay installed-but-hidden (overlay=0, the toggle works).
DEBUG_KEYS = (
    (
        "net",
        "sp_clock_log",
        "1",
        "log every strategic frame even in single-player, where there is no lockstep to gate on. "
        "Without it a single-player report has no per-frame clock record at all.",
    ),
    (
        "trace",
        "temporal_sp",
        "1",
        "record the per-event temporal trace OUTSIDE session mode 3 as well, so a menu/lobby "
        "problem (the half a bug report is most often about) is in mh_temporal.log.",
    ),
    (
        "desync",
        "verbose",
        "1",
        "log every AGREEING state comparison, not only the mismatches. A clean log is what makes "
        "the first disagreement readable as a change rather than as the only data point.",
    ),
    (
        "input",
        "mouse_trace",
        "1",
        "per-present mouse ring telemetry into mh_uidrive.log. The input path is where "
        "'it feels wrong' reports come from, and it is unreconstructable after the fact.",
    ),
)

# (section, key, value, why). The determinism/replay INSTRUMENT, armed in the debug ini of every zip
# that carries libmh.dll (user ruling 2026-09-20; ruling Q4 is why it cannot go in the others -- see
# the module docstring). Applied AFTER DEBUG_KEYS, same _flip, same selftest invariant.
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


def harness_keys_for(modules):
    """HARNESS_KEYS if this zip can ARM the harness, else (). Ruling Q4: the instrument reads the sim
    through libmh.dll's spine and refuses in configuration (1), so the keys follow the spine."""
    return HARNESS_KEYS if "libmh.dll" in modules else ()


SHIP_HEADER = """\
; mh_net.ini -- SHIPPING configuration, packaged with {zipname}.
;
; This file is src/mh_dll/mh_net.example.ini VERBATIM: that file documents every key the DLL reads
; together with its real default, so a copy of it IS the shipping configuration. Nothing below is
; an override -- deleting this file entirely would change nothing about how the DLL behaves.
;
; The DLL arms with no mh_net.ini present at all. Keep this one because it is the reference: every
; key you might want is already in front of you, with its default and what it does.
; ================================================================================================

"""

DEBUG_HEADER = """\
; mh_net.ini -- DIAGNOSTIC configuration, packaged with {zipname}.
;
; This file is src/mh_dll/mh_net.example.ini with {n} keys changed, and nothing else:
;
{keylist}
;
{harness_note}
; The logs land in `logs\\<runid>_<role>\\` inside the game folder. Attach that whole directory to a
; bug report; mh_net.log's first line names the build it came from.
; ================================================================================================

"""

# The paragraph after the key list, per zip: whether the determinism harness is ARMED here.
HARNESS_NOTE_ARMED = """\
; The [harness] keys ARM the determinism/replay instrument (mh_harness.dll): every sim step is
; hashed into mh_harness.log and every dispatched order is recorded to mh_orders.bin, with
; fixed_step=0 so the game clock is NOT pinned -- the same configuration the project's determinism
; gate runs, so what you play is the game, measured. The other keys only make the run REPORT more.
; The debug overlay is installed but HIDDEN: Ctrl+Alt+D shows it, Ctrl+Alt+PgUp/PgDn page it.
;"""
HARNESS_NOTE_UNARMED = """\
; Every one of them only makes the run REPORT more; none of them changes what the game does. The
; determinism harness (mh_harness.dll, in this zip) is NOT armed here and CANNOT be in this
; configuration: it reads the game's state through libmh.dll, which this zip deliberately does not
; carry, so `[harness] enable=1` would be refused at startup (mh_harness_refused.log). The
; brokered-debug zip is the one that records per-step hashes and orders.
; The debug overlay is installed but HIDDEN: Ctrl+Alt+D shows it, Ctrl+Alt+PgUp/PgDn page it.
;"""

README_TXT = """\
{base} {version} -- {config_title}

WHAT THIS IS
  A drop-in for a legally obtained installation of Exterminacja / Mission Humanity (Techland,
  2001). It restores the multiplayer the retail executable builds packets for and never transmits,
  and -- in the brokered configuration -- runs a re-implemented simulation spine inside the retail
  process. It contains NO GAME DATA: no executable, no resource archives, no sprites, audio, text,
  maps or missions. You bring your own copy.

HOW TO INSTALL
  Unzip every file in this archive NEXT TO THE GAME EXECUTABLE, in the folder the game is installed
  in. Keep them at that level -- do not put them in a subfolder. Then start the game the way you
  normally do.

  To uninstall: delete msvfw32.dll. The game then loads the system DLL again and runs unmodified,
  whatever else is left beside it.

WHAT YOU GET: {config_title}
{config_body}

CHECKING WHAT YOU GOT
  Each run creates logs\\<runid>_<role>\\ inside the game folder. Open mh_net.log there. Its first
  line is the build stamp:

      ; [build] mh {version}+<commit>

  and the lines after it report each module's bind outcome by name, so "which configuration am I
  running" is answered by the log rather than guessed. Quote that first line in any bug report.

VERIFYING THE DOWNLOAD
  The release page carries a SHA256SUMS file covering all three zips. On Windows:

      certutil -hashfile <thisfile>.zip SHA256

  and compare with the line for this file in SHA256SUMS.

MULTIPLAYER
  There is no role setting: the host clicks Create, a joiner clicks Join and types the host's
  address. The link is authenticated by a FILE, not a setting -- mh_key.txt appears next to the
  game executable on first run, and the host shares that file with its players. A peer holding the
  wrong key is refused rather than desynchronised.

CONFIGURATION
  mh_net.ini in this archive is {ini_note} It documents every key the DLL reads, each with its
  real default. Two rules that bite: a section you omit ENTIRELY disables that feature, and a
  second block with the same section name is dead text (only the first is read).

BUILDING IT YOURSELF, AND THE REST OF THE DOCUMENTATION
  INSTALL.md in the source repository: prerequisites, the one MSBuild command, the offline test
  gate, and a longer version of everything above. README.md is what the project is and why.

LICENCE
  MIT, on this project's own code and documentation -- see LICENSE. Third-party components and the
  retail-game boundary are in THIRD_PARTY.md. Not affiliated with or endorsed by Techland.
"""

CONFIG_TEXT = {
    "net": (
        "configuration (1), all-original + restored multiplayer",
        """\
  msvfw32.dll    the loader shim. This is what makes the game load mh.dll at all: the game
                 statically imports the system video-for-Windows DLL, and the application
                 directory wins the module search. No byte of the game executable is modified.
  mh.dll         the router and patch host -- the only module that touches the game's addresses.
  mh_net_udp.dll the multiplayer transport over UDP -- THE DEFAULT (`[net] transport=udp`): the
                 one the relay works over and the one the launcher configures.
  mh_net.dll     the multiplayer transport over TCP (`[net] transport=tcp`): direct dial only,
                 the host's TCP port must be reachable. BOTH transports ship, and mh_net.ini
                 decides which one a run uses. Every peer in a match must be set the same way.

  The game runs its OWN simulation, exactly as it shipped. Nothing is promoted and nothing is
  instrumented; a player cannot tell this apart from retail except that multiplayer works.""",
    ),
    "net-debug": (
        "configuration (1) with the diagnostics on",
        """\
  msvfw32.dll    the loader shim (see above).
  mh.dll         the router and patch host.
  mh_net_udp.dll the multiplayer transport over UDP -- THE DEFAULT (`[net] transport=udp`).
  mh_net.dll     the multiplayer transport over TCP (`[net] transport=tcp`, direct dial only).
                 BOTH transports ship, and mh_net.ini decides which one a run uses. Every peer
                 in a match must be set the same way.
  mh_harness.dll the determinism/replay instrument. SHIPPED BUT NOT ARMED, and it cannot be in
                 this configuration: it reads the game's state through libmh.dll, which this zip
                 does not carry, so `[harness] enable=1` here is refused at startup
                 (mh_harness_refused.log beside the game). The brokered-debug zip arms it.

  The game runs its own simulation, as above. The difference from the plain `net` zip is the ini:
  the diagnostic logging keys are on, so the run writes down enough to diagnose afterwards. The
  debug overlay is installed but hidden -- Ctrl+Alt+D shows it.""",
    ),
    "brokered-debug": (
        "configuration (2), brokered + the diagnostics on",
        """\
  msvfw32.dll    the loader shim (see above).
  mh.dll         the router and patch host.
  mh_net_udp.dll the multiplayer transport over UDP -- THE DEFAULT (`[net] transport=udp`).
  mh_net.dll     the multiplayer transport over TCP (`[net] transport=tcp`, direct dial only).
                 BOTH transports ship, and mh_net.ini decides which one a run uses. Every peer
                 in a match must be set the same way.
  mh_harness.dll the determinism/replay instrument, ARMED by this zip's ini: every sim step is
                 hashed into mh_harness.log and every order recorded to mh_orders.bin, with the
                 game clock NOT pinned (`fixed_step=0`, the determinism gate's own setting), so
                 the game plays as it does unarmed -- measured. This is what a desync report needs.
                 The debug overlay is installed but hidden -- Ctrl+Alt+D shows it.
  libmh.dll      the re-implemented spine, HOSTED build. Its presence IS the configuration: with
                 this file beside mh.dll, whole domains are served by re-implemented C++ instead of
                 the original bodies. Remove it and the same install is configuration (1) again.

  KEEP libmh.dll AT THE TOP LEVEL, beside the others. mh.dll looks for it next to itself, and a
  copy left in a subfolder means you are quietly running configuration (1).

  `[config] mode=original` in mh_net.ini runs the game's own bodies even with libmh.dll present.
  That is the supported, tested rollback, and it announces itself in mh_net.log.""",
    ),
}


def read_example_ini():
    if not os.path.isfile(EXAMPLE_INI):
        raise Refusal(
            "no reference ini at %s -- it is the source of both packaged variants"
            % rel(EXAMPLE_INI)
        )
    with io.open(EXAMPLE_INI, encoding="utf-8") as fh:
        return fh.read()


def make_ship_ini(text, zipname):
    return SHIP_HEADER.format(zipname=zipname) + text


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


def debug_keys_for(modules):
    """The full flip list for a debug zip with this module list: the observers, plus the harness keys
    where the harness can arm (ruling Q4 -- see harness_keys_for)."""
    return DEBUG_KEYS + harness_keys_for(modules)


def make_debug_ini(text, zipname, modules):
    body = text
    keys = debug_keys_for(modules)
    for section, key, value, _why in keys:
        body = _flip(body, section, key, value)
    keylist = "\n".join(";   [%s] %s=%s -- %s" % (s, k, v, why) for s, k, v, why in keys)
    note = HARNESS_NOTE_ARMED if harness_keys_for(modules) else HARNESS_NOTE_UNARMED
    return (
        DEBUG_HEADER.format(zipname=zipname, n=len(keys), keylist=keylist, harness_note=note) + body
    )


def debug_header_len(zipname, modules):
    """Length of the header make_debug_ini prepends, so a caller can slice the BODY back out."""
    keys = debug_keys_for(modules)
    keylist = "\n".join(";   [%s] %s=%s -- %s" % (s, k, v, why) for s, k, v, why in keys)
    note = HARNESS_NOTE_ARMED if harness_keys_for(modules) else HARNESS_NOTE_UNARMED
    return len(
        DEBUG_HEADER.format(zipname=zipname, n=len(keys), keylist=keylist, harness_note=note)
    )


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


# --------------------------------------------------------------------------- the hosted libmh check


def contract_rows():
    """The libmh contract symbol list, read out of the GENERATED table mh.dll itself binds
    against. Read rather than hard-coded, so this check cannot fall behind the contract."""
    if not os.path.isfile(CONTRACT_SRC):
        raise Refusal(
            "no libmh contract at %s -- cannot tell the hosted libmh.dll from the "
            "standalone one without it" % rel(CONTRACT_SRC)
        )
    with io.open(CONTRACT_SRC, encoding="utf-8") as fh:
        src = fh.read()
    m = re.search(r"g_libmh_fn_name\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        raise Refusal(
            "%s no longer holds a g_libmh_fn_name[] table in the shape this reader "
            "expects" % rel(CONTRACT_SRC)
        )
    rows = re.findall(r'"([^"]+)"', m.group(1))
    if not rows:
        raise Refusal(
            "%s holds an EMPTY contract table -- an empty contract would accept any "
            "DLL, including the standalone one" % rel(CONTRACT_SRC)
        )
    return rows


def check_hosted_libmh(path, say):
    """The hosted and standalone libmh.dll share a file name and cannot do each other's job
    (docs/dll-split.md). Ask the question mh.dll asks at bind time: does this module export EVERY
    row of the contract? The standalone build exports a small overlapping subset, so the answer
    separates them cleanly rather than by a guessed marker symbol."""
    import lief

    rows = contract_rows()
    binary = lief.parse(path)
    if binary is None:
        raise Refusal("%s is not a PE LIEF can parse" % rel(path))
    exp = binary.get_export()
    have = set(e.name for e in exp.entries) if exp is not None else set()
    missing = [r for r in rows if r not in have]
    if missing:
        raise Refusal(
            "WRONG libmh.dll: %s resolves %d of %d contract rows.\n"
            "    This is what mh.dll would report as `LOADED BUT REFUSED`. The likely cause is that "
            "Release\\standalone\\libmh.dll was picked up instead of Release\\libmh.dll -- the two "
            "share a name and are different binaries (see docs/dll-split.md).\n"
            "    first missing row: %s"
            % (rel(path), len(rows) - len(missing), len(rows), missing[0])
        )
    say(
        "  hosted ok  %-16s resolves %d of %d libmh contract rows"
        % ("libmh.dll", len(rows), len(rows))
    )


# --------------------------------------------------------------------------- the zips

# name -> (modules from the release dir, whether the ini is the debug variant).
# Order matters only for readability; the contents are asserted by the selftest, not by reading.
ZIPS = (
    ("net", ("msvfw32.dll", "mh.dll", "mh_net.dll", "mh_net_udp.dll"), False),
    (
        "net-debug",
        ("msvfw32.dll", "mh.dll", "mh_net.dll", "mh_net_udp.dll", "mh_harness.dll"),
        True,
    ),
    (
        "brokered-debug",
        ("msvfw32.dll", "mh.dll", "mh_net.dll", "mh_net_udp.dll", "mh_harness.dll", "libmh.dll"),
        True,
    ),
)

# The repo files every zip carries beside the binaries.
DOCS = ("LICENSE", "THIRD_PARTY.md")


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
    modules = dict((t, m) for t, m, _d in ZIPS)[tag]
    return sorted(list(modules) + list(DOCS) + ["mh_net.ini", "README.txt"])


def build(version, release_dir, out_dir, allow_dev=False, say=print):
    release_dir = os.path.abspath(release_dir)
    out_dir = os.path.abspath(out_dir)

    if not os.path.isdir(release_dir):
        raise Refusal(
            "no release directory at %s -- build first:\n"
            "    msbuild src\\mh_dll\\mh.sln /t:Build /p:Configuration=Release /p:Platform=x86 "
            "/m /nodeReuse:false" % rel(release_dir)
        )

    # MISSING ARTIFACT, named individually and all at once: a packager that stops at the first
    # absence makes the operator rebuild once per missing file.
    needed = sorted(set(m for _t, mods, _d in ZIPS for m in mods))
    missing = [m for m in needed if not os.path.isfile(os.path.join(release_dir, m))]
    if missing:
        raise Refusal(
            "MISSING ARTIFACT: %d module(s) are not in %s:\n%s\n"
            "A green build line is not proof the set is complete; check the eight artifacts by "
            "name (INSTALL.md section 2)."
            % (len(missing), rel(release_dir), "\n".join("    " + m for m in missing))
        )
    for doc in DOCS:
        if not os.path.isfile(os.path.join(REPO, doc)):
            raise Refusal(
                "MISSING ARTIFACT: %s is not in the repository root, and every zip carries it" % doc
            )

    if allow_dev:
        say(
            "  stamp check SKIPPED (--allow-dev): these zips do not claim to be version %s"
            % version
        )
    else:
        check_stamp(release_dir, version, needed, say)
    check_hosted_libmh(os.path.join(release_dir, "libmh.dll"), say)

    example = read_example_ini()
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    built = []
    for tag, modules, debug_ini in ZIPS:
        name = zip_name(version, tag)
        title, body = CONFIG_TEXT[tag]
        ini = make_debug_ini(example, name, modules) if debug_ini else make_ship_ini(example, name)
        readme = README_TXT.format(
            base=BASE_NAME,
            version=version,
            config_title=title,
            config_body=body,
            ini_note=(
                "the reference configuration with %d diagnostic keys changed (listed in its header)."
                % len(debug_keys_for(modules))
                if debug_ini
                else "the reference configuration, every key at its shipping default."
            ),
        )
        path = os.path.join(out_dir, name)
        # Deterministic-ish: fixed order, fixed date, so two packagings of one build differ only if
        # their inputs do. (The DLLs themselves are not reproducible builds; this is about the
        # container not adding noise of its own.)
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
            for m in modules:
                zf.write(os.path.join(release_dir, m), m)
            for d in DOCS:
                zf.write(os.path.join(REPO, d), d)
            _writestr(zf, "mh_net.ini", ini)
            _writestr(zf, "README.txt", readme)
        got = sorted(zipfile.ZipFile(path).namelist())
        want = expected_entries(tag)
        if got != want:
            raise Refusal("%s holds %r, expected %r" % (name, got, want))
        built.append(name)
        say("  packaged   %-46s %d entries, %d B" % (name, len(got), os.path.getsize(path)))

    sums = os.path.join(out_dir, SUMS_NAME)
    with io.open(sums, "w", encoding="utf-8", newline="\n") as fh:
        for name in built:
            fh.write("%s  %s\n" % (sha256(os.path.join(out_dir, name)), name))
    say("  packaged   %-46s over %d zip(s)" % (SUMS_NAME, len(built)))
    say("release_package: %d zip(s) + %s in %s" % (len(built), SUMS_NAME, rel(out_dir)))
    return built


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
    """Hermetic: a temp tree, fake artifacts, no toolchain, no game. Asserts the file lists, the
    ini-diff invariant, SHA256SUMS, and every refusal. A lint_repo row, and it runs under --ci.

    THE ONE SEAM, stated rather than hidden: `_STAMP_READER` is replaced here, because a fake
    artifact has no VERSIONINFO to read and this file does not write PE resources. What is being
    asserted is the REFUSAL LOGIC -- that a mismatch stops the packaging and names the modules --
    which is the part that can be wrong. The reader is additionally probed against the real
    Release\\mh.dll below when that build happens to exist, and SAYS SO when it does not.
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

    # ---- the ini invariant, PER DEBUG ZIP (the harness keys follow libmh.dll) ---------------
    ship = make_ship_ini(example, "x.zip")
    ship_body = ship[len(SHIP_HEADER.format(zipname="x.zip")) :]
    expect("ship ini body IS the reference file, byte for byte", ship_body == example)
    expect(
        "[debug] overlay is NOT flipped by any debug ini (user ruling 2026-09-20: installed-hidden)",
        not any(s == "debug" and k == "overlay" for s, k, _v, _w in DEBUG_KEYS + HARNESS_KEYS),
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
    for tag, modules, debug_ini in ZIPS:
        if not debug_ini:
            continue
        keys = debug_keys_for(modules)
        debug = make_debug_ini(example, "x.zip", modules)
        debug_body = debug[debug_header_len("x.zip", modules) :]
        expect(
            "%s: harness keys present IFF the zip carries libmh.dll (ruling Q4)" % tag,
            (len(keys) > len(DEBUG_KEYS)) == ("libmh.dll" in modules),
        )
        diffs = ini_diff_lines(ship_body, debug_body)
        expect(
            "%s: debug ini differs from ship ini in exactly %d line(s), got %d"
            % (tag, len(keys), len(diffs)),
            len(diffs) == len(keys),
        )
        flipped = []
        for _ln, before, after in diffs:
            m = re.match(r"^\s*(\w+)\s*=\s*(\S*)", after)
            mb = re.match(r"^\s*(\w+)\s*=\s*(\S*)", before or "")
            flipped.append(m.group(1) if m else None)
            expect(
                "%s: changed line keeps its key and its trailing comment (%r)" % (tag, after[:48]),
                bool(m)
                and bool(mb)
                and m.group(1) == mb.group(1)
                and after.split(";", 1)[1:] == before.split(";", 1)[1:],
            )
        expect(
            "%s: the changed keys are exactly its key list" % tag,
            sorted(x for x in flipped if x) == sorted(k for _s, k, _v, _w in keys),
        )
        expect(
            "%s: every listed value is now set in the debug ini" % tag,
            all(
                re.search(r"^\s*%s\s*=\s*%s\b" % (re.escape(k), re.escape(v)), debug_body, re.M)
                for _s, k, v, _w in keys
            ),
        )
        expect(
            "%s: the header names the harness as ARMED iff the keys arm it" % tag,
            ("ARM the determinism" in debug) == bool(harness_keys_for(modules)),
        )
    refuses(
        "a DEBUG key that is not in the reference ini",
        lambda: _flip(example, "net", "no_such_key_at_all", "1"),
    )
    refuses(
        "a DEBUG key in a section that is not in the reference ini",
        lambda: _flip(example, "no_such_section", "log", "1"),
    )

    # ---- the contract reader --------------------------------------------------------------
    rows = contract_rows()
    expect("libmh contract table read (%d rows)" % len(rows), len(rows) > 20)

    # ---- packaging over fake artifacts ----------------------------------------------------
    real_reader = _STAMP_READER
    tmp = tempfile.mkdtemp(prefix="mh_relpkg_")
    try:
        relsrc = os.path.join(tmp, "Release")
        out = os.path.join(tmp, "dist")
        os.makedirs(relsrc)
        for m in sorted(set(x for _t, mods, _d in ZIPS for x in mods)):
            with open(os.path.join(relsrc, m), "wb") as fh:
                fh.write(b"MZ fake " + m.encode())

        # The hosted-libmh check needs a real PE to read; over fake artifacts it is stubbed the same
        # way the stamp reader is, and its OWN refusal is asserted separately below.
        saved_check = globals()["check_hosted_libmh"]
        globals()["check_hosted_libmh"] = lambda path, say: say("  hosted ok  (stubbed)")
        _STAMP_READER = lambda path: "9.9.9"  # noqa: E731 -- a one-line stub is the point
        try:
            quiet = lambda *a, **k: None  # noqa: E731
            refuses(
                "a version the binaries do not carry",
                lambda: build("1.2.3", relsrc, out, say=quiet),
            )
            _STAMP_READER = lambda path: "1.2.3+deadbeef"  # noqa: E731
            built = build("1.2.3", relsrc, out, say=quiet)
            expect("three zips built", len(built) == 3)
            for tag, _mods, _d in ZIPS:
                name = zip_name("1.2.3", tag)
                got = sorted(zipfile.ZipFile(os.path.join(out, name)).namelist())
                expect(
                    "%s holds exactly its declared file list" % name, got == expected_entries(tag)
                )
                expect(
                    "%s carries no selftest or standalone binary, and no game byte" % name,
                    not any(
                        e.endswith("selftest.exe") or "standalone" in e or e.endswith(".sav")
                        for e in got
                    ),
                )
                expect(
                    "%s puts libmh.dll (when present) at the ROOT" % name,
                    all(("/" not in e and "\\" not in e) for e in got),
                )
            expect(
                "only the brokered zip carries libmh.dll",
                [t for t, m, _d in ZIPS if "libmh.dll" in m] == ["brokered-debug"],
            )
            expect(
                "only the debug zips carry mh_harness.dll",
                [t for t, m, _d in ZIPS if "mh_harness.dll" in m]
                == ["net-debug", "brokered-debug"],
            )
            expect("SHA256SUMS verifies", verify_sums(out, say=quiet) == 3)
            # ... and goes red when a zip is altered underneath it.
            with open(os.path.join(out, zip_name("1.2.3", "net")), "ab") as fh:
                fh.write(b"tamper")
            refuses("a tampered zip against SHA256SUMS", lambda: verify_sums(out, say=quiet))

            # MISSING ARTIFACT
            os.remove(os.path.join(relsrc, "mh_net.dll"))
            refuses(
                "a release dir missing a module",
                lambda: build("1.2.3", relsrc, out, say=quiet),
            )
            refuses(
                "a release dir that does not exist",
                lambda: build("1.2.3", os.path.join(tmp, "nope"), out, say=quiet),
            )
        finally:
            globals()["check_hosted_libmh"] = saved_check
            _STAMP_READER = real_reader

        # ---- the WRONG libmh.dll, on a real PE when there is one ---------------------------
        standalone = os.path.join(DEFAULT_RELEASE_DIR, "standalone", "libmh.dll")
        hosted = os.path.join(DEFAULT_RELEASE_DIR, "libmh.dll")
        if os.path.isfile(standalone) and os.path.isfile(hosted):
            refuses(
                "the STANDALONE libmh.dll offered as the hosted one",
                lambda: check_hosted_libmh(standalone, lambda *a: None),
            )
            try:
                check_hosted_libmh(hosted, lambda *a: None)
                expect("the hosted libmh.dll is accepted", True)
            except Refusal as e:
                expect("the hosted libmh.dll is accepted (%s)" % e, False)
        else:
            print(
                "  [note] no built Release tree -- the hosted/standalone libmh discrimination "
                "was not exercised against real PEs (build first to exercise it)"
            )
        refuses(
            "a file that is not a PE offered as libmh.dll",
            lambda: check_hosted_libmh(os.path.join(relsrc, "mh.dll"), lambda *a: None),
        )

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
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.version:
        ap.error("--version is required (or --selftest)")
    version = args.version.lstrip("v")
    try:
        build(version, args.release_dir, args.out, allow_dev=args.allow_dev)
        verify_sums(os.path.abspath(args.out))
    except Refusal as e:
        print("release_package: REFUSED -- %s" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
