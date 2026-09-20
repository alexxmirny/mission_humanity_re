#!/usr/bin/env python3
"""
make_lane.py -- build a parallel TEST LANE: an install folder that can run a game instance
concurrently with other lanes on the same machine.

    python tools/make_lane.py --dst F:\\games\\mh_lane1 --lane 1 [--port 6511] [--headless]
    python tools/make_lane.py --dst F:\\games\\mh_lane1 --lane 1 --refresh-dll   # just redeploy mh.dll

WHY LANES NEED ANYTHING AT ALL. The game's single-instance guard (WinMain 0x004a0b38) creates a
mutex named "MHMutex" -- a BARE name with no path component, so it is machine-wide and two copies in
two folders still collide. `[uitest] lane=N` makes mh.dll rewrite that string in place to "MHMutNN"
from DllMain, before WinMain reads it. See the parallel-lane notes.

WHAT IS OWNED vs SHARED. mh.dll resolves its ini NEXT TO THE EXE (not from the CWD), which is exactly
what lets each folder carry its own configuration -- so the exe must be a real per-lane file. Bulk
data is symlinked, making a lane ~3.4 MB instead of ~300 MB.

THE PACKS ARE LINKED AS A SET, NEVER CHERRY-PICKED. A .rsr and its .nam are a matched pair (the .nam
is an offset index into the .rsr). A lane built with mh_ex.rsr but no mh_ex.nam cannot find its data
and puts up the INSERT-CD MODAL -- because the no-CD cave points the CD data path at the exe's own
directory, so "cannot find the packs" and "no disc" produce the same dialog (the disc-check RE).
That cost a debugging detour once; hence this script rather than a hand-written copy list.

SYMLINKS need either Developer Mode or an elevated shell on Windows. If creation fails the script
says so and falls back to copying, rather than silently producing a broken lane.
"""

import argparse
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(REPO, "src", "mh_dll", "Release", "mh.dll")


# ---- the machine-wide boot/provision lock --------------------------------------------------------
#
# HOME MOVED HERE FROM ui_test.py (2026-09-10, the gate-parallelisation pass) so that make_lane can
# guard its OWN provisioning with the same lock the launchers use for boots -- ui_test.py imports
# make_lane, so the class could not live there without a cycle. ui_test re-exports it
# (`boot_lock = make_lane.boot_lock`), and every existing `ui_test.boot_lock` caller is unchanged.
#
# Two populations serialise on this ONE file, deliberately: (a) the pack-load window of every game
# BOOT on this machine (lanes share their resource packs by symlink; two instances reading them at
# once lose the race and rsr::TryReadRsrFile raises the Insert-CD modal -- measured 2026-07-28,
# three of four same-second launches), and (b) LANE PROVISIONING, which deletes and rebuilds a lane
# folder around those same shared packs. Until the gate driver existed, provisioning was serial by
# convention ("Provisioning is deliberately NOT parallelised", tact_provision_lanes); with several
# runners in separate PROCESSES a convention cannot serialise anything, so the writer now guards
# itself. Both windows are seconds; the runs they protect are minutes.
BOOT_LOCK = os.path.join(machine.LANE_ROOT, ".boot.lock")
BOOT_LOCK_STALE = 90  # a crashed launcher must not wedge the machine forever


class boot_lock:
    """Serialise the pack-load / lane-provision window across every process on this machine.

    INSTRUMENTED 2026-08-06 (D15). This is the suite's one machine-wide serialisation point, and it
    was invisible: per-lane isolation covers ports, mutexes and setup.dat, so a reader looking for a
    shared resource finds none and concludes the tests are independent. They are not -- all 18 peers
    of a 12-test suite queue HERE, and the queue is unbounded (a waiter only breaks a lock older than
    BOOT_LOCK_STALE). Time spent waiting is charged to the waiting test's OWN --timeout budget, so
    under --jobs the queue converts directly into other tests' timeouts. Both halves are reported
    because they fail differently: a long WAIT means the queue is deep, a long HOLD means one peer
    booted slowly and made everyone else wait.
    """

    def __init__(self, tag):
        self.tag = tag
        self.waited = 0.0

    def __enter__(self):
        t0 = time.time()
        while True:
            try:
                os.makedirs(os.path.dirname(BOOT_LOCK), exist_ok=True)
                fd = os.open(BOOT_LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, ("%d %s" % (os.getpid(), self.tag)).encode())
                os.close(fd)
                self.waited = time.time() - t0
                self.held_at = time.time()
                if self.waited >= 1.0:
                    print(
                        "  [boot] %s waited %.0fs for the machine-wide boot lock"
                        % (os.path.basename(self.tag), self.waited)
                    )
                return self
            except FileExistsError:
                try:  # break a lock left behind by a launcher that died holding it
                    if time.time() - os.path.getmtime(BOOT_LOCK) > BOOT_LOCK_STALE:
                        print("  [boot] breaking a stale boot lock (>%ds old)" % BOOT_LOCK_STALE)
                        os.remove(BOOT_LOCK)
                        continue
                except OSError:
                    pass
                time.sleep(0.5)

    def __exit__(self, *_exc):
        held = time.time() - getattr(self, "held_at", time.time())
        if self.waited >= 1.0 or held >= 10.0:
            print(
                "  [boot] %s held the boot lock %.0fs (waited %.0fs)"
                % (os.path.basename(self.tag), held, self.waited)
            )
        try:
            os.remove(BOOT_LOCK)
        except OSError:
            pass
        return False


# ---- the lane-is-free guard (fork F4H) -----------------------------------------------------------
#
# The boot lock above serialises the pack window. This guards the OTHER shared thing a lane carries,
# and the one that produced six gate reds: its NUMBER. `[uitest] lane=N` renames the game's
# single-instance mutex to "MHMutNN" -- machine-wide, no path component -- so two lanes holding the
# same number are two instances sharing one guard, and the second one dies inside retail's own
# single-instance check: a clean `_exit()`, no window, no frame, no WER report, nothing in any log
# but the `; EXIT utils_abort(status=0)` witness. What the runner can say about that is `did not
# present a frame within 60s`, which is why the condition was diagnosed as CPU starvation twice.
#
# tools/lane_alloc.py makes the ALLOCATED numbers disjoint and gates it. This is the second line, for
# the numbers nothing allocates: hand-provisioned investigation lanes, a leftover game from a killed
# run, two sessions on one box. It costs one OpenMutex and converts a silent death into a sentence.
def lane_conflict(lane_dir):
    """A one-line complaint if this lane's single-instance mutex is already held, else None."""
    ident = read_identity(lane_dir)
    n = ident.get("lane")
    if n is None:
        return None
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lane_alloc  # noqa: PLC0415 -- local, to keep make_lane importable stand-alone

    if not lane_alloc.mutex_in_use(n):
        return None
    return (
        "  [boot] REFUSING to launch %s: lane %d's single-instance mutex %s is ALREADY HELD by "
        "another running game. The launch would be killed inside retail's own single-instance "
        "check -- silently, with no frame and no log line. Find the other lane "
        "(`python tools/lane_alloc.py --list`) or give this one a number from a free block."
        % (os.path.basename(lane_dir), n, lane_alloc.mutex_name(n))
    )


def deploy_dll(dst, src=None):
    """Copy the lane's mh.dll, AND its .pdb when one sits beside it.

    THE PDB IS NOT OPTIONAL FOR EVERY CALLER, which is why this is a function rather than two
    shutil.copy2 lines. tools/coverage.py attributes OpenCppCoverage's output to source lines through
    the PDB; without it the collector runs, exits 0, and reports the DLL as containing no source at
    all -- a clean-looking zero that means "no symbols", not "never executed". The Release deploy has
    never needed it, so it was never copied, and a coverage lane built through this path would have
    inherited that silence.
    """
    src = src or DLL
    if not os.path.isfile(src):
        return False
    shutil.copy2(src, os.path.join(dst, "mh.dll"))
    pdb = os.path.splitext(src)[0] + ".pdb"
    if os.path.isfile(pdb):
        shutil.copy2(pdb, os.path.join(dst, "mh.pdb"))
    return True


# SATELLITE DLLs: siblings mh.dll loads at runtime with LoadLibrary + GetProcAddress (fork F4A's
# mechanism, docs/dll-split.md) -- mh_net.dll today, libmh.dll and mh_harness.dll at F4D/F4E.
#
# THE DEFAULT INVERTED AT F4B, and the inversion is F4A ruling (a) landing. While the only satellite
# was the knob-gated F4A SPIKE, a lane carried one only on `--satellite <name>`: a spike's whole job
# was to be missing, so absent was the sensible default. mh_net.dll is not a spike -- it is the
# multiplayer transport, every MP scenario needs it, and a real satellite is UNCONDITIONAL with
# ABSENCE AS THE CONFIGURATION rather than a knob. So the standard set below is deployed into every
# lane, and the ABSENT arm is now an explicit `--omit-satellite mh_net.dll`, which is exactly what
# the `module_absent` suite scenario asks for.
#
# Leaving the old default would have been the quieter mistake and the worse one: every MP lane would
# have run the degraded configuration while its operator believed it was measuring the shipped one,
# and the scenarios that prove the transport still carries traffic would have been proving nothing.
SATELLITE_SRC = os.path.join(REPO, "src", "mh_dll", "Release")
# THE ONE PLACE THE SATELLITE SET IS WRITTEN. ui_test.SATELLITES and mp_run.SATELLITES now read
# this list rather than repeating it -- F4B shipped three copies and its own hand-off said to derive
# them from one place "if cheap"; at F4D it became cheap, because a second satellite is the point at
# which three hand-kept copies stop being a duplicate and start being a drift (G106). A lane that
# deploys mh_net.dll and not libmh.dll is not a broken lane, it is CONFIGURATION (1) -- which is
# exactly why nobody would notice.
# F4E adds the third and last of them, mh_harness.dll -- the determinism/replay INSTRUMENT. It is
# deployed by default for the same reason the other two are: absence is a configuration, not a knob,
# and the absent arm is the explicit `--omit-satellite mh_harness.dll` the `harness_absent` scenario
# asks for. Note what this one's absence does NOT do: the game is bit-for-bit the game it always
# was, because the harness only observes. That is exactly why a lane that quietly lost it would go
# on passing every pixel test while `--determinism` had nothing to read.
# mp:T1 adds the fourth: mh_net_udp.dll, the UDP transport. It is deployed to EVERY lane even
# though only a `[net] transport=udp` run binds it, and that is the same rule as the other
# three rather than an exception to them -- which module mh.dll binds is CONFIGURATION, read
# out of the lane's own ini at startup, so a lane that carries only one of the two cannot run
# half the configurations the ini can express. Absence would not look like a missing file
# either: mh.dll refuses the run (mh_config_refused.log) and the peer never starts, which on
# a rig reads as a peer that failed to launch.
DEFAULT_SATELLITES = ["mh_net.dll", "mh_net_udp.dll", "libmh.dll", "mh_harness.dll"]

# The proxy shim: a stand-in msvfw32.dll that force-loads mh.dll out of the
# application directory, so a lane can run a byte-for-byte STOCK mh.exe instead of the import-patched
# mh.focus.exe. Opt-in via --stock-exe; see the flag's help for what it swaps.
SHIM = os.path.join(REPO, "src", "mh_dll", "Release", "msvfw32.dll")

# Named lanes live here, one folder per test: <LANE_ROOT>/ui_<test>/. Per-TEST isolation is not
# just tidiness -- it structurally removes the inherited-state trap a shared install has, where a
# test's captures depend on the setup.dat / [video] mode the PREVIOUS test happened to leave behind
# (an in-game UI baseline that passes standalone and fails in the suite). A lane
# is ~3.5 MB, so one per test costs practically nothing.
LANE_ROOT = machine.LANE_ROOT
# A played-in setup.dat, pulled once off a VM and committed: a lane needs the MRU IP list that a
# never-played install lacks. See the copy site below.
SEED_SETUP = os.path.join(REPO, "tools", "uiscripts", "setup.seed.dat")

# Real per-lane files: the exe (so the lane owns its ini), the DLL, the DirectDraw wrapper + its
# config, the DirectInput wrapper, and the persisted settings blob the rig pins.
#
# `dinput.dll` IS dinputto8 (github.com/elishacloud/dinputto8), added 2026-08-25 -- the same shape as
# DDraw.dll being dgVoodoo: a third-party wrapper that sits next to the exe, is fetched rather than
# vendored, and lives in the gitignored artifact store. It converts the game's DirectInput 1-7 calls
# to DirectInput 8, and it is what makes the game PLAYABLE IN A VM: it removes the input lag
# outright, and the residual scale is then handled by `[input] mouse_div` (~51 on a 640-wide
# screen). Measured: the swallowed-input share falls 38% -> 4.9% of event-frames even though the
# divisor went UP, which the shipped path's one-for-one trade-off says is impossible.
# The VM-input notes 9e.
#
# INTERACTIVE RUNS ONLY -- in the sense that only an interactive session NEEDS it. A lane without the
# wrapper still runs every test, which is why its absence has to be REPORTED (see the copy loop)
# rather than left to be discovered by a human whose recording session is unplayable.
#
# WHAT THIS COMMENT USED TO CLAIM, AND WHY IT WAS WRONG (2026-09-08, SPCAMP-FLAKE): it said automated
# headless scenarios "inject into the game's event ring directly and touch neither mouse producer".
# They inject into that ring, but they do not have it to themselves. `llm_input_wndproc_tap`
# (0x004d1194) is the ring's sole producer ENTRY and runs on EVERY window message, so the game keeps
# writing REAL host mouse events into the same ring a journal is replaying into -- measured, with the
# game window on the OPERATOR'S OWN desktop (the replay launcher printed an isolated-desktop banner
# but used a plain subprocess.Popen, which cannot set lpDesktop; fixed 2026-09-08). That is where a year-old "1 run in 3" replay flake lived: moving the physical
# mouse during a replay diverges the simulation at step 16. The harness now suppresses that producer
# for the length of a journal replay (`[harness] replay_isolate_input`, default on), which is what
# makes a replay a replay of the RECORDED input rather than of the recorded input plus whatever the
# machine's mouse was doing. Keep the wrapper here regardless: it is what makes recording possible.
OWNED = [
    "mh.focus.exe",
    "mh.dll",
    "DDraw.dll",
    "dgVoodoo.conf",
    "setup.dat",
    "mh_key.txt",
    "dinput.dll",
]
# Files in OWNED whose absence is worth a word. The rest are either always present (the exe, the
# DLL) or genuinely optional; these two are third-party wrappers a fresh machine has to fetch, and
# both fail SILENTLY -- the lane provisions fine and the game is merely unusable in the way the
# wrapper existed to prevent.
FETCHED = {
    "DDraw.dll": "dgVoodoo -- rendering; a lane without it uses real DirectDraw",
    "dinput.dll": "dinputto8 -- the VM mouse fix; interactive runs only (the VM-input notes 9e)",
}
# Directories shared with the source install.
LINKED_DIRS = ["Res", "Maps"]
# Every pack, both extensions -- see the matched-pair note above.
PACK_EXT = (".rsr", ".nam")


def link_or_copy(src, dst, is_dir):
    try:
        os.symlink(src, dst, target_is_directory=is_dir)
        return "link"
    except OSError as e:
        shutil.copy2(src, dst) if not is_dir else shutil.copytree(src, dst)
        print(
            "    WARN: symlink failed (%s) -- COPIED %s instead"
            % (e.strerror or e, os.path.basename(src))
        )
        return "copy"


def write_ini(dst, lane, port, headless, extra):
    """The lane's own mh_net.ini. Deliberately minimal: this is a lane's identity, not a test config
    -- a runner appends whatever else the scenario needs."""
    lines = ["[net]", "enable=1"]
    if port:
        lines.append("port=%d" % port)
    # `lane` MOVED INTO [uitest] at fork F2G -- it had its own one-key [test] section, which the
    # DLL now refuses outright rather than ignoring (a lane silently back on the stock "MHMutex"
    # is two game instances fighting over one mutex, i.e. a launch that never happens).
    lines += ["", "[uitest]", "lane=%d" % lane, "", "[video]", "size_mode=0"]
    if headless:
        # Cuts the DirectDraw blit only; frames are still composed in software, so captures are
        # byte-identical (proven by A/B). CORRECTNESS RUNS ONLY -- no blit means no vsync wait, which
        # is exactly what makes it wrong for pacing measurement.
        lines.append("no_present=1")
        # no_window rides with headless: the styles are patched AND the offscreen keeper is armed on
        # the present path (which no_present is what frees). Without it the dgVoodoo wrapper maps the
        # window regardless of the style patches.
        lines.append("no_window=1")
    if extra:
        lines += [""] + [ln for ln in extra.split(";") if ln.strip()]
    with open(os.path.join(dst, "mh_net.ini"), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


# The lane's IDENTITY lives in its own file, NOT only in mh_net.ini, because a test runner REWRITES
# mh_net.ini wholesale when it launches a peer (ui_test.local_launch) -- which silently dropped both
# `[uitest] lane=N` and `[video] no_present=1`, putting every lane back on the stock "MHMutex" and back
# on the visible present path. A runner reads this file and re-emits those settings into whatever ini
# it generates, so the lane folder stays authoritative about what the lane IS.
IDENTITY = "lane.json"


def write_identity(dst, lane, port, headless):
    import json

    with open(os.path.join(dst, IDENTITY), "w") as f:
        json.dump({"lane": lane, "port": port, "headless": bool(headless)}, f, indent=1)


def read_identity(lane_dir):
    """{'lane':N,'port':P,'headless':bool} for a lane folder, or {} if it is not a lane."""
    import json

    try:
        with open(os.path.join(lane_dir, IDENTITY)) as f:
            return json.load(f)
    except Exception:
        return {}


def set_dgvoodoo_fps(conf, limit):
    """Set dgVoodoo's `FPSLimit` (0 = unlimited). Returns True if the key was rewritten.

    THE FRAME CAP IS THE WRAPPER'S, NOT THE GAME'S, which is why this lives here rather than as a
    seam. dgVoodoo owns presentation, and its conf ships `FPSLimit = 60` -- so a VISIBLE run is
    pinned to 60 fps no matter what the game or the DLL do, and watching a 24,873-step replay costs
    the wall time of the session that produced it.

    The alternative already in the tree is `[video] no_present=1`, and it is a different thing: it
    cuts the blit, so the run goes as fast as it likes and shows NOTHING. This knob keeps the picture
    and removes only the wait, which is what "watch it, but faster" needs.

    NOT FOR PACING RUNS, and for exactly the reason no_present is not: `mp_pacing_report.py` and the
    adaptive lookahead controller measure a frame rate, and a run with the cap lifted is not the frame
    rate anything ships at. Same rule, same the parallel-lane notes."""
    if not os.path.isfile(conf) or limit is None:
        return False
    out, hit = [], False
    for ln in open(conf, encoding="utf-8", errors="replace"):
        if ln.split("=", 1)[0].strip() == "FPSLimit":
            ln, hit = "FPSLimit                             = %d\n" % limit, True
        out.append(ln)
    if hit:
        with open(conf, "w", encoding="utf-8", newline="") as f:
            f.writelines(out)
    return hit


def tame_dgvoodoo(conf):
    """Make the wrapper's window as unobtrusive as its config allows.

    dgVoodoo owns presentation, so the WINDOW is its business, not the game's -- and it has NO
    hidden/offscreen option (WindowedAttributes offers only borderless / alwaysontop /
    fullscreensize). What it can do, measured 2026-07-28:
      WindowedAttributes  drop `fullscreensize` -> the window stops covering the screen
                          (measured: full-screen -> 512x384 at (0,0))
      CaptureMouse=false  stop it grabbing the physical mouse -- the "it reacts to mouse move" symptom
      SystemHookFlags=    drop the cursor hook
    The window is still MAPPED and still takes focus; hiding it outright needs an inline hook on
    user32!ShowWindow (dgVoodoo's own imports are in ITS import table, so patching the exe's IAT does
    not reach them). See the parallel-lane notes.
    """
    if not os.path.isfile(conf):
        return
    out = []
    for ln in open(conf, encoding="utf-8", errors="replace"):
        k = ln.split("=", 1)[0].strip()
        if k == "WindowedAttributes":
            ln = "WindowedAttributes                   = borderless\n"
        elif k == "CaptureMouse":
            ln = "CaptureMouse                         = false\n"
        elif k == "SystemHookFlags":
            ln = "SystemHookFlags                      = \n"
        out.append(ln)
    with open(conf, "w", encoding="utf-8", newline="") as f:
        f.writelines(out)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--src", default=machine.POLYGON, help="install to build the lane from")
    ap.add_argument("--root", default=LANE_ROOT, help="parent folder for named lanes")
    ap.add_argument(
        "--name", help="lane name under --root (e.g. ui_menu_walk); alternative to --dst"
    )
    ap.add_argument("--dst", help="explicit lane folder (replaced if it exists); overrides --name")
    ap.add_argument(
        "--lane", type=int, required=True, help="lane number 1..99 (0 = stock mutex name)"
    )
    ap.add_argument(
        "--port", type=int, default=0, help="[net] port for this lane (0 = leave default)"
    )
    # Headless is the DEFAULT for a lane (2026-07-28): a lane exists to share a machine with other
    # lanes, and that is exactly what the blit gets in the way of. --headless stays accepted.
    ap.add_argument("--headless", action="store_true", help="(default; kept for compatibility)")
    ap.add_argument(
        "--visible",
        action="store_true",
        help="opt OUT: keep the DirectDraw blit and show this lane's window",
    )
    ap.add_argument(
        "--fps-limit",
        type=int,
        default=None,
        metavar="N",
        help="dgVoodoo FPSLimit for this lane; 0 = UNLIMITED. The wrapper's conf ships 60, so a "
        "--visible run is pinned there however fast the machine is. Use with --visible to watch a "
        "long replay faster than it was played. Not for pacing runs (the cap is what ships).",
    )
    ap.add_argument(
        "--extra-ini", default="", help="';'-separated extra ini lines appended verbatim"
    )
    ap.add_argument(
        "--refresh-dll", action="store_true", help="only redeploy mh.dll into an existing lane"
    )
    ap.add_argument(
        "--dll",
        default="",
        help="deploy THIS mh.dll instead of the Release build (its sibling .pdb rides along). For "
        "tools/coverage.py, which needs the UNOPTIMISED build: a Release /O2+LTCG binary reports "
        "inlined-away bodies as 0%% covered, indistinguishable from never executed.",
    )
    # DEFAULT since 2026-08-27: a lane runs RETAIL bytes. See --patched-exe for the opt-out and
    # I6b for why the default moved.
    ap.add_argument(
        "--stock-exe",
        action="store_true",
        help="(default; kept for compatibility)",
    )
    ap.add_argument(
        "--patched-exe",
        action="store_true",
        help="opt OUT: build the lane around the import-patched mh.focus.exe (the pre-2026-08-27 "
        "mechanism) instead of a stock retail mh.exe + the msvfw32 proxy shim. Either way the "
        "lane's exe is NAMED mh.focus.exe, so every runner and crash_report's image-name "
        "attribution work unchanged; only its BYTES differ.",
    )
    ap.add_argument(
        "--satellite",
        action="append",
        default=[],
        metavar="NAME",
        help="deploy an EXTRA satellite DLL into the lane, beyond the standard set (%s), which is "
        "always deployed. Repeatable; taken from the Release build beside mh.dll."
        % (", ".join(DEFAULT_SATELLITES) or "none"),
    )
    ap.add_argument(
        "--omit-satellite",
        action="append",
        default=[],
        metavar="NAME",
        help="build the lane WITHOUT this satellite -- the absent arm. mh.dll must degrade when a "
        "satellite is missing, and this is how a lane exercises that for real instead of "
        "simulating it with a key; the `module_absent` suite scenario is `--omit-satellite "
        "mh_net.dll`. Repeatable. Refuses a name the lane would not have deployed anyway.",
    )
    args = ap.parse_args()
    args.headless = not args.visible
    # VALIDATED AT PARSE TIME, before a byte of the lane is written: a no-op omission is a lane whose
    # operator believes it is measuring an absence that was never going to be there -- the same class
    # of silent success the unread-key refusals kill elsewhere in this tree -- and refusing it AFTER
    # provisioning would leave a half-built lane behind to be mistaken for a good one.
    for sat in args.omit_satellite:
        if sat not in DEFAULT_SATELLITES and sat not in args.satellite:
            sys.exit(
                "--omit-satellite %s: not a satellite this lane would deploy (default set: %s). "
                "Omitting something that was never going to be copied proves nothing."
                % (sat, ", ".join(DEFAULT_SATELLITES) or "(none)")
            )
    args.stock_exe = not args.patched_exe
    if not args.dst:
        if not args.name:
            sys.exit("give --name (under --root) or --dst")
        args.dst = os.path.join(args.root, args.name)

    if args.refresh_dll:
        if not os.path.isdir(args.dst):
            sys.exit("no such lane: %s" % args.dst)
        if not deploy_dll(args.dst, args.dll or None):
            sys.exit("no such mh.dll: %s" % (args.dll or DLL))
        print("refreshed mh.dll in %s%s" % (args.dst, " (from %s)" % args.dll if args.dll else ""))
        # THE SATELLITES ARE BUILD OUTPUTS TOO, and until fork F4E this path did not refresh them --
        # which was harmless when the only one was a knob-gated spike and is not now. A lane refreshed
        # after a rebuild would keep running yesterday's transport, spine or instrument beside today's
        # mh.dll, and the failure is quiet by construction: it took a live boot printing `Resolved 10
        # of 30 libmh.dll spine symbols` to notice one, because every gate a stale pair still passes.
        #
        # REFRESHED, NEVER CREATED -- ui_test.py's rule, for the same reason: a lane provisioned with
        # `--omit-satellite` is asking for the absent arm, and re-deploying the file it was told to
        # leave out would silently convert that lane into the bound one.
        for sat in DEFAULT_SATELLITES:
            d = os.path.join(args.dst, sat)
            if not os.path.isfile(d):
                continue
            src = os.path.join(SATELLITE_SRC, sat)
            if not os.path.isfile(src):
                sys.exit("no such satellite build output: %s" % src)
            shutil.copy2(src, d)
            print("  refreshed satellite: %s" % sat)
        return 0

    if not os.path.isdir(args.src):
        sys.exit("no such source install: %s" % args.src)
    # The whole rebuild happens under the machine-wide lock (see boot_lock above): a lane being
    # deleted and re-linked while another lane BOOTS off the same shared packs is the same race as
    # two concurrent boots, and with concurrent runners in separate processes only the writer
    # itself can serialise it.
    with boot_lock("provision:%s" % os.path.basename(args.dst)):
        return _provision(args)


def _provision(args):
    if os.path.isdir(args.dst):
        # This deletes the previous run's logs too, and that is SAFE only because ui_test.py
        # archives a dead peer's text logs to tmp/ui_test/postmortem/ the moment it sees the death
        # (postmortem_archive, 2026-09-15) -- three F5H gate crashes left zero evidence to exactly
        # this line before that existed. Blanket retention here would be ~1 GB across the lanes.
        shutil.rmtree(args.dst)
    os.makedirs(args.dst)

    for name in OWNED:
        s = os.path.join(args.src, name)
        if args.stock_exe and name == "mh.focus.exe":
            # The lane's exe is retail's bytes under the runner's expected name. mh.dll arrives via
            # the shim's static import rather than an added import descriptor, and the DLL supplies
            # what the two static code manifests used to (MH_Standalone_Install).
            stock = os.path.join(args.src, "mh.exe")
            if not os.path.isfile(stock):
                sys.exit("--stock-exe needs a stock mh.exe in %s" % args.src)
            if not os.path.isfile(SHIM):
                sys.exit("--stock-exe needs a built shim: %s (build msvfw32_shim)" % SHIM)
            shutil.copy2(stock, os.path.join(args.dst, name))
            shutil.copy2(SHIM, os.path.join(args.dst, "msvfw32.dll"))
            print("  STOCK-EXE lane: retail mh.exe as %s + the msvfw32 shim" % name)
            continue
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(args.dst, name))
        elif name in FETCHED:
            # Not fatal: every automated scenario runs without these. But a silent skip is how a
            # human ends up debugging a mouse that was never wrapped.
            print("  NOTE: %s absent from %s -- %s" % (name, args.src, FETCHED[name]))

    # setup.dat comes from the committed SEED, not from --src. A client peer is pointed at its host by
    # rewriting this file's server IP (setup_dat.set_fields), and that needs the MRU IP list a
    # never-played install does not have: F:\games\mh_en\setup.dat is a 164-byte stub and the pin fails
    # on it. The VM path never hit this because it PULLS each VM's played-in file, edits it and pushes
    # it back -- a lane has nothing to pull from, so the seed is committed instead.
    if os.path.isfile(SEED_SETUP):
        shutil.copy2(SEED_SETUP, os.path.join(args.dst, "setup.dat"))
    # freshly built DLL wins over whatever the source install had
    if args.dll and not os.path.isfile(args.dll):
        sys.exit("--dll: no such file: %s" % args.dll)
    deploy_dll(args.dst, args.dll or None)

    wanted = [s for s in DEFAULT_SATELLITES if s not in args.omit_satellite]
    wanted += [s for s in args.satellite if s not in wanted and s not in args.omit_satellite]
    for sat in wanted:
        s = os.path.join(SATELLITE_SRC, sat)
        if not os.path.isfile(s):
            # FATAL, unlike the FETCHED wrappers above. A lane that wanted a satellite and did not
            # get one would run the ABSENT configuration while its operator believed it was
            # measuring the bound one -- the two arms differ by nothing but this file.
            sys.exit("satellite: no such build output: %s (build the mh.sln first)" % s)
        shutil.copy2(s, os.path.join(args.dst, sat))
        print("  satellite: %s" % sat)
    for sat in args.omit_satellite:
        # Printed, not silent: the absent arm is a CONFIGURATION, and a lane's own log should say
        # which one it is rather than leaving it to be inferred from a missing line.
        print("  satellite OMITTED (the absent arm): %s" % sat)

    packs = 0
    for name in sorted(os.listdir(args.src)):
        s = os.path.join(args.src, name)
        if os.path.isfile(s) and name.lower().endswith(PACK_EXT) and ".bak" not in name.lower():
            link_or_copy(s, os.path.join(args.dst, name), False)
            packs += 1

    for name in LINKED_DIRS:
        s = os.path.join(args.src, name)
        if os.path.isdir(s):
            link_or_copy(s, os.path.join(args.dst, name), True)

    if args.headless:
        tame_dgvoodoo(os.path.join(args.dst, "dgVoodoo.conf"))
    if args.fps_limit is not None:
        conf = os.path.join(args.dst, "dgVoodoo.conf")
        if set_dgvoodoo_fps(conf, args.fps_limit):
            print(
                "  FPSLimit = %s"
                % ("0 (UNLIMITED)" if args.fps_limit == 0 else str(args.fps_limit))
            )
        else:
            # LOUD, because the failure is invisible otherwise: the run just plays at 60 and the
            # caller concludes the flag does nothing rather than that the lane has no wrapper.
            print("  *** --fps-limit IGNORED: no FPSLimit key in %s" % conf)
            print("  *** (a lane without dgVoodoo uses real DirectDraw, which this cannot reach)")

    os.makedirs(os.path.join(args.dst, "logs"), exist_ok=True)
    write_ini(args.dst, args.lane, args.port, args.headless, args.extra_ini)
    write_identity(args.dst, args.lane, args.port, args.headless)

    size = sum(
        os.path.getsize(os.path.join(args.dst, f))
        for f in os.listdir(args.dst)
        if os.path.isfile(os.path.join(args.dst, f))
        and not os.path.islink(os.path.join(args.dst, f))
    )
    print(
        "lane %d -> %s  (%d pack files linked, %.1f MB owned%s)"
        % (args.lane, args.dst, packs, size / 1e6, ", headless" if args.headless else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
