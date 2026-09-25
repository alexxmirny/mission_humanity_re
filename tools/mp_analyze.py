#!/usr/bin/env python3
# tools/mp_analyze.py -- MP run analyzer (Phase 0b of the MP instrumentation work).
#
# Point it at one or more run inputs (a per-run log folder, a bare game dir with the fixed
# log names, or an explicit host+client pair) and it parses every log type, joins host<->client
# by sim step, and prints a one-screen verdict + a machine-readable JSON sidecar.
#
# Log types understood (written by the injected mh.dll):
#   mh_harness.log  -- per-step FNV state hash: "<step> <clock_hex> <combined_hex> <state_hex>"
#                      optional per-region lines (Phase 1a): "R <step> <h0> <h1> ... <hN>"
#                      + a "; per-region breakdown at stop step N:" trailer.
#   mh_lockstep.log -- per-frame lockstep timing, "# wall_ms clock_ms total_ms local_h_ms
#                      committed_ms peer0_ms peer1_ms step_ms stall pcount tx_pkts rx_pkts since_rx_ms"
#   mh_net.log      -- transport arm banner + "net: ..." connection events. Lines carry a real
#                      wall-clock "[HH:MM:SS.mmm]" stamp (mh_log_stamp() / GetLocalTime).
#   mh_launch.log   -- launch-to-state verb events.
#   mh_frametime.log -- per-PRESENT "<qpc_us> <game_mode>" rows (D22). qpc_us is a
#                      QueryPerformanceCounter reading ALREADY CONVERTED TO MICROSECONDS by the
#                      DLL -- it is NOT wall-clock (its epoch is arbitrary, roughly system boot)
#                      and it is NOT raw QPC ticks. This tool divides by 1e3 to get milliseconds,
#                      which is correct for an already-microsecond column. The header line is just
#                      "# qpc_us game_mode" (net_lockstep.cpp, fixed under D22) -- it used to also
#                      print "(qpc_freq=10000000)", the RAW tick rate of the underlying counter,
#                      which is not a divisor for this column: a reader who trusted it and divided
#                      qpc_us by qpc_freq (as if qpc_us were raw ticks) got a timeline ~10x too
#                      short. That field was DROPPED rather than corrected, since the column name
#                      already states the true unit and a second, derived number only invites the
#                      same mistake again. An older log captured before this fix may still show the
#                      stale header text; this parser ignores header lines entirely (see
#                      parse_frametime), so it is unaffected either way.
#                      GAME_MODE 3 covers two different things -- the pre-game MP lobby wait AND a
#                      real in-game "SYNCHRONIZING" overlay stall -- see find_lobby_clip_qpc().
#
# Usage:
#   python tools/mp_analyze.py <path> [<path2> ...]
#       <path> may be a folder (searched for the four log names, incl. per-run subfolders) or a
#       single mh_harness.log / mh_lockstep.log file. Give two harness sources (or two folders,
#       e.g. a host dir and a pulled-VM dir) to get the cross-peer desync diff.
#   Options:
#       --freeze-ms N   frame-gap threshold for the freeze catalog (default 200)
#       --json PATH     write the JSON summary here (default <first-input>/mp_analyze.json)
#       --role-a NAME / --role-b NAME   labels for the two peers (default from folder/role tag)
#
# Read-only; never touches the game or the DB.

import sys, os, re, glob, json, argparse, struct
from collections import Counter

# ---- the region manifest ------------------------------------------------------------------
# ST2M (2026-07-31): the two lists below are GENERATED from tools/data/hash_manifest.json, the
# same file the DLL's HASH_REGIONS[] comes from. They used to be hand-kept in step with
# harness.cpp by tools/lint_region_mirror.py; the lint stays as a belt to these braces.
#
# The adjudication behind the exclusions is NOT generated -- it is the analyzer-side history and
# it is kept by hand, here:
#
# Regions excluded from the state-only desync verdict -- MUST mirror harness.cpp state_excluded().
# order_pending = transient scheduled-command COUNT (lockstep bootstrap); a one-step phase offset of a
# byte-identical event, settled 2026-07-22 via order_log.
#
# p0/p1_ai_econ left this set on 2026-07-27 (D10). They had been excluded as AI-internal bookkeeping
# that "drifts only under host interaction" -- an assumption never tested against a run where the AI
# did anything, because until D10 no determinism run contained an AI. With one seated and mutating its
# store every step, a 1200-step ship-paced run had every region agreeing across peers at every step
# except order_pending.
#
# rng_state was HERE until D3 (2026-07-27) and is not any more. It was excluded as "the strategic PRNG
# channel, proven not to feed game state" -- but the 2026-07-09 measurement behind that was of the FX
# channel, and the region holds three live slots, one of which is the AI's. The DLL now hashes it with
# only the fx slot masked (harness.cpp hash_rng_state), so slots 0 (strategic) and 2 (AI) are part of
# the verdict: they are drawn only from the deterministic sim/AI path, so a divergence there is a real
# desync and an EARLY one.
# O3 (2026-07-28): the order container's three arrays joined the manifest, and they do NOT share one
# sync semantic. MUST MIRROR harness.cpp's state_excluded() -- this set and that function are two
# encodings of one decision, and the DESYNC that first exposed the split had them disagreeing (the
# DLL excluded, this did not, so the analyzer still named regions the verdict had already dropped).
#   order_queue    IN  -- lockstep-synchronized: released at exec_time under the committed horizon,
#                         so every peer must hold the same queue at the same step. This is the region
#                         that refereed O3's asymmetric promotion run.
#   order_staging  OUT -- peer-LOCAL: the orders THIS peer proposed this frame, before broadcast.
#                         Two peers issuing their own orders differ here by construction.
#   order_pending  OUT -- IN-FLIGHT and phase-shifted: a peer's own order enters PENDING when
#   (+ _arr)              schedule() drains staging, the remote's a round-trip later.
# Measured, not assumed: a BASELINE run with both peers on ORIGINAL code went DESYNC at step 60
# (== SYNTH_AT) with order_staging first, which is what established the split.
# L1-P (2026-07-28): the turn engine's three PEER-LOCAL regions, adjudicated by counting per-region
# mismatches across every unpoked step of a both-peers-ORIGINAL run -- not assumed, and not read off a
# snapshot. Six regions went in; three (peer_pending, peer_timing, peer_state) mismatched on ZERO of
# 299 steps and stay IN the verdict. These three are out:
#   peer_horizon 299/299 steps differ. STRUCTURALLY per-peer: a peer's OWN slot is never written (its
#                own request lives in _G_LLM_STRAT_LOCKSTEP_HORIZON), so the host holds
#                [never-written, client's] and the client holds [host's, never-written]. Transposed by
#                construction; they can NEVER match (host peer0=10000/peer1=62980 vs client
#                68320/10000 in the lockstep log).
#   ls_committed 101/299. The min OVER that array, so it inherits the skew -- each peer's VIEW of the
#                barrier, formed from however much it has heard.
#   ls_horizon    89/299. Each peer's OWN advertised horizon, extended by the pump on a FRAME
#                schedule -- and the peers do not share a frame rate (2835 vs 3079 frames over the
#                same 3000 sim steps). Peer-local PACING state, not sim state.
#
# THE METHOD MATTERS, because the first pass at this list got ls_horizon wrong: `breakdown_diff` below
# is the per-region hash at the STOP STEP ONLY and `first_mismatch.regions` is a single step. Reading
# those as an every-step verdict put ls_horizon in the "always agrees" column. Count across ALL common
# steps from the raw `R` lines instead.
#
# Excluding all three does NOT blind the gate to a wrong barrier: game_clock is gated BY committed,
# game_clock is in the verdict, and it matched at every step of the baseline.
# --- BEGIN generated region manifest (tools/gen_state_registry.py) ---
# Source: tools/data/hash_manifest.json. DO NOT EDIT between the markers -- edit the JSON
# and run `python tools/gen_state_registry.py`. REGION_NAMES is POSITIONAL: it maps the
# `R <step> h0 h1 ...` columns to names, so it must stay in the same order as the DLL's
# HASH_REGIONS[] -- which is now the same list, which is the point.
REGION_NAMES = [
    "buildings",  # map::object::building[8][100]
    "productions",  # map::object::production[8][8]
    "mines",  # map::object::mine[8][32]
    "turrets",  # map::object::turret[8][32]
    "unit_storage",  # map::object::unit_storage[8][25]
    "labs",  # map::object::lab[8][25]
    # game::player_data[2] (stride 0x288fc/player) split per-player into 3 sub-slices so a played-vs-
    # idle run localizes the "player_data desync" (docs/mp-instrumentation): ai_gates = AI enable/
    # trigger scalars (reset by llm_strat_init_human_player_data; sim); local = the 65KB block at
    # +0x38..+0x1003c that human-init does NOT touch -> SUSPECTED player-local view/selection/message
    # state (the desync suspect); ai_econ = AI timers/build-plan/groups[32]/bldg_queue/scores (sim).
    "p0_ai_gates",  # player 0: +0x00..+0x38
    "p0_local",  # player 0: +0x38..+0x1003c  (SUSPECT)
    "p0_ai_econ",  # player 0: +0x1003c..+0x288fc
    "p1_ai_gates",  # player 1: +0x00..+0x38
    "p1_local",  # player 1: +0x38..+0x1003c  (SUSPECT)
    "p1_ai_econ",  # player 1: +0x1003c..+0x288fc
    "strat_players",  # llm_strat_player_profile[8] (14848 = 0x3a00)
    "planets",  # cfg::final::struct::Planet[32]
    "prod_slots",  # llm_prod_shuttle_slot[80]
    "planet_status",  # E_PLANET_STATUS[32]
    "projectile_pool",  # llm_strat_projectile[1500]  (stock .bss VA)
    "rng_state",  # _G_LLM_STRAT_RNG_STATE uint[4] -- desync canary. D3
    # (2026-07-27): was 0x40, i.e. 48 bytes of .bss nothing
    # writes. Hashed FOG-STYLE MASKED (hash_rng_state) and
    # INCLUDED in the state-only hash -- see the block below.
    "game_clock",  # _G_LLM_STRAT_GAME_CLOCK double
    "order_pending",  # _G_LLM_STRAT_ORDER_PENDING_COUNT
    # D1 (2026-07-27): the three strategic regions this manifest was MISSING. Found by the cross-check
    # against docs/state-matrix.md that D1's own acceptance test called for (>=30 write-sites, >=4 KB).
    # APPENDED, not inserted: the IDX_* constants below and tools/mp_analyze.py's REGION_NAMES are
    # positional, so anything but an append silently re-labels every existing region.
    "units",  # map::object::unit[8][100] -- 356 write-sites / 1489 reads,
    # the MOST-accessed region in the game and previously unhashed
    "tile_objects",  # map::tile_object_data[256][256] -- map occupancy; 16
    # strategic writers (fog-of-war, walker move, spawn/removal)
    "soldiers",  # _G_LLM_STRAT_SOLDIERS -- 66 write-sites
    # DELIBERATELY NOT HASHED, adjudicated rather than silently omitted (D1):
    #  - `Building` (0x00d9ec80, 451 w) and `Unit` (0x00e4a098, 98 w) are the cfg TYPE tables. 448 of
    #    Building's 451 writes are Construct + llm_strat_bldg_init_defaults, i.e. boot/config; the
    #    stragglers (SwitchToPlanet, CalcBuildingsTechLevel, HandleUpgrade) are deterministic functions
    #    of game state whose effects land in the hashed rosters. Revisit if a tech/upgrade desync is
    #    ever suspected.
    #  - tact_* regions are tactical-mode (single-player) and out of MP scope.
    # D11 (2026-07-27): player_data slots 2..7. `game::player_data` was TYPED [2] in Ghidra, so this
    # manifest covered two players -- but llm_game_land_players_on_planet (0x45534e) loops idx=0..7
    # (`CMP [EBP-0x18],8` @0x45539d) calling llm_strat_init_human_player_data(idx) /
    # llm_strat_spawn_ai_base(idx), each indexing player_data[idx] and raising
    # _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT to idx+1; llm_strat_ai_players_tick then iterates 0..COUNT-1.
    # So the array is [8] (re-typed, EN v150) and ~973 KB of per-player AI/econ store sat outside every
    # region here. Invisible while both peers are humans in slots 0/1 -- and about to stop being
    # invisible, because an AI opponent takes slot >= 2 (D10). Same trap as D1, one slot to the right.
    #
    # NOT inherited: the p0/p1 `ai_econ` state-only EXCLUSION. That exclusion rests on a 2026-07-22
    # measurement of players 0/1 under host interaction; it says nothing about slots 2..7, which are
    # exactly the AI ones. These are all IN the state-only verdict until a measurement says otherwise
    # -- that measurement is D10. In a 2-peer run slots 2..7 are unclaimed and read zero on both peers,
    # so including them cannot make the current gate red for a benign reason.
    "p2_ai_gates",
    "p2_local",
    "p2_ai_econ",
    "p3_ai_gates",
    "p3_local",
    "p3_ai_econ",
    "p4_ai_gates",
    "p4_local",
    "p4_ai_econ",
    "p5_ai_gates",
    "p5_local",
    "p5_ai_econ",
    "p6_ai_gates",
    "p6_local",
    "p6_ai_econ",
    "p7_ai_gates",
    "p7_local",
    "p7_ai_econ",
    # O3 (2026-07-28): THE ORDER CONTAINER'S OWN STATE. Until now the only order region here was
    # `order_pending`, which is the 4-byte COUNT -- the three arrays it counts, 108 KB of the state
    # the container actually owns, were never hashed. That was tolerable while the container was
    # original code. It is not tolerable for O3: promoting mh::orders means the arrays are written by
    # OUR code, and an asymmetric run (one peer original, one peer promoted) can only catch what the
    # hash can see. Without these, a wrong record that the dispatcher happens to treat the same, or a
    # difference in a not-yet-consumed pending entry, is invisible.
    # APPENDED, not inserted -- the IDX_* constants and mp_analyze.py's REGION_NAMES are positional.
    "order_queue",  # llm_strat_order[300], the due-now queue
    "order_queue_count",
    "order_staging",  # llm_strat_order[300], proposed this frame
    "order_staging_count",
    "order_pending_arr",  # llm_strat_order[1000], scheduled -- the ARRAY
    # whose count is region "order_pending" above
    # L1-P (2026-07-28): THE TURN ENGINE'S OWN STATE, and this is O3's step 1 repeated for the other
    # half of the spine. Of everything mh::lockstep reads and writes, this manifest hashed exactly
    # TWO things -- game_clock and the order_pending count. The barrier itself, the per-peer horizon
    # tables and the lockstep status matrix were all invisible, so an asymmetric run could not have
    # caught a wrong barrier at all: the sim clock only diverges once a wrong horizon has already let
    # one peer run past the other, by which point the cause is several steps behind the symptom.
    #
    # APPENDED, not inserted -- the IDX_* constants and mp_analyze.py's REGION_NAMES are positional.
    #
    # EXPECT THE BASELINE TO HAVE AN OPINION ABOUT THE LAST FOUR. `horizon` is what THIS peer has
    # requested and `peer_horizon` is what it has HEARD, so both are in-flight and phase-shifted by
    # the link: at any given step the two peers legitimately hold different values, and only
    # `committed` (the min, which is what actually gates the sim) is required to agree. That is the
    # same shape that made O3's order-container baseline red for a benign reason, which is why these
    # go in BEFORE the promotion and get a both-peers-original baseline run to adjudicate them.
    "ls_horizon",  # _G_LLM_STRAT_LOCKSTEP_HORIZON -- our own request
    "ls_committed",  # _..._COMMITTED_HORIZON -- THE BARRIER; the sim may not
    # pass it, so this is the one that must agree
    "peer_horizon",  # _G_LLM_NET_PEER_HORIZON double[8] -- the barrier's input
    "peer_pending",  # _G_LLM_NET_PEER_HORIZON_PENDING double[8]
    "peer_timing",  # llm_net_lockstep_peer_timing[8], stride 0xc
    "peer_state",  # _G_LLM_NET_LOCKSTEP_PEER_STATE byte[8][8]
    # P0-SPDET (2026-07-29): llm_strat_time_tick's OWN OUTPUTS. Everything above hashes what the sim
    # does with the clock; nothing hashed what time_tick COMPUTES it from. That gap is invisible in
    # MP (the mode-3 block dominates) and fatal in SP: time_tick is C2-class LIVE (mixed), its
    # unconditional lines 44-48 + 177-184 are the real single-player clock, and C8 promotes it. Of
    # the four values those lines write, only game_clock was hashed -- TOTAL_GAME_TIME is PINNED OVER
    # by on_sim_tick's fixed-step replay, and CURRENT/LAST/DELTA + the frame ring were not hashed at
    # all. So a promoted time_tick could get its whole SP block wrong and every existing run stays
    # green. See tracker P0-SPDET.
    # APPENDED, not inserted -- the IDX_* constants and mp_analyze.py's REGION_NAMES are positional.
    # ALL THREE ARE state_excluded() BY DEFAULT and that is deliberate, not an oversight: frame_ring
    # and fps_estimate are FRAME-RATE dependent, and two peers legitimately run at different frame
    # rates (measured 2835 vs 3079 frames over the same 3000 sim steps). Putting them in the
    # state-only verdict would turn the shipped MP gate red for a benign reason. The SP oracle reads
    # them from the per-region `R` lines instead, where a frame-rate-pinned run makes them comparable.
    #
    # SB-HOSTFREE H0 (2026-09-06): SPLIT INTO SIX, AND RENAMED WITH IT. This entry used to be a
    # 48-byte window over all six clock doubles -- but five of them are SEPARATE registry regions,
    # so under a relocated bind they move independently and this slice would have read 8 live bytes
    # followed by 40 bytes of whatever happened to lie past the relocated CURRENT_GAME_TIME. The
    # determinism hash would have stopped observing the clock family entirely while still printing a
    # hash for it: the vacuous-green shape this oracle exists to refuse, in the one instrument
    # SB-HOSTFREE needs in order to trust its own relocation arm. The other five are APPENDED at the
    # end of the table, because the order is a wire contract; gen_state_registry.py now REFUSES any
    # slice that runs past its host region's measured size, so the shape cannot come back by hand.
    # The rename is part of the repair, not decoration -- a name meaning `the six` on a slice
    # covering one is exactly how a desync report comes to name the wrong thing.
    "current_game_time",  # CURRENT_GAME_TIME -- the strategic clock. The other five clock
    # doubles that shared this slice until H0 are appended at the
    # end of the table, each hashed as the region it actually is.
    "frame_ring",  # _G_LLM_STRAT_FRAME_TIME_RING double[20] (time_tick:177-184)
    "fps_estimate",  # _G_LLM_STRAT_FPS_ESTIMATE double
    # D23 (2026-08-29): game::g::Players[8], 8 x 0x34 -- race, colour, the sprite/swatch index,
    # controller_flags, scenario_side_id and the 8-byte relation row. THE ENTIRE per-player identity
    # that llm_lobby_build_players_from_slots produces and that the whole match is built on, and it
    # was in NO hashed region: the oracle was blind to the lobby->game handoff's own output. That is
    # how D18 (a per-peer relation diagonal) survived from N1 on 2026-07-22 to 2026-08-29 with the
    # gate reporting ALL PAIRS IDENTICAL in BOTH arms -- with the bug live and with it fixed.
    # NOT `strat_players`, which is a DIFFERENT table (_G_LLM_STRAT_PLAYERS @0x00cff060, stride
    # 0x740) and is hashed separately above. Confusing the two is the reason this gap read as covered.
    # HASHED WHOLE, WITH NO MASK, and that was MEASURED field-by-field before it was added, not
    # assumed -- all 52 bytes of the stride traced to a writer. Every one is derived from host-synced
    # lobby-slot data (_G_LLM_LOBBY_SLOTS, broadcast in the host's 0x1cd snapshot and copied wholesale
    # by the client) or from compile-time constants. Nothing reads a local socket, a local-only UI
    # selection, or a pointer. The ONE historically peer-local write --
    # build_players_finish's Players[PlayerSide].relation[k] = 2, keyed on the LOCAL peer -- is
    # idempotent only BECAUSE D18 is fixed: it now writes the same constant the synced slot row
    # already carries. This region is hashable because of that fix, so the ordering is not incidental.
    # APPENDED, not inserted -- the IDX_* constants and mp_analyze.py's REGION_NAMES are positional.
    # PROVEN RED 2026-08-29 before it was trusted: region_poke_at=1;region_poke_min=55;region_poke_off=16
    # on the host only -> DESYNC at step 1, and after the peer-local exclusions the state-only diverging
    # set is `players` ALONE; the same command without the poke is ALL PAIRS IDENTICAL over 400 steps.
    # The poke goes to +0x10 (name[32]), which the sim reads nowhere -- byte 0 is race, and poking live
    # state would prove the region is hashed while changing behaviour, contaminating the localization.
    "players",  # game::g::Players[8] -- llm_strat_player_desc[8], 416 = 8 * 0x34
    # SB-HOSTFREE H0 (2026-09-06): the five doubles `time_globals` used to swallow, now hashed as
    # the separate registry regions they have always been. Same bytes, same order within the family,
    # same `excluded` verdict -- what changes is that each one follows ITS OWN region when a host
    # binds it somewhere else, which is the property statetest arm M asserts by poking each of the
    # five in its relocated home. They stay state_excluded() for the reason the family always was:
    # wall-clock derived, and two MP peers legitimately run at different frame rates. The SP oracle
    # compares them explicitly from the per-region R columns (mp_analyze.SP_TIME_REGIONS), which is
    # where a decomposed clock actually pays.
    # APPENDED, not inserted -- the IDX_* constants and mp_analyze.py's REGION_NAMES are positional.
    "last_game_time",  # LAST_GAME_TIME -- previous frame's clock value
    "total_game_time",  # TOTAL_GAME_TIME -- cumulative elapsed session time
    "sim_step_interval",  # _G_LLM_STRAT_SIM_STEP_INTERVAL -- seconds per sim step
    "game_time_delta",  # GAME_TIME_DELTA -- this frame's advance
    "game_speed",  # game_speed -- the speed multiplier the delta is scaled by
    "player_resources",  # player_resources int[8][10] -- the stock every affordability check reads (D25)
    # (2026-09-19): was never hashed; the first real internet match diverged here alongside
    # resource_spent and the verdict could not see it. Appended (the contract), state-only.
]
# Mirrors the DLL's hash_region::excluded flag. NOTE the frame-rate-dependent regions (the
# six clock doubles, frame_ring, fps_estimate) ARE in this set: the harness has always
# excluded them, but it resolved them by NAME at init rather than through an IDX_* constant,
# so the old mirror lint -- which scanned for IDX_ tokens -- could not see them and reported
# the two sides as agreeing while they did not.
STATE_EXCLUDED = {
    "order_pending",
    "order_staging",
    "order_staging_count",
    "order_pending_arr",
    "ls_horizon",
    "ls_committed",
    "peer_horizon",
    "current_game_time",
    "frame_ring",
    "fps_estimate",
    "last_game_time",
    "total_game_time",
    "sim_step_interval",
    "game_time_delta",
    "game_speed",
}
# --- END generated region manifest ---


# ---- the state fold, and the manifest fingerprint (tooling TL-GATE-D25FX, 2026-09-20) ----------
#
# `state` is not an opaque number: libmh/state/world_snapshot.cpp lockstep_hash() folds the per-region
# hashes -- the same values the `R` line prints -- with FNV-1a 64 over each hash's 8 little-endian
# bytes, in manifest order, skipping the excluded regions. So a run's `state` column is REDERIVABLE
# from its `R` columns, and a variant that drops one more region is derivable the same way. That is
# what lets the A/B/C verdict excuse a NAMED region on one leg without a DLL change and without
# touching what the other legs compare: `state_fold(cols)` must reproduce `state` (the self-check
# every consumer runs before trusting the variant), `state_fold(cols, drop={"p0_ai_econ"})` is the
# leg's `state~excused`. Measured before it was relied on: 0 of 18317 + 30042 steps disagreed on
# either arm of both A/B/C fixtures.
FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
FNV32_OFFSET = 2166136261
FNV32_PRIME = 16777619


def state_fold(cols, drop=(), names=None, excluded=None):
    """The `state` hash re-derived from one `R` line's columns, minus the regions in `drop`.

    `cols` are the hex strings of one R line (positional, REGION_NAMES order). Returns the 16-hex
    upper-case string the harness prints, or None if the line is short of the manifest."""
    names = REGION_NAMES if names is None else names
    excluded = STATE_EXCLUDED if excluded is None else excluded
    if len(cols) < len(names):
        return None
    h = FNV64_OFFSET
    for nm, c in zip(names, cols):
        if nm in excluded or nm in drop:
            continue
        v = int(c, 16)
        for i in range(8):
            h ^= (v >> (8 * i)) & 0xFF
            h = (h * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return "%016X" % h


REGIONS_GEN_H = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src",
    "mh_dll",
    "mh",
    "addr",
    "mh_regions.gen.h",
)


def hash_manifest_rows(header_text=None):
    """[(name, rid, offset, len, excluded)] from the generated HASH_REGIONS[] table -- what the DLL
    was compiled from, which is what its fingerprint describes."""
    text = header_text if header_text is not None else open(REGIONS_GEN_H, encoding="utf-8").read()
    m = re.search(r"enum region_id : uint16_t \{(.*?)\};", text, re.S)
    rid = {name: int(val) for name, val in re.findall(r"(RID_\w+)\s*=\s*(\d+)", m.group(1))}
    m = re.search(r"inline constexpr hash_region HASH_REGIONS\[\] = \{(.*?)\n\};", text, re.S)
    rows = re.findall(
        r'\{"(\w+)",\s*(RID_\w+),\s*(\d+)u,\s*MH_STOCK_BASE\(0x[0-9a-fA-F]+u\),\s*(\d+)u,\s*(true|false)\}',
        m.group(1),
    )
    return [(n, rid[r], int(o), int(ln), ex == "true") for n, r, o, ln, ex in rows]


# mp:D29 -- the harness's configuration line (mh_harness/config1.h format_config_line):
#   ; [harness] configuration (1): spine ABSENT, registry=mh.dll, rebased=0 owned=0,
#     uncovered=none, manifest fp=XXXXXXXX
HARNESS_CONFIG_RE = re.compile(
    r"^; \[harness\] configuration \(([12])\):.*\bmanifest fp=([0-9A-Fa-f]{8})\b"
)


def hash_manifest_fingerprint(header_text=None):
    """mh::state::world::hash_manifest_fingerprint(), in Python: FNV-1a 32 over (count, then per
    region rid/offset/len/excluded as u32 LE + the name's bytes). It is what every world blob and
    (since TL-GATE-D25FX) every UI-REC oracle is stamped with, so a fixture captured under another
    manifest is refused as STALE before its numbers are compared. Verified against the DLL: the
    pre-D25 header gives BD36D78E, the value the 2026-09-11/12 fixtures carry."""
    rows = hash_manifest_rows(header_text)
    h = FNV32_OFFSET

    def mix(v):
        nonlocal h
        for i in range(4):
            h ^= (v >> (8 * i)) & 0xFF
            h = (h * FNV32_PRIME) & 0xFFFFFFFF

    mix(len(rows))
    for name, rid, off, ln, ex in rows:
        mix(rid)
        mix(off)
        mix(ln)
        mix(1 if ex else 0)
        for c in name.encode("ascii"):
            h ^= c
            h = (h * FNV32_PRIME) & 0xFFFFFFFF
    return "%08X" % h


LOCKSTEP_COLS = [
    "wall_ms",
    "clock_ms",
    "total_ms",
    "local_h_ms",
    "committed_ms",
    "peer0_ms",
    "peer1_ms",
    "step_ms",
    "stall",
    "pcount",
    "tx_pkts",
    "rx_pkts",
    "since_rx_ms",
]

# The columns AFTER since_rx_ms, parsed only when a row actually carries them. Kept separate from the
# required prefix above on purpose: `parse_lockstep` drops any row shorter than LOCKSTEP_COLS, so
# folding these in would make every pre-2026 log parse as zero rows rather than as a log without the
# newer columns. Optional means older logs keep working and newer ones get read in full.
LOCKSTEP_COLS_OPT = [
    "sess",
    "game",
    "flags",  # hex -- parsed separately below
    "grace_ms",
    "syncwait",
    "countdn",
    "p54bc",
    "sync_ms",
    "sim_burst",
    "icon_calls",
    "icon_shown",
    # mp:T3 (2026-09-18). The first columns in this stream that can be a NON-NUMERIC token: the
    # literal `n/a`, when the bound transport cannot measure a round trip (the TCP module has no
    # channel B) or when no sample has landed yet. `parse_lockstep` keeps them OUT of the row rather
    # than storing a 0 -- see the note there.
    "srtt0_ms",
    "srtt1_ms",
    "rttvar0_ms",
    "rttvar1_ms",
    "ipdv0_ms",
    "ipdv1_ms",
    "loss0_pm",
    "loss1_pm",
    "late_p50_ms",
    "late_tail95_ms",
    "late_tail99_ms",
    "late_n",
    "late_peer",
]


# ---------------------------------------------------------------- parsers


def parse_harness(path):
    """Return the LAST run segment: {steps: {step: {clock,combined,state}},
    regions: {step: [h0..hN]}, breakdown: {name: hash}, banner: str, path}."""
    seg = {
        "steps": {},
        "regions": {},
        "breakdown": {},
        "banner": None,
        "path": path,
        "arming": "",
        # mp:X1b. Two more region-hash streams, keyed by the step they describe. SNAPCAP is the
        # SENDER's reading at the capture step; SNAPIMP is the RECEIVER's re-derived reading right
        # after the import, tagged with the step the BLOB carried (not the receiver's own step), so
        # the two are keyed alike and a comparison needs no alignment argument.
        "snapcap": {},
        "snapimp": {},
        "snapshot_lines": [],
        # mp:X3. The SUB-DOMAIN trail. `domains` maps a domain name to the ordered list of region
        # names the DLL put in it (read from the run's own `; [subdomain]` lines -- see below for
        # why this is read rather than held), and `rd` maps step -> {domain: hash}.
        "domains": {},
        "rd": {},
        # `hold` is the step a `; SIM HOLD` line named, or None. A peer that HELD its sim stopped
        # producing step rows on purpose, and a verdict that read that as a peer dying or lagging
        # would be describing the instrument rather than the run.
        "hold_step": None,
        # mp:D29. The harness's configuration line: '1' (no libmh.dll, spine-free) or '2', and the
        # hash-manifest fingerprint it hashed under. analyse() refuses a pair whose fp differ.
        "config": None,
        "manifest_fp": None,
    }
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("; ==== mh replay harness armed"):
                # new run -> reset (keep only the current/last run)
                seg = {
                    "steps": {},
                    "regions": {},
                    "breakdown": {},
                    "banner": line,
                    "path": path,
                    "arming": "",
                    "snapcap": {},
                    "snapimp": {},
                    "snapshot_lines": [],
                    "domains": {},
                    "rd": {},
                    "hold_step": None,
                    "config": None,
                    "manifest_fp": None,
                }
                continue
            if line.startswith("; all_ai="):
                # The SECOND arming line (all_ai / observer / gameover_step / gameover_stop). Kept
                # because `gameover_step` is what tells an end that was ASKED FOR from one that
                # happened to us -- DET-FLAKE's environmental guard is scoped to the latter, and
                # before this the flag was invisible to it (the banner reset above drops every other
                # comment line).
                seg["arming"] = line
                continue
            if line.startswith("; [harness] configuration ("):
                # mp:D29. Written once, after the step-1 census, in BOTH configurations -- so a
                # MIXED pair (configuration (1) vs mode=original) states on each side which registry
                # it hashed through and under which manifest.
                m = HARNESS_CONFIG_RE.match(line)
                if m:
                    seg["config"] = m.group(1)
                    seg["manifest_fp"] = m.group(2).upper()
                continue
            if line.startswith("; SIM HOLD "):
                # mp:X3. The peer FROZE its sim here, deliberately. Recorded as a step so the
                # rollup can say so instead of reporting a peer that stopped stepping as a peer
                # that stopped.
                m = re.match(r"; SIM HOLD step=(\d+)", line)
                if m:
                    seg["hold_step"] = int(m.group(1))
                continue
            if line.startswith("; [subdomain] "):
                # THE PARTITION IS READ FROM THE RUN, NOT HELD HERE, and that is the point rather
                # than laziness. The DLL owns the region -> domain classification (harness.cpp
                # rd_classify); a second copy in this file is a mirror that goes stale the day a
                # region is appended -- exactly the failure tools/lint_region_mirror.py exists to
                # watch for on the REGION list itself. So a run whose DLL classified a region
                # differently says so in its own log, and this reader simply believes it.
                #
                #   `; [subdomain] units = units unit_storage soldiers projectile_pool`
                #   a trailing * marks a region EXCLUDED from the state-only verdict
                body = line[len("; [subdomain] ") :]
                if "=" in body:
                    name, _, members = body.partition("=")
                    name = name.strip()
                    mem = [w for w in members.split() if w and w != "(none)"]
                    if name:
                        seg["domains"][name] = mem
                continue
            if line.startswith("; SNAPSHOT "):
                # mp:X1b's structural lines: SEND / IMPORT / IMPORT HASHES / RX. Kept verbatim --
                # they are evidence a reader quotes, not fields anything computes on, and a parser
                # that split them would be a second place to keep the format.
                seg["snapshot_lines"].append(line)
                continue
            if line.startswith(";"):
                m = re.match(r";\s+(\w[\w ]*?)\s+([0-9A-Fa-f]{16})\s*$", line)
                if m and m.group(1).strip() in REGION_NAMES:
                    seg["breakdown"][m.group(1).strip()] = m.group(2).upper()
                continue
            parts = line.split()
            if parts and parts[0] in ("SNAPCAP", "SNAPIMP"):
                # Same shape and same manifest order as the R line, deliberately: one column layout,
                # one reader, and a three-way comparison (R / SNAPCAP / SNAPIMP) that needs no
                # translation between them.
                try:
                    seg["SNAPCAP" == parts[0] and "snapcap" or "snapimp"][int(parts[1])] = [
                        x.upper() for x in parts[2:]
                    ]
                except (ValueError, IndexError):
                    pass
                continue
            if parts and parts[0] == "RD":
                # mp:X3: RD <step> units=<16hex> buildings=<16hex> ... -- one fold per sub-domain
                # over its UNEXCLUDED members only, from the same per[] the R row is built from. A
                # domain with no unexcluded member prints `-` and is stored as None rather than
                # dropped, so "this build has no unexcluded member here" and "this run did not emit
                # the column" stay different answers.
                try:
                    step = int(parts[1])
                except (ValueError, IndexError):
                    continue
                row = {}
                for tok in parts[2:]:
                    k, _, v = tok.partition("=")
                    if k:
                        row[k] = None if v == "-" else v.upper()
                seg["rd"][step] = row
                continue
            if parts and parts[0] == "R":
                # per-region hash line: R <step> <h0> <h1> ...
                try:
                    step = int(parts[1])
                    seg["regions"][step] = [p.upper() for p in parts[2:]]
                except (ValueError, IndexError):
                    pass
                continue
            if len(parts) >= 4:
                try:
                    step = int(parts[0])
                except ValueError:
                    continue
                seg["steps"][step] = {
                    "clock": parts[1].upper(),
                    "combined": parts[2].upper(),
                    "state": parts[3].upper(),
                }
    return seg


def parse_lockstep(path):
    rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < len(LOCKSTEP_COLS):
                continue
            try:
                vals = [int(x) for x in parts[: len(LOCKSTEP_COLS)]]
            except ValueError:
                continue
            row = dict(zip(LOCKSTEP_COLS, vals))
            # `flags` is written as 0x%02x, so int(x, 0) rather than int(x); everything else is
            # decimal. A row that stops partway through the optional tail keeps what it had.
            #
            # mp:T3: an UNPARSEABLE optional column is SKIPPED, not a reason to stop reading the row.
            # It used to break out of the loop, which was right while every optional column was an
            # integer -- the first non-integer meant a torn write. T3's latency columns emit the
            # literal `n/a` for "this transport cannot measure that", and over TCP eight of them do
            # on every row; breaking there would have silently dropped the five lateness columns that
            # follow, which ARE measured over TCP. The absent key is the signal: a caller that finds
            # no `srtt0_ms` in the row knows it was not measured, where a 0 would have claimed a
            # perfect link.
            for name, raw in zip(LOCKSTEP_COLS_OPT, parts[len(LOCKSTEP_COLS) :]):
                try:
                    row[name] = int(raw, 0)
                except ValueError:
                    continue
            rows.append(row)
    return rows


def parse_frametime(path):
    """mh_frametime.log: '<qpc_us> <game_mode>' per presented frame. Returns list of (us, mode).

    us is QueryPerformanceCounter already converted to MICROSECONDS by the DLL -- an arbitrary,
    non-wall-clock epoch. See the module docstring's mh_frametime.log entry for the qpc_freq trap
    (the header's printed tick rate is not a divisor for this column)."""
    rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) >= 2:
                try:
                    rows.append((int(p[0]), int(p[1])))
                except ValueError:
                    pass
    return rows


_WALL_TS_RE = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")


def _wall_seconds(line):
    """Wall-clock seconds-of-day for one '[HH:MM:SS.mmm] ...' mh_net.log line, or None."""
    m = _WALL_TS_RE.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


def _first_wall_seconds(net_lines, marker):
    """Wall-clock seconds of the first mh_net.log line containing `marker`, or None."""
    for ln in net_lines or []:
        if marker in ln:
            return _wall_seconds(ln)
    return None


# A real in-game "SYNCHRONIZING" overlay stall is a handful of frames; the MP lobby wait (also
# GAME_MODE 3) is thousands. Only trust the heuristic fallback below above this length, so a
# normal short overlay blip is never misread as "there must have been a lobby".
LOBBY_HEURISTIC_MIN_FRAMES = 500


def find_lobby_clip_qpc(rows, net_lines):
    """Where the pre-game MP lobby wait ends and the real game session starts, as a qpc_us cut
    point: frames before it are the lobby, frames at/after it are real in-game GAME_MODE-3
    (sync-overlay) stalls. Returns (clip_qpc_us, method, detail) or (None, None, None) if no
    lobby boundary can be derived (single-player runs, or a run with no MP lines at all -- in
    which case every mode-3 frame is reported as sync-overlay, matching the pre-D22 behaviour).

    Primary method ("net_log"), DERIVED from mh_net.log, per D22: mh_frametime.log's qpc_us is a
    QueryPerformanceCounter reading on an arbitrary, non-wall-clock epoch (see parse_frametime),
    while mh_net.log's "[HH:MM:SS.mmm]" stamps ARE wall-clock (mh_log_stamp(), GetLocalTime). To
    turn a wall-clock moment into a qpc_us cut point we need one (wall, qpc) pair to anchor the
    two clocks together. The "; present hook armed" line is printed at DLL-init time, which is
    essentially the same moment mh_frametime.log is opened and its first row captured -- so (that
    line's wall time, rows[0]'s qpc_us) is the anchor. qpc_us then advances at 1e6 per wall-clock
    second, so the "; session_begin_multi ENTER" line's wall time (the moment the MP session
    actually starts, which mh_net.log already carries) converts to a qpc_us clip point at the same
    rate. This is an approximation, not an exact join -- the frametime log's true first row can
    lag the armed-line print by up to about one frame -- but cross-checked against the fallback
    heuristic below on the run that motivated this fix, it landed within ~1s of the heuristic's
    answer over a 21.6-minute lobby (~0.08% error), which is why it is the primary method.

    Fallback method ("heuristic"), used only when mh_net.log lacks one of those two lines: take
    the longest contiguous run of mode-3 frames in the frametime data itself and, ASSUMING it is
    the lobby (an assumption, not a proof -- stated here and echoed in the printed output), clip
    right after it. Requires the run to be at least LOBBY_HEURISTIC_MIN_FRAMES long, AND requires
    at least one real "net: ..." transport event line in mh_net.log -- the MP transport seams are
    armed on EVERY run (the "MP bootstrap armed"/"MP transport seams armed" banners print
    unconditionally), so their presence alone proves nothing; an actual "net:" event line proves
    real MP traffic happened. Without that gate this fired on single-player/tactical captures
    whose long GAME_MODE-3 stretch is a level-load screen, not an MP lobby (observed on a
    single-player tactical-determinism capture during D22's own verification).
    """
    if not rows:
        return None, None, None

    armed_wall = _first_wall_seconds(net_lines, "present hook armed")
    sbm_wall = _first_wall_seconds(net_lines, "session_begin_multi ENTER")
    if armed_wall is not None and sbm_wall is not None:
        delta_wall = sbm_wall - armed_wall
        if delta_wall >= 0:
            clip_qpc = rows[0][0] + delta_wall * 1e6
            detail = (
                "net_log: 'session_begin_multi ENTER' wall-clock, anchored to qpc_us via "
                "'present hook armed' (delta=%.3fs)" % delta_wall
            )
            return clip_qpc, "net_log", detail

    # Fallback: longest contiguous mode-3 run. best_end_qpc is the qpc_us of the frame AFTER the
    # run (not the run's own last frame) so the "< clip_qpc" test used below counts the WHOLE run
    # as lobby, not run-length-minus-one; if the run reaches end-of-file, clip just past its last
    # frame instead.
    best_len, best_end_qpc = 0, None
    run_mode, run_start = rows[0][1], 0
    for i in list(range(1, len(rows))) + [len(rows)]:
        if i == len(rows) or rows[i][1] != run_mode:
            length = i - run_start
            if run_mode == 3 and length > best_len:
                best_len = length
                best_end_qpc = rows[i][0] if i < len(rows) else rows[i - 1][0] + 1
            if i < len(rows):
                run_mode, run_start = rows[i][1], i
    had_net_activity = any(_nots(ln).startswith("net:") for ln in net_lines or [])
    if best_len >= LOBBY_HEURISTIC_MIN_FRAMES and had_net_activity:
        detail = (
            "heuristic: longest contiguous mode-3 run = %d frames (>= %d threshold) with real MP "
            "'net:' activity present, ASSUMED to be the lobby wait -- no net.log session anchor "
            "found" % (best_len, LOBBY_HEURISTIC_MIN_FRAMES)
        )
        return best_end_qpc, "heuristic", detail
    return None, None, None


def analyze_frametime(rows, freeze_ms, net_lines=None):
    """Present-to-present deltas + distribution, overall and for the strategic-gameplay (mode 2) subset
    (what the player feels). Freezes = gaps >= threshold.

    mode_frames is clipped to the game session (D22): GAME_MODE 3 before the derived lobby-clip
    point is reported as "lobby", not lumped into "sync-overlay" with the real in-game stalls."""
    if len(rows) < 2:
        return None

    def dist(deltas_us):
        if not deltas_us:
            return None
        ms = sorted(
            d / 1000.0 for d in deltas_us
        )  # us -> ms: /1e3 is correct, us is ALREADY microseconds (see parse_frametime)
        n = len(ms)
        pct = lambda p: ms[min(n - 1, int(p / 100.0 * n))]
        buckets = [8, 12, 17, 25, 33, 50, 100, 200, 500, 1000, 10**9]
        hist = Counter()
        for m in ms:
            for b in buckets:
                if m <= b:
                    hist[b] += 1
                    break
        freezes = sum(1 for m in ms if m >= freeze_ms)
        return {
            "frames": n + 1,
            "mean_ms": round(sum(ms) / n, 2),
            "p50_ms": round(pct(50), 1),
            "p95_ms": round(pct(95), 1),
            "p99_ms": round(pct(99), 1),
            "max_ms": round(ms[-1], 1),
            "fps_p50": round(1000.0 / pct(50), 1) if pct(50) else 0,
            "freezes": freezes,
            "hist": {str(b): hist[b] for b in buckets if hist[b]},
        }

    all_d = [b[0] - a[0] for a, b in zip(rows, rows[1:]) if b[0] >= a[0]]
    # mode-2 (strategic) consecutive deltas only
    strat_d = [
        b[0] - a[0] for a, b in zip(rows, rows[1:]) if a[1] == 2 and b[1] == 2 and b[0] >= a[0]
    ]

    clip_qpc, clip_method, clip_detail = find_lobby_clip_qpc(rows, net_lines)
    mode_counts = Counter()
    # ONLY mode 3 is reclassified, and the reason is measured rather than assumed. A run also has a
    # handful of GAME_MODE-2 frames BEFORE the clip (37 host / 33 peer1 on the 2026-08-28 logs), and
    # the tempting move is to sweep those into the lobby bucket too so the strategic count matches a
    # hand-derived target. They are NOT lobby frames: on both peers they are ONE CONTIGUOUS RUN at the
    # very end of the pre-clip span (host indices 62083..62119 of 62120; peer1 5945..5977 of 5978),
    # i.e. the first in-game strategic frames, rendered in the ~0.6 s before `session_begin_multi
    # ENTER` reaches the net log. So the clip is marginally LATE, and those frames belong to the game.
    # Counting them as strategic is correct; the residual is reported below rather than hidden.
    strat_pre_clip = 0
    for qpc, m in rows:
        pre = clip_qpc is not None and qpc < clip_qpc
        if m == 3 and pre:
            mode_counts["lobby"] += 1
        else:
            if m == 2 and pre:
                strat_pre_clip += 1
            mode_counts[GAME_MODE_NAMES.get(m, "mode%d" % m)] += 1

    result = {
        "overall": dist(all_d),
        "strategic": dist(strat_d),
        "mode_frames": dict(mode_counts),
    }
    if clip_qpc is not None:
        result["lobby_clip"] = {
            "qpc_us": clip_qpc,
            "method": clip_method,
            "detail": clip_detail,
            "lobby_seconds": round((clip_qpc - rows[0][0]) / 1e6, 1),
            # How far the anchor lags the real mode flip, in frames. Surfaced so the boundary is
            # visible: a reader comparing this run's strategic count against another tool's gets to
            # see the size of the disagreement instead of rediscovering it.
            "strategic_before_clip": strat_pre_clip,
        }
    return result


def parse_events(path):
    out = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.strip():
                out.append(line)
    return out


# D21: the peer's own IN-BAND desync verdict, read back out of mh_net.log.
#
# THIS ANALYZER STAYS THE AUTHORITY on whether a run desynced -- it joins both peers' full per-step
# hash streams offline and can localize to a step and a region, which no in-match check can. What the
# in-band detector adds is a DIFFERENT question, and one this tool could never answer: did the game
# ITSELF notice, at the time, while the players were still in it? A run where the analyzer says DESYNC
# and the peers said nothing is a hole in the detector; a run where a peer reported one is the feature
# working. Both are worth printing, and neither is inferred from the other.
_DESYNC_RE = re.compile(r";\s*\[desync\]\s*(.*)$")

# SES0: the match_id -- the UUIDv7 the host mints at lobby creation and every peer writes once per
# distinct id. THE LINE IS A CONTRACT, built in exactly one place
# (mh_net_proto::session_match_id_log_line) and read in exactly one place, here:
#
#     ; [session] match_id=<32 lowercase hex>
#
# It is what makes two machines' logs one match. Until it existed the only way to pair a host's logs
# with a client's was the folder timestamp and the operator's memory -- which is fine on a rig where
# one person starts both peers seconds apart, and useless for the reports a player will send.
_MATCH_ID_RE = re.compile(r";\s*\[session\]\s*match_id=([0-9a-f]{32})\b")


def parse_match_ids(lines):
    """The match_ids this peer logged, in order, de-duplicated.

    A LIST, not a scalar, and deliberately: a peer that hosts, leaves and re-creates a lobby without
    restarting logs a SECOND id (the host_recreate scenario), and a run directory is still per
    PROCESS until SES1 lands. So "the" match_id of a peer is the LAST one -- the match whose steps
    the harness log actually holds -- while the earlier ones are the lobbies it passed through.
    """
    ids = []
    for raw in lines or []:
        m = _MATCH_ID_RE.search(raw)
        if m and (not ids or ids[-1] != m.group(1)):
            ids.append(m.group(1))
    return ids


def parse_desync(lines):
    """mh_net.log lines -> {armed, notices, mismatches, last_status, first, cost_probe, inert}."""
    d = {
        "armed": None,
        "cost_probe": None,
        "notices": 0,
        "mismatch_lines": [],
        "first_mismatch": None,
        "last_status": None,
        "inert": None,
        "manifest_mismatch": None,
    }
    for raw in lines:
        m = _DESYNC_RE.search(raw)
        if not m:
            continue
        body = m.group(1).strip()
        if body.startswith("ARMED:"):
            d["armed"] = body
        elif body.startswith("COST PROBE:"):
            d["cost_probe"] = body
        elif body.startswith("NOTICE SHOWN"):
            d["notices"] += 1
        elif body.startswith("*** DESYNC"):
            d["mismatch_lines"].append(body)
            if d["first_mismatch"] is None:
                d["first_mismatch"] = body
        elif body.startswith("STATUS:"):
            d["last_status"] = body
        elif body.startswith("INERT"):
            d["inert"] = body
        elif body.startswith("DISABLED:"):
            d["manifest_mismatch"] = body
    return d


def desync_counts(status):
    """The integer fields out of a `STATUS: k=v ...` line, or {} if it cannot be read."""
    if not status:
        return {}
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", status)}


# ---------------------------------------------------------------- analysis


def analyze_lockstep(rows, freeze_ms):
    if not rows:
        return None
    walls = [r["wall_ms"] for r in rows]
    gaps = [b - a for a, b in zip(walls, walls[1:]) if b >= a]
    freezes = []
    for i in range(1, len(rows)):
        g = rows[i]["wall_ms"] - rows[i - 1]["wall_ms"]
        if g >= freeze_ms:
            freezes.append(
                {
                    "idx": i,
                    "wall_ms": rows[i]["wall_ms"],
                    "gap_ms": g,
                    "clock_ms": rows[i]["clock_ms"],
                    "stall": rows[i]["stall"],
                }
            )
    # frame-gap histogram (bucketed)
    buckets = [0, 20, 33, 50, 100, 200, 500, 1000, 2000, 10**9]
    hist = Counter()
    for g in gaps:
        for b in buckets:
            if g <= b:
                hist[b] += 1
                break

    def pct(p):
        if not gaps:
            return 0
        s = sorted(gaps)
        return s[min(len(s) - 1, int(p / 100.0 * len(s)))]

    # horizon-staleness: a peer_ms column frozen while committed advances (log-freeze insight).
    stale = {}
    for col in ("peer0_ms", "peer1_ms", "committed_ms"):
        vals = [r[col] for r in rows]
        last_change = 0
        max_frozen = 0
        started = False  # ignore the leading startup constant (the 10s initial horizon before
        for i in range(1, len(rows)):  # horizon exchange begins) -- only flag MID-run stalls
            if vals[i] != vals[i - 1]:
                last_change = i
                started = True
            elif started:
                max_frozen = max(max_frozen, rows[i]["wall_ms"] - rows[last_change]["wall_ms"])
        stale[col] = {"final": vals[-1], "max_frozen_ms": max_frozen}
    span = walls[-1] - walls[0] if len(walls) > 1 else 0
    # MP U20 -- the de-sync corner icon, as a rate rather than an impression. icon_calls/icon_shown are
    # CUMULATIVE columns, so the rate is the end-to-end delta over the frames spanned; they differ only
    # when [net] desync_icon_gate is on, and that difference IS the gate's value made visible.
    #
    # BOTH IMPLEMENTATIONS FEED THEM (2026-08-30): the original's call site via the counting thunk, our
    # promoted body via set_icon_counters. Before that only the thunk wrote them, so on a promoted run
    # -- i.e. every shipped run -- a 0 here meant "nobody counted". It no longer does.
    icon = None
    if "icon_calls" in rows[0] and "icon_shown" in rows[-1]:
        dc = rows[-1]["icon_calls"] - rows[0]["icon_calls"]
        ds = rows[-1]["icon_shown"] - rows[0]["icon_shown"]
        n = len(rows)
        icon = {
            "calls": dc,
            "shown": ds,
            "suppressed": dc - ds,
            "calls_per_1k_frames": round(1000.0 * dc / n, 1),
            "shown_per_1k_frames": round(1000.0 * ds / n, 1),
            # `syncwait` is what the icon rides on -- an at-horizon frame. Reporting it alongside is
            # what separates "the gate suppressed the icons" from "there were no icons to suppress".
            "at_horizon_frames": sum(1 for r in rows if r.get("syncwait", 0) > 0),
        }
    return {
        "icon": icon,
        "frames": len(rows),
        "span_ms": span,
        "gap_mean_ms": round(sum(gaps) / len(gaps), 1) if gaps else 0,
        "gap_p50_ms": pct(50),
        "gap_p95_ms": pct(95),
        "gap_p99_ms": pct(99),
        "gap_max_ms": max(gaps) if gaps else 0,
        "hist": {str(b): hist[b] for b in buckets if hist[b]},
        "freeze_count": len(freezes),
        "freeze_total_ms": sum(f["gap_ms"] for f in freezes),
        "freezes": freezes[:40],
        "final_row": rows[-1],
        "horizon": stale,
        "tx_final": rows[-1]["tx_pkts"],
        "rx_final": rows[-1]["rx_pkts"],
        "max_since_rx_ms": max(r["since_rx_ms"] for r in rows),
    }


def diff_peers(a, b, name_a, name_b, truncate_at=None):
    """Cross-peer state-hash diff. Returns first-diverging step (+ region if per-region present).

    mp:D26. `truncate_at`, when given, is a step number: only steps STRICTLY BEFORE it are
    compared. This is the user's 2026-09-23 ruling -- an EXPECTED match end (a deliberate
    gameover_step arm, or a real game-over reached by every peer) legitimately makes the two
    peers' state hashes diverge from the elimination step on (presence_lost clears the loser's
    ALIVE bit and downgrades SESSION on one side only), and that divergence is not the sim
    disagreeing -- it is the two peers correctly recording that the match ended differently for
    each of them. Comparing past it would either manufacture a DESYNC out of a correct run, or
    (worse) hide a REAL divergence that happens to start before the boundary, which is why
    everything before `truncate_at` is still compared at full strictness."""
    sa, sb = a["steps"], b["steps"]
    common = sorted(set(sa) & set(sb))
    truncated_overlap = None
    if truncate_at is not None:
        kept = [s for s in common if s < truncate_at]
        if len(kept) != len(common):
            truncated_overlap = len(common)
        common = kept
    if not common:
        return {
            "overlap": 0,
            "note": "no overlapping steps between the two peers",
            "truncated_at": truncate_at if truncated_overlap is not None else None,
        }
    mismatches = [s for s in common if sa[s]["state"] != sb[s]["state"]]
    res = {
        "overlap": len(common),
        "first_step_a": common[0],
        "last_step_a": common[-1],
        "mismatch_count": len(mismatches),
        "first_mismatch": None,
        "combined_mismatch_count": sum(1 for s in common if sa[s]["combined"] != sb[s]["combined"]),
        "truncated_at": truncate_at if truncated_overlap is not None else None,
        "untruncated_overlap": truncated_overlap,
    }
    if not mismatches:
        if truncated_overlap is not None:
            res["verdict"] = (
                "IDENTICAL state-hash across all %d overlapping steps BEFORE the truncation "
                "boundary (step %d; %d step(s) after it were not compared -- mp:D26)"
                % (len(common), truncate_at, truncated_overlap - len(common))
            )
        else:
            res["verdict"] = "IDENTICAL state-hash across all %d overlapping steps" % len(common)
        return res
    s0 = mismatches[0]
    res["first_mismatch"] = {"step": s0, "state_a": sa[s0]["state"], "state_b": sb[s0]["state"]}
    # localize the diverging region if per-region hashes are present on both peers at s0
    ra, rb = a["regions"].get(s0), b["regions"].get(s0)
    if ra and rb and len(ra) == len(rb):
        diverged = []
        for i, (x, y) in enumerate(zip(ra, rb)):
            if x != y:
                nm = REGION_NAMES[i] if i < len(REGION_NAMES) else "region[%d]" % i
                diverged.append(nm)
        res["first_mismatch"]["regions"] = diverged
        res["first_mismatch"]["state_only_regions"] = [
            r for r in diverged if r not in STATE_EXCLUDED
        ]
    res["verdict"] = "DESYNC: state-hash first differs at step %d" % s0
    return res


# ---- mp:X1b: DID THE TRANSFERRED WORLD LAND? ----------------------------------------------------
#
# A THIRD COMPARISON, and it is a different question from both of the others. diff_peers asks whether
# two peers STAYED equal while simulating; sp_compare asks whether two runs of one peer agree. This
# asks whether one peer's memory, after importing a blob that crossed a socket, is the SAME WORLD the
# other peer captured -- a single instant on each side, not a trajectory.
#
# THE KEYING IS WHAT MAKES IT AN EQUALITY AND NOT A HOPE. The receiver stamps its SNAPIMP line with
# the step the BLOB carried, not with the step it happened to be on when the transfer finished (which
# is 30-60 s of wall clock later and is nobody's choice). So a SNAPCAP at step N and a SNAPIMP at step
# N are two readings of one world, and the comparison is column against column with no alignment
# argument to get wrong.
#
# EVERY REGION COUNTS HERE, INCLUDING THE EXCLUDED ONES. diff_peers rests on the state-only hash
# because a live pair legitimately differs on peer-local horizons and on frame-rate-dependent clocks.
# None of that applies to a blob: the capture carried those regions too, so if `rng_state` or
# `p0_ai_econ` disagrees after an import, the import did not land. STATE_EXCLUDED is reported for
# context and is not a licence.
def snapshot_compare(cap_peer, imp_peer, name_cap, name_imp):
    """Match each SNAPIMP line on the importing peer to the SNAPCAP of the same step on the other.

    Returns None when neither peer emitted a snapshot line (the overwhelmingly common case: nothing
    in the suite arms the verbs), so callers can skip the section entirely."""
    cap = cap_peer.get("snapcap") or {}
    imp = imp_peer.get("snapimp") or {}
    if not cap and not imp:
        return None
    res = {
        "capturer": name_cap,
        "importer": name_imp,
        "capture_steps": sorted(cap),
        "import_steps": sorted(imp),
        "pairs": [],
    }
    if not cap or not imp:
        # ONE-SIDED IS A REPORTABLE OUTCOME, not an absent section. A run that captured and never
        # imported is the transfer failing, and it must not read as "nothing was armed".
        res["verdict"] = (
            "INCOMPLETE: %s captured %d snapshot(s) and %s imported %d -- the transfer did not "
            "complete" % (name_cap, len(cap), name_imp, len(imp))
        )
        return res
    for step in sorted(imp):
        row = {"step": step, "differ": [], "columns": len(imp[step])}
        if step not in cap:
            row["verdict"] = "NO CAPTURE at step %d on %s" % (step, name_cap)
            res["pairs"].append(row)
            continue
        a, b = cap[step], imp[step]
        if len(a) != len(b):
            row["verdict"] = (
                "COLUMN COUNT DIFFERS (%d vs %d) -- the two peers hash different "
                "manifests" % (len(a), len(b))
            )
            res["pairs"].append(row)
            continue
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y:
                row["differ"].append(REGION_NAMES[i] if i < len(REGION_NAMES) else "region[%d]" % i)
        row["verdict"] = (
            "IDENTICAL across all %d regions" % len(a)
            if not row["differ"]
            else "DIFFER in %d of %d regions" % (len(row["differ"]), len(a))
        )
        # rng_state is called out BY NAME because it is the clause the item is written around and
        # because it is the region the MP verdict used to exclude (D3) -- a reader must not have to
        # scan a 41-name list to find whether the RNG crossed.
        row["rng_state_ok"] = "rng_state" not in row["differ"]
        res["pairs"].append(row)
    clean = [r for r in res["pairs"] if not r.get("differ") and "IDENTICAL" in r.get("verdict", "")]
    res["verdict"] = (
        "SNAPSHOT IMPORT VERIFIED: %d of %d imported world(s) match the capturing peer's "
        "per-region hashes exactly" % (len(clean), len(res["pairs"]))
        if len(clean) == len(res["pairs"]) and res["pairs"]
        else "SNAPSHOT IMPORT MISMATCH: %d of %d imported world(s) differ from the capture"
        % (len(res["pairs"]) - len(clean), len(res["pairs"]))
    )
    return res


# ---- mp:X3: THE SUB-DOMAIN TRAIL ----------------------------------------------------------------
#
# WHAT IT ADDS OVER THE R ROW, because "another per-region comparison" would be worth nothing. The R
# row is 61 columns and costs over a kilobyte per emission, so a run samples it every
# `region_hash_step` steps -- fine in SPACE, coarse in TIME. The RD row is nine folds, cheap enough
# to write on EVERY step -- coarse in space, fine in time. So the pair answers a question neither
# answers alone: the RD trail gives the FIRST STEP a subsystem moved, and the R row then names the
# region inside that subsystem at the next sampled step. Without the trail, "the desync began
# somewhere in the 200 steps before the sample" is the best a single run can do.
#
# THE PARTITION COMES FROM THE LOG (`; [subdomain]` lines, emitted once by the DLL). A copy here
# would be a mirror of a table in harness.cpp with nothing holding the two level.
def subdomain_compare(a, b, name_a, name_b):
    """First diverging step per sub-domain, across two peers' RD trails.

    Returns None when neither peer emitted an RD row -- the overwhelmingly common case, since
    `[harness] domain_hash_step` is off by default -- so callers can skip the section entirely."""
    ra, rb = a.get("rd") or {}, b.get("rd") or {}
    if not ra and not rb:
        return None
    res = {
        "peer_a": name_a,
        "peer_b": name_b,
        "rows_a": len(ra),
        "rows_b": len(rb),
        "domains": {},
        # The two peers' partitions, reported rather than merged: two builds that classify a region
        # differently produce comparable-looking hashes that are not comparable, and that has to be
        # visible instead of silently averaged.
        "partition_a": a.get("domains") or {},
        "partition_b": b.get("domains") or {},
    }
    if not ra or not rb:
        res["verdict"] = (
            "INCOMPLETE: %s emitted %d RD row(s) and %s emitted %d -- only one peer "
            "armed the trail" % (name_a, len(ra), name_b, len(rb))
        )
        return res
    pa, pb = res["partition_a"], res["partition_b"]
    if pa and pb and pa != pb:
        res["partition_mismatch"] = True
    common = sorted(set(ra) & set(rb))
    res["common_steps"] = len(common)
    if not common:
        res["verdict"] = "NO OVERLAP: the two RD trails share no step"
        return res
    names = []
    for st in common[:1]:
        names = [k for k in ra[st].keys() if k in rb[st]]
    for dom in names:
        first = None
        n_diff = 0
        for st in common:
            x, y = ra[st].get(dom), rb[st].get(dom)
            if x is None or y is None:
                continue
            if x != y:
                n_diff += 1
                if first is None:
                    first = st
        res["domains"][dom] = {"first_diverging_step": first, "diverging_steps": n_diff}
    diverged = {k: v for k, v in res["domains"].items() if v["first_diverging_step"] is not None}
    if not diverged:
        res["verdict"] = "ALL SUB-DOMAINS IDENTICAL over %d common step(s)" % len(common)
    else:
        earliest = min(v["first_diverging_step"] for v in diverged.values())
        lead = sorted(k for k, v in diverged.items() if v["first_diverging_step"] == earliest)
        res["earliest_step"] = earliest
        res["earliest_domains"] = lead
        res["verdict"] = (
            "SUB-DOMAIN DIVERGENCE: %s first at step %d (%d of %d domain(s) ever "
            "differ)" % ("/".join(lead), earliest, len(diverged), len(res["domains"]))
        )
    return res


# THE REGION INSIDE THE DOMAIN. The trail names a subsystem; this turns that into the region, using
# the R rows the same run already carries and the partition the DLL printed. Kept separate from
# subdomain_compare because it answers the SECOND question and needs a different input (R, not RD),
# and because a run may have one without the other.
def subdomain_regions_at(a, b, step, partition):
    """Names, per domain, the regions whose R-column differs at `step`. {} if either peer has no row."""
    ra, rb = (a.get("regions") or {}).get(step), (b.get("regions") or {}).get(step)
    if not ra or not rb or len(ra) != len(rb):
        return {}
    owner = {}
    for dom, members in (partition or {}).items():
        for m in members:
            owner[m.rstrip("*")] = dom
    out = {}
    for i, (x, y) in enumerate(zip(ra, rb)):
        if x == y:
            continue
        rn = REGION_NAMES[i] if i < len(REGION_NAMES) else "region[%d]" % i
        if rn in STATE_EXCLUDED:
            continue  # peer-local by construction; never the culprit named
        out.setdefault(owner.get(rn, "other"), []).append(rn)
    return out


# ---- P0-SPDET: the SINGLE-PLAYER equivalence comparison -----------------------------------------
# Different question from diff_peers, and the difference is why it is its own function rather than a
# flag on that one. diff_peers compares TWO PEERS of ONE run, where several regions legitimately
# differ (peer-local horizons, transposed peer_horizon arrays) and the verdict rests on the
# state-only hash. sp_compare compares TWO RUNS of ONE peer -- promoted vs unpromoted, same recorded
# input, same pinned wall clock. Nothing is peer-local, so EVERY channel must agree, including the
# three regions the harness excludes from the MP verdict on purpose.
#
# THOSE EIGHT ARE THE POINT. The six clock doubles + frame_ring + fps_estimate hold
# llm_strat_time_tick's own outputs; they are state_excluded() because they are frame-rate dependent
# and MP peers run at different frame rates. In an SP run with [harness] pin_wallclock=1 they are
# deterministic, so they have to be compared EXPLICITLY from the per-region R columns -- reading only
# the state-only hash here would silently skip the very seam this oracle exists for.
#
# SB-HOSTFREE H0 (2026-09-06): the first six used to be ONE entry, `time_globals`, hashing a 48-byte
# window over what are six independently bindable registry regions. Naming them one by one is not
# only what the split forces -- it is what makes an SP failure say WHICH clock value moved, where
# before this tuple could only say "the clock family" and only while nothing had relocated.
SP_TIME_REGIONS = (
    "current_game_time",
    "last_game_time",
    "total_game_time",
    "sim_step_interval",
    "game_time_delta",
    "game_speed",
    "frame_ring",
    "fps_estimate",
)

# X-SPINE clause (6a), 2026-09-10: THE ARM-SYMMETRIC VERDICT CHANNEL, stated as a set of columns.
#
# The `R <step> <h0>..<hN>` line is address-content: harness.cpp hashes each region's BYTES at a
# fixed sample point, so it reads the same numbers whichever implementation wrote them. That is the
# property the clause is about -- not whether a hook exists, but whether the channel produces
# COMPARABLE OUTPUT IN BOTH ARMS. A channel our promoted body EMITS (a call into tooling, a coverage
# record of our C++) has no counterpart in the all-original arm and is silenced by un-promotion, so
# it can only ever be a LIVENESS instrument. This one cannot be silenced by promoting, un-promoting
# or rebinding anything.
#
# SP_TIME_REGIONS above is the eight columns the MP verdict excludes and the SP oracle must add back.
# This is the WHOLE column set, and passing it turns the R line from a localisation aid (its use in
# diff_peers, which reads it only after `state` has already disagreed) into a per-region VERDICT: 61
# channels, each with its own compared-count and first-mismatch step, each of which must be non-empty
# and clean. `combined` already folds the same 61 hashes into one number -- so this adds no new
# claim, it adds ATTRIBUTION: `combined` can say two runs differed and never which slice moved.
#
# THE SAMPLING TRIGGER IS NOT ITSELF ARM-SYMMETRIC, and saying so is part of the claim. The per-step
# sample is taken by a detour on llm_strat_sim_step -- present in BOTH arms, so it perturbs neither
# comparison, but the channel is "hook-free" in its CONTENT, not in its CADENCE. That trigger is
# dispositioned at SIM1-P clause (6b)(i): the harness owns sim_step's entry and the promoted arm
# rebinds the detour's fall-through onto our entry thunk, so the sample is taken at the same point
# in both arms. A clause claiming hook-freedom without saying which half it means is the kind this
# ledger keeps having to retract.
SP_ALL_REGIONS = tuple(REGION_NAMES)


def sp_compare(a, b, name_a="baseline", name_b="promoted", time_regions=SP_TIME_REGIONS):
    """Compare two single-peer harness runs step-by-step. Returns a verdict dict.

    `a`/`b` are parse_harness() segments. Channels compared, each reported separately so a failure
    localizes without a re-run:
      state     -- the state-only hash (the sim rosters)
      combined  -- every region including the excluded ones
      <region>  -- each time-tick output region, from the R columns
    """
    sa, sb = a["steps"], b["steps"]
    common = sorted(set(sa) & set(sb))
    res = {
        "compared_steps": len(common),
        "name_a": name_a,
        "name_b": name_b,
        "channels": {},
        "region_columns_present": bool(a["regions"] and b["regions"]),
    }
    if not common:
        # A zero-step comparison is the vacuous pass this project keeps rediscovering -- it is a
        # FAILURE here, not an empty success. mp_analyze once called a
        # zero-compared-step run "ALL PAIRS IDENTICAL".
        res["ok"] = False
        res["verdict"] = "NO OVERLAPPING STEPS -- the two runs compared nothing"
        return res

    def channel(nm, get_a, get_b):
        """One comparison channel. A channel that compared NOTHING is a FAILURE, not agreement.

        Found by this function's own negative test: with R columns present in one run and absent in
        the other, skipping the un-pairable steps left every channel at 0 mismatches and the verdict
        green. That is the vacuous pass this whole item exists to prevent, reproduced inside the
        comparator meant to detect it -- so `compared` is counted and asserted rather than implied.
        """
        bad, compared = [], 0
        for s in common:
            x, y = get_a(s), get_b(s)
            if x is None or y is None:
                continue
            compared += 1
            if x != y:
                bad.append(s)
        res["channels"][nm] = {
            "compared": compared,
            "mismatch_count": len(bad),
            "first_mismatch": bad[0] if bad else None,
        }
        return compared > 0 and not bad

    ok = channel("state", lambda s: sa[s]["state"], lambda s: sb[s]["state"])
    ok &= channel("combined", lambda s: sa[s]["combined"], lambda s: sb[s]["combined"])

    idx = {nm: i for i, nm in enumerate(REGION_NAMES)}
    missing = []
    for nm in time_regions:
        i = idx.get(nm)
        if i is None:
            missing.append(nm)
            continue

        def col(regs, step, i=i):
            r = regs.get(step)
            return r[i] if r and i < len(r) else None

        # BOTH sides must carry the column. Checking only `a` is what let a run with no R columns in
        # `b` report agreement -- see channel()'s note.
        if not any(
            col(a["regions"], s) is not None and col(b["regions"], s) is not None for s in common
        ):
            missing.append(nm)
            continue
        ok &= channel(nm, lambda s, c=col: c(a["regions"], s), lambda s, c=col: c(b["regions"], s))
    res["uncompared_regions"] = missing
    if missing:
        # Same rule as the zero-step case: a region the run could not compare must not read as a
        # region that agreed. Missing R columns usually means region_hash_step was left at 0.
        ok = False
    res["ok"] = ok
    res["verdict"] = (
        "IDENTICAL across all %d compared steps (%d channels)" % (len(common), len(res["channels"]))
        if ok
        else "DIVERGED"
    )
    return res


def diff_breakdown(a, b):
    if not a["breakdown"] or not b["breakdown"]:
        return None
    out = {}
    for nm in REGION_NAMES:
        va, vb = a["breakdown"].get(nm), b["breakdown"].get(nm)
        if va and vb:
            out[nm] = "MATCH" if va == vb else "DIFFER"
    return out


# ---------------------------------------------------------------- input discovery

# mh_net.log lines carry a "[HH:MM:SS.mmm] " local-wall-clock stamp (DLL, 2026-08-06) so the "; " seam
# lines and the "net: " transport lines interleave into one timeline that can be correlated with the
# SSH tunnel log. Substring/regex matching is unaffected; anything that ANCHORS or SLICES a line has
# to drop the stamp first, which is what this does. Tolerates unstamped lines (older runs, and
# mh_launch.log, which parse_events also reads) -- the sub is simply a no-op there.
_TS_RE = re.compile(r"^\[\d{2}:\d{2}:\d{2}\.\d{3}\] ")


def _nots(line):
    """The line without its leading log timestamp, for anchored matching."""
    return _TS_RE.sub("", line, count=1)


LOG_NAMES = {
    "harness": "mh_harness.log",
    "lockstep": "mh_lockstep.log",
    "net": "mh_net.log",
    "launch": "mh_launch.log",
    "frametime": "mh_frametime.log",
}

GAME_MODE_NAMES = {2: "strategic", 3: "sync-overlay", 6: "tactical"}


def read_session_json(folder):
    """SES1: a session directory's session.json, or None. The machine-readable half of the
    SESSION_BEGIN/SESSION_END records -- the id, the roster, the reason, and `process_dir`.

    Deliberately read instead of parsing the log lines: pairing two peers by a regex over a sentence
    written for a human is a pairing that breaks the first time the sentence is improved."""
    fp = os.path.join(folder, "session.json")
    if not os.path.isfile(fp):
        return None
    try:
        with open(fp, encoding="utf-8", errors="replace") as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return None
    return d if isinstance(d, dict) else None


def _hits_in(folder):
    hits = {}
    for kind, nm in LOG_NAMES.items():
        fp = os.path.join(folder, nm)
        if os.path.isfile(fp):
            hits[kind] = fp
    return hits


def discover(path):
    """path -> dict of {kind: filepath}. A folder is searched (incl. one level of per-run
    subfolders, newest by mtime); a bare file is classified by name.

    SES1 ADDED THE SECOND HALF OF THIS FUNCTION. A run's output is now TWO directories, not one:
    the SESSION directory (mh_net.log, mh_lockstep.log, mh_frametime.log, mh_launch.log,
    session.json) and the PROCESS directory it hangs off, which keeps the streams whose subject is
    the process rather than the match -- above all `mh_harness.log`, whose path mh_harness.dll
    copies once at init, before any lobby exists. A discover() that stopped at the session folder
    would return no harness log, and the determinism verdict is computed FROM the harness log: the
    gate would have gone from ALL PAIRS IDENTICAL to "0 peers with a harness log" on a rename.

    So: take the session directory's streams, then fill any MISSING kind from the process directory
    that session.json names. Session beats process on a collision, which is the right precedence --
    a session's own mh_net.log is the match's, and the process one is the menu phase around it."""
    found = {}
    if os.path.isfile(path):
        base = os.path.basename(path).lower()
        for kind, nm in LOG_NAMES.items():
            if base == nm:
                found[kind] = path
        if not found:  # unknown filename -> guess harness
            found["harness"] = path
        return found
    # folder: use its own logs if present (legacy fixed-name dir, or a specific per-run folder);
    # otherwise fall back to the newest per-run subfolder (pointed at a logs/ parent).
    subs = [d for d in glob.glob(os.path.join(path, "*")) if os.path.isdir(d)]
    subs.sort(key=lambda d: os.path.getmtime(d), reverse=True)
    for cand in [path] + subs:
        hits = _hits_in(cand)
        if not hits:
            continue
        sess = read_session_json(cand)
        proc = (sess or {}).get("process_dir") or ""
        if proc:
            # `process_dir` is a LEAF name, not a path: the two directories are copied off a VM
            # together and an absolute path does not survive the trip. Resolve it beside the session.
            pdir = os.path.join(os.path.dirname(os.path.abspath(cand)), proc)
            if os.path.isdir(pdir):
                for kind, fp in _hits_in(pdir).items():
                    hits.setdefault(kind, fp)
        return hits
    return {}


def load_peer(path):
    logs = discover(path)
    peer = {"path": path, "logs": logs}
    # SES1: the directory's own declaration of which match it is, independent of the log prose.
    if os.path.isdir(path):
        peer["session"] = read_session_json(path)
    if "harness" in logs:
        peer["harness"] = parse_harness(logs["harness"])
    if "lockstep" in logs:
        peer["lockstep_rows"] = parse_lockstep(logs["lockstep"])
    if "net" in logs:
        peer["net"] = parse_events(logs["net"])
    if "launch" in logs:
        peer["launch"] = parse_events(logs["launch"])
    if "frametime" in logs:
        peer["frametime_rows"] = parse_frametime(logs["frametime"])
    return peer


# ---------------------------------------------------------------- report


def role_of(peer, default):
    # infer host/client from net.log banner or folder name. N1: a client label carries its player id
    # ("client2") so an N-peer run's pairwise labels stay distinct (two bare "client"s would collide).
    # host-assign: the connect line logs the PRE-assignment id (all clients "player 1"); the WELCOME line
    # carries the authoritative host-assigned id -- prefer it.
    for ln in peer.get("net", []):
        m = re.search(r"assigned us player (\d+)", ln)
        if m:
            return "client%s" % m.group(1)
    for ln in peer.get("net", []):
        if "HOST listening" in ln:
            return "host"
        if "CLIENT connected" in ln:
            m = re.search(r"as player (\d+)", ln)
            return ("client%s" % m.group(1)) if m else "client"
    b = os.path.basename(peer["path"].rstrip("/\\")).lower()
    if "host" in b:
        return "host"
    m = re.search(r"client(\d+)", b)
    if m:
        return "client%s" % m.group(1)
    if "client" in b or "join" in b or "vm" in b:
        return "client"
    return default


def fmt(v):
    return "%d" % v if isinstance(v, int) else str(v)


# ---------------------------------------------------------------- DET-FLAKE: guard selftest fixtures
#
# Seven arms over synthetic peer logs -- no rig, no game, runs inside lint_repo. Each asserts BOTH the
# printed verdict and the JSON's all_pairwise_clean, because ui_test.py gates on the flag while a
# human reads the string, and a guard whose two halves disagree is worse than none.
def _fx_peer(
    d,
    steps,
    interval,
    role,
    mismatch_at=None,
    end_at=None,
    silence_ms=0,
    stall_ms=0,
    stall_at=None,
    gameover_step=None,
    deliberate=False,
    real_gameover_step=None,
):
    """One synthetic peer. `end_at` makes the clock LEAVE the fixed quantum from that step, which is
    what a real match end does (turn_engine.cpp:379-386 takes one variable step to TOTAL_GAME_TIME).

    `gameover_step` models net_lockstep.cpp's U17 fast-drop cascade: a TRANSPORT failure that forces
    a game-over, so it writes BOTH the `; U17 fast-drop` marker and the `on_gameover` line together --
    this is the UNPLANNED-death shape DET-FLAKE's environmental guard owns.

    `real_gameover_step` (mp:D26) models the OTHER kind: a genuine elimination-driven game-over with
    NO transport failure anywhere in the log (outcome=4, `on_gameover_pre`'s presence_lost-loser code
    -- net_lockstep.cpp ~286) -- the shape `expected_match_end`'s real-game-over branch is FOR. Kept
    as a separate knob from `gameover_step` rather than a flag on it, because the two must be able to
    combine (a fixture that wants "an unplanned drop AND a coincidentally-clean gameover line on the
    OTHER peer" needs both bits addressable independently)."""
    os.makedirs(d, exist_ok=True)
    banner = (
        "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=%d fixed_step=0 "
        "pin_fpu=1 region_hash_step=1 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        % steps
    )
    if deliberate:
        banner += "; all_ai=1 observer=-1 gameover_step=50 gameover_stop=1\n"
    with open(os.path.join(d, "mh_harness.log"), "w") as f:
        f.write(banner)
        clock = 0.0
        for s in range(1, steps + 1):
            clock += interval if (end_at is None or s < end_at) else interval * 1.37
            state = 0xAAAA0000 + s
            if mismatch_at is not None and s >= mismatch_at and role != "host":
                state ^= 0xF0F0
            # The COMBINED hash differs on every step of every real run (peer_horizon, the clock
            # family and the order pipeline are peer-local by construction), and the NO-DATA floor
            # counts combined-hash differences as its "did we compare anything" proxy -- so a
            # fixture with identical combined hashes reads as a peer that produced nothing.
            combined = 0xBBBB0000 + s + (0 if role == "host" else 1)
            f.write(
                "%d %016X %016X %016X\n"
                % (s, struct.unpack(">Q", struct.pack(">d", clock))[0], combined, state)
            )
    with open(os.path.join(d, "mh_lockstep.log"), "w") as f:
        f.write("# " + " ".join(LOCKSTEP_COLS) + "\n")
        wall, mark = 0, (end_at or steps) - 1
        # `stall_at` places the freeze somewhere OTHER than the silence window, which is how arm 8
        # asks "does a freeze that lines up with nothing still count?" (it must not).
        freeze_row = mark if stall_at is None else stall_at
        for s in range(1, steps + 1):
            wall += stall_ms if (stall_ms and s == freeze_row) else 20
            ms = int(round(s * interval * 1000))
            f.write(
                "%d %d %d 30 10000 10000 10000 30 0 2 %d %d %d\n"
                % (wall, ms, ms, s * 3, s * 3, silence_ms if (silence_ms and s >= mark) else 40)
            )
    with open(os.path.join(d, "mh_net.log"), "w") as f:
        f.write(
            "[00:00:00.000] net: link watchdog armed (ping every 1000 ms, drop after 10000 ms "
            "of silence)\n"
        )
        # The noise line that made the first draft of the marker list fire on a HEALTHY run.
        f.write(
            "[00:00:00.001] ; [tombstone] armed: llm_net_player_remove_timeout(p) "
            "llm_net_lockstep_pump(p)\n"
        )
        # The in-band watch, ARMED and reporting nothing -- without these the detector-gap
        # accusation has no `armed` peer to fire on and arm 7 could not tell suppression from
        # absence.
        f.write("[00:00:00.002] ; [desync] ARMED: every=50 regions=61 fp=58CAC8A6\n")
        f.write("[00:00:03.000] ; [desync] STATUS: samples=8 compared=4 mismatching=0 too_old=0\n")
        if stall_ms:
            f.write(
                "[00:00:01.000] net: link watchdog was blocked %d ms (a send holding the conn "
                "lock, a suspend, or a debugger)\n" % stall_ms
            )
        if gameover_step is not None:
            f.write(
                "[00:00:02.000] ; U17 fast-drop: transport-dead peer side=1 -> broadcast "
                "removal (PARKED after 1 frames)\n"
            )
            f.write(
                "[00:00:02.001] ; on_gameover ENTER sess=2 outcome=8 gclk=%d (downgrade=0)\n"
                % int(round(gameover_step * interval * 1000))
            )
        if real_gameover_step is not None:
            # mp:D26. NO fast-drop marker here -- that absence is the whole point of this knob.
            f.write(
                "[00:00:02.001] ; on_gameover ENTER sess=2 outcome=4 gclk=%d (downgrade=0)\n"
                % int(round(real_gameover_step * interval * 1000))
            )


def selftest():
    import io
    import shutil
    import tempfile

    root = tempfile.mkdtemp(prefix="mp_analyze_selftest_")
    base = dict(steps=200, interval=0.01)
    dead = dict(silence_ms=12000, stall_ms=20000)
    # The cross-peer arms need a LONGER game-clock axis than the 2 s that 200 steps of 0.01 s gives:
    # the coverage tolerance is 5 s of game clock, so on the short axis every freeze covers every
    # silence and arm 8 could not fail. 0.1 s/step puts the whole run on a 20 s axis.
    wide = dict(steps=200, interval=0.1)
    arms = [
        ("1 clean + healthy link", dict(**base), "ALL PAIRS IDENTICAL", True, {}),
        (
            "2 desync + healthy link -- MUST NOT be excused",
            dict(mismatch_at=120, **base),
            "DESYNC in >=1 pair",
            False,
            {},
        ),
        (
            "3 desync + unplanned match end -- MUST relabel",
            dict(mismatch_at=120, end_at=120, gameover_step=120, **dead, **base),
            "FAIL: ENVIRONMENTAL (link-dead/match-ended at step 120)",
            False,
            {},
        ),
        (
            "4 CLEAN but the match died early -- MUST fail, not pass",
            dict(end_at=150, gameover_step=150, **dead, **base),
            "FAIL: ENVIRONMENTAL (link-dead/match-ended before the run finished)",
            False,
            {},
        ),
        (
            "5 three of four conditions (no grid departure) -- MUST stay DESYNC",
            dict(mismatch_at=120, gameover_step=120, **dead, **base),
            "DESYNC in >=1 pair",
            False,
            {},
        ),
        (
            "6 DELIBERATE gameover_step arm, divergence AFTER it -- mp:D26 truncates -> IDENTICAL",
            dict(mismatch_at=120, end_at=120, gameover_step=120, deliberate=True, **dead, **base),
            "ALL PAIRS IDENTICAL",
            True,
            {},
        ),
        # The widening (user's ruling, 2026-09-11). The peer that goes SILENT is not the peer that
        # FROZE -- A hears nothing because B stopped running -- so B's freeze has to be creditable as
        # the explanation for A's silence. Here only the HOST freezes and only the CLIENT observes
        # silence, which is the shape the first cut could not express.
        (
            "7 CROSS-PEER: only the remote peer froze, and it covers the silence -- MUST relabel",
            dict(mismatch_at=120, end_at=120, gameover_step=120, **wide),
            "FAIL: ENVIRONMENTAL (link-dead/match-ended at step 120)",
            False,
            {
                "host": dict(stall_ms=20000, silence_ms=12000),
                "client1": dict(stall_ms=0, silence_ms=12000),
            },
        ),
        # ...and the other half of the same claim: a freeze that does NOT line up with the silence
        # explains nothing. Without this arm the widening would be "any freeze anywhere", which is
        # what it was written to stop being.
        (
            "8 CROSS-PEER: the only freeze is far from the silence window -- MUST stay DESYNC",
            dict(mismatch_at=120, end_at=120, gameover_step=120, **wide),
            "DESYNC in >=1 pair",
            False,
            {
                "host": dict(stall_ms=20000, stall_at=20, silence_ms=12000),
                "client1": dict(stall_ms=0, silence_ms=12000),
            },
        ),
        # mp:D26 (user decision, 2026-09-23). Real game-over, no deliberate arm, reached by BOTH
        # peers -- the other half of "expected end": no harness banner tells this run to end, but
        # every peer independently logs on_gameover at the SAME game-clock (600 ms = step 60 on this
        # peer's own axis), which is what a genuine mutual elimination looks like. The divergence
        # starts exactly there, so truncation must exclude it -- proving the clock-mapped branch of
        # `expected_match_end`, not just the arming-line branch arm 6 exercises.
        (
            "9 REAL game-over on BOTH peers, divergence AT it -- mp:D26 truncates -> IDENTICAL",
            dict(mismatch_at=60, real_gameover_step=60, **base),
            "ALL PAIRS IDENTICAL",
            True,
            {},
        ),
        # ...and the negative of arm 9: the SAME real-game-over marker, but on only ONE peer -- the
        # other never corroborates it (a crash / a one-sided log, not a mutual elimination). This is
        # the "one peer ends, the other does not" case the user's ruling names explicitly, and it must
        # stay DESYNC: a one-sided game-over is not distinguishable from the unplanned-death shape, so
        # `expected_match_end` must refuse to call it expected.
        (
            "10 UNEXPECTED end: only ONE peer logs a game-over -- MUST stay DESYNC",
            dict(mismatch_at=60, **base),
            "DESYNC in >=1 pair",
            False,
            {"host": dict(real_gameover_step=60)},
        ),
    ]
    bad = 0
    try:
        for name, kw, want, want_clean, per_role in arms:
            d = os.path.join(root, name.split()[0])
            dirs = []
            for role in ("host", "client1"):
                p = os.path.join(d, role)
                _fx_peer(p, role=role, **dict(kw, **per_role.get(role, {})))
                dirs.append(p)
            ns = argparse.Namespace(
                paths=dirs,
                freeze_ms=200,
                min_common=1,
                json=os.path.join(d, "out.json"),
                selftest=False,
                allow_mismatch=False,
            )
            buf, keep = io.StringIO(), sys.stdout
            sys.stdout = buf
            try:
                rc = analyse(ns)
            finally:
                sys.stdout = keep
            printed = buf.getvalue()
            clean = json.load(open(ns.json)).get("all_pairwise_clean")
            got = next(
                (
                    ln.split("VERDICT:", 1)[1].strip()
                    for ln in printed.splitlines()
                    if "VERDICT:" in ln
                ),
                "(no verdict printed)",
            )
            ok = got.startswith(want) and clean is want_clean
            # G269: the EXIT CODE carries the same verdict -- 0 only for a clean run. Every
            # `post_check: ["tools/mp_analyze.py"]` row gates on it, so a regression here would make
            # those rows green on a DESYNC again.
            if (rc == 0) is not want_clean:
                ok, name = (
                    False,
                    name + " (+exit code %r disagrees with clean=%s)" % (rc, want_clean),
                )
            # arm 7 rides on arm 3's fixture: the false detector-gap accusation must be suppressed,
            # and the noise-line check rides on arm 1: a healthy run must report no drop marker.
            # NOTE: match the leading tag EXACTLY (not startswith) -- "10" also startswith "1", which
            # would silently fold arm 10 into arm 1's check the moment a double-digit arm existed.
            tag = name.split(" ", 1)[0]
            if tag == "3" and "NOT evidence of a detector gap" not in printed:
                ok, name = False, name + " (+arm 7: in-band suppression)"
            if tag == "1" and "peer-removal marker" in printed:
                ok, name = False, name + " (+marker false positive on a GREEN run)"
            # mp:D26 (user decision, 2026-09-23): arms 6 and 9 are the two EXPECTED-end shapes and
            # must both print the truncation line; arm 10 is the UNEXPECTED (one-sided) shape and
            # must NOT -- proving the guard doesn't fire on evidence it shouldn't accept.
            if tag in ("6", "9") and "D26: EXPECTED match end" not in printed:
                ok, name = False, name + " (+mp:D26: no truncation line printed)"
            if tag == "10" and "D26: EXPECTED match end" in printed:
                ok, name = False, name + " (+mp:D26: truncated an end that was NOT expected)"
            print("   %-64s %s" % (name, "ok" if ok else "FAIL -- got %r clean=%s" % (got, clean)))
            bad += 0 if ok else 1
        # ---- SES1: pairing by match_id, and the REFUSAL ------------------------------------------
        #
        # The acceptance clause reads "mp_analyze pairs peers by match_id and refuses to pair two
        # directories with different ids". Both halves are asserted here rather than on the rig,
        # because staging the negative on the rig means deliberately handing the analyzer the wrong
        # folder -- a mistake you cannot commit as evidence. Each arm builds two ORDINARY clean
        # peers (identical hashes, so the determinism verdict would be green) and varies only
        # session.json: a green run that is refused is exactly the point.
        A = "01a0af721310721a8d552b0ddedd707c"
        B = "01a0b00000007111aabbccddeeff0011"

        def _ses_arm(slug, name, host_id, client_id, allow, want_rc, want_text):
            nonlocal bad
            d = os.path.join(root, "ses_" + slug)
            dirs = []
            for role, mid in (("host", host_id), ("client1", client_id)):
                p = os.path.join(d, role)
                _fx_peer(p, role=role, steps=50, interval=0.01)
                if mid:
                    with open(os.path.join(p, "mh_net.log"), "w") as f:
                        f.write("[00:00:01.000] ; [session] match_id=%s\n" % mid)
                    with open(os.path.join(p, "session.json"), "w") as f:
                        json.dump(
                            {"match_id": mid, "slot": 0, "role": role, "reason": "gameover"}, f
                        )
                dirs.append(p)
            ns = argparse.Namespace(
                paths=dirs,
                freeze_ms=200,
                min_common=1,
                json=os.path.join(d, "out.json"),
                selftest=False,
                allow_mismatch=allow,
            )
            buf, keep = io.StringIO(), sys.stdout
            sys.stdout = buf
            try:
                rc = analyse(ns)
            finally:
                sys.stdout = keep
            printed = buf.getvalue()
            ok = (rc or 0) == want_rc and want_text in printed
            print("   %-64s %s" % (name, "ok" if ok else "FAIL -- rc=%s" % (rc,)))
            bad += 0 if ok else 1

        _ses_arm(
            "same", "SES1 same match_id -> paired", A, A, False, 0, "SAME MATCH on all 2 peer(s)"
        )
        _ses_arm("diff", "SES1 different match_ids -> REFUSED", A, B, False, 2, "REFUSED")
        _ses_arm(
            "allow",
            "SES1 --allow-mismatch overrides the refusal",
            A,
            B,
            True,
            0,
            "MISMATCH ALLOWED",
        )
        _ses_arm(
            "none",
            "SES1 no ids at all (pre-SES0 logs) -> not compared",
            None,
            None,
            False,
            0,
            "NOT COMPARED",
        )

        # ---- mp:D29: the manifest-fingerprint refusal ---------------------------------------------
        # Two clean peers with identical hashes; only the harness's configuration line varies. The
        # MIXED arm (configuration (1) on one side, (2) on the other, SAME fp) is the shape D29's
        # rig gate runs, so it must PASS; a fp mismatch must be REFUSED even though every hash agrees.
        def _fp_arm(slug, name, host_line, client_line, want_rc, want_text, absent_text=None):
            nonlocal bad
            d = os.path.join(root, "fp_" + slug)
            dirs = []
            for role, ln in (("host", host_line), ("client1", client_line)):
                p = os.path.join(d, role)
                _fx_peer(p, role=role, steps=50, interval=0.01)
                if ln:
                    with open(os.path.join(p, "mh_harness.log"), "a") as f:
                        f.write(ln + "\n")
                dirs.append(p)
            ns = argparse.Namespace(
                paths=dirs,
                freeze_ms=200,
                min_common=1,
                json=os.path.join(d, "out.json"),
                selftest=False,
                allow_mismatch=False,
            )
            buf, keep = io.StringIO(), sys.stdout
            sys.stdout = buf
            try:
                rc = analyse(ns)
            finally:
                sys.stdout = keep
            printed = buf.getvalue()
            ok = (rc or 0) == want_rc and want_text in printed
            if absent_text and absent_text in printed:
                ok = False
            print("   %-64s %s" % (name, "ok" if ok else "FAIL -- rc=%s" % (rc,)))
            bad += 0 if ok else 1

        c1 = (
            "; [harness] configuration (1): spine ABSENT, registry=mh.dll, rebased=0 owned=0, "
            "uncovered=none, manifest fp=%s"
        )
        c2 = (
            "; [harness] configuration (2): spine PRESENT, registry=libmh.dll, rebased=0 owned=2, "
            "uncovered=none, manifest fp=%s"
        )
        _fp_arm(
            "mixed",
            "D29 MIXED pair (config 1 vs 2), same manifest fp -> compared",
            c1 % "5A11D00D",
            c2 % "5A11D00D",
            0,
            "ALL PAIRS IDENTICAL",
            absent_text="HASH MANIFEST (D29)",
        )
        _fp_arm(
            "diff",
            "D29 different manifest fps -> REFUSED although hashes agree",
            c1 % "5A11D00D",
            c1 % "0BADF00D",
            2,
            "REFUSED: the peers hashed under 2 DIFFERENT hash manifests",
        )
        _fp_arm(
            "one",
            "D29 only one peer states a fp (pre-D29 build) -> not refused",
            c1 % "5A11D00D",
            None,
            0,
            "ALL PAIRS IDENTICAL",
            absent_text="HASH MANIFEST (D29)",
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("mp_analyze --selftest: %s" % ("PASS" if not bad else "%d ARM(S) FAILED" % bad))
    return 1 if bad else 0


# ---------------------------------------------------------------- DET-FLAKE: the environmental guard
#
# WHAT IT CLOSES. A determinism run whose MATCH DIED mid-way used to be reported as `DESYNC` -- i.e.
# the analyzer blamed the tree for a dead link -- or, when the peers happened to agree up to the
# death, as `ALL PAIRS IDENTICAL`, which passes a run that stopped measuring. Both were observed:
# 2026-09-09 is the first, and the archive holds three more of the same
# shape. Every discriminator this needs was ALREADY computed and printed by this tool
# (`max_since_rx_ms`, the freeze catalog, the net markers); none of it reached the verdict.
#
# FAIL-CLOSED, IN BOTH DIRECTIONS. The guard never converts a red into a pass and never suppresses a
# mismatch. It can only ADD a failure to a run that would otherwise pass, or RELABEL an existing
# failure when four INDEPENDENT conditions all agree. Missing any one -> plain DESYNC.
#
# SCOPE: unplanned link death / match end only. A run whose harness banner arms a deliberate end
# (`gameover_step != 0`) passes through untouched -- an elimination legitimately desyncs the peers
# and that class is filed separately rather than folded in here.
# SES2b (dead-ends G198): this list used to also carry "kicked-off the game", which nothing in
# src/mh_dll ever emitted -- grepped at mp:SES2 (tools/data/log_formats.json's now-removed
# net.unplanned_kickoff row). The only place that wording exists at all is a COMMENT in
# launch.cpp's mp_sync (the retail kick_player_confirm_cb callback announces "kicked off" AND
# "incompatible version" text) documenting why we deliberately do NOT take that retail path for a
# disconnection -- kick_player_confirm_cb itself has no committed prototype and nothing calls it
# from the injected DLL, so it can write nothing to mh_net.log. Dropped rather than rewired: there
# is no real emitter to point it at.
UNPLANNED_END_MARKERS = ("fast-drop: transport-dead peer",)
# Lines that merely NAME those functions rather than reporting one. Without this the `[tombstone]
# armed:` banner matched on every run including the GREEN control -- caught by running the guard over
# a known-good pair before wiring it up, which is why the green control is a selftest arm.
_MARKER_NOISE = ("[tombstone]", "[promote]", "armed:")


def parse_match_end(lines):
    """mh_net.log lines -> the match-end / link-health evidence, or empty fields when it says none."""
    d = {
        "gameover_gclk_ms": None,  # `on_gameover ENTER ... gclk=<ms>` -- the game-clock of the end
        "gameover_line": None,
        "drop_markers": [],  # peer removed / kicked / transport-dead
        "max_block_ms": 0,  # `link watchdog was blocked N ms` -- a process stall, self-reported
        "rx_timeout_ms": None,  # `drop after N ms of silence` -- read from the log, never hardcoded
    }
    for raw in lines or []:
        if d["gameover_gclk_ms"] is None and "on_gameover ENTER" in raw:
            m = re.search(r"gclk=(\d+)", raw)
            if m:
                d["gameover_gclk_ms"] = int(m.group(1))
                d["gameover_line"] = raw.strip()[:160]
        if "watchdog was blocked" in raw:
            m = re.search(r"blocked (\d+) ms", raw)
            if m:
                d["max_block_ms"] = max(d["max_block_ms"], int(m.group(1)))
        if d["rx_timeout_ms"] is None and "link watchdog armed" in raw:
            m = re.search(r"drop after (\d+) ms", raw)
            if m:
                d["rx_timeout_ms"] = int(m.group(1))
        if any(k in raw for k in UNPLANNED_END_MARKERS) and not any(
            n in raw for n in _MARKER_NOISE
        ):
            d["drop_markers"].append(raw.strip()[:160])
    return d


def step_clock_seconds(seg, step):
    """The game-clock (seconds) this peer logged at `step`, or None."""
    try:
        return struct.unpack(">d", bytes.fromhex(seg["steps"][step]["clock"]))[0]
    except (KeyError, ValueError, struct.error):
        return None


def step_for_clock(seg, clock_s, tol=0.5):
    """mp:D26. The inverse of `step_clock_seconds`: this peer's own step whose game-clock is
    nearest `clock_s`, or None if nothing is within `tol` seconds. Used to turn a `mh_net.log`
    on_gameover line's `gclk` (wall-independent game-clock, ms) into a STEP NUMBER so it can be
    compared against the hashed-step axis `diff_peers` truncates on."""
    best, best_d = None, None
    for s in seg.get("steps", {}):
        c = step_clock_seconds(seg, s)
        if c is None:
            continue
        d = abs(c - clock_s)
        if best_d is None or d < best_d:
            best, best_d = s, d
    return best if best_d is not None and best_d <= tol else None


def grid_departure(seg, probe=20, tol=1e-4):
    """The step at which this peer LEFT the fixed lockstep quantum, or None if it never did.

    WHY THIS IS VERDICT-GRADE AND NOT A HEURISTIC. In SESSION_MP_LOCKSTEP, llm_strat_sim_tick's
    mode-3 loop advances GAME_CLOCK by exactly SIM_STEP_INTERVAL per sim step
    (src/mh_dll/libmh/lockstep/turn_engine.cpp:372-378), so every hashed step's clock is an exact
    multiple of the interval. The `else` branch -- taken in every OTHER session mode -- instead takes
    ONE VARIABLE step straight to TOTAL_GAME_TIME (:379-386), which is wall-clock driven. So "this
    peer's clock is off the quantum grid" reads "this peer is no longer running the lockstep sim",
    independently of the network log, the hashes, and the guard's other three conditions.

    Measured over the whole rig archive (483 gate-shape pairs, 2026-07-12..2026-09-10): every pair
    that left the grid had a dead link, and every pair with a dead link left the grid. No exceptions
    in either direction.
    """
    ks = sorted(seg.get("steps", {}))
    if len(ks) < probe + 2:
        return None
    deltas = []
    for i in range(probe):
        a, b = step_clock_seconds(seg, ks[i]), step_clock_seconds(seg, ks[i + 1])
        if a is not None and b is not None:
            deltas.append(round(b - a, 9))
    if not deltas:
        return None
    deltas.sort()
    iv = deltas[len(deltas) // 2]
    if not iv or iv <= 0:
        return None
    for s in ks:
        c = step_clock_seconds(seg, s)
        if c is None:
            continue
        q = c / iv
        if abs(q - round(q)) > tol:
            return s
    return None


def environmental_verdict(peers, first_mismatch_step, first_mismatch_clock_s):
    """Did an UNPLANNED match end happen, and does it account for this run's result?

    Returns (kind, reasons):
      None             -- nothing environmental; the hash verdict stands unchanged
      "ended"          -- the match ended / the link died before the run finished. The run FAILS even
                          if every compared step agreed (the vacuous-pass killer).
      "ended_explains" -- as above AND all four conditions place the end at or before the first
                          mismatch, so the DESYNC is relabelled FAIL: ENVIRONMENTAL.
    """
    reasons = []
    for p in peers:
        arming = (p.get("harness") or {}).get("arming") or ""
        if re.search(r"gameover_step=([1-9]\d*)", arming):
            return None, [
                "a DELIBERATE gameover_step arm is present -- the environmental guard does "
                "not apply to runs that were told to end"
            ]

    ends = [parse_match_end(p.get("net")) for p in peers]
    rx_timeout = next((e["rx_timeout_ms"] for e in ends if e["rx_timeout_ms"]), 10000)

    def _ls(p, key):
        return (p.get("lockstep_analysis") or {}).get(key, 0)

    silent = [
        (p["role"], _ls(p, "max_since_rx_ms"))
        for p in peers
        if _ls(p, "max_since_rx_ms") >= rx_timeout
    ]
    gos = [e["gameover_gclk_ms"] for e in ends if e["gameover_gclk_ms"] is not None]
    drops = [m for e in ends for m in e["drop_markers"]]
    # CONDITION (3), WIDENED 2026-09-11 (user's ruling) -- and made stricter in the same move.
    #
    # Silence on peer A is normally explained by a freeze on peer B: B stopped sending because B
    # stopped running. The first cut only asked "did ANYONE stall", which credited a stall to a
    # silence it might have nothing to do with, and could not credit B's freeze as the explanation
    # for A's silence at all. Now each silent peer must be MATCHED to a freeze that covers its
    # silence window -- on any peer, itself or another -- and a silence nobody's freeze covers
    # leaves the condition unmet. Wider in what counts, narrower in what it will accept.
    #
    # THE SHARED AXIS IS THE GAME CLOCK, not wall time. `wall_ms` is each peer's own counter and the
    # two are not comparable; `clock_ms` is GAME_CLOCK, which is the same quantity on both peers for
    # as long as they are in lockstep -- which is exactly the window this is about. Measured on the
    # 2026-09-09 red: the host's silence opens at clock 38490 and the client's freezes sit at 38460
    # and 38480, i.e. both peers park within a few hundred ms of game time of each other.
    SILENCE_COVER_TOL_MS = 5000

    def _silence_window(rows):
        sil = [r["clock_ms"] for r in rows if r.get("since_rx_ms", 0) >= rx_timeout]
        return (sil[0], sil[-1]) if sil else None

    def _freezes(rows):
        return [
            (rows[i]["wall_ms"] - rows[i - 1]["wall_ms"], rows[i - 1]["clock_ms"])
            for i in range(1, len(rows))
            if rows[i]["wall_ms"] - rows[i - 1]["wall_ms"] >= rx_timeout
        ]

    explained, unexplained = [], []
    for p in peers:
        if _ls(p, "max_since_rx_ms") < rx_timeout:
            continue
        win = _silence_window(p.get("lockstep_rows") or [])
        cover = None
        if win:
            lo, hi = win[0] - SILENCE_COVER_TOL_MS, win[1] + SILENCE_COVER_TOL_MS
            for q in peers:
                for gap, clk in _freezes(q.get("lockstep_rows") or []):
                    if lo <= clk <= hi:
                        cover = (q["role"], gap, clk)
                        break
                if cover:
                    break
        (explained if cover else unexplained).append((p["role"], win, cover))
    # EVERY silence must be accounted for, not just one of them -- fail-closed.
    stalled = bool(explained) and not unexplained
    departed = [(p["role"], grid_departure(p.get("harness") or {})) for p in peers]
    departed = [(r, s) for r, s in departed if s is not None]

    if silent:
        reasons.append(
            "peer silence %s, at or past the link's own %d ms drop threshold"
            % (", ".join("%s=%dms" % (r, v) for r, v in silent), rx_timeout)
        )
    if gos:
        reasons.append(
            "the match ENDED: on_gameover at game-clock %s"
            % ", ".join("%.2fs" % (g / 1000.0) for g in gos)
        )
    if drops:
        reasons.append("peer-removal marker: %s" % drops[0])
    for role, win, cover in explained:
        reasons.append(
            "%s's silence (from game-clock %.2fs) is covered by a %s freeze of %dms at %.2fs"
            % (
                role,
                win[0] / 1000.0,
                "LOCAL" if cover[0] == role else cover[0],
                cover[1],
                cover[2] / 1000.0,
            )
        )
    for role, win, _ in unexplained:
        reasons.append(
            "%s went silent%s but NO peer's freeze covers that window -- condition (3) unmet, so "
            "this is not relabelled"
            % (role, "" if not win else " from game-clock %.2fs" % (win[0] / 1000.0))
        )
    for p, e in zip(peers, ends):
        if e["max_block_ms"]:
            reasons.append(
                "%s's own link watchdog reports it was blocked %dms"
                % (p["role"], e["max_block_ms"])
            )
    if departed:
        reasons.append(
            "left the lockstep clock grid at step %s"
            % ", ".join("%s@%d" % (r, s) for r, s in departed)
        )

    if not (silent and (gos or drops)):
        return None, reasons
    if first_mismatch_step is None:
        return "ended", reasons

    end_at_or_before = bool(departed) and min(s for _, s in departed) <= first_mismatch_step
    if gos and first_mismatch_clock_s is not None:
        # One step of slack: the two peers end one step apart (measured 2026-09-09 -- host 38.49 s,
        # client 38.48 s), so an exact <= on the clock would reject the very case this is for.
        end_at_or_before = end_at_or_before and (min(gos) / 1000.0 <= first_mismatch_clock_s + 1e-6)
    if not (stalled and departed and end_at_or_before):
        reasons.append(
            "NOT relabelled: the four conditions do not all place the end at or before "
            "the first mismatch, so this stays a DESYNC"
        )
        return "ended", reasons
    return "ended_explains", reasons


# ---------------------------------------------------------------- mp:D26: the EXPECTED-end truncation
#
# USER DECISION (2026-09-23): mp_analyze TRUNCATES the comparison at match end, but ONLY when the end
# was EXPECTED -- a deliberate/declared end (a `gameover_step` harness arm) or a real game-over that
# EVERY peer independently reaches. An UNEXPECTED end (one peer ends, the other does not -- a crash, a
# dropped link, anything the OTHER peer never corroborates) is NOT expected-end evidence and must
# still report DESYNC: that class is exactly what a real bug or a real dead link looks like, and this
# function's job is to tell the two apart, not to excuse either.
#
# WHY "every peer" for the real-game-over branch. A elimination-triggered game-over is client code
# running on BOTH sides from the SAME deterministic order, so a genuine one is logged by every peer
# that is still in the match. A game-over logged by only SOME peers is indistinguishable from one peer
# silently dying mid-match and never reaching the line at all -- i.e. it is not evidence the end was
# expected, it is the unexpected-end shape by another name, so this deliberately does NOT relax to "at
# least one peer".
#
# WHY THE DELIBERATE ARM NEEDS NO CLOCK MAPPING. `gameover_step=N` in the harness arming banner is the
# step the run was TOLD to force a game-over at (harness.cpp) -- it names a step directly, so it is
# authoritative on its own and is checked first.
def expected_match_end(peers):
    """Is this match's end EXPECTED, and if so, at what step should the comparison truncate?

    Returns (step, reason) when expected-end evidence exists (`step` is the first step to EXCLUDE --
    `diff_peers(truncate_at=step)` compares steps < step only), or (None, reason) when there is none --
    including the one-sided case, which the caller must still let read as DESYNC.
    """
    for p in peers:
        arming = (p.get("harness") or {}).get("arming") or ""
        m = re.search(r"gameover_step=([1-9]\d*)", arming)
        if m:
            step = int(m.group(1))
            return step, "deliberate gameover_step=%d harness arm" % step

    ends = [(p, parse_match_end(p.get("net"))) for p in peers]
    gos = [(p, e["gameover_gclk_ms"]) for p, e in ends if e["gameover_gclk_ms"] is not None]
    if not peers or len(gos) < len(peers):
        return None, (
            "no deliberate gameover_step arm, and not every peer logged a real on_gameover "
            "(%d of %d) -- an unexpected/one-sided end is not expected-end evidence"
            % (len(gos), len(peers))
        )
    # An UNPLANNED-drop marker (net_lockstep.cpp's U17 fast-drop -- a transport failure) anywhere in
    # the match, alongside a game-over every peer reached, is exactly DET-FLAKE's environmental class
    # (a link death that cascades into a forced game-over on both sides), not the clean-elimination
    # shape this branch is for -- leave it to `environmental_verdict`'s own 4-condition guard instead
    # of taking it here on weaker evidence.
    if any(e["drop_markers"] for _, e in ends):
        return None, (
            "every peer logged a real on_gameover, but an unplanned transport-drop marker is also "
            "present -- this is DET-FLAKE's environmental class, not a clean elimination"
        )
    steps = []
    for p, gclk in gos:
        s = step_for_clock(p.get("harness") or {}, gclk / 1000.0)
        if s is None:
            return None, (
                "every peer logged a real on_gameover, but %s's gclk=%dms maps to no hashed "
                "step -- cannot place the truncation boundary" % (p.get("role"), gclk)
            )
        steps.append((p.get("role"), s))
    step = min(s for _, s in steps)
    return step, "real on_gameover reached by all %d peer(s) at step(s) %s" % (
        len(peers),
        ", ".join("%s@%d" % (r, s) for r, s in steps),
    )


def main():
    ap = argparse.ArgumentParser(description="MP run analyzer")
    ap.add_argument("paths", nargs="*")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="DET-FLAKE: run the environmental-guard arms over synthetic peer logs (no rig, no "
        "game) and exit non-zero if any arm produced the wrong verdict",
    )
    ap.add_argument("--freeze-ms", type=int, default=200)
    ap.add_argument("--json", default=None)
    ap.add_argument(
        "--min-common",
        type=int,
        default=1,
        help="minimum overlapping hashed steps a peer PAIR must have for its verdict to count. Below "
        "this the pair is NO DATA and the run is not clean -- otherwise a peer that died on launch "
        "yields mismatch=0 and a vacuous 'ALL PAIRS IDENTICAL'. Raise it (e.g. to ~the requested step "
        "count) to also reject runs that only got part-way.",
    )
    ap.add_argument(
        "--allow-mismatch",
        action="store_true",
        help="SES1: compare peers whose match_ids DISAGREE. Off by default: a run directory is per "
        "MATCH since SES1, so two folders with different ids are two different matches and a "
        "cross-match desync report is a wrong explanation with a rig's authority behind it. Pass "
        "this for the pre-SES1 per-process layout, or for a deliberate cross-match look.",
    )
    ap.add_argument(
        "--snapshot-verify",
        action="store_true",
        help="mp:X1b: exit non-zero unless a snapshot captured on one peer was imported on another "
        "and EVERY per-region hash of the imported world matches the capture, rng_state included. "
        "Without it the snapshot section is reported and the exit code ignores it -- which is right "
        "for a person reading a run and wrong for a gate, because a run in which the transfer never "
        "completed has no snapshot section at all and would otherwise pass by saying nothing.",
    )
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.paths:
        ap.error("give at least one run path (or --selftest)")
    rc = analyse(args) or 0
    if args.snapshot_verify:
        rc = snapshot_verdict_rc(rc)
    return rc


# mp:X1b. THE GATE'S HALF OF snapshot_compare, kept apart from it for one reason: analyse() prints a
# report and its exit code is about the DESYNC verdict. A caller that wants the snapshot claim to
# decide the run needs the absence of a snapshot to be a failure too, and "there was no section"
# cannot be expressed as a verdict string inside a section that does not exist.
_SNAPSHOT_SEEN = []


def snapshot_verdict_rc(rc):
    if not _SNAPSHOT_SEEN:
        print(
            "\nSNAPSHOT VERIFY: FAILED -- no peer pair produced a SNAPCAP/SNAPIMP pair. Either the "
            "verbs were never armed (`[harness] snapshot_at` on the host, `snapshot_import` on both) "
            "or the transfer did not complete inside the run."
        )
        return rc or 1
    bad = [v for v in _SNAPSHOT_SEEN if not v.startswith("SNAPSHOT IMPORT VERIFIED")]
    if bad:
        print("\nSNAPSHOT VERIFY: FAILED -- %s" % "; ".join(bad))
        return rc or 1
    print("\nSNAPSHOT VERIFY: PASS -- %s" % "; ".join(_SNAPSHOT_SEEN))
    return rc


def analyse(args):
    """The whole analysis, factored out of main() so --selftest can exercise THE REAL PATH.

    A selftest that re-implements the verdict logic proves nothing about what the gate runs. This
    one writes synthetic peer logs, calls this function, and reads back both the printed verdict and
    the JSON flag ui_test.py actually gates on -- the two have to agree or the guard is decorative.
    """
    peers = [load_peer(p) for p in args.paths]
    for i, pr in enumerate(peers):
        pr["role"] = role_of(pr, "peer%d" % i)

    out = {"inputs": args.paths, "peers": [], "freeze_ms": args.freeze_ms}
    print("=" * 72)
    print("MP RUN ANALYSIS")
    print("=" * 72)

    for pr in peers:

        def _rel(v):
            try:
                return os.path.relpath(v)
            except ValueError:
                return v  # different drive (e.g. pulled VM logs on another mount)

        pj = {
            "path": pr["path"],
            "role": pr["role"],
            "logs": {k: _rel(v) for k, v in pr["logs"].items()},
        }
        print("\n[%s]  %s" % (pr["role"].upper(), pr["path"]))
        for kind in ("harness", "lockstep", "net", "launch"):
            if kind in pr["logs"]:
                print("   %-9s %s" % (kind, pr["logs"][kind]))

        if "harness" in pr:
            h = pr["harness"]
            nsteps = len(h["steps"])
            pj["harness"] = {
                "banner": h["banner"],
                "steps": nsteps,
                "per_region_steps": len(h["regions"]),
                "has_breakdown": bool(h["breakdown"]),
                "config": h.get("config"),
                "manifest_fp": h.get("manifest_fp"),
            }
            if h.get("config"):
                print(
                    "   harness: configuration (%s), manifest fp=%s"
                    % (h["config"], h.get("manifest_fp") or "?")
                )
            if h["banner"]:
                print("   harness: %s" % h["banner"].strip("; ="))
            print(
                "   harness: %d steps hashed, %d per-region step-lines, breakdown=%s"
                % (nsteps, len(h["regions"]), "yes" if h["breakdown"] else "no")
            )

        if pr.get("lockstep_rows"):
            la = analyze_lockstep(pr["lockstep_rows"], args.freeze_ms)
            pj["lockstep"] = la
            pr["lockstep_analysis"] = la  # DET-FLAKE: the environmental guard reads it at rollup
            print(
                "   perf: %d frames over %.1fs | gap mean %.1fms p50 %d p95 %d p99 %d max %d"
                % (
                    la["frames"],
                    la["span_ms"] / 1000.0,
                    la["gap_mean_ms"],
                    la["gap_p50_ms"],
                    la["gap_p95_ms"],
                    la["gap_p99_ms"],
                    la["gap_max_ms"],
                )
            )
            print(
                "   perf: %d freezes >=%dms (total %.1fs); tx=%d rx=%d max_since_rx=%dms"
                % (
                    la["freeze_count"],
                    args.freeze_ms,
                    la["freeze_total_ms"] / 1000.0,
                    la["tx_final"],
                    la["rx_final"],
                    la["max_since_rx_ms"],
                )
            )
            if la.get("icon"):
                ic = la["icon"]
                print(
                    "   icon: %d wanted / %d shown (%d suppressed) | %.1f wanted %.1f shown per 1k "
                    "frames | %d at-horizon frames  [MP U20]"
                    % (
                        ic["calls"],
                        ic["shown"],
                        ic["suppressed"],
                        ic["calls_per_1k_frames"],
                        ic["shown_per_1k_frames"],
                        ic["at_horizon_frames"],
                    )
                )
            # The SELF slot in _G_LLM_NET_PEER_HORIZON is never updated (a peer's own horizon lives in
            # _G_LLM_STRAT_LOCKSTEP_HORIZON = local_h_ms) and commit_horizon skips it (PlayerSide != i),
            # so its "frozen" value is expected, not a stall. host self = peer0, client self = peer1.
            # SELF horizon column is the peer's own player id (host=0, clientK=K); its frozen value is expected.
            if pr["role"] == "host":
                self_col = "peer0_ms"
            else:
                m = re.search(r"client(\d+)", pr["role"])
                self_col = ("peer%s_ms" % m.group(1)) if m else "peer1_ms"
            for col, st in la["horizon"].items():
                if st["max_frozen_ms"] > 2000 and col != self_col:
                    print(
                        "   perf: WARNING %s frozen up to %.1fs (final=%d) -- horizon staleness"
                        % (col, st["max_frozen_ms"] / 1000.0, st["final"])
                    )
        if pr.get("frametime_rows"):
            fa = analyze_frametime(pr["frametime_rows"], args.freeze_ms, pr.get("net"))
            if fa:
                pj["frametime"] = fa
                sd = fa["strategic"] or fa["overall"]
                which = "strategic" if fa["strategic"] else "all-frame"
                print(
                    "   frame: %d presented frames; modes=%s"
                    % (
                        len(pr["frametime_rows"]),
                        ", ".join("%s:%d" % (k, v) for k, v in fa["mode_frames"].items()),
                    )
                )
                lc = fa.get("lobby_clip")
                if lc:
                    # D22: print WHICH timestamp the lobby/session split was clipped at, so a
                    # reader can check the derivation rather than trust the bucket counts blind.
                    print(
                        "   frame: lobby clip at qpc_us=%d (~%.1f min lobby) -- %s"
                        % (lc["qpc_us"], lc["lobby_seconds"] / 60.0, lc["detail"])
                    )
                    if lc.get("strategic_before_clip"):
                        print(
                            "   frame: %d strategic frame(s) fall BEFORE the clip -- the anchor lags "
                            "the GAME_MODE flip by that much; they are counted in-game (see "
                            "analyze_frametime)" % lc["strategic_before_clip"]
                        )
                if sd:
                    print(
                        "   frame(%s): mean %.1fms p50 %.1f (~%.0f fps) p95 %.1f p99 %.1f max %.1f | %d freeze>=%dms"
                        % (
                            which,
                            sd["mean_ms"],
                            sd["p50_ms"],
                            sd["fps_p50"],
                            sd["p95_ms"],
                            sd["p99_ms"],
                            sd["max_ms"],
                            sd["freezes"],
                            args.freeze_ms,
                        )
                    )

        if pr.get("session"):
            # SES1: the directory SAYS which match it holds. Printed before the log-derived ids
            # below because it is the stronger statement: session.json is written by the peer that
            # opened the session and rewritten when it closes, so it describes THIS FOLDER, while
            # the `; [session] match_id=` lines describe everything the peer passed through.
            s = pr["session"]
            pj["session"] = s
            print(
                "   session.json: match_id=%s slot=%s map=%s%s"
                % (
                    s.get("match_id") or "(none)",
                    s.get("slot"),
                    s.get("map") or "?",
                    (" reason=%s" % s["reason"]) if s.get("reason") else " (still open / no END)",
                )
            )

        if pr.get("net"):
            # SES0: which match(es) this peer was in, by id.
            mids = parse_match_ids(pr["net"])
            pj["match_ids"] = mids
            pj["match_id"] = mids[-1] if mids else None
            # SES1: inside a session directory the FOLDER's id wins. A session's mh_net.log can
            # still carry an earlier id (the client logs the host's advert before it joins), and
            # "the last one mentioned" is the pre-SES1 approximation this replaces.
            if pr.get("session") and pr["session"].get("match_id"):
                pj["match_id"] = pr["session"]["match_id"]
            if mids:
                print(
                    "   session: match_id=%s%s"
                    % (
                        mids[-1],
                        ""
                        if len(mids) == 1
                        else "  (after %d earlier lobby/lobbies: %s)"
                        % (len(mids) - 1, ", ".join(mids[:-1])),
                    )
                )
            else:
                print("   session: no match_id logged (pre-SES0 build, or never in a lobby)")
            # D21: what this peer said about the match WHILE IT WAS RUNNING.
            dw = parse_desync(pr["net"])
            pj["desync_watch"] = dw
            if dw["armed"]:
                if dw["manifest_mismatch"]:
                    print("   desync-watch: DISABLED -- %s" % dw["manifest_mismatch"])
                elif dw["inert"]:
                    print("   desync-watch: INERT on this peer (no per-step sampling point)")
                elif dw["mismatch_lines"]:
                    # The full `*** DESYNC` lines are THROTTLED (first few, then a rollup), so their
                    # count is a floor, not the total. The STATUS line carries the real tally.
                    total = desync_counts(dw["last_status"]).get(
                        "mismatching", len(dw["mismatch_lines"])
                    )
                    print(
                        "   desync-watch: *** REPORTED %d mismatching sample(s) IN-GAME "
                        "(%d logged in full), %d notice(s)"
                        % (total, len(dw["mismatch_lines"]), dw["notices"])
                    )
                    print("   desync-watch: first -- %s" % dw["first_mismatch"])
                else:
                    c = desync_counts(dw["last_status"])
                    print(
                        "   desync-watch: clean -- %s compared sample(s), 0 mismatching"
                        % (c.get("compared", "?"))
                        if c
                        else "   desync-watch: armed, no sample reached a comparison"
                    )
                if dw["cost_probe"]:
                    print("   desync-watch: %s" % dw["cost_probe"])
            evs = [e for e in pr["net"] if _nots(e).startswith("net:")]
            pj["net_events"] = evs
            for e in evs:
                # Print the line WHOLE (stamp included) rather than re-assembling it from a slice:
                # the timestamp is the most useful column in a transport post-mortem, and the old
                # `e[4:]` slice assumed the line began exactly at "net:".
                print("   %s" % e.strip())
        out["peers"].append(pj)

    # ---- SES0/SES1: do the peers agree on WHICH MATCH this was? ----------------------------------
    # SES0 MADE THIS A WARNING. SES1 MAKES IT A REFUSAL, and the reason is that the ground under it
    # moved. Under SES0 a run directory was per PROCESS, so two folders disagreeing about the id
    # could mean nothing worse than "the host re-created its lobby and the client's last advert was
    # the old one" -- a real possibility that had nothing to do with the operator. A refusal then
    # would have turned an ordinary sequence into a red.
    #
    # A directory is now per SESSION. Two session directories carrying different ids are two
    # DIFFERENT MATCHES, full stop; there is no reading under which comparing their steps means
    # anything. Continuing would produce a desync report -- possibly a spectacular one -- for peers
    # that were never in the same game, which is worse than no report: it is a wrong explanation
    # arriving with a rig's authority behind it. So it stops, names both ids, and says which flag
    # says "I know, these are the old per-process folders": `--allow-mismatch`.
    #
    # Comparison is over each peer's OWN id: session.json's when the folder declares one, else the
    # last id its mh_net.log mentions (the pre-SES1 approximation, kept for legacy layouts).
    paired = [p for p in out["peers"] if p.get("match_id")]
    ids = {p["match_id"] for p in paired}
    out["match_ids_by_peer"] = {p["role"]: p.get("match_ids") for p in out["peers"]}
    out["match_id"] = list(ids)[0] if len(ids) == 1 else None
    out["match_id_agree"] = (len(ids) == 1) if len(paired) >= 2 else None
    if len(out["peers"]) >= 2:
        print("\n" + "-" * 72)
        print("SESSION ID (SES0)")
        print("-" * 72)
        for p in out["peers"]:
            print("   %-10s %s" % (p["role"], p.get("match_id") or "(none logged)"))
        if len(paired) < 2:
            print(
                "   -> NOT COMPARED: %d of %d peer(s) logged a match_id (a pre-SES0 build, or a peer "
                "that never reached a lobby)" % (len(paired), len(out["peers"]))
            )
        elif len(ids) == 1:
            print("   -> SAME MATCH on all %d peer(s): %s" % (len(paired), out["match_id"]))
        elif getattr(args, "allow_mismatch", False):
            print(
                "   -> MISMATCH ALLOWED (--allow-mismatch): peers report %d DIFFERENT match_ids. "
                "Everything below compares peers that were NOT in the same match." % len(ids)
            )
        else:
            out["refused"] = "match_id_mismatch"
            print(
                "   -> REFUSED: peers report %d DIFFERENT match_ids, so these directories are %d "
                "DIFFERENT MATCHES and there is nothing to compare between them."
                % (len(ids), len(ids))
            )
            for p in out["peers"]:
                print(
                    "        %-10s %s  %s" % (p["role"], p.get("match_id") or "(none)", p["path"])
                )
            print(
                "      A session directory is per MATCH since SES1, so this is not the "
                "host-re-created-its-lobby case a warning used to cover -- it is the wrong folder. "
                "Check the paths (the newest `*_host` folder is the LAST match that peer played).\n"
                "      Pass --allow-mismatch to compare anyway (pre-SES1 per-process folders, or a "
                "deliberate cross-match look)."
            )
            if args.json:
                with open(args.json, "w", encoding="utf-8") as fh:
                    json.dump(out, fh, indent=2)
                print("\nJSON -> %s" % args.json)
            return 2

    # ---- mp:D29: do the peers hash the SAME MANIFEST? --------------------------------------------
    # Each harness states its manifest fingerprint (the ordered slice list: rid/offset/len/excluded
    # and the name). Two peers under different manifests have per-region columns that MEAN
    # different things, so an IDENTICAL verdict between them would be noise with a rig's authority --
    # the same reason a match_id mismatch is refused above. Peers that carry no line (a pre-D29
    # build) are listed and not compared; a MIXED pair (configuration (1) vs (2)) is fine as long as
    # the fingerprints agree, which is the whole point of that shape.
    fps = [
        (
            p["role"],
            (p.get("harness") or {}).get("manifest_fp"),
            (p.get("harness") or {}).get("config"),
        )
        for p in out["peers"]
    ]
    stated = {fp for _r, fp, _c in fps if fp}
    out["manifest_fp_by_peer"] = {r: fp for r, fp, _c in fps}
    out["harness_config_by_peer"] = {r: c for r, _fp, c in fps}
    if len([1 for _r, fp, _c in fps if fp]) >= 2 and len(stated) > 1:
        out["refused"] = "manifest_fp_mismatch"
        print("\n" + "-" * 72)
        print("HASH MANIFEST (D29)")
        print("-" * 72)
        for r, fp, c in fps:
            print("   %-10s configuration (%s)  manifest fp=%s" % (r, c or "?", fp or "(none)"))
        print(
            "   -> REFUSED: the peers hashed under %d DIFFERENT hash manifests -- their per-region "
            "columns do not describe the same regions, so no verdict between them means anything. "
            "Rebuild both peers from one tree." % len(stated)
        )
        if args.json:
            with open(args.json, "w", encoding="utf-8") as fh:
                json.dump(out, fh, indent=2)
            print("\nJSON -> %s" % args.json)
        return 2

    # cross-peer diff -- ALL pairwise combinations (N1: a 3-peer game must be identical across all 3 pairs,
    # not just host-vs-first-client). With 2 peers this is the single pair as before.
    import itertools

    harness_peers = [p for p in peers if "harness" in p]
    if len(harness_peers) >= 2:
        # mp:D26 (user decision, 2026-09-23): decide BEFORE the pairwise loop, since the truncation
        # boundary is a property of the MATCH (all peers), not of one pair, and every pair's diff
        # must be truncated at the same step or the pairwise rollup would disagree with itself.
        d26_step, d26_reason = expected_match_end(harness_peers)
        if d26_step is not None:
            print(
                "\nD26: EXPECTED match end at step %d (%s) -- the comparison below is TRUNCATED "
                "to steps < %d; a divergence at or after it is the match ending, not the sim "
                "disagreeing" % (d26_step, d26_reason, d26_step)
            )
        out["d26_expected_end_step"] = d26_step
        out["d26_expected_end_reason"] = d26_reason
        out["desync_pairs"] = []
        pair_verdicts = []
        for a, b in itertools.combinations(harness_peers, 2):
            print("\n" + "-" * 72)
            print("CROSS-PEER DESYNC DIFF  [%s] vs [%s]" % (a["role"].upper(), b["role"].upper()))
            print("-" * 72)
            d = diff_peers(a["harness"], b["harness"], a["role"], b["role"], truncate_at=d26_step)
            d["pair"] = [a["role"], b["role"]]
            out["desync_pairs"].append(d)
            print("   overlap: %d steps" % d.get("overlap", 0))
            if "verdict" in d:
                print("   %s" % d["verdict"])
            if d.get("first_mismatch"):
                fm = d["first_mismatch"]
                print(
                    "   first mismatch: step %d  %s != %s"
                    % (fm["step"], fm["state_a"], fm["state_b"])
                )
                if "regions" in fm:
                    print(
                        "   diverging regions: %s"
                        % (", ".join(fm["regions"]) or "(none in per-region set)")
                    )
                    sor = fm.get("state_only_regions", [])
                    print(
                        "   -> state-only (excl ai_econ/order_pending): %s"
                        % (", ".join(sor) if sor else "NONE -- excluded-region drift only")
                    )
                print(
                    "   total mismatching steps: %d (combined-hash: %d)"
                    % (d["mismatch_count"], d["combined_mismatch_count"])
                )
            # mp:X3. The SUB-DOMAIN trail, printed BEFORE the snapshot section and after the
            # region list above, because it answers the question a reader of those two lines asks
            # next: the region list is a single step's worth, and this says when the subsystem the
            # regions belong to FIRST moved.
            sd = subdomain_compare(a["harness"], b["harness"], a["role"], b["role"])
            if sd is not None:
                d["subdomain"] = sd
                print("   sub-domain trail: %s" % sd["verdict"])
                if sd.get("partition_mismatch"):
                    print(
                        "   !! the two peers printed DIFFERENT sub-domain partitions -- their "
                        "folds are not comparable; compare the `; [subdomain]` lines"
                    )
                for dom in sorted(sd.get("domains", {})):
                    row = sd["domains"][dom]
                    if row["first_diverging_step"] is None:
                        continue
                    print(
                        "     %-10s first differs at step %d (%d step(s) differ)"
                        % (dom, row["first_diverging_step"], row["diverging_steps"])
                    )
                # ...and then the REGION inside it, off the R rows of the nearest sampled step at or
                # after the earliest divergence. Named separately because the trail cannot do it:
                # the RD fold is over a domain, so a difference in it names a domain and nothing
                # finer, by construction.
                if sd.get("earliest_step") is not None:
                    part = sd.get("partition_a") or sd.get("partition_b")
                    cand = sorted(
                        st
                        for st in (a["harness"].get("regions") or {})
                        if st >= sd["earliest_step"] and st in (b["harness"].get("regions") or {})
                    )
                    if cand:
                        named = subdomain_regions_at(a["harness"], b["harness"], cand[0], part)
                        d["subdomain_regions_step"] = cand[0]
                        d["subdomain_regions"] = named
                        print(
                            "     region(s) inside, at the nearest sampled step %d: %s"
                            % (
                                cand[0],
                                ", ".join(
                                    "%s -> %s" % (k, "/".join(v)) for k, v in sorted(named.items())
                                )
                                or "(no unexcluded region differs there)",
                            )
                        )
                    else:
                        print(
                            "     no R row at or after step %d on both peers -- raise "
                            "[harness] region_hash_step to name the region" % sd["earliest_step"]
                        )
            # mp:X3. A peer that HELD its sim said so, and the rollup has to repeat it: a held peer
            # stops emitting step rows on purpose, so every "stopped at step N" reading below is
            # about the instrument rather than about the peer.
            for pr in (a, b):
                hs = pr["harness"].get("hold_step")
                if hs is not None:
                    print(
                        "   %s HELD its sim at step %d ([harness] snapshot_hold) -- it emits no "
                        "step row after that BY DESIGN, so a short trail on this peer is the knob, "
                        "not a death" % (pr["role"], hs)
                    )
            # mp:X1b. Tried BOTH WAYS round because the pair is unordered here: whichever peer
            # captured is the one with SNAPCAP lines, and the registry does not promise which of the
            # two a `combinations()` pair puts first.
            for cp, ip in ((a, b), (b, a)):
                sc = snapshot_compare(cp["harness"], ip["harness"], cp["role"], ip["role"])
                if sc is None:
                    continue
                d["snapshot"] = sc
                _SNAPSHOT_SEEN.append(sc["verdict"])
                print("   snapshot: %s" % sc["verdict"])
                for row in sc["pairs"]:
                    print(
                        "     step %d: %s%s"
                        % (
                            row["step"],
                            row["verdict"],
                            ""
                            if not row.get("differ")
                            else "  [%s]" % ", ".join(row["differ"][:8]),
                        )
                    )
                    if "rng_state_ok" in row:
                        print(
                            "       rng_state: %s"
                            % ("IDENTICAL" if row["rng_state_ok"] else "DIFFERS")
                        )
                for ln in ip["harness"].get("snapshot_lines", []):
                    print("     %s" % ln)
                break
            bd = diff_breakdown(a["harness"], b["harness"])
            if bd:
                d["breakdown_diff"] = bd
                diff = [k for k, v in bd.items() if v == "DIFFER"]
                print(
                    "   stop-breakdown regions differing: %s"
                    % (", ".join(diff) if diff else "NONE (all match)")
                )
            pair_verdicts.append(
                (d["pair"], d.get("mismatch_count", 0), d.get("combined_mismatch_count", 0))
            )
        # keep the legacy single-pair key for existing consumers (first pair)
        out["desync"] = out["desync_pairs"][0]
        # N1 all-pairwise rollup: the run is determinism-clean iff EVERY pair matches on the state hash.
        print("\n" + "=" * 72)
        print(
            "ALL-PAIRWISE ROLLUP  (%d peers, %d pairs)" % (len(harness_peers), len(pair_verdicts))
        )
        print("=" * 72)
        # A pair that compared ZERO overlapping steps has mismatch==0 and would otherwise score "OK",
        # so a run where a peer died on launch reported "ALL PAIRS IDENTICAL" having compared nothing.
        # Observed 2026-07-25 (combined-hash=0 after an intermittent client crash). Zero comparable
        # steps is NO DATA, never a pass -- a green that cannot see its own regression is worse than a
        # red.
        worst = 0
        nodata = 0
        for pair, mm, cmb in pair_verdicts:
            worst = max(worst, mm)
            if cmb < args.min_common:
                nodata += 1
                status = "NO DATA (%d compared steps < --min-common %d)" % (cmb, args.min_common)
            else:
                status = "OK" if mm == 0 else "DESYNC"
            print(
                "   [%s vs %s]  state-mismatch steps=%d  combined-hash=%d  %s"
                % (pair[0], pair[1], mm, cmb, status)
            )
        # DET-FLAKE: did the MATCH survive long enough for any of this to mean anything? Computed
        # before the verdict is chosen, because it can both add a failure and rename one.
        fm0 = next(
            (d.get("first_mismatch") for d in out["desync_pairs"] if d.get("first_mismatch")), None
        )
        fm_step = fm0["step"] if fm0 else None
        fm_clock = (
            step_clock_seconds(harness_peers[0]["harness"], fm_step)
            if fm_step is not None
            else None
        )
        env_kind, env_reasons = environmental_verdict(harness_peers, fm_step, fm_clock)
        for r in env_reasons:
            print("   LINK/MATCH: %s" % r)
        out["environmental"] = env_kind in ("ended", "ended_explains")
        out["environmental_kind"] = env_kind
        out["environmental_reasons"] = env_reasons
        # mp:X3 / D26. MACHINE-READABLE key, kept for existing consumers (a resync trigger that wants
        # ONLY the deliberate-arm case without re-deriving the arming line). D26 ITSELF is resolved
        # (user decision, 2026-09-23): the truncation this implies is now actually APPLIED, above the
        # pairwise loop, via `expected_match_end()` / `out["d26_expected_end_step"]` -- that is the
        # authoritative D26 signal and covers BOTH expected-end shapes (this deliberate arm, and a
        # real game-over every peer reaches); this narrower flag is the deliberate-arm-only subset of
        # it, retained because X3 already publishes it and a consumer may want exactly that distinction.
        out["deliberate_match_end"] = any(
            re.search(r"gameover_step=([1-9]\d*)", ((p.get("harness") or {}).get("arming") or ""))
            for p in harness_peers
        )

        out["all_pairwise_clean"] = worst == 0 and nodata == 0 and not out["environmental"]
        out["pairs_without_data"] = nodata
        if env_kind == "ended_explains":
            verdict = (
                "FAIL: ENVIRONMENTAL (link-dead/match-ended at step %d) -- the steps compared BEFORE "
                "it were identical on every state channel, so this run did not measure determinism "
                "past that point, and the mismatches after it are two peers no longer in the same "
                "match: this is the match DYING, not the sim diverging (DET-FLAKE)." % fm_step
            )
        elif env_kind == "ended" and worst == 0 and not nodata:
            # The match died but nothing disagreed: the run FAILS anyway, because agreement over a
            # match that stopped early is not a measurement. An UNEXPLAINED mismatch never lands
            # here -- it falls through to DESYNC below, which is what fail-closed means.
            verdict = (
                "FAIL: ENVIRONMENTAL (link-dead/match-ended before the run finished) -- the peers "
                "agreed on what they did compare, but the match stopped early, so a pass would be "
                "vacuous -- the match DIED, the sim did not diverge (DET-FLAKE)."
            )
        elif nodata:
            verdict = (
                "NO COMPARABLE STEPS in %d pair(s) -- NOT clean (a peer produced no state hashes; "
                "this would otherwise be a VACUOUS pass)" % nodata
            )
        elif worst == 0:
            verdict = "ALL PAIRS IDENTICAL (determinism-clean)"
        else:
            verdict = "DESYNC in >=1 pair -- NOT clean"
        # PUBLISHED, so ui_test.py prints THIS string rather than re-deriving one from the flags.
        # It re-derived one until 2026-09-11, and the two disagreed the moment the guard arrived: a
        # run this file deliberately kept as DESYNC (evidence present but not conclusive) came back
        # from the runner as "FAIL: ENVIRONMENTAL", i.e. the fail-closed rule was undone one layer up.
        out["verdict"] = verdict
        print("   VERDICT: %s" % verdict)

        # D21: the IN-BAND verdict, alongside this analyzer's, and DELIBERATELY NOT FOLDED INTO IT.
        # They answer different questions -- "did the two sims agree" (here, offline, over every step)
        # versus "did the game notice at the time" -- so a disagreement between them is a finding
        # about the DETECTOR, and folding them would destroy exactly that signal. The run is marked
        # INVALID if either says so.
        watchers = [(p["role"], p.get("desync_watch") or {}) for p in out["peers"]]
        armed = [(r, d) for r, d in watchers if d.get("armed")]
        reported = [(r, d) for r, d in armed if d.get("mismatch_lines")]
        out["inband_desync_peers"] = [r for r, _ in reported]
        out["inband_desync"] = bool(reported)
        out["run_invalid"] = bool(reported) or worst != 0 or bool(nodata) or out["environmental"]
        if not armed:
            print(
                "   IN-BAND: no peer had the runtime desync watch armed (pre-D21 build, or "
                "[desync] enabled=0) -- this run has no in-game verdict, only the offline one"
            )
        elif reported:
            for role, d in reported:
                print(
                    "   IN-BAND: [%s] REPORTED a desync in-game -- %s" % (role, d["first_mismatch"])
                )
        else:
            comp = [desync_counts(d.get("last_status")).get("compared", 0) for _, d in armed]
            print(
                "   IN-BAND: no peer reported a desync in-game (%s compared sample(s) across %d "
                "armed peer(s))" % ("/".join(str(c) for c in comp), len(armed))
            )
        # The one combination worth shouting about: the offline oracle found a divergence that the
        # live detector slept through. That is a hole in D21, not in the run.
        if worst != 0 and armed and not reported and not out["environmental"]:
            print(
                "   IN-BAND: *** the offline oracle found a desync that NO peer reported live -- "
                "that is a gap in the runtime detector, not in this analysis"
            )
        elif worst != 0 and armed and not reported:
            # DET-FLAKE / G158: this accusation was made once and was FALSE. When the match has
            # ended, the in-band watch has no counterpart left to compare against -- the sample
            # counts say so themselves (149 compared in the 2026-09-09 red vs 350 and 349 in each
            # green re-run of the same commit). A detector cannot be shown blind by a run in which
            # its input no longer exists.
            print(
                "   IN-BAND: the live comparison stopped when the match ended -- the samples taken "
                "while both peers were still in it were clean. This is NOT evidence of a detector "
                "gap (mutation-proven separately: see DET-FLAKE)"
            )
        print("   RUN VALID: %s" % ("NO" if out["run_invalid"] else "yes"))

    json_path = args.json or os.path.join(
        args.paths[0] if os.path.isdir(args.paths[0]) else os.path.dirname(args.paths[0]) or ".",
        "mp_analyze.json",
    )
    with open(json_path, "w") as f:
        json.dump(out, f, indent=2)
    print("\nJSON: %s" % json_path)
    # THE EXIT CODE IS THE VERDICT (G269). Until 2026-09-22 this function fell off the end -- 0 on a
    # DESYNC, 0 on NO COMPARABLE STEPS, 0 on FAIL: ENVIRONMENTAL -- and every
    # `post_check: ["tools/mp_analyze.py"]` row in tools/test_ui.py (d25_buildclick, u39_diplomacy,
    # ...) gated on `returncode == 0`, i.e. on nothing. ui_test.py / det_run_report read the JSON and
    # are unaffected; mp_run.py ignores the code. A single-peer run (no pair compared) stays 0: it
    # made no cross-peer claim.
    return 1 if out.get("run_invalid") else 0


if __name__ == "__main__":
    # sys.exit, so --selftest's failure count reaches the shell (and lint_repo) instead of being
    # printed and discarded.
    sys.exit(main() or 0)
