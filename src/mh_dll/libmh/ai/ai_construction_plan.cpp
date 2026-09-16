//
// ai/ai_construction_plan.cpp -- see ai_construction_plan.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_plan_construction_004e5ae0.asm); the paired .c (Ghidra's decompile) agrees
// with the assembly at every branch and every call site once its raw offsets are read back as the
// named fields below -- this is the one function in the current slice that needed no correction to
// its control flow, only a re-expression of `*(byte*)(x + 0xe93a70 + player*0x288fc)` as
// `player_data[player].ai_building_type_available[x]` (see the two DECLARED-NEED-adjacent notes below
// for the one piece of state this function reads that has no home in the view yet).
//
#include "ai/ai_construction_plan.h"


namespace mh::ai {
namespace detail {

namespace {

// cfg::final::data::Progress[].type, the value meaning "this invention row names a BUILDING" (CMP
// byte ptr [..],0x1 @0x004e5b2c). The sibling value 2 ("names a UNIT") already has a name,
// INVENTION_TYPE_UNIT, in ai_train_plan.h -- not reused from there per the brief's "no new shared
// helpers" rule; this is the same cfg_enum_E_INVETION_TYPE domain, named locally because this file is
// the only other AI1 translation that reads it.
inline constexpr uint8_t INVENTION_TYPE_BUILDING = 1;

// player_data::ai_building_type_available[] values this function's first two loops write. Per the
// field's own plate: 0 = not researched, 1 = researched but not yet built, 2 = researched AND already
// built. Only 1 is ever tested by a READER (the field comment says so; this WRITER also needs both).
inline constexpr uint8_t BUILDING_AVAIL_RESEARCHED     = 1;
inline constexpr uint8_t BUILDING_AVAIL_RESEARCHED_BLT = 2;
inline constexpr uint8_t BUILDING_AVAIL_TESTED_VALUE   = 1; // the only value scan_construction_sites
                                                            // et al. ever compare against

} // namespace

void plan_construction(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player) {
    // Read through `pd` for the bulk of the function; writes to player_data go through `mine`
    // (same split ai_construction_sites.cpp uses, `player_data &me = own.players[player];`).
    const player_data &pd   = v.players[player];
    player_data       &mine = own.players[player];

    // ---- part 1: rebuild ai_building_type_available[] from the cfg INVENTION table -----------------
    //
    // Loop A (0x004e5af5-0x004e5b20): CLEAR. Bound is cfg_building_sec->total, INCLUSIVE (JBE), start
    // 0. This is the same "clear the whole per-building-type row" idiom ai_train_plan.h's clear loop
    // uses (and, like that one, the bound is a BUILDING count even though nothing here reads a unit
    // count instead -- there is no cross-domain bound bug in THIS loop, unlike that one's).
    const uint32_t building_total = (uint32_t)v.cfg_building_sec->total;
    for (uint32_t i = 0; i <= building_total; ++i) {
        mine.ai_building_type_available[i] = 0;
    }

    // Loop B (0x004e5b22-0x004e5b97): SCAN. Bound is cfg_progress_sec->total, INCLUSIVE (JBE), start 1
    // -- both properties reproduced verbatim, not "fixed" to 0/exclusive.
    //
    // `iVar3` IN THE PAIRED .c (`player_idx * 0x288fc`, recomputed in the for-loop's own comma
    // expression every iteration) is NOT a stale-variable artifact: it is Watcom/Ghidra hoisting the
    // player's own per-player byte stride into the loop header, and it is IDENTICAL on every
    // iteration because `player` (ESI in the assembly) is never reassigned anywhere in this function.
    // Confirmed from the listing at 0x004e5b7b-0x004e5b8f, which recomputes the exact same product
    // (SHL/ADD/SHL/SUB/SHL/SHL/ADD sequence -> player*0x288fc) on every pass through the loop
    // condition, and again by the fact that EDI keeps that value, unrecomputed, all the way to the
    // function's final block (0x004e5bdb) where the paired .c's mid-plan branch reads `iVar3` one more
    // time. So every access this translation writes as `ai_building_type_available[x]` (indexed off
    // `player`, not off a carried scratch value) is exactly what the assembly computes, including in
    // the unrelated final block below.
    const uint32_t progress_total = (uint32_t)v.cfg_progress_sec->total;
    for (uint32_t row = 1; row <= progress_total; ++row) {
        const cfg_invention &inv = v.cfg_inventions[row];
        if (inv.type == INVENTION_TYPE_BUILDING) {
            const player_progress &prog = progress_of(v, (int32_t)player, (int32_t)row);
            if (prog.available != 0) {
                mine.ai_building_type_available[inv.index] =
                    (prog.f3 != 0) ? BUILDING_AVAIL_RESEARCHED_BLT : BUILDING_AVAIL_RESEARCHED;
            }
        }
    }

    // ---- part 2: the two planners that always run, whichever arm follows ----------------------------
    //
    // DECLARED NEED: neither of these two callees, nor the five further below, exists yet as an
    // ai_calls member -- see the header's declared-needs note. All eight already have committed
    // __watcall prototypes (dll_call_protos.json, status "ok"); they are simply not yet wired into
    // this struct because no earlier AI1 translation called out to them.
    const int32_t housing_short = gc.rebalance_building_workers(player); // iVar2 in the paired .c
    gc.plan_turret_upgrade(player);

    // ---- part 3: the one branch, taken by (plan_len & 0x7fffffff) <= plan_cursor, UNSIGNED ----------
    if ((pd.ai_build_plan_len_and_flag & 0x7fffffffu) <= (uint32_t)pd.ai_build_plan_cursor) {
        // ---- LIVE PRIORITY LOGIC (0x004e5c02-0x004e5d9d) --------------------------------------------

        // Secondary candidate: queue it unless already queued, UNLESS power supply is short (in which
        // case queue regardless of the count-by-id check) or none exist yet.
        if (gc.bldg_type_already_queued(player, (uint32_t)pd.ai_build_candidate_secondary) == 0) {
            // FP COMPARISON (see uncertainties): the original computes this via FCOMPP/FNSTSW/SAHF/JA
            // directly on the x87-stack double `calc_power_supply_ratio` returns, against the float
            // constant at 0x00669380 widened to double. `gc.calc_power_supply_ratio` already returns a
            // plain `double` across the call boundary (the ABI forces the x87-stack value out to a
            // real double on return), so this `<` performs the identical comparison with no extra
            // rounding step introduced by the translation -- there is no intermediate for us to round
            // differently. NaN behaves identically too: FCOMPP+SAHF+JA takes the branch only when
            // ST(0) > ST(1) with neither NaN, exactly as C++ `<` returns false for either operand NaN.
            const double power_ratio = gc.calc_power_supply_ratio(player);
            if (power_ratio < (double)*v.energy_ratio || // DECLARED NEED: *v.energy_ratio, see header
                gc.bldg_count_by_id(player, pd.ai_build_candidate_secondary) == 0) {
                gc.bldg_queue_construction(player, pd.ai_build_candidate_secondary, -1, 0);
            }
        }

        // Primary candidate: only reached when the worker-rebalance call above decided housing is
        // short. Queuing it also rotates it to the front of the AI build queue.
        if (housing_short != 0) {
            if (gc.bldg_type_already_queued(player, (uint32_t)pd.ai_build_candidate_primary) == 0) {
                gc.bldg_queue_construction(player, pd.ai_build_candidate_primary, -1, 0);
                gc.queue_rotate_newest_to_front(player);
            }
        }

        gc.plan_mine_construction((int32_t)player);

        // Shortage candidate: queue it when not already queued AND the silo-capacity gate says short.
        if (gc.bldg_type_already_queued(player, (uint32_t)pd.ai_build_candidate_shortage) == 0 &&
            gc.storage_capacity_short_and_cap_check((int32_t)player) != 0) {
            gc.bldg_queue_construction(player, pd.ai_build_candidate_shortage, -1, 0);
        }

        // A second, independent shortage reaction: only when no shortage state is already latched AND
        // the need score has not yet reached the threshold (both read fresh here, UNSIGNED compare on
        // the threshold test per 0x004e5d1b/0x004e5d21).
        if (pd.ai_resource_shortage_state == 0 &&
            (uint32_t)pd.ai_resource_need_score < (uint32_t)pd.ai_resource_need_threshold) {
            gc.react_resource_shortage((int32_t)player);
        }

        gc.maintain_unit_housing((int32_t)player);

        // The expansion-site scan. Normal mode (high bit of ai_build_plan_len_and_flag clear): one
        // category-4 call, unconditional. Spiral mode (bit set): a category-0 probe first; only if
        // THAT placed nothing (return 1, "placed nothing") do categories 1-3 run and the bit get
        // cleared -- clearing it is what takes the player back to normal mode on a later tick.
        if ((pd.ai_build_plan_len_and_flag & 0x80000000u) == 0) {
            gc.scan_construction_sites((int32_t)player, 4);
        } else {
            if (gc.scan_construction_sites((int32_t)player, 0) == 0) {
                gc.scan_construction_sites((int32_t)player, 1);
                gc.scan_construction_sites((int32_t)player, 2);
                gc.scan_construction_sites((int32_t)player, 3);
                mine.ai_build_plan_len_and_flag &= 0x7fffffffu;
            }
        }
        return;
    }

    // ---- ADVANCE THE PRECOMPUTED PLAN (0x004e5bbd-0x004e5bfd) ---------------------------------------
    //
    // `iVar3` here is the SAME stable player-stride value the part-1 loop used (see the long comment
    // above) -- so this is `ai_building_type_available[ai_build_plan[cursor]]`, this player's own
    // table, not some other player's.
    if (pd.ai_bldg_queue_count != 0) return;

    const int32_t plan_type = pd.ai_build_plan[pd.ai_build_plan_cursor];
    if (pd.ai_building_type_available[plan_type] != BUILDING_AVAIL_TESTED_VALUE) return;

    gc.bldg_queue_construction(player, plan_type, -1, 0);
    mine.ai_build_plan_cursor += 1;
}

// llm_strat_ai_calc_power_supply_ratio @0x004e3926, translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_calc_power_supply_ratio_004e3926.asm). Same COUNT-DRIVEN building-roster
// walk idiom as ai_opponent_relations.cpp's building loop (seed `remaining` from buildings[player][0]
// .index, decrement only on an occupied slot, index starts at 1) -- re-derived independently here
// rather than shared, since the two functions' roster walks were translated in different sessions and
// this project's "no new shared helpers" rule (ai_construction_plan.h's own INVENTION_TYPE_BUILDING
// comment) applies to loop SHAPES as much as to named constants.
double calc_power_supply_ratio(const ai_view &v, int32_t player) {
    uint32_t generated = 0; // EDI (0x004e393b)
    uint32_t consumed  = 0; // ESI (0x004e393d)

    uint32_t remaining      = (uint16_t)building_of(v, (uint32_t)player, 0).index; // 0x004e3958
    int32_t  building_index = 1;                                                   // EBX (0x004e393f)
    while (remaining != 0) {                                                       // 0x004e3a19/0x004e3a1b
        const building &b = building_of(v, (uint32_t)player, building_index);
        if (b.building_id != 0) { // 0x004e3991/0x004e3999
            const cfg_building &cb = v.cfg_buildings[b.building_id];
            // The four generator types, tested UNCONDITIONALLY (no is_alien_race gate) -- see the
            // header banner and ai_state.h's BLDG_TYPE_A_PLANT/H_PLANT comment.
            if (cb.type == BLDG_TYPE_A_PLANT || cb.type == BLDG_TYPE_H_PLANT || // 0x004e39ac-0x004e39ce
                cb.type == BLDG_TYPE_A_MOTHER || cb.type == BLDG_TYPE_H_MOTHER) {
                generated += (uint32_t)cb.electric_power; // 0x004e3a09
            } else {
                consumed += (uint32_t)cb.electric_power; // 0x004e3a11
            }
            --remaining;
        }
        ++building_index;
    }

    // 0x004e3a21-0x004e3a69: both zero -> 0.0; consumers zero (generators nonzero, since the
    // both-zero case is already handled above) -> the fixed sentinel 1.1; else the true quotient.
    if (generated == 0 && consumed == 0) return 0.0;
    if (consumed == 0) return 1.1;
    return (double)generated / (double)consumed;
}

} // namespace detail

void plan_construction(uint32_t player) {
    const ai_state st = state();
    detail::plan_construction(st.read, st.own, live_calls(), player);
}

double calc_power_supply_ratio(int32_t player) {
    const ai_state st = state();
    return detail::calc_power_supply_ratio(st.read, player);
}


} // namespace mh::ai
