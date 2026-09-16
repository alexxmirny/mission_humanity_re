//
// sim/sim_unit_teardown.cpp -- see sim_unit_teardown.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_teardown_00487ba5.asm), not from Ghidra's C: the draft's overall shape
// (soldier detach / housing decrement / ai_group_index clear / selection cleanup / energy accumulator
// / target release / units_alive+presence-lost / state+microstep / mother-ship re-election / AI
// notify) reads correctly, but the soldier-loop guard's phantom `unit_id`/`player` fields, the
// housing ladder's threshold ORDER, and the x87 energy comparison were all re-walked branch-by-branch
// against the raw MOVZX/IMUL/CMP/Jcc sequence rather than trusted from the exported .c -- see the
// header hazards for the full derivation of each.
//
#include "sim/sim_unit_teardown.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_teardown_calls &live_unit_teardown_calls() {
    static const unit_teardown_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_leave),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_player_teardown_hook_stub),
        MH_LIBMH_BIND(llm_strat_ai_notify_object_removed),
    };
    return c;
}

namespace detail {

void unit_teardown(const sim_view &v, sim_store &own, const unit_teardown_calls &c, uint32_t player,
                   uint16_t unit_index) {
    const uint32_t p = player & 0xffffu; // 0x00487bc2 et al.: every roster access below reads only
                                         // the low word of the stored `player` local, matching every
                                         // sibling sim/ TU's own `p`/masking convention.

    unit &u = own.unit_at(p, unit_index);

    // ---- unit_proto_id, read ONCE and reused -- see the header hazard note ------------------------
    const uint16_t  unit_proto_id = u.unit_proto_id;
    const cfg_unit &proto         = v.cfg_units[unit_proto_id];

    // ---- (1) soldier-chain detach (0x00487bc2-0x00487c87) -------------------------------------------
    // Guard: Unit[proto].soldier_count != 0 AND unit.unit_above != 0 -- a SINGLE 16-bit field test,
    // not the two struct members the exported .c names (`unit_id`/`player`); see the header hazard.
    if (proto.soldier_count != 0) {
        const uint32_t head = uint32_t(u.unit_above[0]) | (uint32_t(u.unit_above[1]) << 8);
        if (head != 0) {
            uint32_t walker = head;
            do {
                own.soldier_at(p, walker).owner_unit = 0;        // 0x00487c32
                own.soldier_at(p, 0).owner_unit -= 1;            // 0x00487c45: DEC word, per-player count
                walker = own.soldier_at(p, walker).next_soldier; // 0x00487c5c
            } while (walker != 0);
            u.unit_above[0] = 0; // 0x00487c7e: single word-width clear, split per the byte[2] field
            u.unit_above[1] = 0;
        }
    }

    // ---- (2) housing-bucket decrement by cfg Unit.type (0x00487c87-0x00487d42) ----------------------
    // Four-way ladder over the SAME boundaries sim_unit_housing_count.h's pair use, but this body's
    // OWN threshold order (< A_WALKER first) -- see the header hazard on why this is NOT routed
    // through llm_strat_unit_housing_count_remove.
    if (proto.type < UNIT_TYPE_A_WALKER) {
        if (proto.type != UNIT_TYPE_UNDEFINED) {
            own.unit_housing_at(p).used_soldiers -= proto.soldier_count; // LAB_00487d0c
        }
        // proto.type == UNIT_TYPE_UNDEFINED: nothing (LAB_00487c1a via the cd7 JMP).
    } else if (proto.type < UNIT_TYPE_A_HELI) {
        own.unit_housing_at(p).used_vehicles -= 1; // LAB_00487cdf
    } else if (proto.type < UNIT_TYPE_A_PLANE) {
        own.unit_housing_at(p).used_helis -= 1; // LAB_00487cee
    } else if (proto.type < UNIT_TYPE_A_HELI_MOTHER) {
        own.unit_housing_at(p).used_planes -= 1; // LAB_00487cfd
    }
    // proto.type >= UNIT_TYPE_A_HELI_MOTHER: nothing.

    // ---- (3) MP-lockstep-only ai_group_index clear (0x00487d42-0x00487d6a) --------------------------
    if (*v.session_mode == SESSION_MP_LOCKSTEP) {
        u.ai_group_index = 0;
    }

    // ---- (4) UI selection/click-target cleanup (0x00487d6a-0x00487db5) ------------------------------
    // NOTE the asymmetry with sim_unit_on_destroyed.cpp / sim_unit_remove_from_map.h's matching arms:
    // this local-player branch does NOT call set_event (the asm jumps straight past it), and the
    // click-select flag test here is ONLY the `player|0x80` variant (on_destroyed also tests
    // `player|0x20`; this function's asm does not).
    if (static_cast<int16_t>(p) == *v.player_side) {
        c.unit_ctrlgroup_leave(unit_index); // 0x00487d7a
    } else if (*v.click_select_target_flags == static_cast<uint16_t>(p | 0x80u) &&
               own.click_select_target_id() == unit_index) {
        own.click_select_target_id() = 0; // 0x00487da2
        c.set_event(EVENT_INFO_REFRESH);  // 0x00487db0
    }

    // ---- (5) slot-0 live accumulator (0x00487dcb-0x00487e1e) -----------------------------------------
    // x87 FLDZ/FCOMP/FNSTSW/SAHF/JNC: block runs iff 0.0 < unit.energy (i.e. energy > 0.0), matching
    // the .c's own `if (0.0 < ...)` reading -- re-derived from the raw carry-flag semantics rather
    // than trusted, see the header hazard's FP note and uncertainties[] below.
    if (u.energy > 0.0) {
        u.energy = 0.0;                                            // 0x00487dee-0x00487df8
        own.unit_at(p, 0).energy += UNIT_TEARDOWN_LIVE_ACCUM_STEP; // 0x00487e0c-0x00487e18
    }

    // ---- (6) release both target reservations (0x00487e1e-0x00487efe) -------------------------------
    // Independent ifs (both can fire), not else-if. Mode literals 1/3 match sim_order_dispatch.h's
    // own RELEASE_MODE_PRIMARY_CLEAR/RELEASE_MODE_SECONDARY_CLEAR values -- see the header's DECLARED
    // NEED on why they are not reused by name here (ODR collision risk across sim/*.h).
    if (u.target_ref != 0) {
        c.target_release_ref(p, unit_index, 1); // primary, RELEASE_MODE_PRIMARY_CLEAR
        u.target_ref   = 0;
        u.target_index = 0;
    }
    if (u.target2_ref != 0) {
        c.target_release_ref(p, unit_index, 3); // secondary, RELEASE_MODE_SECONDARY_CLEAR
        u.target2_ref   = 0;
        u.target2_index = 0;
    }

    // ---- (7) units_alive / presence-lost (0x00487efe-0x00487f41) ------------------------------------
    player_profile &prof = own.profile_at(p);
    prof.units_alive[*v.planet_index] -= 1;
    if (prof.units_alive[*v.planet_index] == 0) {
        c.player_presence_lost(p, 0);
    }

    // ---- (8) state -> corpse_fow_decay, move_microstep=0 (0x00487f41-0x00487f73) --------------------
    // The write follows the call in the asm -- reproduced in that order (unit_set_state_of is
    // original code and could plausibly touch move_microstep itself; we do not assume it doesn't).
    c.unit_set_state_of(static_cast<int32_t>(p), static_cast<int32_t>(unit_index),
                        UNIT_TEARDOWN_STATE_CORPSE_FOW_DECAY);
    u.move_microstep = 0;

    // ---- (9) mother-ship re-election (0x00487f73-0x00488012) -----------------------------------------
    // UNCONDITIONAL call once type+id both match -- no further session_mode/PlayerSide gate here
    // (unlike sim_unit_on_destroyed.cpp's own mother-ship tail, which has one around its voice-line
    // block; this function's asm has no second CMP anywhere between the clear and the call).
    if (proto.type == UNIT_TYPE_A_HELI_MOTHER || proto.type == UNIT_TYPE_H_HELI_MOTHER) {
        if (prof.primary_mother_unit[*v.planet_index] == static_cast<int32_t>(unit_index)) {
            prof.primary_mother_unit[*v.planet_index] = 0;        // 0x00487fff
            c.player_teardown_hook_stub(static_cast<int32_t>(p)); // 0x0048800d
        }
    }

    // ---- (10) AI notify, always (0x00488012-0x00488025) ----------------------------------------------
    c.ai_notify_object_removed(p | 0x80u, unit_index, 0);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_teardown(uint32_t player, uint16_t unit_index) {
    sim_state st = state();
    detail::unit_teardown(st.read, st.own, live_unit_teardown_calls(), player, unit_index);
}


} // namespace mh::sim
