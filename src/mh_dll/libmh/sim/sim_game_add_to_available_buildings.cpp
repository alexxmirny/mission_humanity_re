//
// sim/sim_game_add_to_available_buildings.cpp -- see sim_game_add_to_available_buildings.h. Translated
// from the DISASSEMBLY (tmp/decomp/game_AddToAvailableBuildings_004141a1.asm,
// tmp/decomp/game_AddToAvailableBuildingsWithCheck_00440049.asm), which the Ghidra .c drafts agree with
// exactly for game_AddToAvailableBuildings; WithCheck's draft is right about the logic but flattens it
// into a comma-operator chain -- see the header banner for the branch-by-branch re-derivation into
// ordinary nested ifs.
//
#include "sim/sim_game_add_to_available_buildings.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const add_to_available_buildings_calls &live_add_to_available_buildings_calls() {
    static const add_to_available_buildings_calls c = {
        MH_LIBMH_BIND(game_InsertItemInPlayerArray),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// This function's own literal operand with no backing Ghidra enum (rule 17a fallback) -- see the
// header banner's derivation (AvailableBuildings is 1600 bytes / 8 players / sizeof(int32_t) = 50
// slots/row, and 0x32 == 50 is exactly the capacity argument the assembly passes).
inline constexpr int32_t AVAILABLE_BUILDINGS_ROW_CAP = 0x32;

// game::e::event member 15 (the strategic-sim notes' resolved event table: "mark build page 0 dirty;
// building became available"). Declared locally per-TU (not hoisted to sim_event_codes.h) to avoid the
// ODR collision that header's own banner documents already having happened once for a bare name at
// `mh::sim` scope -- see the header banner's fuller explanation.
inline constexpr uint32_t BUILD_BUILDINGS_REFRESH = 15u;

} // namespace

namespace detail {

// 0x004141a1-0x004141e7: one call, no branches. DECLARED NEED 1 (see header):
// own.available_buildings_row() does not exist on sim_store yet.
void add_to_available_buildings(sim_store &own, const add_to_available_buildings_calls &c,
                                uint32_t player, int32_t b_i) {
    c.insert_item_in_player_array(own.available_buildings_row(player), AVAILABLE_BUILDINGS_ROW_CAP,
                                  player, 0, b_i);
}

// 0x00440049-0x00440137. See the header banner for the branch-by-branch derivation of every gate below
// against the assembly's jump targets.
void add_to_available_buildings_with_check(const sim_view &v, sim_store &own,
                                           const add_to_available_buildings_calls &c, uint16_t player,
                                           int32_t b_i) {
    const cfg_building &cb = v.cfg_buildings[b_i];

    // Gate 1 (0x0044006d-0x00440074).
    if (cb.upgrade_lvl != 1) return;

    // Gate 2 (0x0044007a-0x004400a5): the port-type exclusion is SKIPPED entirely in single-player --
    // only a non-SP session can be rejected here.
    if (*v.session_mode != SESSION_SP) {
        if (cb.type == BUILDING_TYPE_A_PORT || cb.type == BUILDING_TYPE_H_PORT) return;
    }

    // Gate 3 (0x004400a5-0x004400c7).
    if (cb.type == BUILDING_TYPE_A_MOTHER || cb.type == BUILDING_TYPE_H_MOTHER) return;

    // Gate 4 (0x004400c7-0x004400e9).
    if (cb.type == BUILDING_TYPE_A_MAIN_BASE || cb.type == BUILDING_TYPE_H_MAIN_BASE) return;

    // Gate 5 (0x004400e9-0x0044010d).
    if (cb.type == BUILDING_TYPE_A_CIVIL || cb.type == BUILDING_TYPE_H_CIVIL) return;

    // All five gates passed (0x0044010d-0x00440114): in-TU call to the sibling function above, not
    // through the `calls` struct (same closure, same TU, per the batch context's pairing note). The
    // MOVZX zero-extend in the assembly is reproduced by the uint16_t->uint32_t widening here.
    add_to_available_buildings(own, c, static_cast<uint32_t>(player), b_i);

    // 0x00440119-0x0044012a: viewing-player-only dirty event. Plain 16-bit equality; no width mismatch
    // to resolve (player is already uint16_t, *v.player_side is int16_t).
    if (player == static_cast<uint16_t>(*v.player_side)) { c.set_event(BUILD_BUILDINGS_REFRESH); }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void add_to_available_buildings(uint32_t player, int32_t b_i) {
    sim_state st = state();
    detail::add_to_available_buildings(st.own, live_add_to_available_buildings_calls(), player, b_i);
}

void add_to_available_buildings_with_check(uint16_t player, int32_t b_i) {
    sim_state st = state();
    detail::add_to_available_buildings_with_check(st.read, st.own, live_add_to_available_buildings_calls(),
                                                  player, b_i);
}


} // namespace mh::sim
