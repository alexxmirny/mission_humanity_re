#!/usr/bin/env python3
"""Shared machinery of the UI suite's entry points (tooling:TL-SUITE-SPLIT).

tools/test_ui.py (the registry suite) and its sibling mode scripts -- det_arms.py, tact_test.py,
ui_abc.py, soak_test.py -- import from here: the scenario registries, the ONE ui_test.py argv builder
(build_scenario_argv), RunnerConfig, lane provisioning, the relay process, the shared flags.
"""

import atexit
import contextlib
import dataclasses
import glob
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desktop  # noqa: E402  the raw CreateProcessW launch that honours lpDesktop
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import ui_test  # noqa: E402  boot_lock + wait_past_pack_load, shared with the UI suite
import ui_registry  # noqa: E402  TL-SUITE-REGDATA: the scenario registries (registry.yaml)
import machine_config as machine  # noqa: E402
import win_job  # noqa: E402  TL-SUITE-TEARDOWN: kill-on-close for the relay + relay-shim below

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_TEST = os.path.join(REPO, "tools", "ui_test.py")


@dataclasses.dataclass
class RunnerConfig:
    """Runner-wide launch choices, resolved ONCE in main() and passed down (tooling:TL-SUITE-CONFIG).
    The default instance is the interactive desktop on the stock exe -- what a library caller gets."""

    desktop: str = ""  # isolated desktop name; "" = the interactive one
    no_desktop: bool = False  # --no-desktop: the opt-out is forwarded too, not just the opt-in
    stock_exe: bool = True  # retail exe + msvfw32 proxy (I6b); --patched-exe opts out
    _desktop_held: bool = dataclasses.field(default=False, repr=False, compare=False)
    _lock: object = dataclasses.field(default_factory=threading.Lock, repr=False, compare=False)

    @classmethod
    def from_args(cls, args):
        """Isolation is ON unless --no-desktop, or --visible without an explicit --desktop."""
        no_desktop = bool(getattr(args, "no_desktop", False))
        name = getattr(args, "desktop", None)
        on = not no_desktop and (name or not getattr(args, "visible", False))
        return cls(
            desktop=(name or "mh_rig") if on else "",
            no_desktop=no_desktop,
            stock_exe=not getattr(args, "patched_exe", False),
        )

    def hold_desktop_once(self):
        """Create + hold the isolated desktop before the first direct launch. Locked: concurrent
        arms (--tact-jobs, parallel A/B/C arms, the suite's journal tail) race to it."""
        with self._lock:
            if not self._desktop_held:
                desktop.hold(self.desktop)
                self._desktop_held = True


# ---- mp:F2b: the MERGED Cyrillic/Polish font install, and whether this run can reach one --------
#
# F2's positive case (real Cyrillic + Polish glyphs, not the guard's substitute) was verified once by
# hand against a private merged install; its baseline depends on 61 RU retail bitmaps `fnt.py merge`
# lifts from a real RU disc, and the tool deliberately writes nothing it produces into this repo. So
# the font_merged scenario (registry.yaml) is OPT-IN: it runs a real lane against a merged mh_ex pack when one
# exists on this machine, and SKIPS BY NAME -- not FAIL -- when it does not.
#
# THE PRECONDITION IS A READ, NOT A MARKER FILE. `fnt.py install` writes no sentinel of its own (a
# hand-built merge -- `fnt.py merge` + a manual repack -- would carry none either), so the check reads
# the FONTLAY entry count straight off the candidate install's mh_ex pack with fnt.py's OWN reader
# (the same Source/font_layer machinery --selftest uses) and asks whether one probe character from
# each of the merge's three shapes actually resolves: a Cyrillic letter LIFTED byte-for-byte from the
# RU override, a Cyrillic letter DRAWN from scratch (the RU override never shipped it), and the
# guard's own DRAWN box glyph. Any one missing means this install was never merged, or the merge is
# stale/partial -- either way, not what this scenario probes.
#
# WHY A SEPARATE LANE SOURCE, NOT THE SHARED POLYGON. Every ordinary lane symlinks its packs from
# machine.POLYGON (make_lane.py's default --src), and font_guard's NEGATIVE baseline assumes that
# install is stock retail data -- merging Cyrillic into it would flip font_guard red on purpose (see
# that scenario's own header). So font_merged points its OWN lane at a SEPARATE, machine-local
# install via make_lane.py --src (the `lane_src` registry key) and never touches the
# polygon.
FONT_MERGE_DIR = os.environ.get("MH_FONT_MERGE_DIR") or os.path.join(REPO, "workdir", "mh_en_fonts")

# One probe code point per merge shape: U+041F P (Cyrillic, LIFTED byte-for-byte from the RU
# override), U+041A K (Cyrillic, DRAWN -- a straight copy of Latin K, since the RU override never
# shipped an uppercase K glyph at all), U+FFFD (the guard's box, DRAWN). All three land in FONTLAY
# only after a real F2 merge -- none of the three is in any shipped retail FONTLAY (measured: the RU
# override covers 61 of the 64 plain Cyrillic letters and zero Polish; EN base/override cover neither
# alphabet at all).
_FONT_MERGE_PROBE_CPS = (0x041F, 0x041A, 0xFFFD)


def _import_fnt():
    """src/formats/fnt.py is not normally on sys.path from tools/ -- same pattern gen_lzw_fixtures.py
    already uses for decompress.py, one directory over."""
    d = os.path.join(REPO, "src", "formats")
    if d not in sys.path:
        sys.path.insert(0, d)
    import fnt  # noqa: E402

    return fnt


def font_merge_available(game_dir):
    """True if `game_dir`'s mh_ex pack carries the F2b merge -- read directly, not from a marker
    `fnt.py install` may or may not have left behind."""
    if not os.path.isdir(game_dir):
        return False
    try:
        layer = _import_fnt().Source(game_dir).font_layer("mh_ex")
    except (FileNotFoundError, OSError, ValueError):
        return False
    if layer is None:
        return False
    return all(cp in layer.index for cp in _FONT_MERGE_PROBE_CPS)


def font_merge_precondition(args):
    """(ok, reason) for the font_merged scenario's `requires` hook. Checked ONCE, before
    provisioning (see the --local branch in main()) -- an unmet precondition must SKIP this one
    test, not crash make_lane.py on a --src that does not exist and abort every OTHER scenario's
    provisioning with it (the same whole-suite-abort shape fork F4H already had to fix once)."""
    if not args.local:
        # make_lane.py --src is the only channel that can point a lane at an alternate install; the
        # VM/rig topology has no equivalent (a peer's game directory there is fixed), so this
        # scenario can only ever run under --local, merged install or not.
        return False, "needs --local -- only a local lane can be pointed at a merged install"
    if not font_merge_available(FONT_MERGE_DIR):
        return False, (
            "no merged font install at %s (or its mh_ex pack is not a full F2 merge) -- build one "
            "with `python src/formats/fnt.py install --game <a scratch dir under workdir/, NEVER "
            "the polygon>`, or point MH_FONT_MERGE_DIR at one" % FONT_MERGE_DIR
        )
    return True, ""


# TL-HARN17. The DLL-side per-step watchdog (ui_drive.cpp inside mh_harness.dll) is irreducibly
# frame-counted -- that does not change here, and changing it would be a DLL rebuild (see the note
# where --timeout-frames is resolved in ui_test.py). What DOES change: every bare frame COUNT in this
# file used to be picked to "look big enough" at some assumed frame rate, which is exactly the unit
# mismatch the name invites -- a reader sees `1200000` and has no way to tell what real-world wait it
# is supposed to survive. `frames_for_seconds` makes the INTENT (a wall-clock budget) the thing
# written down, and derives the frame count from a documented FLOOR rate -- roughly half of the
# lowest rate this file has ever cited for that lane shape (see each constant's comment for its
# source), so the derived budget still holds under a slowdown of that size instead of assuming the
# nominal rate holds for the whole run. This is the "convert to a wall-clock budget derived from the
# lane's measured frame rate" fix; the flag/ini key stay named `timeout_frames` because at the DLL
# boundary that is exactly what they are. Defined before the registry load (rather than beside
# LOCAL_TIMEOUT_FRAMES further down) because registry.yaml's `!frames` rows resolve through it at import.
SOLO_HEADLESS_FPS_FLOOR = 900  # half of the ~1876 fps measured 2026-07-28 (see the det3 section
# further down: "the client aborted `peers <1` at 60001 frames / 31.985 s")
BLIT_LOCAL_FPS_FLOOR = 25  # half of the "lobby's ~52 fps" cited where --timeout-frames is resolved
# for a kept-blit --determinism run, further down
BLIT_VM_FPS_FLOOR = 1500  # half of "VM peers were measured at ~3115 fps WITH the blit on this rig"
# (det3 section further down)


def frames_for_seconds(seconds, floor_fps):
    """A wall-clock seconds budget -> the frame count the DLL's per-step watchdog actually wants,
    using a documented FLOOR rate (not the nominal one) for the lane shape -- see the block comment
    above. Always at least 1 frame."""
    return max(1, round(seconds * floor_fps))


# The four scenario registries live in tools/uiscripts/registry.yaml, schema-checked on load by
# tools/ui_registry.py (TL-SUITE-REGDATA). Its `!ref` / `!frames` values resolve against these names.
_REGISTRY = ui_registry.load(
    refs={
        "frames_for_seconds": frames_for_seconds,
        "SOLO_HEADLESS_FPS_FLOOR": SOLO_HEADLESS_FPS_FLOOR,
        "font_merge_precondition": font_merge_precondition,
        "FONT_MERGE_DIR": FONT_MERGE_DIR,
        "machine.DEAD_PEER_IP": machine.DEAD_PEER_IP,
    }
)
TESTS = _REGISTRY["tests"]
SIM_SCENARIOS = _REGISTRY["sim_scenarios"]
# sim_resid's session-entry bodies run in exactly the sim soaks, so it is an alias, not a second set.
SIM_RESID_SCENARIOS = SIM_SCENARIOS
UIREC_SCENARIOS = _REGISTRY["uirec_scenarios"]
TACT_SCENARIOS = _REGISTRY["tact_scenarios"]


def vm_reachable(ip, port=22, timeout=4):
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def sp_newest_run(lane_dir):
    # SES1: prefer the PROCESS ("menu") directory. A single-player lane never opens a lobby so it
    # only ever has one shape of folder -- but this is also used to read a lane that DID, and there
    # the harness outputs (which is what every caller here wants) are in the process directory, while
    # a newer session directory would win a plain mtime sort.
    runs = sorted(glob.glob(os.path.join(lane_dir, "logs", "*")), key=os.path.getmtime)
    menu = [d for d in runs if "_menu_" in os.path.basename(d)]
    return (menu or runs)[-1] if runs else None


def sp_newest_session_run(lane_dir):
    """mp:D28: the INVERSE of sp_newest_run's menu-preference, for a checker whose evidence is
    match-time-only content in the SESSION directory's own mh_net.log (not the process/"menu"
    directory's copy, which is frozen at the lobby -- see check_cancel_task.py's net_log_lines: it
    reads a given run dir's own mh_net.log PLUS the process dir session.json names, i.e. it needs
    to be handed the SESSION dir to find both halves; handed the process dir instead (what every
    other post_check consumer wants, since mh_harness.log/mh_lockstep.log live there and a boot-time
    banner does too) it can only ever see the boot-time half. Existing checkers (mp_analyze.py,
    check_u39_echo.py's KEPT_LINE) only need boot-time-or-harness content, so they were never
    exposed to this gap; check_cancel_task.py's `; D28: ... routed as order` line is written well
    after Start, into the session dir alone. Prefer a NON-`_menu_` dir (the newest one), falling
    back to sp_newest_run's normal resolution when none exists (a solo/menu-only lane)."""
    runs = sorted(glob.glob(os.path.join(lane_dir, "logs", "*")), key=os.path.getmtime)
    session = [d for d in runs if "_menu_" not in os.path.basename(d)]
    return session[-1] if session else sp_newest_run(lane_dir)


# ---- TACT-PREP: the TACTICAL determinism oracle -------------------------------------------------
# Two sequential runs of one local lane through the SAME --tactical entry, compared on the per-frame
# `T`/`TR` lines the llm_tact_frame cadence emits.
#
# WHY IT DOES NOT GO THROUGH ui_test.py LIKE THE OTHER ORACLES. Every other mode drives the game with
# a UI SCRIPT; this one has nothing to drive. The `--tactical` launch verb IS the driver -- it fires
# at menu-idle, synthesises the squad blackboard and enters the mission -- and after that the
# mission runs itself, because tactical is all-AI as shipped (the tactical-probe work 5b: both sides
# go through llm_tact_unit_owner_tick, and every shipped POZ*.DAT assigns DEFENSE:GUARD/ATTACK to
# the player's own units). So there is no script to pass and no input to record; there is a command
# line and a stop frame.
#
# THE SELFTEST ARM IS NOT OPTIONAL DECORATION. A hash over a region nothing writes is identical
# across two runs too, so a green comparison alone cannot distinguish "deterministic" from
# "measuring nothing". --tact-selftest pokes ONE region mid-run and requires the compare to fail at
# EXACTLY that frame and to name EXACTLY that region. Default is tact_doors, deliberately a region
# that does not otherwise change during the run -- poking a moving region would still divergence-
# match if the hash only tracked the units slice.
# The DirectInput divisor a HUMAN-DRIVEN tactical session needs in a VM (the VM-input notes 9e).
#
# 32767/640: a hypervisor presents an ABSOLUTE pointer whose DirectInput range is 0..32767, and the
# game maps counts onto a 640-wide screen -- so this is the scale the device is actually reporting
# at, derived, not tuned. It was proposed on that basis and then WITHDRAWN in 9d, because in the
# shipped path a divisor this large creates a dead zone (the poll's `dwData / divisor` is a signed
# IDIV that carries no remainder, so every count below the divisor contributes exactly zero). With
# dinputto8 in front of the game that objection does not hold -- measured 4.9% swallowed
# event-frames at div=51 against 38% at div=40 without the wrapper -- so the derivation stands
# again, and it is the default rather than something a human has to remember mid-recording.
TACT_PLAY_MOUSE_DIV = 51


def tact_merge_ini(lane_dir, fragments, section="shadow"):
    """Merge ini `fragments` into the lane's mh_net.ini BY SECTION. Returns (merged_text, armed_keys).

    Shared by --tact-arm (TACT-RIG, the shadow vehicle) and the promoted-vs-original A/B driver (TACT-AB, the
    promoted-golden A/B), which arm DIFFERENT sections of the same file -- `[shadow]` and
    `[promote]` -- through the same merge. Extracted rather than copied because the merge is where
    the trap lives: GetPrivateProfile* returns the FIRST matching section, so an APPENDED second
    `[shadow]` (or `[promote]`) block is in the file and unreachable, and the run comes back with
    every key unarmed while looking like a domain nothing calls.

    `armed_keys` is read back OUT of the merged text, from the named section only. Counting every
    `=1` in the file would fold in `enable=1`, `no_present=1` and the rest of the lane's identity and
    print a number that looks like a key count and is not one -- the sort of banner that makes a
    mis-merged fragment invisible. Reading it back rather than echoing what was passed in is the
    same discipline tact_read applies to the DLL's own banners: a caller cannot see the key it
    failed to set.
    """
    net_ini = os.path.join(lane_dir, "mh_net.ini")
    base = open(net_ini, encoding="utf-8").read()
    for frag in fragments:
        path = frag if os.path.isabs(frag) else os.path.join(REPO, frag)
        if not os.path.isfile(path):
            raise FileNotFoundError(path)
        base = ui_test.ini_merge_fragment(base, open(path, encoding="utf-8").read())
    with open(net_ini, "w", newline="\r\n") as f:
        f.write(base)

    armed, in_section = [], False
    for ln in base.splitlines():
        t = ln.strip()
        if t.startswith("["):
            in_section = t.lower() == "[%s]" % section.lower()
        elif in_section and t.endswith("=1") and not t.startswith(";"):
            armed.append(t.split("=")[0])
    return base, armed


# ---- THE D11 CONFIGURATION SELECTOR (fork F2C, 2026-09-12) ---------------------------------------
#
# `[config] mode` (src/mh_dll/mh/config/config.h) is ONE key naming which implementation the process
# runs: `original` (the game's own bodies), `brokered` (today's ship configuration). Every promotion
# default and the rebind-row default derive from it.
#
# WHAT THIS REPLACED, and why the replacement is not merely shorter. The all-original arm used to be
# DERIVED on every write -- 47 `[promote]` keys scraped out of the sources plus all 719 `[rebind]`
# rows read out of libmh_rebind.json -- because a hand-listed rollback goes stale silently, and a
# rollback arm that misses a knob is not a rollback but a quieter copy of the configuration it was
# meant to disprove. That derivation was right for a per-key world and it was still a treadmill:
# rollback_original.ini's own header records three separate gate runs lost to a key that shipped ON
# and was not named. F2A made the configuration a VALUE, so there is nothing left to enumerate, and
# F2A measured the two arms identical -- `mode=original` against the 47-key + 719-row fragment, 8000
# steps x 63 region channels, with perturbation and wrong-seed controls going red on cue.
#
# AND IT FIXES THE ROUTE, not only the size (the F2A hand-off note). The derived fragment harvested
# `wire` -- a key turn_engine.cpp REFUSES by name because C8-d retired it -- so the rollback arm
# reached "not promoted" through `RUN-CONFIG: REFUSED (retired key wire)`, i.e. the stale-config
# refusal, and not through the promotion gate at all. It compared correctly by accident: a fragment
# whose first act is to make the DLL reject its own configuration is not the configuration under
# test. The selector fragment carries no `[promote]` key at all, so `install_promotion` reaches its
# ordinary `lockstep` gate with a default of 0 and returns there. The retired-key refusal is
# untouched and still guards the real case it was built for -- an old fragment on disk naming `wire`
# / `wire_seams` / `lockstep_seams` -- and det_standard_selftest's sixth rule still scans every
# committed fragment for one.
CONFIG_SECTION = "config"
MODE_ORIGINAL = "original"
MODE_BROKERED = "brokered"

# NO BRACKETED `promote`/`rebind` TOKEN IN THIS BANNER, on purpose: F2E's drop-vocabulary gate scans
# for that spelling, and a generated file that trips it would make the gate argue with its own
# tooling. Say what the mode does instead of naming the sections it replaces.
ALL_ORIGINAL_WHY = (
    "; GENERATED by tools/test_ui.py (write_all_original_ini) -- do not hand-edit, do not commit.\n"
    "; THE ALL-ORIGINAL ARM: the game runs its OWN bodies, end to end. One key, because D11 made the\n"
    "; configuration a value -- every promotion default and the rebind-row default derive from it\n"
    "; (src/mh_dll/mh/config/config.h). Proven identical to the 47-key + 719-row fragment this\n"
    "; replaced over 8000 steps x 63 region channels (tracker fork F2A).\n"
)


def write_config_mode_ini(path, mode, banner):
    """Write an --extra-ini fragment selecting ONE D11 configuration. Returns the mode."""
    if mode not in (MODE_ORIGINAL, MODE_BROKERED):
        # `standalone` is a property of the BUILD (MH_LIBMH_BUILD) and is not selectable from an ini;
        # a hosted lane claiming it would be describing a binary layout it does not have.
        raise ValueError("%r is not an ini-selectable [config] mode" % (mode,))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(banner)
        f.write("[%s]\n%s=%s\n" % (CONFIG_SECTION, "mode", mode))
    return mode


def write_all_original_ini(path):
    """An --extra-ini fragment putting the WHOLE DLL back on the game's original bodies."""
    return write_config_mode_ini(path, MODE_ORIGINAL, ALL_ORIGINAL_WHY)


def ini_selected_mode(text):
    """The `[config] mode` an ini TEXT actually selects, modelling GetPrivateProfile*'s reader.

    None when the file names none -- which is not the same as `brokered`, even though the DLL
    defaults there: a fragment that was supposed to select a mode and does not is the failure this
    exists to catch, and answering `brokered` for it would hide exactly that. Routed through
    ui_test.ini_effective so an UNREACHABLE key -- one in a duplicate `[config]` block, the trap
    ini_merge_section exists for -- reads as absent here too, the way the game reads it.
    """
    return ui_test.ini_effective(text, CONFIG_SECTION, "mode")


def ini_file_mode(path):
    """`ini_selected_mode` for a fragment/lane ini on disk. None if the file is missing."""
    if not os.path.isfile(path):
        return None
    return ini_selected_mode(open(path, encoding="utf-8", errors="replace").read())


def tact_apply_extra_ini(lane_dir, args):
    """Merge --extra-ini into a tactical lane and REPORT what it did. Returns the merged text, or
    None if a fragment was missing (the caller returns 1).

    EVERY tactical arm must call this. Until 2026-09-04 only --tact-arm did: --tact-play,
    --tact-verify and --tact-replay accepted the flag and dropped it. That is worse than rejecting
    it -- a session played with `--extra-ini tmp/c5_baseline.ini` ran the SHIP config and read as
    evidence that rolling the domain back changed nothing, which is exactly backwards. The three
    journal/play arms are the ones a rollback fragment is FOR: they are how you ask "is this
    regression ours?" of a recorded human session.

    Reporting is per SECTION and counts BOTH polarities, because tact_merge_ini's own `armed` list
    is `[shadow]`-only and counts `=1` alone -- it would print "0 keys" for a rollback fragment, the
    fragment most likely to be passed here. The live count is read back OUT of the merged file, not
    echoed from the fragment: an appended duplicate section is present and unreachable
    (GetPrivateProfile* takes the FIRST match), which is the trap tact_merge_ini documents.
    """
    try:
        merged, _armed = tact_merge_ini(lane_dir, args.extra_ini or [])
    except FileNotFoundError as e:
        print("FAIL: --extra-ini fragment not found: %s" % e)
        return None
    if not getattr(args, "extra_ini", None):
        return merged

    want = {}
    for frag in args.extra_ini:
        path = frag if os.path.isabs(frag) else os.path.join(REPO, frag)
        sect = None
        for ln in open(path, encoding="utf-8"):
            t = ln.strip()
            if t.startswith("["):
                sect = t.strip("[]").lower()
            elif sect and "=" in t and not t.startswith(";"):
                _k, _, v = t.partition("=")
                want.setdefault(sect, [0, 0])[0 if v.strip() == "0" else 1] += 1
    for sect in sorted(want):
        off, on = want[sect]
        live, seen = False, 0
        for ln in merged.splitlines():
            t = ln.strip()
            if t.startswith("["):
                live = t.lower() == "[%s]" % sect
            elif live and "=" in t and not t.startswith(";"):
                seen += 1
        print(
            "  extra-ini [%s] %d off / %d on from the fragment(s); %d key(s) live in the lane ini"
            % (sect, off, on, seen)
        )
    return merged


class _tact_pid_handle:
    """A subprocess.Popen-shaped wrapper over a bare PID.

    desktop.spawn returns a PID, not a Popen, because it is a raw CreateProcessW. The arm runner
    wants wait(timeout=)/kill(), so give it those over an OpenProcess handle rather than reshaping
    the caller around which launch path was taken -- the two paths must be interchangeable or they
    will drift, and the drift is what put a window on the operator's screen in the first place."""

    def __init__(self, pid):
        self.pid = pid

    def wait(self, timeout=None):
        import ctypes

        SYNCHRONIZE, PROCESS_TERMINATE, PROCESS_QUERY = 0x00100000, 0x0001, 0x0400
        h = ctypes.windll.kernel32.OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY, False, self.pid
        )
        if not h:
            return 0  # already gone
        try:
            ms = 0xFFFFFFFF if timeout is None else int(timeout * 1000)
            if ctypes.windll.kernel32.WaitForSingleObject(h, ms) == 0x102:
                raise subprocess.TimeoutExpired(self.pid, timeout)
            code = ctypes.c_ulong(0)
            ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
            return code.value
        finally:
            ctypes.windll.kernel32.CloseHandle(h)

    def kill(self):
        import ctypes

        h = ctypes.windll.kernel32.OpenProcess(0x0001, False, self.pid)
        if h:
            ctypes.windll.kernel32.TerminateProcess(h, 1)
            ctypes.windll.kernel32.CloseHandle(h)


def soak_ai_premise(text, ai_on, nplay, mask):
    """(ok, report lines) for the AI-premise half of the soak shape rules.

    EXTRACTED so it can be tested WITHOUT A RIG (2026-09-05). The premise is per scenario kind and it
    is silent when it is working, which is exactly the class det_standard_selftest exists for -- and
    this rule spent the day asserting the wrong thing for save-loaded soaks with nobody able to check
    it offline. Every arm is exercised in that selftest.
    """
    lines, ok = [], True
    n = int(nplay)
    seated = mask[:n]
    lines.append(
        "      AI: master_gate=%s players=%s mask=[%s] (claimed slots: %s)"
        % (ai_on, nplay, mask, "all AI" if seated.count("1") == n else "MIXED")
    )
    # THE PREMISE IS PER SCENARIO KIND, AND IT IS DERIVED FROM THE RUN (2026-09-05, user's call).
    # "every seat thinks" is the DEFAULT-START soak's premise and is simply the wrong assertion for a
    # save-loaded one: the ALLAI conversion happens at LANDING, then LOADGAME REPLACES the world with
    # the save's own roster, so a developed 2-player save is MIXED by construction. That scenario is
    # registered for COVERAGE BREADTH -- a quieter developed world reaches branches an empty 8-way
    # start never does -- and holding it to the busy-start premise made it report `soak: FAIL` on all
    # three runs while coverage.py pinned a baseline from them anyway (tracker SOAK-SAVED-MIXED).
    #
    # DERIVED, not declared: the registry carried `soak_shape_fails: True` on that scenario for exactly
    # this, and nothing ever read it -- a dead flag whose only effect was to make a reader think the
    # case was handled. The LOADGAME line is a fact about THIS run, so the check asks the run instead.
    # Same reasoning as SIM1-P clause 6a preferring a derived list to a hand-kept one.
    loaded = re.search(r"; \[save\] LOADGAME step=(\d+) name=(\S+) -> rc=1", text)
    if ai_on == "0":
        ok = False
        lines.append(
            "      FAIL: the AI master gate is OFF, so no seat thinks at all. That is fatal for every "
            "soak shape, loaded or not."
        )
    elif loaded:
        # The weaker premise, and it still has teeth: a loaded world with ZERO AI simulates nothing and
        # would report coverage of an idle map.
        if seated.count("1") == 0:
            ok = False
            lines.append(
                "      FAIL: after LOADGAME at step %s the save's roster has NO AI-enabled slot, so "
                "nothing drives the world and any coverage from it is of an idle map."
                % loaded.group(1)
            )
        else:
            lines.append(
                "      AI: save-loaded premise applied (LOADGAME step=%s name=%s) -- %d of %d claimed "
                "slots think, which is what this scenario kind asserts; 'every seat thinks' is the "
                "default-start premise and does not survive a world replacement."
                % (loaded.group(1), loaded.group(2), seated.count("1"), n)
            )
    elif seated.count("1") != n:
        ok = False
        lines.append(
            "      FAIL: %d of %d claimed slots are not AI-enabled. The default-start soak's whole "
            "premise is that every seat thinks (a save-loaded run is judged by the weaker premise "
            "instead, and this run loaded nothing)." % (n - seated.count("1"), n)
        )
    return ok, lines


def build_scenario_argv(
    *,
    script=None,
    host=None,
    clients=(),
    connect_ip=None,
    port=None,
    determinism=False,
    harness=False,
    steps=None,
    extra_ini=(),
    extra_ini_host=None,
    extra_ini_client=None,
    client_dead_ip=None,
    det_exclude=(),
    shim=None,
    timeout=None,
    tol=None,
    pixdelta=None,
    update_baselines=False,
    net_extra=None,
    headless=None,
    timeout_frames=None,
    launch_args=None,
    deploy_save=None,
    harness_extra=None,
    harness_extra_host=None,
    net_extra_client=None,
    ship_pacing=False,
    force_headless=False,
    no_client_ip=False,
    client_game_name=None,
    signal_touch=None,
    client_after_exit=(),
    client_expect_exit=(),
    host_dir=None,
    omit_satellite=(),
    ai_probe=None,
    record=None,
    pull_logs=None,
    dll=None,
    cfg=None,
):
    """THE ONE PLACE a tools/ui_test.py argv is built (tooling:TL-SUITE-ARGV). Every option is
    emitted at most once, in one fixed order; a caller resolves any override (row vs suite) itself.
    `headless`: True/False/None = --headless/--visible/neither. `shim`: {target, delay, jitter,
    listen_port, control_port, timeline, triggers}. `cfg`: append the runner-wide exe/desktop
    choice when a RunnerConfig is given (the soak's direct call never carried one)."""
    argv = [script] if script else []
    argv += ["--determinism"] if determinism else []
    argv += ["--harness"] if harness else []
    argv += ["--steps", str(steps)] if steps is not None else []
    argv += ["--host", host] if host else []
    argv += ["--connect-ip", connect_ip] if connect_ip else []
    argv += ["--port", str(port)] if port is not None else []
    argv += [a for f in extra_ini for a in ("--extra-ini", f)]
    argv += ["--extra-ini-host", extra_ini_host] if extra_ini_host else []
    argv += ["--extra-ini-client", extra_ini_client] if extra_ini_client else []
    argv += [a for c in clients for a in ("--client", c)]
    argv += ["--client-dead-ip", client_dead_ip] if client_dead_ip else []
    argv += [a for p in det_exclude for a in ("--det-exclude", p)]
    if shim:
        argv += ["--shim", shim["target"], "--shim-delay", str(shim.get("delay", 0))]
        argv += ["--shim-jitter", str(shim["jitter"])] if shim.get("jitter") else []
        argv += ["--shim-listen-port", str(shim["listen_port"])] if shim.get("listen_port") else []
        if shim.get("control_port"):
            argv += ["--shim-control-port", str(shim["control_port"])]
        argv += ["--shim-timeline", shim["timeline"]] if shim.get("timeline") else []
        argv += [a for t in shim.get("triggers") or [] for a in ("--shim-trigger", json.dumps(t))]
    argv += ["--timeout", str(timeout)] if timeout is not None else []
    argv += ["--tol", str(tol)] if tol is not None else []
    argv += ["--pixdelta", str(pixdelta)] if pixdelta is not None else []
    argv += ["--update-baselines"] if update_baselines else []
    argv += ["--net-extra", net_extra] if net_extra else []
    if headless is not None:
        argv.append("--headless" if headless else "--visible")
    argv += ["--timeout-frames", str(timeout_frames)] if timeout_frames is not None else []
    argv += ["--launch-args", launch_args] if launch_args else []
    argv += ["--deploy-save", deploy_save] if deploy_save else []
    argv += ["--harness-extra", harness_extra] if harness_extra else []
    argv += ["--harness-extra-host", harness_extra_host] if harness_extra_host else []
    argv += ["--net-extra-client", net_extra_client] if net_extra_client else []
    argv += ["--ship-pacing"] if ship_pacing else []
    argv += ["--force-headless"] if force_headless else []
    argv += ["--no-client-ip"] if no_client_ip else []
    argv += ["--client-game-name", client_game_name] if client_game_name else []
    argv += ["--signal-touch", signal_touch] if signal_touch else []
    argv += [a for c, p in client_after_exit for a in ("--client-after-exit", "%d:%d" % (c, p))]
    argv += [a for c in client_expect_exit for a in ("--client-expect-exit", str(c))]
    argv += ["--host-dir", host_dir] if host_dir else []
    argv += [a for s in omit_satellite for a in ("--omit-satellite", s)]
    argv += ["--ai-probe", str(ai_probe)] if ai_probe else []
    argv += ["--record", str(record)] if record else []
    argv += ["--pull-logs", pull_logs] if pull_logs else []
    argv += ["--dll", dll] if dll else []
    if cfg is not None:
        # BOTH halves of each runner-wide choice: the child re-derives its own default otherwise
        argv += [] if cfg.stock_exe else ["--patched-exe"]
        if cfg.desktop:
            argv += ["--desktop", cfg.desktop]
        elif cfg.no_desktop:
            argv.append("--no-desktop")
    return argv


def ui_test_cmd(argv):
    """The ui_test.py command line for a build_scenario_argv() result."""
    return [sys.executable, UI_TEST, *argv]


def run_ui_test(argv, timeout, capture=False):
    """Invoke ui_test.py with argv (list). Returns (exit code, captured text).

    `capture` buffers the child's output instead of streaming it -- required under --jobs, where
    several tests run at once and live streams would interleave into an unreadable mess. The buffered
    text is printed as one block when the test finishes.
    """
    cmd = ui_test_cmd(argv)
    banner = "    $ python tools/ui_test.py %s" % " ".join(argv)
    if not capture:
        print(banner)
        try:
            return subprocess.run(cmd, cwd=REPO, timeout=timeout).returncode, ""
        except subprocess.TimeoutExpired:
            print("    !! ui_test.py exceeded the wrapper timeout (%ds)" % timeout)
            return 2, ""
    try:
        r = subprocess.run(cmd, cwd=REPO, timeout=timeout, capture_output=True, text=True)
        return r.returncode, banner + "\n" + (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired as e:
        tail = (
            (e.stdout or b"").decode("utf-8", "replace")
            if isinstance(e.stdout, bytes)
            else (e.stdout or "")
        )
        return (
            2,
            banner
            + "\n"
            + tail
            + "\n    !! ui_test.py exceeded the wrapper timeout (%ds)" % timeout,
        )


# ---- PROGRESS WATCHDOG (tooling:TL-HARN19) -------------------------------------------------------
#
# A row's `timeout` is a FLAT wall clock: run_ui_test above kills the whole ui_test.py child the
# moment it is exceeded, whether the scenario is wedged or simply walking slower than usual because
# --jobs N put seven other lanes on the same CPU. Measured: u39_diplomacy solo 51s / inside a
# contended suite 424s against its 420s budget, ALL PAIRS IDENTICAL either way -- the run was never
# wrong, only slow, and the flat bound cannot tell the difference. Below: a watchdog keyed on
# whether any lane is still WRITING, not on how long it has been running.
#
# ui_log() (src/mh_dll/mh/seams/ui_drive.cpp) opens mh_uidrive.log, appends one line and closes the
# handle on EVERY `; [script]` line, with no buffering -- so the file's mtime/size change the instant
# a step happens, and staying unchanged is a real (if imperfect) proxy for "nothing happened", not an
# artifact of some flush interval. It is imperfect in one direction only: a single WAIT step logs
# nothing while it waits (only its "ok"/"TIMEOUT" line, when it ends) -- see script_tick() -- so a
# stall threshold has to be picked comfortably above one step's own legitimate wait, not above one
# scenario's whole walk. `WALK_STALL_S` is that threshold; `WALK_BACKSTOP_MULT` sizes the generous
# last-resort bound behind it, for a run that never leaves ANY evidence to read progress from at all.
# 240, not 90: the longest single wait in passing runs was 74 s (resync_storm_repro, measured
# 2026-09-26 over three days of lane logs) at solo speed, and gameclock waits stretch under load.
WALK_STALL_S = 240
WALK_BACKSTOP_MULT = 3  # ui_test.py's own --timeout, raised to max(row timeout, this x budget_s)


def _walk_log_fingerprint(run_dir):
    """(mtime, size) of one run dir's mh_uidrive.log, or None if it does not exist yet/is
    unreadable. None reads as "no evidence from this lane yet", never as a stall -- a lane that
    has not booted is NEVER-STARTED's territory (classify_result), not WALK-STALLED's."""
    try:
        st = os.stat(os.path.join(run_dir or "", "mh_uidrive.log"))
        return (st.st_mtime, st.st_size)
    except OSError:
        return None


def _run_watched(cmd, backstop, run_dirs_fn, stall_s=0, capture=False, poll_s=5, banner=None):
    """Run `cmd` (a full argv, not just ui_test.py's tail), killing it EARLY -- before `backstop` --
    only once NONE of `run_dirs_fn()`'s directories has an mh_uidrive.log that changed in the last
    `stall_s` seconds. Never kills while at least one lane is still writing, however slowly.
    `stall_s=0` disables the early kill (degrades to a flat `backstop`-second kill, same shape as
    run_ui_test). `backstop` is the absolute ceiling either way -- a run with no evidence to read
    progress from at all (a boot failure) is bounded by it alone, same as an ever-advancing run that
    somehow never finishes. Split from run_ui_test_watched() so a selftest can drive it against a
    fake child instead of a real ui_test.py invocation.

    Returns (exit code, captured text). Both kill paths return rc=2 -- the vocabulary run_ui_test's
    own TimeoutExpired path already uses, so looks_like_timeout() (rc == 2) needs no change.
    """
    if not capture and banner:
        print(banner)
    proc = subprocess.Popen(
        cmd,
        cwd=REPO,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True,
    )
    t0 = time.time()
    last_progress_t = t0
    last_fp = None
    kill_why = None
    out = None
    while True:
        try:
            out, _ = proc.communicate(timeout=poll_s)
            break  # exited on its own -- the common, fast case
        except subprocess.TimeoutExpired:
            pass
        now = time.time()
        fps = [_walk_log_fingerprint(d) for d in run_dirs_fn()]
        fp = tuple(fps) if any(f is not None for f in fps) else None
        if fp != last_fp:
            last_fp, last_progress_t = fp, now
        if now - t0 > backstop:
            kill_why = "backstop %ds exceeded (no run ever finished)" % backstop
            break
        if stall_s and fp is not None and now - last_progress_t > stall_s:
            kill_why = "no lane's mh_uidrive.log advanced for %ds" % stall_s
            break
    if kill_why:
        proc.kill()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            pass  # best-effort drain; win_job's kill-on-close job still reaps the peer processes
        msg = "    !! PROGRESS WATCHDOG: %s" % kill_why
        if not capture:
            print(msg)
        tail = ((banner + "\n") if capture and banner else "") + (out or "") + "\n" + msg
        return 2, tail if capture else ""
    if capture:
        return proc.returncode, ((banner + "\n") if banner else "") + (out or "")
    return proc.returncode, ""


def run_ui_test_watched(argv, backstop, run_dirs_fn, stall_s=0, capture=False, poll_s=5):
    """run_ui_test's progress-watchdog sibling (tooling:TL-HARN19): same signature shape and return
    contract, but the kill decision is `_run_watched`'s (see there), not a flat `timeout`."""
    banner = "    $ python tools/ui_test.py %s" % " ".join(argv)
    return _run_watched(
        ui_test_cmd(argv),
        backstop,
        run_dirs_fn,
        stall_s=stall_s,
        capture=capture,
        poll_s=poll_s,
        banner=banner,
    )


# ---- LOCAL LANES: one folder per test, and one per (peer x test) --------------------------------
#
# `--local` runs the whole suite on THIS machine instead of the VMs, giving every peer of every test
# its own lane folder. That is worth more than the ssh it saves: a shared install makes a test's
# captures depend on the setup.dat / [video] mode the PREVIOUS test left behind (it then
# passes standalone and fails in the suite), and a folder per peer removes the inheritance entirely.
# A lane is ~3.5 MB, so ~18 of them cost less than one copy of Res\.
#
# NAMING: solo -> ui_<test>;  multi -> ui_<test>_host, ui_<test>_c1, ui_<test>_c2 ...
#
# LANE NUMBERS are unique across the WHOLE suite, not per test, because the number is what renames the
# single-instance mutex -- two lanes sharing a number could never run at the same time, which is
# exactly what concurrency needs.
#
# PORTS ARE PER TEST, NOT PER PEER. `[net] port` is both the host's listen port and the port a client
# DIALS, so peers of one match must share it; distinct ports separate concurrent MATCHES. Getting this
# backwards leaves the host on `peers 1` and the client on `sessions 1`, which reads like a discovery
# failure.
LOCAL_PORT_BASE = 6600
# A shim test is the ONE case where peers of a match do NOT share a port. The shim and the host lane
# are both on this box, so they cannot both own the game port -- and the shim binds first, so the GAME
# is what fails (`net: bind(:6600) failed 10013`), leaving the client stuck on `sessions 1` looking
# exactly like a discovery bug (H3, 2026-07-28). The client dials the shim's port instead, and the shim
# forwards to the host's. Its own band, well clear of LOCAL_PORT_BASE + len(TESTS).
LOCAL_SHIM_PORT_BASE = 6700
# mp:R2b -- a CLIENT lane that HOSTS a lobby of its own (`host_lanes` in a registry row: 1-based
# client indices) cannot share the test's port: a UDP host binds `[net] port` with
# SO_EXCLUSIVEADDRUSE, so the second host on one port is REFUSED at bind and its lobby, though
# published to the relay, can never receive a JOIN (measured 2026-09-22, browser_two_rows' first
# standalone run: `udp bind(:6600) REFUSED`, the client seated itself in an empty lobby). Its own
# band, like the shim's. When such a row runs as a share_lanes sharer this is moot -- it borrows a
# HOST lane from a second target, whose port is that target's own -- so this only decides the
# standalone (target-absent) shape.
LOCAL_HOST2_PORT_BASE = 6800
# --local implies --headless, and headless removes the vsync wait -- so the per-step watchdog, which is
# budgeted in FRAMES, expires far sooner in wall-clock than it does with the blit enabled. 1500 (the
# default) does not even survive boot on this host. This is a floor; a test with its own timeout_frames
# still wins. Headless frames tick so fast they blow a FRAME-budget watchdog.
#
# TL-HARN17: see frames_for_seconds() near the top of this file for what this derives from. 90 s at
# the floor rate (was a bare 80000, i.e. ~89 s at this same floor -- this is not a cut).
LOCAL_TIMEOUT_FRAMES = frames_for_seconds(90, SOLO_HEADLESS_FPS_FLOOR)


def client_lane_slot(test, cidx):
    """The 1-based CLIENT LANE SLOT client #cidx (1-based, in `clients` order) actually uses --
    itself, unless `client_shares_lane` (mp:GS1(b)) remaps it onto an EARLIER client's slot.

    That key exists for the PROCESS-EXIT relaunch shape (paired with ui_test.py's
    `--client-after-exit`): client #2's process is a brand-new one launched only after client #1's
    has actually exited, so it is safe -- and, on this tree's 99-mutex lane ceiling (lane_alloc.py
    TL-LANEPOOL, the `suite` block already at its registry demand), NECESSARY -- for it to reuse
    client #1's own lane folder rather than costing the suite a third lane it does not have.
    """
    reuse = test.get("client_shares_lane") or {}
    seen = set()
    while cidx in reuse:
        if cidx in seen:
            raise ValueError(
                "test %r: client_shares_lane cycle at client %d" % (test["name"], cidx)
            )
        seen.add(cidx)
        cidx = reuse[cidx]
    return cidx


def client_lane_slots(test):
    """The DISTINCT client lane slots this test's `clients` resolve to, in first-use order (1-based).
    Length < len(test["clients"]) exactly when `client_shares_lane` folds two or more onto one."""
    slots = []
    for i in range(len(test["clients"])):
        s = client_lane_slot(test, i + 1)
        if s not in slots:
            slots.append(s)
    return slots


def share_targets(test):
    """The scenarios a `share_lanes` row borrows lanes from, as a list. One name (the mp:R7a form) or
    a LIST of names (mp:R2b): a row that needs MORE lanes than any one comparable scenario owns
    borrows from several -- browser_two_rows needs three peers (two hosts + a browser) and no
    registry row owns three lanes. The borrowed lane set is every target's HOST lane first, then
    their client lanes, because a host lane is the only lane whose game port is unique in the run
    (provision_lanes gives one port per test, shared by its peers): a sharer that hosts N lobbies
    must put them on N host lanes, or the second host's UDP bind is refused against the first's.
    The runner schedules a sharer serially with ALL of its targets (one worker), as it does a pair."""
    tg = test.get("share_lanes")
    if not tg:
        return []
    return [tg] if isinstance(tg, str) else list(tg)


def lane_names(test):
    """Every lane a test needs, in peer order (host first).

    mp:R7a -- a test with `share_lanes: "<other>"` OWNS NO LANES: it reuses the lanes (and port) of a
    comparable scenario it never runs concurrently with, and so contributes NOTHING to the suite's
    lane demand. The capture suite allocates one lane per registry row and the block is at the DLL's
    mutex ceiling (lane_alloc.py, TL-LANEPOOL), so a new row that needed its own two lanes could not
    be added at all; sharing is how direct_dial_with_relay_set joins the registry without one. The
    reuse is made safe by the runner scheduling a share pair SERIALLY (never both on the shared lane
    at once) and by every run redeploying its own script/ini into the lane folder before it launches.

    mp:GS1(b) -- `client_shares_lane` (see client_lane_slots) folds a relaunched client onto an
    earlier one's lane WITHIN one test, so the lane it needs is not counted twice either.
    """
    if test.get("share_lanes"):
        return []
    if test["kind"] == "solo":
        return ["ui_" + test["name"]]
    return ["ui_%s_host" % test["name"]] + [
        "ui_%s_c%d" % (test["name"], s) for s in client_lane_slots(test)
    ]


def provision_lanes(tests, headless=True, port_base=None, lane_base=0, stock_exe=True):
    """Build a lane folder per (peer x test). Returns {test_name: (port, shim_port, [lane names])}.

    `port_base`/`lane_base` exist so a caller that is NOT the capture suite can carve out its own
    range: the lane number is the mutex separation and the port is the game port, so two runs sharing
    either would fight. --det-local uses them for exactly that reason.
    """
    # THE DEMAND IS CHECKED BEFORE ANYTHING IS PROVISIONED (fork F4H). This loop derives its lane
    # numbers from the registry's length, which is exactly how the capture suite grew 20 -> 36 lanes
    # and silently took over ui_soak's 32, --sp-determinism's 31 and the tactical lanes' 33..36 --
    # every one of those a machine-wide single-instance mutex, and every collision a game that dies
    # at boot without a log line. Overflowing the block is now a refusal with the two numbers in it.
    # mp:R7a -- a share_lanes test borrows another's lanes ONLY WHEN THAT OTHER IS ALSO IN THIS RUN.
    # Run alone (e.g. iterating on direct_dial_with_relay_set by itself) it provisions its own lanes,
    # so a subset run does not fail for want of the scenario it usually borrows from. The full-registry
    # demand the lint checks (lane_alloc.suite_demand -> lane_names, which returns [] for a sharer)
    # always has the target present, so a sharer is 0 there -- which is the headroom this buys.
    present = {t["name"] for t in tests}

    def _provision_names(t):
        tg = share_targets(t)
        if tg and all(x in present for x in tg):
            return []
        if tg:  # a target absent from this run -> stand on our own lanes
            # Same shape lane_names() computes for a non-sharer -- calling it directly here (rather
            # than re-deriving it) is what keeps client_shares_lane (mp:GS1(b)) honoured in this
            # fallback path too, without a second place to remember the rule.
            return [n for n in lane_names(dict(t, share_lanes=None))]
        return lane_names(t)

    need = sum(len(_provision_names(t)) for t in tests)
    owner = next((n for n, (b, _c) in lane_alloc.BLOCKS.items() if b == lane_base), None)
    if owner:
        cap = lane_alloc.block(owner)[1]
        if need > cap:
            print(
                "  lane allocation REFUSED: %d test(s) need %d lane(s) and the %r block holds %d "
                "(lanes %d..%d). Widen it in tools/lane_alloc.py -- taking the next block's numbers "
                "is what fork F4H had to undo."
                % (len(tests), need, owner, cap, lane_base + 1, lane_base + cap)
            )
            return None
    plan, lane_no = {}, lane_base
    port_base = LOCAL_PORT_BASE if port_base is None else port_base
    for ti, t in enumerate(tests):
        port = port_base + ti
        shim_port = (LOCAL_SHIM_PORT_BASE + ti) if t.get("shim") else 0
        names = _provision_names(t)
        for i, nm in enumerate(names):
            lane_no += 1
            # host lane (i == 0) keeps the game port; a shim test's CLIENTS dial the shim's port;
            # a client lane that HOSTS (mp:R2b host_lanes) binds a port of its own
            lane_port = shim_port if (shim_port and i > 0) else port
            if i > 0 and i in (t.get("host_lanes") or []):
                lane_port = LOCAL_HOST2_PORT_BASE + ti
            cmd = [
                sys.executable,
                os.path.join(REPO, "tools", "make_lane.py"),
                "--name",
                nm,
                "--lane",
                str(lane_no),
                "--port",
                str(lane_port),
            ]
            # PASS THE OPT-OUT, not just the opt-in. make_lane.py computes
            # `args.headless = not args.visible`, so OMITTING --headless does not produce a visible
            # lane -- it produces a headless one. Until 2026-08-02 this branch only ever appended
            # --headless, which means `test_ui.py --visible` has been provisioning HEADLESS lanes and
            # then applying the visible 1500-frame watchdog budget to a run ticking at thousands of
            # fps: instant TIMEOUT at step 0. Same shape as the HEADLESS-global bug in ui_test.py --
            # a flag that computes the right answer and then fails to transmit it.
            cmd.append("--headless" if headless else "--visible")
            # Run the suite against a byte-for-byte RETAIL exe, with
            # mh.dll force-loaded by the msvfw32 proxy shim instead of an added import.
            if not stock_exe:
                cmd.append("--patched-exe")
            # fork F4B: a scenario may ask for a lane BUILT WITHOUT a satellite DLL, which is how
            # `module_absent` makes mh_net.dll's absence REAL instead of simulating it with a key.
            # Per-test rather than global: every OTHER lane must carry the transport, or the nine MP
            # scenarios would silently be measuring the degraded configuration.
            # mp:D29: a `host:` / `client:` prefix (ui_test's --omit-satellite syntax) scopes the
            # omission to that side -- the MIXED configuration (1) shape omits libmh.dll on the
            # host lane only.
            for spec in t.get("omit_satellite", []) or []:
                role, _, sat = spec.rpartition(":")
                if role == "" or (role == "host") == (i == 0):
                    cmd += ["--omit-satellite", sat]
            # mp:F2b: a scenario may point its OWN lane at a non-polygon install (font_merged's
            # merged mh_ex pack). Additive -- every other test omits this key and keeps the default
            # machine.POLYGON source make_lane.py already falls back to.
            if t.get("lane_src"):
                cmd += ["--src", t["lane_src"]]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print("  lane %s FAILED: %s" % (nm, (r.stderr or r.stdout).strip()[:200]))
                return None
        plan[t["name"]] = (port, shim_port, names)
        if names:
            print(
                "  %-14s port %d%s  lanes: %s"
                % (
                    t["name"],
                    port,
                    ("  shim %d" % shim_port) if shim_port else "",
                    ", ".join(names),
                )
            )
    # mp:R7a -- a share_lanes test that borrowed (provisioned no lanes of its own) aliases its plan
    # entry to the scenario it borrows, so build_argv finds the same port/lanes. The runner schedules
    # the pair serially, so the borrowed lane is never in use by both at once. A sharer running WITHOUT
    # its target present provisioned its own lanes above and keeps them.
    for t in tests:
        targets = share_targets(t)
        if targets and all(x in present for x in targets):
            if len(targets) == 1:
                plan[t["name"]] = plan[targets[0]]
            else:
                # mp:R2b -- several targets: their host lanes first, then their client lanes (see
                # share_targets), on the FIRST target's port (the sharer's own host lane is that
                # target's host lane, and --port is what the runner's readiness probe watches).
                port, shim_port, _n = plan[targets[0]]
                hosts = [plan[x][2][0] for x in targets if plan[x][2]]
                rest = [n for x in targets for n in plan[x][2][1:]]
                # Only as many lanes as the sharer has PEERS: a surplus lane still holds the lender's
                # last run, and a post_check_peers checker reads every lane it is handed (mp:R7b,
                # browser_two_rows picked up relay_browse's client log once that client listed rows).
                own = len(lane_names(dict(t, share_lanes=None)))
                plan[t["name"]] = (port, shim_port, (hosts + rest)[:own])
            print(
                "  %-14s SHARES the lanes of %s (mp:R7a, TL-LANEPOOL headroom)"
                % (t["name"], " + ".join(targets))
            )
    return plan


# ---- mp:R2: a real relay PROCESS beside the rig --------------------------------------------------
#
# A relayed scenario is not a configuration of a LAN scenario: the peers dial a third process, and
# whether that process behaves is half of what the test asserts. So a test carrying `"relay": True`
# gets one started for it, on this box, for the length of the run -- and the acceptance is then
# reproducible on any machine with the repo, rather than depending on a relay somebody left running.
#
# WHY A LOCAL RELAY AND NOT THE DEPLOYED ONE. The deployed relay is a deployment fact (it moves, it
# restarts, it serves other people); a gate that needs it is a gate that goes red for reasons the
# tree cannot see. The VPS relay is where a LIVE run is proven; this is where the REGRESSION is.
# THE PORT IS EPHEMERAL, NOT 7100, AND THAT IS NOT TIDINESS. Two relay scenarios in the suite's
# multi-peer pool start at the same moment; with a fixed port the second relay cannot bind and its
# test SKIPs -- which is what happened on the first run of this pair, and a SKIP is the one verdict
# that looks like nothing went wrong. Binding :0 and reading the port back out of the relay's own
# "listening" line makes the count of concurrent relayed scenarios a non-question.
RELAY_PORT = 0
# `tracing-subscriber` colourises even when stdout is a FILE, so the line reads
# `addr<ESC>[0m<ESC>[2m=<ESC>[0m0.0.0.0:54901` -- a plain `addr=` needle finds nothing and the
# scenario SKIPs with the relay running perfectly well beside it. Strip the escapes, then match.
RELAY_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
RELAY_LISTEN_RE = re.compile(r"addr=(?:[0-9.]+|\[[^\]]+\]):(\d+)")
# mp:R4b -- the `listening` LINE specifically. RELAY_LISTEN_RE above matches any `addr=` field,
# which `peer_registered` lines carry too; that was harmless while only the FIRST match was read
# (the listening line is the first line), and wrong the moment a restart has to find the SECOND.
RELAY_LISTENING_LINE_RE = re.compile(r'"listening"[^\n]*?addr=(?:[0-9.]+|\[[^\]]+\]):(\d+)')


def relay_source_newer_than(exe):
    """The newest source the relay crate is built from, if it is newer than `exe` (else None).

    A prebuilt `mh_relay.exe` is silently STALE once src/relay changes, and a stale relay does not
    fail loudly: it refuses the new op as a counted `bad_op` once a second while the peers keep
    talking to it, so a scenario like relay_punch simply never promotes (dead-ends G241, wave 9:
    the R1c leg re-key merged, the 11:42 relay kept answering bad_op, relay_punch went red with
    nothing in the peers' logs). Walking the crate here costs a few stats per scenario.
    """
    try:
        exe_m = os.path.getmtime(exe)
    except OSError:
        return None
    crate = os.path.join(REPO, "src", "relay")
    newest = None
    for root, _dirs, files in os.walk(os.path.join(crate, "src")):
        for f in files:
            if f.endswith(".rs"):
                q = os.path.join(root, f)
                if os.path.getmtime(q) > exe_m and (
                    newest is None or os.path.getmtime(q) > os.path.getmtime(newest)
                ):
                    newest = q
    for f in ("Cargo.toml", "Cargo.lock"):
        q = os.path.join(crate, f)
        if os.path.isfile(q) and os.path.getmtime(q) > exe_m:
            newest = q
    return newest


def relay_binary(build_if_missing=True):
    """`mh_relay.exe`, building it when it is missing OR older than the crate's sources.

    Returns (path, note) or (None, why). "prebuilt" means the exe is at least as new as every
    source under src/relay; a stale exe is rebuilt (or, with build_if_missing=False, refused with
    the offending source named) -- never handed out silently.
    """
    target = os.environ.get("CARGO_TARGET_DIR") or os.path.join(REPO, "target")
    exe = os.path.join(target, "release", "mh_relay.exe")
    stale = relay_source_newer_than(exe) if os.path.isfile(exe) else None
    if os.path.isfile(exe) and stale is None:
        return exe, "prebuilt"
    if not build_if_missing:
        if stale:
            return None, "%s is STALE: %s is newer -- `cargo build --release -p mh_relay`" % (
                exe,
                stale,
            )
        return None, "no %s" % exe
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lint_rust  # noqa: E402  the one place that knows where cargo is on this machine

    cargo = lint_rust.resolve_cargo(os.environ)
    if not cargo:
        return (
            None,
            "cargo not found (no MH_CARGO, none on PATH) -- `cargo build --release -p mh_relay`",
        )
    p = subprocess.run(
        [cargo, "build", "--release", "-p", "mh_relay"],
        cwd=REPO,
        capture_output=True,
        text=True,
    )
    if p.returncode != 0 or not os.path.isfile(exe):
        return None, "cargo build -p mh_relay failed: %s" % (p.stderr or "").strip()[-400:]
    return exe, ("rebuilt (was older than %s)" % os.path.relpath(stale, REPO)) if stale else "built"


class RelayProc:
    """One `mh_relay` for one scenario. Human-readable log into the run directory, and the counters
    line it prints at shutdown is what says whether the relay saw the traffic the test claims."""

    def __init__(self, port, log_path, restart_request=None, extra_args=None):
        self.port = port
        self.log_path = log_path
        self.proc = None
        self.note = ""
        # mp:R4a -- a row's `relay_args`: extra mh_relay CLI switches, e.g. `--advertise-level 0` to
        # stage a relay that CLAIMS a lower protocol level (or none) than the peers were built with.
        self.extra_args = list(extra_args or [])
        # mp:R4b -- a path that, when it appears, makes the watcher below kill and respawn the relay
        # on the SAME port (same log, appended). The relay_restart scenario's client script touches
        # it through ui_test's --signal-touch once both peers are in the live game; what the
        # scenario then proves is that the pair survives the relay coming back with no state.
        self.restart_request = restart_request
        self.restarts = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._watcher = None

    def _spawn(self, bind_port):
        exe, why = relay_binary()
        if not exe:
            return None, why
        key = ui_test.rig_key_text().splitlines()[0].strip()
        proc = subprocess.Popen(
            [
                exe,
                "--bind",
                "0.0.0.0:%d" % bind_port,
                "--key",
                key,
                "--log",
                "human",
                "--stats-secs",
                "10",
            ]
            + self.extra_args,
            cwd=REPO,
            stdout=self.fh,
            stderr=subprocess.STDOUT,
        )
        # tooling:TL-SUITE-TEARDOWN -- the same kill-on-close job TL-RIG6 gave the shim: this
        # process (test_ui.py) owns the relay's whole lifetime (RelayProc's own docstring), but a
        # supervisor that kills test_ui.py itself without letting __exit__ run (e.g. under --jobs)
        # would otherwise orphan mh_relay.exe the same way an unprotected shim used to be orphaned.
        proc.rig6_job = win_job.assign_kill_on_close(proc.pid)
        return proc, why

    def _await_listening(self, min_lines):
        """Wait for the `listening` line number `min_lines` (1-based) in the log; returns the port."""
        for _ in range(100):
            time.sleep(0.1)
            try:
                with open(self.log_path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                text = ""
            hits = RELAY_LISTENING_LINE_RE.findall(RELAY_ANSI_RE.sub("", text))
            if len(hits) >= min_lines:
                return int(hits[min_lines - 1])
            if self.proc is not None and self.proc.poll() is not None:
                return -1
        return 0

    def _watch(self):
        while not self._stop.wait(0.1):
            if not os.path.isfile(self.restart_request):
                continue
            try:
                os.remove(self.restart_request)
            except OSError:
                pass
            self.restart()

    def restart(self):
        """Kill the relay and bring a fresh one up on the same port -- the redeploy, in miniature."""
        with self._lock:
            if self.proc is None:
                return False
            t0 = time.time()
            try:
                self.proc.terminate()
                self.proc.wait(timeout=10)
            except Exception:
                self.proc.kill()
            win_job.close_job(getattr(self.proc, "rig6_job", None))
            self.fh.write("[test_ui] relay restart requested -- respawning on :%d\n" % self.port)
            self.fh.flush()
            self.restarts += 1
            self.proc, _ = self._spawn(self.port)
            got = self._await_listening(self.restarts + 1)
            secs = time.time() - t0
            self.fh.write("[test_ui] relay back after %.1f s (listening=%s)\n" % (secs, got))
            self.fh.flush()
            return got == self.port

    def __enter__(self):
        # The rig peers hold ui_test's pinned PSK, and the relay authenticates its leg with a key
        # derived from that same value -- a relay on a different key answers nothing and the run
        # reads as "no host in the room", which is the least informative way to be wrong.
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        self.fh = open(self.log_path, "w", encoding="utf-8", errors="replace")
        self.proc, why = self._spawn(self.port)
        if self.proc is None:
            self.note = why
            return self
        # READINESS IS THE RELAY'S OWN "listening" LINE, which is also where the ephemeral port
        # comes back from. A relay that failed to bind must not look like a relay that is simply
        # quiet -- the peers would then fail for a reason nothing in their logs explains.
        got = self._await_listening(1)
        if got > 0:
            self.port = got
            self.note = "listening on :%d (%s)" % (self.port, why)
            if self.restart_request:
                try:
                    os.remove(self.restart_request)
                except OSError:
                    pass
                self._watcher = threading.Thread(target=self._watch, daemon=True)
                self._watcher.start()
            return self
        if got < 0:
            try:
                with open(self.log_path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                text = ""
            self.note = "exited at once -- %s" % (text.strip().splitlines() or ["(no output)"])[-1]
            return self
        self.note = "FAILED to report a listening address within 10 s"
        return self

    def __exit__(self, *_):
        self._stop.set()
        if self._watcher is not None:
            self._watcher.join(timeout=2)
        with self._lock:
            if self.proc is not None:
                try:
                    self.proc.terminate()
                    self.proc.wait(timeout=10)
                except Exception:
                    self.proc.kill()
                win_job.close_job(getattr(self.proc, "rig6_job", None))
                self.fh.close()
        return False

    @property
    def ok(self):
        return self.proc is not None and self.proc.poll() is None and self.port > 0


class RelayShimProc:
    """mp:P14 clause (5): tools/net_shim.py sat between ONE peer and a --relay relay, so that peer's
    LEG to the relay carries a real one-way delay while the other peer's leg does not -- the
    discriminator clause (5) needs (a local relay otherwise makes every peer's hop to it ~1 ms, so
    the adaptive start's `rtt` line cannot be told apart from the true end-to-end figure).

    Deliberately the simplest possible shape next to RelayProc, not a reuse of ui_test.shim_start:
    that function is written around a CHILD ui_test.py process's own args (`.shim`/`.port`/
    `.shim_listen_port`) and its job is shimming the peer-to-peer DIRECT dial, not a leg of a THIRD
    process (the relay) this PARENT process (test_ui.py) already owns the lifetime of via RelayProc.
    Same PARENT-owned, whole-run lifetime as RelayProc, so the two start and stop together.

    net_shim.py has no "listening" line to await (unlike mh_relay.exe, which RelayProc's
    _await_listening reads), so this waits a fixed beat and then trusts the port probe that already
    ran: if the bind failed, net_shim.py would have exited by the time that beat elapses.
    """

    def __init__(self, listen_port, control_port, target, delay_ms, jitter_ms, udp, log_path):
        self.listen_port = listen_port
        self.control_port = control_port
        self.target = target
        self.delay_ms = delay_ms
        self.jitter_ms = jitter_ms
        self.udp = udp
        self.log_path = log_path
        self.proc = None
        self.fh = None
        self.note = ""
        self.job = None  # win_job handle, tooling:TL-SUITE-TEARDOWN

    def __enter__(self):
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        probe_kind = socket.SOCK_DGRAM if self.udp else socket.SOCK_STREAM
        with socket.socket(socket.AF_INET, probe_kind) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                probe.bind(("0.0.0.0", self.listen_port))
            except OSError:
                self.note = (
                    "%s port %d is already in use -- a relay-shim from an earlier run is "
                    "probably still alive; kill it, or pass a different --relay-shim-listen-port"
                    % ("udp" if self.udp else "tcp", self.listen_port)
                )
                return self
        self.fh = open(self.log_path, "w", encoding="utf-8", errors="replace")
        cmd = [
            sys.executable,
            "-u",
            os.path.join(REPO, "tools", "net_shim.py"),
            "--listen",
            "0.0.0.0:%d" % self.listen_port,
            "--target",
            self.target,
            "--delay",
            str(self.delay_ms),
            "--jitter",
            str(self.jitter_ms),
            "--control",
            str(self.control_port),
            "--log",
            self.log_path,
        ]
        if self.udp:
            cmd.append("--udp")
        self.proc = subprocess.Popen(cmd, cwd=REPO, stdout=self.fh, stderr=subprocess.STDOUT)
        # tooling:TL-SUITE-TEARDOWN -- same mechanism as ui_test.py's own shim_start (TL-RIG6): this
        # leg is a second, independent net_shim.py instance owned by test_ui.py itself rather than by
        # a child ui_test.py, but it is exactly as orphanable if THIS process is killed without its
        # __exit__ running.
        self.job = win_job.assign_kill_on_close(self.proc.pid)
        time.sleep(
            0.3
        )  # no "listening" line to await -- the port probe above already proved the bind
        if self.proc.poll() is not None:
            self.note = "exited at once -- see %s" % self.log_path
            return self
        self.note = "listening on :%d -> %s, %.0f ms one-way (rtt %.0f ms)" % (
            self.listen_port,
            self.target,
            self.delay_ms,
            2 * self.delay_ms,
        )
        return self

    def __exit__(self, *_exc):
        if self.proc is not None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=10)
            except Exception:
                self.proc.kill()
            win_job.close_job(self.job)
        if self.fh is not None:
            self.fh.close()
        return False

    @property
    def ok(self):
        return self.proc is not None and self.proc.poll() is None


def relay_addr_for_peers(local):
    """Where the PEERS reach this box. Lanes on this machine use loopback; VM peers need the LAN
    address, which is the one machine_config already calls HOST_IP for exactly this purpose."""
    return "127.0.0.1" if local else machine.HOST_IP


# ---- the flags every entry point shares (tooling:TL-SUITE-SPLIT) -------------------------------


def add_runner_args(ap):
    """Launch flags every UI-suite entry point reads: desktop, exe, VMs, budgets."""
    ap.add_argument(
        "--vms",
        nargs="+",
        default=list(machine.RIG_PEERS),
        help="VM IPs: [0]=host, [1..]=clients (default = machine_config.RIG_PEERS)",
    )
    ap.add_argument(
        "--update-baselines", action="store_true", help="regenerate baselines instead of diffing"
    )
    ap.add_argument(
        "--desktop",
        nargs="?",
        const="mh_rig",
        default=None,
        help="name of the isolated desktop (default: mh_rig). Isolation is ON unless --no-desktop or "
        "--visible.",
    )
    ap.add_argument(
        "--no-desktop",
        action="store_true",
        help="opt OUT of desktop isolation: run every game process on YOUR interactive desktop.",
    )
    ap.add_argument("--headless", action="store_true", help="(default; kept for compatibility)")
    ap.add_argument(
        "--patched-exe",
        action="store_true",
        help="opt OUT: provision lanes and peers around the import-patched mh.focus.exe (the "
        "pre-2026-08-27 mechanism) instead of a byte-for-byte RETAIL mh.exe + the msvfw32 proxy "
        "shim. Lane/peer exes are NAMED mh.focus.exe either way; only the bytes differ.",
    )
    ap.add_argument(
        "--visible",
        action="store_true",
        help="opt OUT of headless: restore the blit and show each game window. --determinism and "
        "--ship-pacing do this for themselves (no blit = no vsync wait = the wrong frame rate to "
        "measure pacing at); ui_test.py --force-headless overrides that.",
    )
    ap.add_argument(
        "--per-test-timeout", type=int, default=280, help="wrapper kill-timeout per test (s)"
    )
    # pass-throughs to ui_test.py
    ap.add_argument("--timeout", type=int, default=200, help="ui_test run wall-clock (s)")


def add_net_args(ap):
    """[net] flags for the runs that start peers: transport, relay, extras."""
    # HEADLESS IS THE DEFAULT (2026-07-28). The blit inside llm_gfx_present_flip is cut, but the frame
    # is still composed in software, so CAPTURES ARE BYTE-IDENTICAL (A/B-verified by SHA-256) and every
    # baseline applies unchanged -- the suite is 12/12 headless on both the VM and local topologies. It
    # costs no wall clock and steals no focus. See the parallel-lane notes.
    # DESKTOP ISOLATION IS THE DEFAULT (2026-08-02) -- see ui_test.py's resolve_desktop. Orthogonal to
    # --headless. Applies to LOCAL launches only, which is every capture test but no determinism run
    # (those are VM-only); see the note in run_ui_test.
    ap.add_argument(
        "--net-extra",
        default="",
        help="';'-separated k=v appended to [net], overriding the same key in place (including the "
        "rig's pinned pacing). Forwarded to --determinism runs AND to every registry scenario.",
    )
    ap.add_argument(
        "--net-extra-client",
        default="",
        help="mp:R7a's CLIENT-only twin of --net-extra, at the top level: ';'-separated k=v that "
        "OVERRIDES --net-extra's same key on the CLIENT peer only (ui_test.py's make_ini rule), "
        "leaving the HOST on --net-extra's value. Forwarded to --determinism runs the same way "
        "--net-extra is. mp:P14 clause (5) is the first --determinism-level caller: it appends its "
        "own `relay=` override here rather than replacing whatever the operator already passed.",
    )
    ap.add_argument(
        "--transport",
        choices=("tcp", "udp"),
        default="udp",
        help="mp:T1 -- which net module EVERY peer of this run binds: udp = mh_net_udp.dll (the "
        "shipping default since 2026-09-20), tcp = mh_net.dll. Folded into --net-extra as "
        "`transport=<t>`, so it reaches "
        "the determinism run and every registry scenario by the one channel ui_test already "
        "overrides [net] keys through. It is a suite-level flag and not a per-test one because the "
        "two transports speak different wire formats: a udp client cannot join a tcp host, so a run "
        "with peers on different transports does not fail, it hangs.",
    )
    ap.add_argument(
        "--relay",
        action="store_true",
        help="mp:R2 -- start an mh_relay process on THIS box for the run and point every peer at "
        "it (`[net] relay=<this box>:<the port it binds>`, folded into --net-extra the same way "
        "--transport is). Applies to --determinism and to any registry scenario the flag is used "
        "with; the two relay_* scenarios start one for themselves and do not need it. Use it to "
        "run the ordinary determinism gate over the relayed path -- the relay's own log lands in "
        "tmp/relay_run.log.",
    )
    ap.add_argument(
        "--relay-shim-delay",
        type=float,
        default=0.0,
        metavar="MS",
        help="mp:P14 clause (5) -- needs --relay. On a rig where the relay is local to both peers "
        "(the usual case here), --relay alone gives every peer the SAME near-zero hop to it, so "
        "the net.adaptive_start log line (tools/data/log_formats.json) reads the relay-LEG rtt "
        "(~1 ms) rather than the end-to-end rtt clause (5) needs measured -- the two are "
        "indistinguishable on this rig without a real delay somewhere on the path. This puts "
        "tools/net_shim.py between ONE peer (the CLIENT) and the relay this box just started, "
        "delaying only that leg by MS ms one-way (rtt = 2x); the HOST still dials the relay "
        "directly. This is a SEPARATE leg from --shim-delay, which shims the peer-to-peer DIRECT "
        "dial (client to host) and has nothing to do with the relay leg -- the two are not wired "
        "together in this tree and compose independently if both are given (neither refuses the "
        "other's knob). The relay-shim's own log is tmp/relay_shim_run.log.",
    )
    ap.add_argument(
        "--relay-shim-jitter",
        type=float,
        default=0.0,
        metavar="MS",
        help="+/- uniform ms around --relay-shim-delay; meaningless without it.",
    )
    ap.add_argument(
        "--relay-shim-listen-port",
        type=int,
        default=6698,
        metavar="PORT",
        help="where the --relay-shim-delay shim listens for the delayed (client) peer's traffic to "
        "the relay. Default 6698 (one below net_shim's own default control port 6699, which this "
        "shim's OWN control channel also avoids by using PORT+1). Override if it collides with "
        "something else already using it.",
    )


def add_steps_arg(ap):
    """--steps (determinism shapes and the soak)."""
    # 8000 => the --min-common floor below is 4000 (steps//2). RAISED FROM 800 on 2026-08-27, once
    # the harness flush bug was fixed and it became visible what the gate had been covering: 800
    # steps is 8 SECONDS of game time, while the run it gates plays for over two minutes. A cascading
    # desync -- the only kind that matters -- does not happen in the first eight seconds. 8000 steps
    # is 80 s of game time and costs the run nothing extra in wall clock, because the peers were
    # already alive that long waiting out the watchdog.
    ap.add_argument(
        "--steps", type=int, default=8000, help="--determinism: in-game steps to compare"
    )


def add_extra_ini_arg(ap):
    """--extra-ini (every mode that merges a fragment into its lanes)."""
    ap.add_argument(
        "--extra-ini",
        action="append",
        default=[],
        metavar="FILE",
        help="--determinism AND --soak: append this ini fragment to BOTH peers' mh_net.ini "
        "(ui_test.py --extra-ini). The reason it exists is the reimpl loop: arming a shadow batch is "
        "an ini gate, so a shadow-arming fragment turned a determinism run into the "
        "differential oracle pass for that batch as well. The capture suite takes its fragment per "
        "test instead. REPEATABLE, and it has to be: this was a scalar until 2026-08-05, which "
        "silently kept only the LAST fragment -- the same defect ui_test.py's own --extra-ini "
        "carried until 2026-08-02, where it armed 2 of 4 shadow sites on a run whose arming set "
        "check_arming_set.py had validated over all 4.",
    )


def add_harness_extra_arg(ap):
    """--harness-extra (determinism shapes and the soak)."""
    ap.add_argument(
        "--harness-extra",
        default="",
        help="--soak: extra ';'-separated [harness] keys appended to the mode's own set.",
    )


def parse_mode_args(ap, argv, lenient=False):
    """Parse a mode script's argv. `lenient` is the forwarding shim's path: an old test_ui.py
    command line may carry flags this mode never read, which the old shared parser accepted."""
    if not lenient:
        return ap.parse_args(argv)
    args, unknown = ap.parse_known_args(argv)
    if unknown:
        print(
            "[deprecated] ignored (this mode never read them): %s" % " ".join(unknown),
            file=sys.stderr,
        )
    return args


def print_desktop_banner(cfg):
    if cfg.desktop:
        print("[rig] isolated desktop: %s -- game windows cannot reach your desktop" % cfg.desktop)


def apply_net_args(ap, args, peers_local):
    """Fold --transport / --relay into args.net_extra and start the run-long relay (+ its shim).
    Returns an exit code on failure, else None. `peers_local`: do the peers dial this box?"""
    # --transport folds into --net-extra rather than travelling beside it, so there is exactly ONE
    # thing the child reads and one thing the printed replay command shows. Appended LAST because
    # ui_test.resolve_transport takes the last `transport=` it sees -- an explicit --net-extra
    # transport= plus a --transport would otherwise resolve by argument order, which is not a rule
    # anyone should have to know.
    if (
        args.transport != "udp"
    ):  # udp is the DLL's default (2026-09-20); only the other one is folded
        args.net_extra = (args.net_extra + ";" if args.net_extra else "") + (
            "transport=%s" % args.transport
        )

    # mp:P14 clause (5) -- checked before the relay is even started: a shim with nothing to shim a
    # leg of is a flag that silently does nothing, which is a worse failure than refusing outright.
    if getattr(args, "relay_shim_delay", 0.0) and not getattr(args, "relay", False):
        ap.error("--relay-shim-delay needs --relay (there is no relay leg to delay without one)")

    # mp:R2 -- `--relay` folds in the same way and for the same reason: one channel the child reads,
    # one thing the replay command shows. The process lives for the whole run (atexit, so it is
    # stopped on every exit path including a KeyboardInterrupt), because the peers of a --determinism
    # run come and go and a per-scenario lifetime would take the relay with the first of them.
    if getattr(args, "relay", False):
        _relay_stack = contextlib.ExitStack()
        atexit.register(_relay_stack.close)
        _rp = _relay_stack.enter_context(
            RelayProc(RELAY_PORT, os.path.join(REPO, "tmp", "relay_run.log"))
        )
        if not _rp.ok:
            print("[relay] could not start: %s" % _rp.note)
            return 2
        # WHICH ADDRESS THE PEERS DIAL, and the one way to get it wrong: `--local` defaults to TRUE
        # (it is how the capture suite runs), but a --determinism run without --det-local puts its
        # peers on the VMs, where 127.0.0.1 is the VM itself. The first relayed determinism run
        # handed both VMs `relay=127.0.0.1` and they sat in a lobby nothing could reach -- reported
        # as a launch timeout, which names neither the relay nor the address.
        _local = peers_local
        _knob = "relay=%s:%d" % (relay_addr_for_peers(_local), _rp.port)
        args.net_extra = (args.net_extra + ";" if args.net_extra else "") + _knob
        print("[relay] %s -- every peer dials %s (log: tmp/relay_run.log)" % (_rp.note, _knob))

        # mp:P14 clause (5) -- a LOCAL relay otherwise gives every peer the same ~1 ms hop to it, so
        # the adaptive start's rtt line cannot be told apart from a genuine end-to-end figure. Put
        # net_shim.py between the CLIENT and the relay ONLY: the host keeps the direct `_knob` above
        # (unshimmed), the client's own relay= is overridden (via --net-extra-client, which OVERRIDES
        # --net-extra's same key on the client -- make_ini's mp:R7a rule) to dial the shim instead,
        # and each side's own `net: udp conn N rtt ... srtt` line then reflects only ITS leg: the
        # host's stays ~1 ms (its leg is unshimmed), the client's carries the injected delay -- and
        # since P14's adaptive-start formula seeds from EACH peer's OWN measured srtt (not a shared
        # one), the client's start line is the one clause (5) reads.
        if getattr(args, "relay_shim_delay", 0.0):
            _rs_listen = args.relay_shim_listen_port
            _rs = _relay_stack.enter_context(
                RelayShimProc(
                    _rs_listen,
                    _rs_listen + 1,
                    "%s:%d" % (relay_addr_for_peers(_local), _rp.port),
                    args.relay_shim_delay,
                    args.relay_shim_jitter,
                    args.transport == "udp",
                    os.path.join(REPO, "tmp", "relay_shim_run.log"),
                )
            )
            if not _rs.ok:
                print("[relay-shim] could not start: %s" % _rs.note)
                return 2
            _rs_knob = "relay=%s:%d" % (relay_addr_for_peers(_local), _rs_listen)
            args.net_extra_client = (
                args.net_extra_client + ";" if args.net_extra_client else ""
            ) + _rs_knob
            print(
                "[relay-shim] %s -- the CLIENT dials %s instead of the relay directly (log: "
                "tmp/relay_shim_run.log); the HOST is unaffected" % (_rs.note, _rs_knob)
            )
    return None
