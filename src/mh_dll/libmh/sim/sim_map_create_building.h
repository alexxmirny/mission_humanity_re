#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event member 14 ("MAP_OBJECTS_REFRESH" per the strategic-sim notes' resolved enum table,
// same gap sim_unit_init_record.h documents -- no generated C++ enum exists). Named locally per that
// file's own established convention (one member per call site rather than a shared header).
inline constexpr uint32_t CREATE_BUILDING_MAP_OBJECTS_REFRESH = 14u;

// llm_strat_bldg_state member CONSTRUCTION (0x64 = 100). A real Ghidra enum member (the .c draft
// auto-substitutes this name at the state-init site, and sim_bldg_find_idle_producer.h independently
// documents IDLE_ACTIVATE=1/IDLE_NOOP_88=0x88 as members of the SAME enum) but, like
// MAP_OBJECTS_REFRESH above, has no generated C++ enum header -- named locally per the same fallback.
inline constexpr uint16_t CREATE_BUILDING_STATE_CONSTRUCTION = 0x64u;

// ---- the outward calls, indirected so the body stays testable by net_selftest.exe simtest ----------
// A direct mh::call:: (fill_data/set_event) OR a direct public-wrapper call (the five translated
// siblings) inside a detail:: body reaches the live game image / resolves state() to a stock VA, which
// makes the body uncoverable offline -- matching every other multi-callee TU in this domain. See the
// SIBLINGS banner above for why the five already-translated siblings are members here (bound to their
// real public wrappers in live_create_building_calls(), so production behaviour is unchanged).
struct create_building_calls {
    // utils_fill_data @0x004d1780. Bulk zero-fill of the whole new record, an address escape to an
    // ORIGINAL callee -- same narrow exception class sim_state.h's text_scratch()/ctrl_group_at() and
    // sim_unit_init_record.cpp's HAZARD 1 document (one record's address, one call, no arithmetic past
    // it, no pointer held afterward).
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_);
    // game_SetEvent @0x00413a52. Called unconditionally with CREATE_BUILDING_MAP_OBJECTS_REFRESH.
    uint32_t (*set_event)(uint32_t type);

    // ---- ALREADY-TRANSLATED siblings, bound to their PUBLIC wrappers (see the SIBLINGS banner) ------
    // llm_strat_sight_add_circle @0x004968b6 (sim_fog_of_war.h). UNCONDITIONAL -- registers the sight
    // circle over the footprint.
    void (*sight_add_circle)(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                             uint8_t sight);
    // map_ApplyAreaToMap @0x00424406 (sim_map_apply_area.h). UNCONDITIONAL -- registers map-region
    // occupancy over the SAME footprint. `area` is the whole 10x10 mask (const in the sibling;
    // non-const uint8_t* here to match the committed public-wrapper prototype exactly -- TACT1-P C6,
    // 2026-09-04).
    void (*apply_area_to_map)(int32_t x_0, int32_t y_0, uint8_t *area);
    // llm_strat_bldg_assign_workers @0x00491b78 (sim_bldg_worker_assign.h). CONDITIONAL (idle human
    // population AND cfg builder_count != 0). Returns the actual workers moved (discarded here).
    int32_t (*assign_workers)(uint32_t player, uint32_t building_id, int32_t count);
    // llm_strat_bldg_set_staffed_flag @0x004966a4 (sim_bldg_staffed_flag.h). CONDITIONAL (type needs no
    // builders OR workers already present).
    void (*set_staffed_flag)(uint16_t player, int32_t building_index);
    // llm_strat_refresh_building @0x004705de (sim_refresh_building.h). UNCONDITIONAL, fires last.
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
};

const create_building_calls &live_create_building_calls();

namespace detail {

// map_CreateBuilding @0x004622cb. See the header banner above for the full derivation.
void create_building(const sim_view &v, sim_store &own, const create_building_calls &c, uint16_t player,
                     uint32_t index, uint32_t x_b, int32_t y_b, int32_t building_id, uint32_t param_6,
                     uint32_t sub_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_create_building_calls(). Matches the original's
// committed `__mh_watcall_ecx_ebx_volatile` shape (sig_map_CreateBuilding).
void create_building(uint16_t player, uint32_t index, uint32_t x_b, int32_t y_b, int32_t building_id,
                     uint32_t param_6, uint32_t sub_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
