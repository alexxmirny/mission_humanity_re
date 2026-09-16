//
// sim/sim_unit_teardown_mapped.cpp -- see sim_unit_teardown_mapped.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_teardown_mapped_0048753e.asm) -- the exported .c draft's overall
// shape and field mapping were cross-checked address-by-address against the raw asm and found
// faithful throughout (unlike some sibling TUs' drafts), so no HAZARD section is needed here; the
// header derivation above already carries the step-by-step address ranges.
//
#include "sim/sim_unit_teardown_mapped.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_teardown_mapped_calls &live_unit_teardown_mapped_calls() {
    static const unit_teardown_mapped_calls c = {
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_leave),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_strat_ai_notify_object_removed),
    };
    return c;
}

namespace detail {

void unit_teardown_mapped(const sim_view &v, sim_store &own, const unit_teardown_mapped_calls &c,
                          uint32_t player, uint32_t unit_index) {
    const uint32_t p = player & 0xffffu; // 0x0048755b et al.: every roster access below reads only the
                                         // low word of the stored `player` local, matching every
                                         // sibling sim/ TU's own `p`/masking convention.

    unit &u = own.unit_at(p, unit_index);

    // ---- (1) path-slot release (0x0048755b-0x00487583) ----------------------------------------------
    if (u.path_slot_id != 0xffu) {
        c.path_free_slot(static_cast<uint16_t>(p), static_cast<int32_t>(unit_index));
    }

    // ---- (2) housing decrement, unconditional (0x00487583-0x0048758a) -------------------------------
    // No type ladder here (unlike sim_unit_teardown.cpp's off-map sibling) -- this function's sole
    // caller (squad_merge) only ever absorbs soldier-class members.
    own.unit_housing_at(static_cast<int32_t>(p)).used_soldiers -= 1;

    // ---- (3) slot-0 live accumulator (0x004875a3-0x004875ed) -----------------------------------------
    // x87 FLDZ/FCOMP/FNSTSW/SAHF/JNC: block runs iff 0.0 < unit.energy (i.e. energy > 0.0).
    if (u.energy > 0.0) {
        u.energy = 0.0;                                                // 0x004875c3-0x004875d1
        own.unit_at(p, 0).energy += UNIT_TEARDOWN_MAPPED_ENERGY_DELTA; // 0x004875d7-0x004875ed
    }

    // ---- (4)-(6) tile placement teardown + FoW (0x004875f3-0x004876b7) -------------------------------
    const uint32_t x = u.x;
    const uint32_t y = u.y;

    own.tile_object_at(static_cast<int32_t>(x), static_cast<int32_t>(y)).building    = 0;
    own.tile_object_at(static_cast<int32_t>(x), static_cast<int32_t>(y)).class_owner = 0;
    own.passable_at(static_cast<int32_t>(x), static_cast<int32_t>(y))                = u.origin_tile_was_passable;

    c.fow_remove_sight(p, static_cast<int32_t>(x), static_cast<int32_t>(y), v.cfg_units[u.unit_proto_id].sight);

    // ---- (7) MP-lockstep-only ai_group_index clear (0x004876b7-0x004876dc) --------------------------
    if (*v.session_mode == SESSION_MP_LOCKSTEP) {
        u.ai_group_index = 0;
    }

    // ---- (8) UI selection/click-target cleanup (0x004876dc-0x00487730) ------------------------------
    // Local-player arm DOES fire SetEvent here (unlike sim_unit_teardown.cpp's off-map sibling),
    // matching sim_unit_remove_from_map.cpp / sim_unit_on_destroyed.cpp's identical arm instead -- see
    // the header derivation.
    if (static_cast<int16_t>(p) == *v.player_side) {
        c.unit_ctrlgroup_leave(unit_index);
        c.set_event(EVENT_INFO_REFRESH);
    } else if (*v.click_select_target_flags == static_cast<uint16_t>(p | 0x80u) &&
               own.click_select_target_id() == unit_index) {
        own.click_select_target_id() = 0;
        c.set_event(EVENT_INFO_REFRESH);
    }

    // ---- (9) units_alive / presence-lost (0x00487730-0x0048777d) ------------------------------------
    player_profile &prof = own.profile_at(static_cast<int32_t>(p));
    prof.units_alive[*v.planet_index] -= 1;
    if (prof.units_alive[*v.planet_index] == 0) {
        c.player_presence_lost(p, 0);
    }

    // ---- (10) state -> corpse_fow_decay, move_microstep=0 (0x0048777d-0x004877a1) -------------------
    // The write follows the call in the asm -- reproduced in that order.
    c.unit_set_state_of(static_cast<int32_t>(p), static_cast<int32_t>(unit_index),
                        UNIT_TEARDOWN_MAPPED_STATE_CORPSE_FOW_DECAY);
    u.move_microstep = 0;

    // ---- (11) AI notify, always (0x004877a1-0x004877bb) ----------------------------------------------
    c.ai_notify_object_removed(p | 0x80u, unit_index, 0);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_teardown_mapped(uint32_t player, uint32_t unit_index) {
    sim_state st = state();
    detail::unit_teardown_mapped(st.read, st.own, live_unit_teardown_mapped_calls(), player, unit_index);
}


} // namespace mh::sim
