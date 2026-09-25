//
// sim/sim_step.cpp -- see sim_step.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_sim_step_0043f512.asm), using the Ghidra .c draft's PLATE as a phase map (it
// is an exhaustive, already-correct behavioral summary -- read it first) but re-deriving every line
// of the BODY against the raw instruction sequence rather than trusting the decompile, per the
// translator brief and this unit's own hazard note about this being the domain root.
//
#include "sim/sim_step.h"

#include "addr/mh_calls.gen.h"   // typed callables for the original functions we still call OUT to
#include "addr/mh_export.gen.h"  // MH_EXPORT_REPLACE -- the generated entry thunk the harness rebinds to
#include "ai/ai_state.h"         // ai_say -- the shared trace sink (non-vacuity milestones), not AI state
#include "sim/sim_event_codes.h" // EVENT_INFO_REFRESH -- shared across sim/ TUs
#include "addr/mh_rebind.gen.h"  // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

// LIB-SPINE-API: the REPLAY SPINE's C facade, by relative path exactly as state/spine.cpp and
// state/host_bind.h reach it -- libmh/include is on no module's include path, and adding it for one
// TU would be a silent layering change. Declares libmh_sim_step, the entry promoted_arm::sim_step
// routes the game's outer crossing through. (The per-body inbound surface is libmh_host_in.h, which
// the six promote TUs include; the spine lives in libmh.h and is a different header by design.)
#include "../../libmh/include/libmh.h"

#include <cfloat> // _controlfp_s -- CRT-X87-CPP step 1's control-word read, see promoted_arm

namespace mh::sim {

// ---- locally-duplicated domain constants ---------------------------------------------------------
//
// This codebase's established pattern for a domain that has a real Ghidra enum/field-comment
// backing it but no *generated* C++ enum: declare a local, file-scoped copy citing the source, rather
// than inventing a new shared header (translator brief 17a: "no enum exists -> declare a need", but
// these three ALREADY have a prior citation elsewhere in libmh/sim/, so this is "another independent
// copy of the same constant", the same posture sim_combat_credit_planet_conquest_kills.h's own
// STRAT_PLAYER_STATUS_ALIVE comment documents -- not a fresh discovery, and not #include'd from those
// files directly to avoid pulling their large, unrelated per-function declarations into this TU).

// llm_strat_player_profile::status_flags bit 0x2 -- ALIVE. mh_structs.gen.h's own field comment on
// status_flags cites THIS function's TEST at 0x43f580 by address as the bit's SIM-OWNED gate, so this
// is a hand-confirmed reuse. Independent copies: sim_combat_credit_planet_conquest_kills.h's
// STRAT_PLAYER_STATUS_ALIVE, ai/ai_state.h's PLAYER_STATUS_ALIVE, orders/order_queue.h's PLAYER_ALIVE,
// lockstep/turn_engine.h's PLAYER_ALIVE.
inline constexpr uint32_t STRAT_PLAYER_STATUS_ALIVE = 0x2u;

// cfg_enum_E_BUILDING members the sub-tick-A dead-docked-purge scan tests (0x0043f7f7-0x0043fa38) --
// the twelve "has a storage/dock roster that could hold a now-dead unit" building types. Values read
// directly off the .asm's CMP-byte immediates; cross-checked against sim_order_enqueue.h's own
// BUILDING_TYPE_* table (same Ghidra enum dump, same values) -- another independent copy of that
// table, not a new discovery.
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_H_BARRACKS = 0x1b;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_H_GARAGE   = 0x1c;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_H_AIRFIELD = 0x1d;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_H_HELIPAD  = 0x1e;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_H_PORT     = 0x20;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_H_SHUTTLE  = 0x21;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_A_BARRAKS  = 0x07;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_A_GARAGE   = 0x08;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_A_AIRFIELD = 0x09;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_A_HELIPAD  = 0x0a;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_A_PORT     = 0x0c;
inline constexpr uint8_t SIM_STEP_BUILDING_TYPE_A_SHUTTLE  = 0x0d;

// game::e::event member 8 -- BUILD_UNITS_REFRESH (fired after the per-building tick loop, per the
// change_flag/change_flag2 dirty pair). sim_event_codes.h only hoists member 6 so far
// (EVENT_INFO_REFRESH, reused below for the per-unit loop's own notify); member 8's one prior site is
// sim_bldg_notify_state_change.h's EVENT_BUILD_UNITS_REFRESH -- another independent copy of that,
// same reasoning as the building-type table above.
inline constexpr uint32_t SIM_STEP_EVENT_BUILD_UNITS_REFRESH = 8;

const sim_step_calls &live_sim_step_calls() {
    static const sim_step_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_players_tick),
        MH_LIBMH_BIND(llm_strat_order_queue_dispatch),
        MH_LIBMH_BIND(llm_strat_power_recompute),
        MH_LIBMH_BIND(llm_strat_check_population_change),
        MH_LIBMH_BIND(llm_strat_storage_purge_dead_docked),
        MH_LIBMH_BIND(llm_strat_check_storage_overflow),
        MH_LIBMH_BIND(llm_strat_production_complete),
        MH_LIBMH_BIND(llm_strat_unit_tick),
        MH_LIBMH_BIND(llm_strat_building_tick),
        MH_LIBMH_BIND(llm_strat_projectile_tick),
        MH_LIBMH_BIND(llm_strat_fx_anim_tick),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (ST0-in/ST0-out, x87-register-only -- not marshallable through
// mh_calls.gen.h's ordinary parameter-passing helpers, per `MH_UNAVAILABLE__parameter_storage_not_
// marshallable`). INLINED per this subsystem's established rule -- precedent: sim_bldg_find_
// mothership_position.cpp / sim_unit_population_remove.cpp, both of which reproduce the identical
// instruction sequence as their own per-TU copy rather than a new shared helper (the established
// per-TU-copy convention this file follows for the exact same reason). FISTP width confirmed 32-bit
// AT THIS CALL SITE (opcode bytes `db 5d d8` @0x0043f6d7 -> 0xDB ModRM 0x5D, reg field 3 => 0xDB /3 ==
// FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

void sim_step(const sim_view &v, sim_store &own, const sim_step_calls &c) {
    // ---- HEAD GATES (0x0043f52a-0x0043f552) ----------------------------------------------------
    if (*v.ai_enabled != 0) {                  // DECLARED NEED: v.ai_enabled -- see sim_step.h
        c.ai_players_tick(*v.game_time_delta); // DECLARED NEED: v.game_time_delta
    }
    if (own.order_count() != 0) {
        c.order_queue_dispatch();
    }

    // ---- PER-PLAYER LOOP (0x0043f552-0x0043fd7f), player = 0..MAX_PLAYERS-1 --------------------
    //
    // CUR_PLAYER is the ambient global that drives this loop in the original (`CMP word[CUR_PLAYER],
    // 8` at the top of every pass, `INC word[CUR_PLAYER]` at the bottom) -- reproduced with a local
    // loop variable mirrored into the real memory slot at the TOP of every iteration (matching the
    // original's own per-iteration read-fresh-from-memory behaviour), because callees below (check_
    // population_change / check_storage_overflow / storage_purge_dead_docked / production_complete)
    // are original, not-yet-translated functions that may read CUR_PLAYER ambiently in addition to
    // their explicit `player` argument -- not observable from this function alone. See uncertainties.
    //
    // OPEN (reimpl-verify, 2026-08-16, unresolved suspicion, not a confirmed divergence): this covers
    // the READ direction only. If some callee reached from this loop's body WRITES CUR_PLAYER as a
    // side effect, the original's own later-in-iteration address math (which re-reads the memory cell
    // fresh at nearly every access, per the ASM) would observe that mutation for the rest of the same
    // iteration, while this draft keeps using its unchanged local `player` until the next loop-top
    // set_cur_player() call. None of the six functions called directly from this body write it; a
    // transitive/deeper writer was not ruled out. Grep the closure before treating this loop as fully
    // settled.
    for (uint16_t player = 0; player < MAX_PLAYERS; ++player) {
        own.set_cur_player(player); // DECLARED NEED: sim_step.h (3)

        if ((v.profiles[player].status_flags & STRAT_PLAYER_STATUS_ALIVE) == 0) continue;

        // ---- STAT ROLLOVER (0x0043f589-0x0043f629): housing_prev = housing_accum; the four
        // housing-cap latches; the ten storage-cap latches. ----
        pop_stats &pop   = own.population_at(player);
        pop.housing_prev = pop.housing_accum;

        housing_stats &hs    = own.unit_housing_at(player);
        hs.cap_prev_vehicles = hs.cap_accum_vehicles;
        hs.cap_prev_soldiers = hs.cap_accum_soldiers;
        hs.cap_prev_planes   = hs.cap_accum_planes;
        hs.cap_prev_helis    = hs.cap_accum_helis;

        storage_stats &ss = own.storage_stats_at(player); // DECLARED NEED: sim_step.h (9)
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            ss.cap_prev[i] = ss.cap_accum[i];
        }

        // ---- POWER RECOMPUTE ON DELTA (0x0043f672-0x0043f6c2) ----
        power_stats &ps = own.power_stats_at(player);
        if (ps.generated != ps.prev_generated || ps.consumed != ps.prev_consumed) {
            c.power_recompute(player);
        }

        // ---- pop_total = trunc(pop_fraction) (0x0043f6c9-0x0043f6ed) ----
        pop.pop_total = trunc_to_int32(pop.pop_fraction);

        // ---- SUB-TICK A: population change + dead-docked purge (0x0043f6f7-0x0043fa5b) ---------
        //
        // LOOP CONDITION -- the ORDERED idiom, NOT the NaN-continuing one sim_unit_tick.cpp's own
        // loops use: the .asm's FCOMP/FNSTSW/SAHF/JC sequence (0x0043f706-0x0043f712) computes
        // ST0(delta_a) vs SUBTICK_A_PERIOD and exits (JC taken) on BOTH "delta_a < SUBTICK_A_PERIOD"
        // (C0=1) AND unordered/NaN (C0=1 there too). So the loop body runs only for ORDERED
        // "delta_a >= SUBTICK_A_PERIOD" -- plain C++ `SUBTICK_A_PERIOD <= delta_a` already expresses
        // exactly that (false on NaN), no special idiom needed. This is the OPPOSITE FP shape from
        // unit_tick's JNC-based loops; see uncertainties for why both are believed correctly derived.
        double delta_a = *v.game_clock - pop.subtick_a_clock;
        while (*v.subtick_a_period <= delta_a) { // DECLARED NEED: v.subtick_a_period
            c.check_population_change(player);
            pop.subtick_a_clock += *v.subtick_a_period_pos; // DECLARED NEED: v.subtick_a_period_pos
            delta_a += *v.subtick_a_period_neg;             // DECLARED NEED: v.subtick_a_period_neg

            // -- the dead-docked purge scan (0x0043f74c-0x0043fa56): CUR_INDEX/CUR_BUILDING walk
            // buildings[player][1..] in lockstep, bounded by buildings[player][0].index (the
            // sentinel/count slot -- MOVZX-read, i.e. ZERO-extended even though the field is
            // declared int16_t) with NO upper array-index bound -- the original trusts the sentinel
            // count exactly, the same idiom sim_bldg_find_mothership_position.cpp's headcount walk
            // already documents for a different roster. Reassembled from the asm's test-at-top/
            // increment-at-bottom shape (prologue pre-sets slot 1; every iteration's bottom advances
            // to the next slot before the next test) into an equivalent C++ loop -- see
            // uncertainties for the control-flow-reassembly note. The type-dispatch cascade below is
            // likewise reassembled from a chain of pairwise JZ/JMP short-circuits into one flat OR --
            // provably equivalent since every arm is an independent, side-effect-free equality test
            // on the same `type` byte (see uncertainties).
            own.set_cur_index(1);                                  // DECLARED NEED: sim_step.h (4)
            own.set_cur_building_ptr(&own.building_at(player, 1)); // DECLARED NEED: sim_step.h (6)
            uint32_t remaining =
                static_cast<uint32_t>(static_cast<uint16_t>(building_of(v, player, 0).index));
            int32_t slot = 1;
            while (remaining != 0) {
                building &b = own.building_at(player, slot);
                if (b.building_id != 0) {
                    --remaining;
                    if (b.online_state != 0) {
                        const uint8_t type = v.cfg_buildings[b.building_id].type;
                        if (type == SIM_STEP_BUILDING_TYPE_H_GARAGE ||
                            type == SIM_STEP_BUILDING_TYPE_H_AIRFIELD ||
                            type == SIM_STEP_BUILDING_TYPE_H_HELIPAD ||
                            type == SIM_STEP_BUILDING_TYPE_H_BARRACKS ||
                            type == SIM_STEP_BUILDING_TYPE_H_PORT ||
                            type == SIM_STEP_BUILDING_TYPE_H_SHUTTLE ||
                            type == SIM_STEP_BUILDING_TYPE_A_GARAGE ||
                            type == SIM_STEP_BUILDING_TYPE_A_AIRFIELD ||
                            type == SIM_STEP_BUILDING_TYPE_A_HELIPAD ||
                            type == SIM_STEP_BUILDING_TYPE_A_BARRAKS ||
                            type == SIM_STEP_BUILDING_TYPE_A_SHUTTLE ||
                            type == SIM_STEP_BUILDING_TYPE_A_PORT) {
                            c.storage_purge_dead_docked(static_cast<int32_t>(player), b.sub_id);
                        }
                    }
                }
                ++slot;
                own.set_cur_index(static_cast<uint16_t>(slot));
                own.set_cur_building_ptr(&own.building_at(player, slot));
            }
        }

        // ---- SUB-TICK B: storage overflow (0x0043fa60-0x0043fb73) ----
        // Same ORDERED-idiom loop shape as sub-tick A above (JC-based exit -- see uncertainties).
        double delta_b = *v.game_clock - ss.subtick_b_clock;
        while (*v.subtick_b_period <= delta_b) {           // DECLARED NEED: v.subtick_b_period
            ss.subtick_b_clock += *v.subtick_b_period_pos; // DECLARED NEED: v.subtick_b_period_pos
            c.check_storage_overflow(player);
            delta_b += *v.subtick_b_period_neg; // DECLARED NEED: v.subtick_b_period_neg
        }

        // ---- PER-SECOND PRODUCTION-READY SCAN (0x0043fabd-0x0043fb73), gated on SIM_ACTIVE -------
        //
        // LOOP CONDITION -- the OTHER (NaN-continuing) idiom, matching unit_tick's own shape here:
        // `FLD1; FCOMP delta_p; FNSTSW; SAHF; JA exit` exits only on ORDERED "1.0 > delta_p" (JA
        // needs CF=0 AND ZF=0, both false on an unordered compare). So the loop body runs on ORDERED
        // "delta_p >= 1.0" OR unordered (NaN). Plain C++ `!(1.0 > delta_p)` reproduces this exactly
        // with no isnan() call needed: an ordered `>` compare is itself false for NaN, so its
        // negation is true for NaN too -- see uncertainties. The 1.0 threshold itself is a literal
        // FLD1 immediate baked into the instruction stream, not a global read; only the per-iteration
        // decrement is a global (PROD_CHECK_PERIOD_NEG).
        if (*v.sim_active != 0) {
            player_profile &prof    = own.profile_at(player);
            double          delta_p = *v.game_clock - prof.prod_check_clock;
            while (!(1.0 > delta_p)) {
                for (int32_t slot = 1; slot < PROD_SHUTTLE_SLOTS_PER_PLAYER; ++slot) {
                    if (v.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot].status ==
                        200) {
                        c.production_complete(player, static_cast<uint32_t>(slot), delta_p);
                    }
                }
                prof.prod_check_clock += 1.0;
                delta_p += *v.prod_check_period_neg; // DECLARED NEED: v.prod_check_period_neg
            }
        }

        // ---- TICK-END RESET (0x0043fb73-0x0043fc27): zero the accumulators the rollover above
        // just latched away. ----
        ps.generated          = 0;
        ps.consumed           = 0;
        pop.colony_hp_sum     = 0;
        pop.colony_hp_max_sum = 0;
        pop.housing_accum     = 0;
        hs.cap_accum_vehicles = 0;
        hs.cap_accum_soldiers = 0;
        hs.cap_accum_planes   = 0;
        hs.cap_accum_helis    = 0;
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            ss.cap_accum[i] = 0;
        }

        // ---- PER-UNIT TICK LOOP (0x0043fc5c-0x0043fceb) --------------------------------------
        // Same walk shape as the buildings purge scan above: units[player][1..], bounded by
        // units[player][0].unit_above read as a WORD -- the field is `uint8_t unit_above[2]`
        // (`map_t_unit_full_id`, a packed {id, owner} pair), and the sentinel-count reinterpretation
        // of it as one 16-bit value is the SAME idiom sim_bldg_unmap_footprint.cpp already
        // established for this exact field (`(uint32_t)a[0] | ((uint32_t)a[1] << 8)`, bit-identical
        // to the asm's own WORD-sized MOVZX read). The per-slot gate, by contrast, IS a plain 16-bit
        // field (unit_proto_id, offset+2) -- no packed-word handling needed there.
        {
            own.change_flag()  = 0;
            own.change_flag2() = 0;
            own.set_cur_index(1);
            own.set_cur_unit_ptr(&own.unit_at(player, 1)); // DECLARED NEED: sim_step.h (5)
            const unit &sentinel    = unit_of(v, player, 0);
            uint32_t    remaining_u = static_cast<uint32_t>(sentinel.unit_above[0]) |
                                   (static_cast<uint32_t>(sentinel.unit_above[1]) << 8);
            int32_t slot = 1;
            while (remaining_u != 0) {
                unit &u = own.unit_at(player, slot);
                if (u.unit_proto_id != 0) {
                    c.unit_tick();
                    --remaining_u;
                }
                ++slot;
                own.set_cur_index(static_cast<uint16_t>(slot));
                own.set_cur_unit_ptr(&own.unit_at(player, slot));
            }
        }
        if (own.change_flag() != 0 || own.change_flag2() != 0) {
            // `general._36_4_ != 0` in the Ghidra draft reads change_flag/change_flag2 TOGETHER as
            // one dword; this OR of the two int16 fields is bit-identical for a !=0/==0 test (the
            // dword is zero iff both halves are the zero bit pattern) -- see uncertainties.
            c.set_event(EVENT_INFO_REFRESH);
        }

        // ---- PER-BUILDING TICK LOOP (0x0043fceb-0x0043fd7a) -----------------------------------
        // Same walk shape as the sub-tick-A purge scan, reusing the SAME roster/sentinel field
        // (buildings[player][0].index) and a FRESH change_flag/change_flag2 reset (the SECOND reset
        // of this tick, distinct from the per-unit loop's own reset above). No online_state gate
        // here, unlike the purge scan -- only building_id, matching the asm exactly.
        {
            own.change_flag()  = 0;
            own.change_flag2() = 0;
            own.set_cur_index(1);
            own.set_cur_building_ptr(&own.building_at(player, 1));
            uint32_t remaining_b =
                static_cast<uint32_t>(static_cast<uint16_t>(building_of(v, player, 0).index));
            int32_t slot = 1;
            while (remaining_b != 0) {
                building &b = own.building_at(player, slot);
                if (b.building_id != 0) {
                    c.building_tick();
                    --remaining_b;
                }
                ++slot;
                own.set_cur_index(static_cast<uint16_t>(slot));
                own.set_cur_building_ptr(&own.building_at(player, slot));
            }
        }
        if (own.change_flag() != 0 || own.change_flag2() != 0) {
            c.set_event(SIM_STEP_EVENT_BUILD_UNITS_REFRESH);
        }
    }
    // reimpl-verify caught this (2026-08-16): the original's per-player loop is a TOP-tested,
    // TAIL-incremented `while`, so the memory-backed CUR_PLAYER cell (0x00e58144) is bumped to 8
    // (one past the last valid index) by the failing 8th retest's own increment BEFORE the loop
    // exits -- 0x0043f56a `INC word[0xe58144]` unconditionally follows every body pass, including
    // player=7's, and only THEN does the retest fail and fall through. A C++ `for` loop's `++player`
    // that produced the exit condition has no corresponding store, so the real slot stayed at 7
    // (the last value set_cur_player() actually wrote) without this line -- observable during the
    // trailing projectile-pool/fx-anim-pool walks below and by the NEXT sim_step call's own head
    // gates (ai_players_tick/order_queue_dispatch), which run before that call's own reset to 0.
    own.set_cur_player(MAX_PLAYERS);

    // ---- GLOBAL WALK: the strategic projectile pool (0x0043fd7f-0x0043fdc9), OUTSIDE the
    // per-player loop. Slot [0]'s `.active` doubles as the pool's live-count (struct's own field
    // comment). Same sentinel-count-with-no-upper-bound walk shape as the roster loops above; the
    // asm's starting pointer is a compile-time-constant literal (&pool[1], folded by the original
    // compiler since the pool's base is itself a fixed address) -- reproduced here via the
    // region-registry-bound accessor rather than a literal VA, per Law 1. ----
    {
        own.set_cur_index(1);
        own.set_cur_projectile_ptr(&own.projectile_pool_at(1)); // DECLARED NEED: sim_step.h (7)
        int32_t remaining_p = v.projectile_pool[0].active;
        int32_t slot        = 1;
        while (remaining_p != 0) {
            projectile &pr = own.projectile_pool_at(slot);
            if (pr.active != 0) {
                c.projectile_tick();
                --remaining_p;
            }
            ++slot;
            own.set_cur_index(static_cast<uint16_t>(slot));
            own.set_cur_projectile_ptr(&own.projectile_pool_at(slot));
        }
    }

    // ---- GLOBAL WALK: the fx-anim pool (0x0043fdc9-0x0043fe14), OUTSIDE the per-player loop.
    // Slot [0]'s `.live` doubles as the pool's live-count, same convention as the projectile pool
    // above (a different field OFFSET though -- live is at +0x5, not +0x0, so CUR_FX_ANIM itself
    // points at the struct base, unlike CUR_PROJECTILE which happens to point straight at `.active`
    // since that field is at +0x0). ----
    {
        own.set_cur_index(1);
        own.set_cur_fx_anim_ptr(&own.fx_anim_pool_at(1)); // DECLARED NEED: sim_step.h (8)
        int32_t remaining_f = v.fx_anim_pool[0].live;
        int32_t slot        = 1;
        while (remaining_f != 0) {
            fx_anim &fx = own.fx_anim_pool_at(slot);
            if (fx.live != 0) {
                c.fx_anim_tick();
                --remaining_f;
            }
            ++slot;
            own.set_cur_index(static_cast<uint16_t>(slot));
            own.set_cur_fx_anim_ptr(&own.fx_anim_pool_at(slot));
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

namespace promoted_arm {
namespace {
extern void (*g_pre_hook)(); // defined below, with the block comment that explains it
}
} // namespace promoted_arm

// THE PRE-HOOK FIRES HERE, IN THE BODY, NOT IN THE ENTRY WRAPPER (mp:RM1, 2026-09-21). It used to sit
// at the top of promoted_arm::sim_step (the __watcall entry the game VA jumps to), which is exactly
// one of this function's TWO routes in. The other is the REBIND: with `[rebind] llm_strat_sim_step
// -> OURS` armed -- the ship configuration -- mh::lockstep's promoted sim_tick calls THIS function
// directly (turn_engine.cpp live_calls: MH_LIBMH_BIND resolves to &::mh::sim::sim_step), never
// touching the entry wrapper. So in every rig run with libmh bound the D21 desync sampler was
// "CHAINED onto the PROMOTED root", printed its ARMED + cost-probe lines, and sampled NOTHING for the
// whole run -- no `first sample sent`, no rollup, no verdict -- while the field's configuration (1)
// (no libmh.dll, the trampoline route) sampled every 50 steps. G68's shape a third time, and the
// reason `[desync] step N reached the sampling cadence but NO sample was taken` now exists
// (desync_watch.cpp). Firing at the top of the BODY makes the sampling point route-independent:
// entry route -> wrapper -> body (once), rebind route -> body (once). The wrapper keeps its
// served-count ladder, which is about the ENTRY being reached and stays true to that.
void sim_step() {
    if (promoted_arm::g_pre_hook) promoted_arm::g_pre_hook();
    sim_state st = state();
    detail::sim_step(st.read, st.own, live_sim_step_calls());
}

// ---- the PROMOTED arm (the domain root's oracle -- G13's answer, HARNESS-MEDIATED per C6) ----------
//
// The determinism harness owns llm_strat_sim_step's entry (its per-step golden-hash + the SIM-CUT
// exactly-once probe live in the detour), so this promotion is installed by REBINDING the harness's
// fall-through onto our entry thunk, NOT by MH_EXPORT_REPLACE-ing the entry (which the harness refuses,
// "already hooked"). Under `[promote] sim_step=1` the detour runs promoted_arm::sim_step in place of the
// original body, after on_sim_step hashed the pre-body state; the seven+ callees stay ORIGINAL (via
// mh::call::), so the per-step golden trajectory verifies OUR tick LOGIC in composition over thousands
// of steps -- the only oracle the domain root admits. Anonymous-namespace
// INTERNAL linkage: promoted_arm::active/mark_installed already exist EXTERNAL in sim_order_dispatch /
// sim_unit_tick, so a third external pair would ODR-collide; the MH_EXPORT_REPLACE macro still reaches
// promoted_arm::sim_step (the anon namespace is transparent within this TU).
namespace promoted_arm {
namespace {
volatile long g_step_calls = 0;

// ROOTS-LIVE (2026-09-04): ONE OWNER, ONE INSTALL. There are now two routes onto this entry -- the
// harness rebind and, when no harness armed, the direct entry patch -- and they must never both take.
// The flag is what makes "installed once" a property of the code rather than of reimpl_probe's
// if/else staying written the way it is today. hook/promoted.h's entry_owner_of would refuse the
// second install anyway, but it would refuse it as a LOG LINE in a 2 MB log (U30's whole lesson), and
// a root that silently ran through a refusal is exactly the G106 shape this item exists to end.
bool g_promoted = false;

// mp:D32. WHICH route set g_promoted, kept separately because the two routes are fed differently: the
// rebind route leaves the harness's own detour owning the real entry (it calls
// mh::desync::on_sim_step_hashed unconditionally before ever branching to us), while the direct-install
// route leaves this entry with no other feeder at all. set_sim_step_pre_hook() below reads this to
// refuse the rebind case -- see the D32 comment there for the failure this prevents.
bool g_promoted_via_rebind = false;

// The PRE-HOOK, and it exists because promoting this root took an instrument's hook away.
//
// llm_strat_sim_step's entry is the D21 desync sampler's only sampling point in a no-harness run
// (seams/net_seams.cpp install_desync_watch: "no determinism harness this run, so this is the
// sampling point"), and `[desync] enabled` defaults to 1 -- so the ship configuration samples there.
// Once ROOTS-LIVE made the root promote by DIRECT ENTRY INSTALL, that entry is ours and the
// sampler's exclusive trampoline is refused: measured 2026-09-04, the detector armed, printed its
// cost probe, and then sampled NOTHING for the whole run while its own log line blamed a harness that
// was not present. That is G68's shape exactly.
//
// So the seam layer hands its callback in and the promoted arm runs it first -- the same "on_sim_step
// still runs first" contract the harness rebind has always had, reconstructed for the route that
// replaced it. A function POINTER rather than an include because a reimplementation TU may not reach
// a seams header (the layering lint), which is the same reason the lib_trans frame pair takes its two
// chain hooks as parameters.
void (*g_pre_hook)() = nullptr;

// CRT-X87-CPP step 1: THE GATING MEASUREMENT, read HERE and not at arm time.
//
// mh/fp/'s replacement candidates divide into "agrees at any precision control" and "agrees only at
// PC=53", and which pile a helper lands in decides whether it can be C++ at all -- so the item is
// blocked on what the control word ACTUALLY says inside the strategic sim, in the configuration that
// SHIPS. Two things were known and neither answers that: `_control87` has zero xrefs in mh.exe (the
// game never sets the word, it inherits Watcom's startup value), and harness.cpp's `pin_fpu` installs
// PC=53 at arm time. Whether the pin still holds this deep, and what runs when there is no pin at
// all, was never read back -- the same "read the arming state out of the DLL's own banner" rule the
// tactical `TACT CLOCK fpu=` line already follows on the other mode's path.
//
// THIS LINE IS DELIBERATELY NOT HARNESS-GATED. `MH_Harness_Init` returns at its no-ini gate BEFORE it
// would pin anything, so a harness-gated probe cannot observe the shipping path by construction --
// it would only ever report the pinned value back to itself. This wrapper, by contrast, runs in EVERY
// configuration: the promoted body owns the root by direct entry install with no ini (ROOTS-LIVE),
// and by harness rebind when there is one. Riding the existing non-vacuity ladder rather than adding
// a cadence keeps it to five lines a run and makes DRIFT visible -- a word that changes between call
// #1 and call #100000 is a different finding from one that was never right.
//
// `_controlfp_s` rather than `fnstcw`, so this stays plain C in a translated TU; fptest and
// crt_vendor_selftest both assert the two agree (`((fnstcw >> 8) & 3) == 2` right after
// `_controlfp_s(_PC_53)`), so the abstraction is pinned elsewhere and not taken on trust here.
const char *x87_precision_note() {
    unsigned cur = 0;
    if (_controlfp_s(&cur, 0, 0) != 0) return " (x87 PC=UNREADABLE)";
    switch (cur & _MCW_PC) {
        case _PC_53: return " (x87 PC=53)";
        case _PC_64: return " (x87 PC=64)";
        case _PC_24: return " (x87 PC=24)";
        default: return " (x87 PC=?)";
    }
}

void sim_step() {
    // The D21 pre-hook used to fire here; it fires in mh::sim::sim_step (the body) since mp:RM1, so
    // the rebind route -- which enters the body without passing this wrapper -- samples too.
    // NON-VACUITY (gates can pass vacuously): a golden that "stayed identical" proves
    // nothing unless our body actually ran. Log the first call + widening milestones so a run can state
    // in writing that the harness detour served real time-ticks through OUR domain root.
    // ROOTS-LIVE widened the ladder to #100/#10000: in SHIP config there is no harness stop step to
    // print a final count from, so the milestones ARE the served-count evidence, and a ladder whose
    // second rung is 1000 leaves a short UI scenario able only to say "at least one".
    const long c = ++g_step_calls;
    if (c == 1 || c == 100 || c == 1000 || c == 10000 || c == 100000)
        mh::ai::ai_say("; [promote] sim_step body served call #%ld from game code%s\n", c,
                       x87_precision_note());
    // LIB-SPINE-API: THE BROKERED CROSSING. Through the ABI entry, not straight into the C++ body.
    //
    // This is the OUTER crossing -- the game's entry into the sim -- and it is the one the
    // `brokered` configuration exists to prove. The hosted frame drive is the ORIGINAL
    // llm_strat_sim_tick (it is deliberately NOT promoted: the determinism harness owns its entry,
    // turn_engine.h C6, so it sits outside the default [promote] lockstep closure), which calls
    // llm_strat_sim_step at its VA; the harness detour -- or, harness-off, the ROOTS-LIVE direct
    // entry install -- lands here. So routing this one line is what makes every A/B scenario and
    // the determinism run exercise libmh_sim_step on the real game with its real world, instead of
    // leaving that entry to the standalone replay alone.
    //
    // NO RECURSION, and the distinction is the whole reason the spine rows were excepted at
    // LIB-REF-IN. libmh_sim_step is a thin forward into this same mh::sim::sim_step() -- 1:1, no
    // logic (state/spine.cpp says why an entry that computed anything would be a second
    // implementation verified by nobody). The recursion hazard belongs to the OTHER three rows
    // disposition'd onto this entry -- advisor_tick, invasion_alert_poll, invasion_due_check -- which
    // are llm_strat_sim_tick's TAIL, not the step; a seam of theirs calling libmh_sim_step would
    // re-enter the whole sim. They are adjudicated measured-impossible (driver-absorbed) instead.
    //
    // The liveness line above stays BEFORE the entry call, the sim_hostreach_promote.cpp precedent:
    // a run that enters the seam prints it whatever the entry does.
    ::libmh_sim_step();
}
} // namespace
} // namespace promoted_arm

} // namespace mh::sim

// Generates mh_export_thunk_llm_strat_sim_step (the __watcall(void) entry thunk the harness rebinds to)
// AND mh_export_install_llm_strat_sim_step (unused here -- the harness owns the entry). Declaring it also
// puts llm_strat_sim_step into gen_dll_patches.py's promotable set, so the C1 interlock covers any byte
// patch aimed inside its body. Taking the thunk's address installs nothing.
MH_EXPORT_REPLACE(llm_strat_sim_step, mh::sim::promoted_arm::sim_step)

namespace mh::sim {

// The entry thunk for a promoted llm_strat_sim_step, for the determinism harness's detour to fall
// through to (C6) instead of the stolen-prologue trampoline. Taking its address installs nothing;
// reimpl_probe hands it to MH_Harness_RebindSimStep. Mirrors mh::lockstep::sim_tick_entry_thunk().
void *sim_step_entry_thunk() {
#ifdef MH_LIBMH_BUILD
    return nullptr; // LIB-REF-SPLIT: no harness detour standalone, so no thunk is generated
#else
    return (void *)mh_export_thunk_llm_strat_sim_step;
#endif
}

// ---- ROOTS-LIVE (2026-09-04): the two install ROUTES, and why the second one now exists -----------
//
// Until today this root was promoted ONLY by rebinding the determinism harness's detour. The stated
// reason (reimpl_probe.cpp, since deleted) was that a no-harness run "has nothing to compare and
// promoting would just run our body untested". That treats the A/B as a RUNTIME MONITOR when it is a
// gate-time verification instrument -- and the consequence was measured: with no harness armed the
// ORIGINAL root ran, so every row reachable only through it was inert in 17 of 18 UI scenarios and in
// ordinary gameplay. SIM1-P closed over 4 live rows out of 530 for exactly this reason
// (equivalence cannot see COVERAGE). The replacement policy -- what stands in for the harness trajectory when
// nothing is armed -- is the reimplementation plan section 3, "The ship-config oracle"; it is a COMPOSITION,
// not a single instrument, and it is what this route is authorised by.
//
// TWO ROUTES, ONE OWNER. The harness detour keeps the entry whenever it armed (it carries the
// per-step golden hash + the SIM-CUT probe, and if the promotion won that race the INSTRUMENT would
// be the thing that refused -- a determinism run silently VOIDED rather than failed). Only when no
// detour owns the entry is the direct patch legitimate, which is the same rule and the same order
// sim_tick has used since C6 (seams/net_lockstep.cpp install_sim_tick_promotion).

// The direct entry patch. Refuses if this run already promoted the root by either route. Returns 1 if
// ours now owns the entry. mh_export_install_* is `static` in this TU (the macro's doing), which is
// why this non-static one-liner exists at all -- reimpl_probe cannot reach the generated installer.
int install_promotion_sim_step_direct() {
    if (promoted_arm::g_promoted) return 0;
    if (!mh_export_install_llm_strat_sim_step()) return 0;
    promoted_arm::g_promoted = true;
    mh::ai::ai_say("; [promote] sim_step: is LIVE by DIRECT ENTRY INSTALL (no harness detour owns the "
                   "entry this run) -- ours IS the function, there is no original arm in this run\n");
    return 1;
}

// The rebind route's other half: called by reimpl_probe AFTER MH_Harness_RebindSimStep took, never
// before -- the same order mh::tact::register_promotion_frame's contract requires, and for the same
// reason (a listed-but-unarmed root turns a mismatch into a silent zero).
int register_promotion_sim_step_rebound() {
    if (promoted_arm::g_promoted) return 0;
    promoted_arm::g_promoted            = true;
    promoted_arm::g_promoted_via_rebind = true; // mp:D32 -- the harness's detour already feeds the detector
    mh::ai::ai_say("; [promote] sim_step: is LIVE by HARNESS REBIND (the determinism detour falls "
                   "through to OURS; on_sim_step still runs first) -- ours IS the function, there is "
                   "no original arm in this run\n");
    return 1;
}

bool sim_step_promoted() { return promoted_arm::g_promoted; }
bool sim_step_promoted_via_rebind() { return promoted_arm::g_promoted_via_rebind; }
long sim_step_served_calls() { return promoted_arm::g_step_calls; }

// TEST-ONLY (mp:D32) -- see the header comment. Bookkeeping only, no entry write.
void sim_step_promotion_force_direct_for_test() {
    promoted_arm::g_promoted            = true;
    promoted_arm::g_promoted_via_rebind = false;
}

// TEST-ONLY. The flag is process-global and production has no un-promote (a promotion lasts the life
// of the process), so a selftest driving both routes in one process must clear it between scenarios
// or the second scenario inherits the first's owner. Never called in the game.
void sim_step_promotion_reset_for_test() {
    promoted_arm::g_promoted            = false;
    promoted_arm::g_promoted_via_rebind = false;
    promoted_arm::g_step_calls          = 0;
    promoted_arm::g_pre_hook            = nullptr;
}

// Install the per-call pre-hook the promoted body runs BEFORE its own work. See the block comment at
// promoted_arm::g_pre_hook: this is how the D21 desync sampler keeps its sampling point now that the
// root's entry belongs to us. Refuses (returns 0) when the root is NOT promoted this run -- there is
// no body to chain onto, and the caller must install its own entry trampoline instead. Refuses a
// SECOND hook rather than overwriting: two instruments silently sharing one slot is the failure this
// whole file is otherwise built to prevent. mp:D32: ALSO refuses when the root was promoted BY REBIND
// -- that route leaves the harness's own detour owning the real entry, and its handler
// (harness.cpp's on_sim_step) already calls mh::desync::on_sim_step_hashed unconditionally, before
// ever reaching this promoted body. Accepting the hook there too would feed the detector from BOTH
// paths every step (measured: "2999 steps seen" for a 1500-step run). Only the direct-install route
// has no other feeder and may take this slot.
int set_sim_step_pre_hook(void (*fn)()) {
    if (!fn || !promoted_arm::g_promoted || promoted_arm::g_pre_hook) return 0;
    if (promoted_arm::g_promoted_via_rebind) return 0;
    promoted_arm::g_pre_hook = fn;
    return 1;
}


} // namespace mh::sim
