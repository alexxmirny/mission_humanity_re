//
// sim/sim_bldg_state_dismantle.cpp -- see sim_bldg_state_dismantle.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_state_dismantling_00472eb7.asm,
// _dismantle_finish_004736a2.asm), cross-checked against the Ghidra .c drafts (tmp/decomp_sim/*.c) --
// both agree with the assembly byte-for-byte, no draft/asm disagreement found in either function.
//
#include "sim/sim_bldg_state_dismantle.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder (SIM1-P clause 2)
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const bldg_state_dismantling_calls &live_bldg_state_dismantling_calls() {
    static const bldg_state_dismantling_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

const bldg_state_dismantle_finish_calls &live_bldg_state_dismantle_finish_calls() {
    static const bldg_state_dismantle_finish_calls c = {
        MH_PROMOTED_ROW(llm_strat_ai_notify_object_removed),
        MH_LIBMH_BIND(llm_strat_bldg_refund_resources_scaled_by_energy),
        MH_LIBMH_BIND(llm_strat_prod_unbind_planet),
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_LIBMH_BIND(llm_strat_bldg_unmap_footprint),
        MH_LIBMH_BIND(llm_strat_sight_add_circle),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// anim[48] is TWELVE int32 slots (cfg_t_frame_index[12]), not 48 independent bytes. While a building
// is RUBBLE / RUBBLE_SIGHT_DECAY, slot 0 (byte offset 0x93) is repurposed as the pending sight-circle
// countdown -- store/load it as a raw uint32_t. Same per-TU file-local helper shape
// sim_map_create_building.cpp / sim_bldg_state_charge.cpp already define (copied, not shared cross-TU,
// per this project's established per-TU convention).
inline void     store_u32_le(uint8_t *dst, uint32_t value) { std::memcpy(dst, &value, 4); }
inline uint32_t load_u32_le(const uint8_t *src) {
    uint32_t v;
    std::memcpy(&v, src, 4);
    return v;
}

// llm_strat_ai_notify_object_removed's flags argument ORs this into cur_player's low byte
// (`OR AL,0x40` after loading the 16-bit cur_player into AX) -- bit-identical to
// `(uint32_t)cur_player | 0x40u` for this game's 0-7 player range (no overlap between the value range
// and the flag bit). No backing Ghidra enum found for this flag (rule 17a fallback).
inline constexpr uint32_t AI_NOTIFY_OBJECT_REMOVED_HARD_REMOVE_FLAG = 0x40u;

// game_SetEvent(0xe) -- MAP_OBJECTS_REFRESH, same value every other sim/ TU's own file-local copy of
// this constant uses (sim_unit_state_die_explode.cpp's UNIT_STATE_DIE_EXPLODE_MAP_OBJECTS_REFRESH,
// sim_unit_state_remove_silent.cpp's own copy) -- redeclared file-local here per the same
// "no shared TU constant" convention (not a new cross-TU abstraction).
inline constexpr uint32_t DISMANTLE_FINISH_MAP_OBJECTS_REFRESH = 0xeu;

} // namespace

namespace detail {

void bldg_state_dismantling(const sim_view &v, sim_store &own, const bldg_state_dismantling_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- cycle_progress -= (tick_budget * efficiency) / DISMANTLE_PROGRESS_DIVISOR (0x00472ecf-
    // 0x00472eec). DECLARED NEED: v.dismantle_progress_divisor does not exist in sim_view yet -- see
    // the header banner. Written AS IF it does.
    b.cycle_progress -= (own.tick_budget() * b.efficiency) / *v.dismantle_progress_divisor;

    // ---- completion gate (0x00472ef4-0x00472f1f): FLDZ; FCOMP cycle_progress; JNC taken means
    // 0.0 >= cycle_progress, i.e. cycle_progress <= 0.0 -- and the unordered/NaN case sets CF=1 (JNC
    // NOT taken), landing on the SAME "else" arm a plain IEEE `cycle_progress <= 0.0` would (false for
    // NaN). So the naive `<=` is exactly equivalent here, ordered or not. See uncertainties[].
    if (b.cycle_progress <= 0.0) {
        b.state = BLDG_STATE_DISMANTLE_FINISH;
    } else {
        own.tick_budget() = 0.0;
    }

    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

void bldg_state_dismantle_finish(const sim_view &v, sim_store &own,
                                 const bldg_state_dismantle_finish_calls &c) {
    const uint16_t cur_player = *v.cur_player;
    const uint16_t cur_index  = *v.cur_index;

    // ---- unconditional prologue (0x004736ba-0x004736e9) --------------------------------------------
    c.ai_notify_object_removed(static_cast<uint32_t>(cur_player) | AI_NOTIFY_OBJECT_REMOVED_HARD_REMOVE_FLAG,
                               static_cast<uint32_t>(cur_index), /*hard_remove=*/1);
    c.bldg_refund_resources_scaled_by_energy(cur_player, cur_index);

    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- shuttle-slot release, if bound (0x004736ee-0x0047374d) --------------------------------------
    if (b.shuttle_slot != 0) {
        if (static_cast<int32_t>(b.shuttle_slot) == v.profiles[cur_player].prod_queue_slot[*v.planet_index]) {
            c.prod_unbind_planet(cur_player, *v.planet_index);
        }
        c.prod_shuttle_slot_release(cur_player, b.shuttle_slot);
    }

    // ---- HQ energy credit, gated on cur_building's own energy != 0.0 (0x0047374d-0x00473793) --------
    // The assembly's sign-masked bit-pattern test is exactly the IEEE `!= 0.0` predicate (see header
    // banner) -- `b.energy != 0.0` is a faithful translation, not an approximation.
    // DECLARED NEED: v.bldg_dismantle_hq_energy_credit does not exist in sim_view yet -- see the header
    // banner. Written AS IF it does.
    if (b.energy != 0.0) {
        b.energy = 0.0;
        own.building_at(cur_player, 0).energy += *v.bldg_dismantle_hq_energy_credit; // buildings[player][0], the HQ
    }

    // ---- unmap footprint, roster bookkeeping (0x00473793-0x004737f1) ----------------------------------
    c.bldg_unmap_footprint(cur_player, cur_index);
    player_profile &prof = own.profile_at(cur_player);
    prof.buildings_alive[*v.planet_index] -= 1;
    if (prof.buildings_alive[*v.planet_index] == 0) {
        c.player_presence_lost(cur_player, /*mode=*/0);
    }

    // ---- transition to RUBBLE_SIGHT_DECAY, reset cycle_progress, seed the sight countdown -------------
    // (0x004737f1-0x00473863). See the header banner on anim[0]'s repurposing.
    b.cycle_progress     = 0.0;
    b.state              = BLDG_STATE_RUBBLE_SIGHT_DECAY;
    const uint32_t sight = static_cast<uint32_t>(v.cfg_buildings[b.building_id].sight);
    store_u32_le(&b.anim[0], sight);
    c.sight_add_circle(cur_player, b.x, b.y, b.building_id, static_cast<uint8_t>(load_u32_le(&b.anim[0])));

    // ---- tail: zero the shared tick-budget scratch, MAP_OBJECTS_REFRESH, notify UI --------------------
    own.tick_budget() = 0.0;
    c.game_SetEvent(DISMANTLE_FINISH_MAP_OBJECTS_REFRESH);
    c.bldg_notify_ui(cur_player, static_cast<uint32_t>(cur_index));
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_dismantling() {
    sim_state st = state();
    detail::bldg_state_dismantling(st.read, st.own, live_bldg_state_dismantling_calls());
}

void bldg_state_dismantle_finish() {
    sim_state st = state();
    detail::bldg_state_dismantle_finish(st.read, st.own, live_bldg_state_dismantle_finish_calls());
}


} // namespace mh::sim
