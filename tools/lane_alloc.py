#!/usr/bin/env python3
"""lane_alloc.py -- the ONE place a lane NUMBER comes from, plus the gate that keeps them disjoint.

WHAT A LANE NUMBER IS, and why a duplicate is not a cosmetic clash. `[uitest] lane=N` makes mh.dll
rewrite the game's single-instance mutex name in place: `wsprintfA(name, "MHMut%02d", lane % 100)`
(net_seams.cpp, over the retail "MHMutex" string -- same length both ways). The mutex is
MACHINE-WIDE with no path component, so two lanes carrying the same number are two game instances
sharing one single-instance guard, and the second one DIES AT BOOT: a clean `_exit()` with no window,
no frame, no WER report and no line in any log except the `; EXIT utils_abort(status=0)` witness.
Most lane runners also derive the lane's game port as `LOCAL_PORT_BASE + lane`, so a duplicate
number is usually a duplicate port as well.

WHY THIS FILE EXISTS (fork F4H, 2026-09-13). The numbers used to be hand-picked constants sitting
next to comments that named the other consumers -- "37: clear of the capture suite (1..~20), the
single-lane helper (31), ui_soak (32) ...". Every one of those comments was true when written. Then
the capture suite grew 20 -> 26 tests, its lane block is DERIVED from the registry (one per peer, so
36 lanes today), and it walked straight into 31, 32 and 33..36. Nothing said so: the collision only
bites while the other lane's game is actually running, which off the gate it usually is not.

What it produced instead was six gate reds read as something else entirely. The suite's lane 32 is
`pause_mp_gate`'s host and also `ui_soak`, which the gate's `ab` unit runs for six minutes
concurrently; lane 31 is `pause_hotkey` and also `--sp-determinism`; 34/35 are `key_repeat` and
`tact_panel` against the tactical-journal lanes. Those are EXACTLY the scenarios that kept failing
under overlap and passing standalone, and the diagnosis filed at F3 close -- "boot starvation, the
box is CPU-saturated so a booting game cannot present within 60 s" -- was refuted by measurement at
F4H: 14 CPU burners make the same scenario take 110 s instead of 30 and it PASSES, while one
concurrent `ui_soak` on an otherwise IDLE box reproduces the death on the first try.

So the rule this file enforces is not "pick a free number", it is "no number is picked by hand at
all". Blocks below; `lane(block, index)` is the only accessor; `--check` recomputes every lane the
tools can emit -- including the capture suite's demand read off the live TESTS registry -- and fails
on an overlap or an overflow. Growing the suite past its block is now a LINT failure with a number
in it, not a gate red in a scenario that has nothing to do with the change.

    python tools/lane_alloc.py --list      # the blocks and what currently occupies them
    python tools/lane_alloc.py --check     # the gate (lint_repo row)
    python tools/lane_alloc.py --selftest  # the gate's own negative cases
"""

import argparse
import json
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# THE HARD CEILING IS 99, and it is the DLL's, not a convention: the rewrite is `%02d` over a
# 7-character buffer and the value is taken `% 100`. Lane 132 would silently become "MHMut32" -- the
# same mutex as lane 32, i.e. the exact bug this file exists to stop, wearing a legal-looking number.
LANE_MAX = 99

# base, capacity -- a block owns base+1 .. base+capacity, and `lane(name, i)` is base+1+i.
# CAPACITY IS THE POINT, not the base: a consumer that outgrows its block fails --check with the
# demand and the capacity printed, which is the signal the old hand-picked constants could not give.
BLOCKS = {
    # The capture suite: one lane per PEER, so its demand is the registry's peer count and it is the
    # only block that grows on its own. 73 is 2 peers of headroom over the 71 the registry needs
    # after the 2026-09-18 wave-9 landings (map_absent/map_conflict/map_have took it 63 -> 71 an
    # hour after 70 was set over 63 for wave 8 -- net_hud, mp_snapshot, relay_punch; it was 60 over 57 a
    # few hours earlier, 56 over 49 a day before, 48 over 36 before that), and the lint row is what
    # says when that has run out -- the whole failure was headroom nobody re-measured. The ten lanes
    # came out of the hand-sized blocks below (soak 10 -> 6, tact 6 -> 4, ui_play 6 -> 4,
    # det_local 4 -> 2), each still at or above its consumer's live demand; lane() refuses a slot
    # past a block's width, so an under-sized block is a loud refusal, never aliasing.
    # 75 on 2026-09-19: d25_buildclick (2 peers) took the last two lanes of headroom; the two came
    # out of `sweep` (4 -> 2, sweep_saves' default --jobs lowered to match), the one remaining
    # block whose consumer is hand-run and whose own usage example already says --jobs 2.
    # 79 later the same day: codepage_adopt + codepage_refused (mp:F3c, 2 peers each) -- ZERO
    # headroom now. Three came out of `soak` (6 -> 3: migration_ab's default --jobs 2 -> 1, one
    # plan entry's record + three arms at a time; --jobs 2 now refuses at lane() rather than
    # aliasing) and one out of `tact` (4 -> 3, exactly --tact-jobs' default). The next registry
    # row has to find its lane in one of the hand-run blocks below or lower a consumer's default.
    # 81 an hour later (mp:R7 relay_browse_local, 2 peers): tact 3 -> 2 (--tact-jobs default 3 ->
    # 2) and sweep 2 -> 1 (sweep_saves --jobs 2 -> 1). EVERY hand-run block is now at its floor;
    # the next row cannot be paid for by shrinking a block -- tooling TL-LANEPOOL (allocate suite
    # lanes per concurrent JOB rather than per registry row) is the way out of the 99 ceiling.
    "suite": (0, 81),
    # test_ui --soak, one lane per --soak-slot. migration_ab runs one plan entry's record + its
    # three verification arms on disjoint slots (--jobs 1 since 2026-09-19); 3 is that demand.
    "soak": (81, 3),
    # The tactical journal lanes (--tact-jobs / the suite's pooled journal tail), one per slot.
    # 2 = --tact-jobs' default since 2026-09-19 (was 3 = all arms at once).
    "tact": (84, 2),
    # ui_play: the recorded-session lanes (--ui-replay / --ui-abc), one per --ui-slot. The gate runs
    # two A/B/C units at disjoint slot bases, so four is the live demand (the headroom went to the
    # capture suite, 2026-09-18).
    "ui_play": (86, 4),
    # --sp-determinism's single lane.
    "sp_det": (90, 1),
    # --det-local's blitted host+client pair (provision_lanes numbers from the base) -- exactly two.
    "det_local": (91, 2),
    # The U28 3-peer barrier's LOCAL third peer.
    "det3": (93, 1),
    # check_inmem_patch_parity's stock-exe lane -- a GATE UNIT, and it was on lane 9, i.e. inside the
    # capture suite's block. Three sub-second launches beside a suite that owns lane 9 is the same
    # aliasing as ui_soak's, just with a much narrower window to be unlucky in.
    "inmem": (94, 1),
    # sweep_saves' worker lanes -- `50 + i`, which was inside the suite's block AND on top of the old
    # `50 + slot` soak numbering. 4 -> 2 -> 1 on 2026-09-19 (the capture suite needed the lanes;
    # the tool's default --jobs is 1 to match, and lane() refuses a slot past the width).
    "sweep": (95, 1),
    # Hand-provisioned investigation lanes (f3a, f4e_bound, ui_probe, ...). Nothing here is allocated
    # automatically -- the block exists so a one-off session has somewhere to take a number FROM
    # instead of guessing into an automatic block. The live-mutex guard (`mutex_in_use`) is what
    # covers the case where somebody guesses anyway.
    # Shrunk 11 -> 3 on 2026-09-18 to give the capture suite room. NOT LOWER: since TL-LANECOLLIDE
    # this is also the per-TREE solo pool (tree_slot), one slot per concurrent worktree agent, and
    # --selftest exhausts exactly three. The wave-9 X2 landing took its two lanes from sp_det and
    # det3 (single-lane consumers) instead.
    "scratch": (96, 3),
}


# Most single-lane runners derive their game port as LOCAL_PORT_BASE + lane, so moving a lane block
# moves a port band with it. These mirror test_ui.py's constants and --check asserts they still do --
# a copy that drifts is the failure this whole file is about. The rig firewall rule that has to
# contain every one of them is tools/provision_rig.py's FW_LO..FW_HI.
PORT_BASE = 6600  # test_ui.LOCAL_PORT_BASE -- the capture suite's band is PORT_BASE + test index
SHIM_PORT_BASE = 6700  # test_ui.LOCAL_SHIM_PORT_BASE
# The blocks whose lane number IS a port offset. (det_local and det3 pin their own ports instead:
# peers of one match must share a port, so those two cannot derive theirs per-lane.)
PORT_DERIVED = ("soak", "tact", "ui_play", "sp_det", "sweep")


def block(name):
    try:
        return BLOCKS[name]
    except KeyError:
        raise KeyError(
            "no lane block %r -- the roster is %s" % (name, ", ".join(sorted(BLOCKS)))
        ) from None


def lane(name, index=0):
    """The `index`-th lane number of block `name`. Raises past the block's capacity."""
    base, cap = block(name)
    if not 0 <= index < cap:
        raise IndexError(
            "lane block %r holds %d lane(s) (%d..%d); asked for index %d. Widen the block in "
            "tools/lane_alloc.py -- do NOT reach into the next one."
            % (name, cap, base + 1, base + cap, index)
        )
    return base + 1 + index


def lanes(name):
    base, cap = block(name)
    return list(range(base + 1, base + cap + 1))


def mutex_name(lane_no):
    """The single-instance mutex mh.dll gives this lane -- net_seams.cpp's own format string."""
    return "MHMut%02d" % (lane_no % 100)


def mutex_in_use(lane_no):
    """True if some live process already holds this lane's single-instance mutex.

    THE POINT IS THE DIAGNOSIS, not the prevention. A duplicate lane number kills the second game
    with a clean `_exit()` inside the retail single-instance guard: no window, no frame, no log line,
    and a runner that can only report `did not present a frame within 60s`. Six gate reds were read
    as CPU starvation on exactly that evidence. Probing the mutex before the launch turns the same
    condition into a sentence naming the lane.

    Windows-only by construction (it is a Win32 mutex); returns False anywhere else rather than
    pretending to know.
    """
    if os.name != "nt":
        return False
    try:
        import ctypes
        from ctypes import wintypes

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.OpenMutexW.restype = wintypes.HANDLE
        k32.OpenMutexW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        SYNCHRONIZE = 0x00100000
        h = k32.OpenMutexW(SYNCHRONIZE, False, mutex_name(lane_no))
        if h:
            k32.CloseHandle(h)
            return True
    except Exception:
        pass  # a guard that cannot run must not be the thing that fails a run
    return False


# ---- TL-LANECOLLIDE (2026-09-18): a per-TREE claim on the "scratch" block -----------------------
#
# THE BUG. `workdir/mh_lanes` (machine.LANE_ROOT) is ONE machine-wide folder, and BLOCKS above hands
# out ONE machine-wide set of numbers -- both built when "a tree" meant the single interactive
# checkout. A git WORKTREE is a second, fully independent process tree on the same box -- its own
# source, build outputs and commits, coordinating with the main checkout only through host-global
# leases outside any tree (the same pattern tools/hostlock.py uses) -- but it imports this exact
# same file and computes the exact same numbers from it. Two worktrees each running their own "solo"
# scenario (the ui-testing skill's dev loop, `python tools/ui_test.py <script>`, or a hand-provisioned
# investigation lane) picked the same lane, i.e. the same bare, machine-wide `MHMutNN` mutex -- and the
# second one died inside retail's own single-instance guard: "lane 1's single-instance mutex MHMut01
# is ALREADY HELD".
#
# THE FIX is not a bigger allocation (the DLL's `%02d` ceiling is 99 and every number above is already
# spoken for) -- it is a small, HOST-GLOBAL, PERSISTENT claim on the block this file already reserves
# for exactly this kind of ad hoc use ("scratch": hand-provisioned investigation lanes; nothing
# allocates there automatically). `tree_slot()` hands each TREE (identified by its own absolute repo
# root) the lowest scratch index no other still-fresh tree has claimed, and remembers the assignment
# in `machine.SHARED_LOCK_DIR` -- the SAME host-global directory `hostlock.py` already uses for its
# leases, so every worktree and the main checkout agree on one location without either owning the
# other's tree. The claim is a REGISTRY entry, not a lock: it is not released when a run ends, only
# when explicitly released or aged out (STALE_TREE_TTL) -- a tree keeps the same solo lane for the
# life of the worktree, which is what "the SAME tree gets the SAME number on every later call" means.
SOLO_BLOCK = "scratch"
STALE_TREE_TTL = 7 * 24 * 3600  # an unrefreshed claim this old is presumed an abandoned worktree


def _tree_tag():
    """A stable identity for THIS git tree (a worktree or the main checkout): its own absolute repo
    root. Every process launched from the same tree resolves to the same tag; a DIFFERENT worktree
    resolves to a different one -- that difference is the whole fix. Overridable for tests."""
    return os.environ.get("MH_LANE_TREE_TAG") or os.path.normcase(os.path.abspath(REPO))


def _slots_root():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import machine_config as machine  # noqa: PLC0415 -- lazy: only the tree-slot allocator needs it

    return os.path.join(machine.SHARED_LOCK_DIR, "lane_trees")


def _slot_dir(block_name):
    return os.path.join(_slots_root(), block_name)


def _slot_path(block_name, index):
    return os.path.join(_slot_dir(block_name), "%d.json" % index)


def _read_slot(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):  # absent, or a torn read mid-write -- both mean "nothing usable"
        return None


def _write_slot_excl(path, obj):
    """Atomically CREATE `path` with `obj`. False if it already exists (another tree got there
    first)."""
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except FileExistsError:
        return False
    try:
        os.write(fd, json.dumps(obj).encode("utf-8"))
    finally:
        os.close(fd)
    return True


def tree_claims(block_name=SOLO_BLOCK):
    """Every fresh claim on `block_name`'s slots: {index: {"tag":..., "at":...}}. A claim older than
    STALE_TREE_TTL with no refresh is dropped (and removed from disk) -- a worktree deleted weeks ago
    must not permanently squat a slot."""
    d = _slot_dir(block_name)
    out = {}
    if not os.path.isdir(d):
        return out
    now = time.time()
    for fname in os.listdir(d):
        if not fname.endswith(".json"):
            continue
        try:
            idx = int(fname[:-5])
        except ValueError:
            continue
        entry = _read_slot(os.path.join(d, fname))
        if entry is None:
            continue
        if now - float(entry.get("at", 0) or 0) > STALE_TREE_TTL:
            try:
                os.remove(os.path.join(d, fname))
            except OSError:
                pass
            continue
        out[idx] = entry
    return out


def tree_slot(block_name=SOLO_BLOCK, tag=None, claim=True):
    """This TREE's own lane number inside `block_name` (default "scratch") -- host-global, persistent,
    and disjoint from every OTHER tree's claim on the same block (TL-LANECOLLIDE, 2026-09-18).

    The SAME tree gets the SAME number back on every call (refreshed so it does not go stale under
    it); a DIFFERENT tree gets a DIFFERENT number, the lowest one nobody else has fresh-claimed. If
    every slot in the block is already claimed by OTHER live trees, returns None -- there is no
    number left to hand out automatically, and the caller must free one (`--release-solo`) or pick a
    number by hand.

    `claim=False` only LOOKS (diagnostics / `--list`); it never allocates or refreshes."""
    tag = tag or _tree_tag()
    base, cap = block(block_name)
    d = _slot_dir(block_name)
    claims = tree_claims(block_name)
    for idx, entry in claims.items():
        if entry.get("tag") == tag:
            if claim:
                with open(_slot_path(block_name, idx), "w", encoding="utf-8") as f:
                    json.dump({"tag": tag, "at": time.time()}, f)
            return base + 1 + idx
    if not claim:
        return None
    os.makedirs(d, exist_ok=True)
    for idx in range(cap):
        if idx in claims:
            continue
        if _write_slot_excl(os.path.join(d, "%d.json" % idx), {"tag": tag, "at": time.time()}):
            return base + 1 + idx
    return None  # every slot in the block is claimed by another live tree


def release_tree_slot(block_name=SOLO_BLOCK, tag=None):
    """Give up THIS tree's claim on `block_name`, if it has one. Not called automatically -- a solo
    lane is meant to persist for the worktree's whole life, the same way its lane FOLDER does."""
    tag = tag or _tree_tag()
    for idx, entry in tree_claims(block_name).items():
        if entry.get("tag") == tag:
            try:
                os.remove(_slot_path(block_name, idx))
            except OSError:
                pass


# ---- the gate ------------------------------------------------------------------------------------


def suite_demand():
    """How many lanes the capture suite's registry currently needs (one per peer).

    Read off the LIVE registry rather than recorded here, because the whole failure was a recorded
    number going stale: every hand-picked constant this file replaced sat beside a comment that said
    "the capture suite (1..~20)" while the suite was at 36."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import test_ui  # noqa: PLC0415 -- lazy: test_ui is heavy and only --check needs it

    return sum(len(test_ui.lane_names(t)) for t in test_ui.TESTS)


def check(demand=None, blocks=None, verbose=True):
    """Returns a list of problem strings; empty means the allocation is sound."""
    blocks = BLOCKS if blocks is None else blocks
    bad = []
    owner = {}
    for name in sorted(blocks, key=lambda n: blocks[n][0]):
        base, cap = blocks[name]
        for n in range(base + 1, base + cap + 1):
            if n > LANE_MAX:
                bad.append(
                    "block %r reaches lane %d, past the DLL's %d ceiling (MHMut%%02d, lane %% 100)"
                    % (name, n, LANE_MAX)
                )
            if n in owner:
                bad.append("lane %d is in BOTH block %r and block %r" % (n, owner[n], name))
            else:
                owner[n] = name
    # The mod-100 aliasing check is not implied by the ceiling check above -- it is what would catch a
    # future block placed at 100+ "because there is room there".
    alias = {}
    for n, who in owner.items():
        m = mutex_name(n)
        if m in alias and alias[m] != n:
            bad.append(
                "lanes %d (%s) and %d (%s) both resolve to mutex %s"
                % (alias[m], owner[alias[m]], n, who, m)
            )
        alias[m] = n
    if demand is None:
        demand = suite_demand()
        # The mirrored port constants, checked against the originals rather than trusted. Only when
        # the demand came from the live registry -- the selftest's synthetic blocks have no test_ui.
        import test_ui  # noqa: PLC0415

        for ours, theirs, what in (
            (PORT_BASE, test_ui.LOCAL_PORT_BASE, "PORT_BASE"),
            (SHIM_PORT_BASE, test_ui.LOCAL_SHIM_PORT_BASE, "SHIM_PORT_BASE"),
        ):
            if ours != theirs:
                bad.append("%s is %d here and %d in test_ui.py" % (what, ours, theirs))
    _base, cap = blocks["suite"]
    # THE PORT BAND MOVES WITH THE LANE BLOCK, so it is checked here rather than left to be
    # discovered. A derived port landing in the capture suite's band is the port-shaped version of
    # the same bug: two concurrent runs on one port, and the second one's bind fails 10013 while the
    # symptom reads as a discovery failure.
    suite_ports = set(range(PORT_BASE, PORT_BASE + max(demand, cap)))
    shim_ports = set(range(SHIM_PORT_BASE, SHIM_PORT_BASE + max(demand, cap)))
    for name in PORT_DERIVED:
        if name not in blocks:
            continue
        for n in range(blocks[name][0] + 1, blocks[name][0] + blocks[name][1] + 1):
            p = PORT_BASE + n
            if p in suite_ports:
                bad.append(
                    "block %r lane %d derives port %d, inside the capture suite's band"
                    % (name, n, p)
                )
            if p in shim_ports:
                bad.append("block %r lane %d derives port %d, inside the shim band" % (name, n, p))
    if demand > cap:
        bad.append(
            "the capture suite needs %d lanes and its block holds %d -- widen the 'suite' block "
            "(and move the blocks above it up) in tools/lane_alloc.py" % (demand, cap)
        )
    if verbose:
        print("lane blocks (ceiling %d):" % LANE_MAX)
        for name in sorted(blocks, key=lambda n: blocks[n][0]):
            base, cap = blocks[name]
            note = "  <- registry demand %d" % demand if name == "suite" else ""
            print("  %-10s %3d..%-3d  (%2d)%s" % (name, base + 1, base + cap, cap, note))
    return bad


def selftest():
    """The gate's own negative cases. A checker nobody has seen fail is a checker nobody has tested."""
    cases = [
        (
            "overlapping blocks",
            {"suite": (0, 10), "soak": (8, 4)},
            5,
            "is in BOTH block",
        ),
        (
            "a block past the 99 ceiling",
            {"suite": (0, 10), "far": (95, 10)},
            5,
            "past the DLL's 99 ceiling",
        ),
        (
            "the suite outgrowing its block",
            {"suite": (0, 10), "soak": (10, 4)},
            11,
            "the capture suite needs 11 lanes",
        ),
    ]
    ok = True
    for label, blocks, demand, needle in cases:
        bad = check(demand=demand, blocks=blocks, verbose=False)
        hit = any(needle in b for b in bad)
        print("  %-34s %s" % (label, "CAUGHT" if hit else "MISSED"))
        if not hit:
            print("     wanted %r, got: %s" % (needle, bad or "(no complaint at all)"))
            ok = False
    # ...and the live allocation must be CLEAN, or the three cases above prove only that the checker
    # complains about everything.
    live = check(verbose=False)
    print("  %-34s %s" % ("the live allocation", "clean" if not live else "DIRTY"))
    for b in live:
        print("     " + b)
    tree_ok = _selftest_tree_slots()
    return 0 if ok and not live and tree_ok else 1


def _selftest_tree_slots():
    """TL-LANECOLLIDE: two (or more) TREES claiming the same block must get DISJOINT numbers, the
    same tree must get the SAME number back, and a released slot must become claimable again.
    Isolated in a temp directory -- this must never read or write the box's REAL tree claims, which
    other worktrees may be relying on for a solo run happening right now."""
    import tempfile

    global _slots_root
    real_slots_root = _slots_root
    tmp = tempfile.mkdtemp()
    _slots_root = lambda: tmp  # noqa: E731 -- selftest-local, restored in `finally`
    ok = True

    def check_(desc, cond):
        nonlocal ok
        print("  %-34s %s" % (desc, "ok" if cond else "XX"))
        if not cond:
            ok = False

    try:
        base, cap = block(SOLO_BLOCK)
        tag_a, tag_b, tag_c, tag_d = (
            "selftest:tree-A",
            "selftest:tree-B",
            "selftest:tree-C",
            "selftest:tree-D",
        )
        slot_a = tree_slot(tag=tag_a)
        slot_b = tree_slot(tag=tag_b)
        check_("two DIFFERENT trees get DIFFERENT solo lanes", slot_a != slot_b)
        check_(
            "both are real numbers inside the block's range",
            slot_a is not None
            and slot_b is not None
            and base + 1 <= slot_a <= base + cap
            and base + 1 <= slot_b <= base + cap,
        )
        check_("mutex names differ too (the actual collision this fixes)", slot_a != slot_b)
        check_(
            "the SAME tree gets the SAME lane back on a later call", tree_slot(tag=tag_a) == slot_a
        )
        # exhaust the block's remaining capacity (cap=3 today: A and B took 2, C takes the last).
        slot_c = tree_slot(tag=tag_c)
        check_(
            "a third tree fills the block's last slot",
            slot_c is not None and slot_c not in (slot_a, slot_b),
        )
        check_(
            "a fourth tree, with none free, is told so rather than handed a colliding number",
            tree_slot(tag=tag_d) is None,
        )
        release_tree_slot(tag=tag_a)
        slot_d = tree_slot(tag=tag_d)
        check_("releasing a claim frees it for the next tree that asks", slot_d == slot_a)
        check_("claim=False never allocates", tree_slot(tag="selftest:tree-E", claim=False) is None)
    finally:
        _slots_root = real_slots_root
        import shutil as _shutil

        _shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true", help="print the blocks and exit")
    ap.add_argument("--check", action="store_true", help="the gate: no overlap, no overflow")
    ap.add_argument("--selftest", action="store_true", help="the gate's negative cases")
    ap.add_argument(
        "--solo",
        action="store_true",
        help="print THIS tree's own persistent solo-scenario lane number (TL-LANECOLLIDE), "
        "claiming one if it doesn't have one yet; e.g. `make_lane.py --name ui_h --lane "
        "$(python tools/lane_alloc.py --solo)`",
    )
    ap.add_argument(
        "--release-solo", action="store_true", help="give up this tree's solo-lane claim"
    )
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.solo:
        n = tree_slot()
        if n is None:
            print(
                "no free solo lane -- every slot in block %r is claimed by another live tree "
                "(`python tools/lane_alloc.py --list` shows who)" % SOLO_BLOCK,
                file=sys.stderr,
            )
            return 1
        print(n)
        return 0
    if args.release_solo:
        release_tree_slot()
        print("released this tree's solo-lane claim (if it had one)")
        return 0
    if args.list:
        check(demand=suite_demand())
        for name in sorted(BLOCKS, key=lambda n: BLOCKS[n][0]):
            for n in lanes(name):
                if mutex_in_use(n):
                    print("  lane %d (%s): mutex %s IS HELD RIGHT NOW" % (n, name, mutex_name(n)))
        my_tag = _tree_tag()
        claims = tree_claims(SOLO_BLOCK)
        if claims:
            base = block(SOLO_BLOCK)[0]
            print("  tree claims on block %r:" % SOLO_BLOCK)
            for idx, entry in sorted(claims.items()):
                mine = " (this tree)" if entry.get("tag") == my_tag else ""
                print("    lane %d -> %s%s" % (base + 1 + idx, entry.get("tag"), mine))
        return 0
    bad = check(verbose=args.check)
    if bad:
        print("\nLANE ALLOCATION IS UNSOUND:")
        for b in bad:
            print("  " + b)
        return 1
    if args.check:
        print("lane allocation ok -- every block disjoint, suite demand within its block")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
