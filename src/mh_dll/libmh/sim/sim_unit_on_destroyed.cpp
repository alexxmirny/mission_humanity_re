//
// sim/sim_unit_on_destroyed.cpp -- see sim_unit_on_destroyed.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_on_destroyed_004877bc.asm), not from Ghidra's C: the draft's overall
// shape (the shuttle/population/FoW/ctrlgroup/click-select/mother-ship sequence) reads correctly,
// but the mother-ship-lost tail's comma-operator condition was re-walked branch-by-branch against
// the raw CMP/JZ/JNZ targets -- see the header HAZARD note on why the clear write is unconditional
// once the id matches, independent of the voice-line block.
//
#include "sim/sim_unit_on_destroyed.h"

#include "addr/mh_calls.gen.h"      // typed callables for the original functions we still call OUT to
#include "lockstep/overlay_hoist.h" // R3b: outcome_dialog's gated panel hoist
#include "ai/ai_state.h"            // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_on_destroyed_calls &live_unit_on_destroyed_calls() {
    static const unit_on_destroyed_calls c = {
        MH_LIBMH_BIND(llm_strat_prod_unbind_planet),
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_population_remove),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_leave),
        MH_LIBMH_BIND(game_SetEvent),
        mh::state::evt::text_queue_id,
        mh::state::evt::snd_play,
        mh::state::evt::outcome_dialog_i32,
        &mh::lockstep::live_overlay_hoist_ops(), // LIB-ABI stage E / R3b
    };
    return c;
}

namespace detail {

void unit_on_destroyed(const sim_view &v, sim_store &own, const unit_on_destroyed_calls &c,
                       uint16_t player, uint32_t unit_idx) {
    const int32_t idx = static_cast<int32_t>(unit_idx);

    // ---- snapshot x/y FIRST (0x004877dd-0x00487810), matching the asm's own read order -------------
    // Captured into locals before anything else touches the roster, because llm_strat_unit_unlink_tile
    // (below) is original code that plausibly clears a departing unit's tile linkage -- the
    // fow_remove_sight call in (4) reuses these same snapshots rather than re-reading the roster. The
    // relative order of these two reads vs. the shuttle_slot read right after has no behavioural
    // effect (no write/call is interleaved among the three), but is kept in the asm's own order anyway.
    const uint8_t snapshot_x = unit_of(v, player, idx).x;
    const uint8_t snapshot_y = unit_of(v, player, idx).y;

    // ---- (1) shuttle slot (0x00487813-0x00487897) --------------------------------------------------
    const uint8_t shuttle_slot = unit_of(v, player, idx).shuttle_slot;
    if (shuttle_slot != 0) {
        // Conditional unbind: only if THIS unit held the planet's currently-bound production slot.
        if (shuttle_slot == v.profiles[player].prod_queue_slot[*v.planet_index]) {
            c.prod_unbind_planet(static_cast<int32_t>(player), *v.planet_index);
        }
        // Unconditional release once shuttle_slot != 0 -- runs whether or not the unbind above fired.
        c.prod_shuttle_slot_release(static_cast<int32_t>(player), static_cast<int32_t>(shuttle_slot));
    }

    // ---- (2) unlink from the map tile (0x00487897-0x004878a4) ---------------------------------------
    c.unit_unlink_tile(static_cast<uint32_t>(player), static_cast<uint16_t>(unit_idx));

    // ---- unit_proto_id, read ONCE and reused -- see the header hazard note ------------------------
    const uint16_t  unit_proto_id = unit_of(v, player, idx).unit_proto_id;
    const cfg_unit &proto         = v.cfg_units[unit_proto_id];

    // ---- (3) population (0x004878a4-0x00487966) ----------------------------------------------------
    if (proto.human != 0) {
        own.population_at(player).human += proto.human;
        own.population_at(player).human_in_field -= proto.human;
        c.population_remove(static_cast<uint32_t>(player), proto.human);
    }

    // ---- (4) FoW sight removal (0x00487966-0x0049c) -- ALWAYS, using the (2) snapshots -------------
    c.fow_remove_sight(static_cast<uint32_t>(player), snapshot_x, snapshot_y, proto.sight);

    // ---- (5) AI-group unlink, MP lockstep only (0x0049c-0x004879c1) --------------------------------
    if (*v.session_mode == SESSION_MP_LOCKSTEP) {
        own.unit_at(player, idx).ai_group_index = 0;
    }

    // ---- (6) UI selection/click-target cleanup (0x004879c1-0x00487a28) -----------------------------
    if (static_cast<int16_t>(player) == *v.player_side) {
        // Local player's own unit: leave any control group it was assigned to and refresh the info
        // panel. `else if` below is never evaluated in this arm -- the original's JMP past it.
        c.unit_ctrlgroup_leave(unit_idx);
        c.set_event(EVENT_INFO_REFRESH);
    } else if ((*v.click_select_target_flags == static_cast<uint16_t>(player | 0x20u) ||
                *v.click_select_target_flags == static_cast<uint16_t>(player | 0x80u)) &&
               own.click_select_target_id() == unit_idx) {
        // The pending click-selection target (owned by a DIFFERENT player's flags/id pair) pointed at
        // this exact (player, unit_idx): clear it so the UI does not keep referencing a dead unit.
        own.click_select_target_id() = 0;
        c.set_event(EVENT_INFO_REFRESH);
    }

    // ---- (7) mother-ship-lost tail (0x00487a28-0x00487b1c) -- see the header HAZARD note -----------
    if (proto.type == UNIT_TYPE_A_HELI_MOTHER || proto.type == UNIT_TYPE_H_HELI_MOTHER) {
        if (own.profile_at(player).primary_mother_unit[*v.planet_index] == idx) {
            // UNCONDITIONAL once the type + id both match -- runs regardless of the session/PlayerSide
            // check that follows. Do NOT fold this into one combined condition with the block below.
            own.profile_at(player).primary_mother_unit[*v.planet_index] = 0;

            if (*v.session_mode == SESSION_SP && static_cast<int16_t>(player) == *v.player_side) {
                const int32_t text_id =
                    (*v.player_race == 1) ? MOTHER_LOST_TEXT_ID_RACE1 : MOTHER_LOST_TEXT_ID_OTHER;
                c.ui_print_queue_text_id(text_id);
                c.ui_print_queue_text_id(MOTHER_LOST_TEXT_ID_SUFFIX);
                c.snd_play(MOTHER_LOST_SOUND_ID, MOTHER_LOST_SOUND_VOLUME);
                c.ui_outcome_dialog(MOTHER_LOST_OUTCOME_CODE);
                // LIB-ABI stage E hoist: the callee's GAME_MODE=3 @0x004c6cec is unconditional.
                own.game_mode() = 3;
                // R3b: outcome_dialog's gated panel pair -- see overlay_hoist.h.
                mh::lockstep::hoist_outcome_panel(c.hoist);
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_on_destroyed(uint16_t player, uint32_t unit_idx) {
    sim_state st = state();
    detail::unit_on_destroyed(st.read, st.own, live_unit_on_destroyed_calls(), player, unit_idx);
}


} // namespace mh::sim
