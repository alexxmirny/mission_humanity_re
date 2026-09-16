#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// This file's own literal operands (the resolved `llm_strat_bldg_state` enum member names
// BLDG_STATE_TURRET_SCAN=0x7a/_ATTACK=0x7b, rule 17a, and the DAT_005012e4 DECLARED-NEED placeholder)
// are defined file-locally inside an ANONYMOUS namespace in the .cpp, not here -- unlike
// sim_bldg_state_reset_idle.h's own TURRET_SCAN copy (plain `mh::sim`-scope `inline constexpr`, which
// works only as long as no single TU ever includes both headers), giving them internal linkage per
// TU rules out any such collision, matching sim_order_dispatch_bldg.cpp's own BLDG_STATE_TURRET_ATTACK
// precedent. Neither constant is needed by this header's own declarations.

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
// Signatures copied verbatim from addr/mh_calls.gen.h.
struct bldg_state_turret_scan_calls {
    uint32_t (*turret_acquire_target)(uint32_t player, int32_t building_index, uint32_t *out_target_ref,
                                      uint32_t *out_target_slot);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

struct bldg_state_turret_attack_calls {
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
    uint32_t (*dist_out_of_range)(int32_t owner_filter, int32_t threshold, int32_t own_tile_x,
                                  int32_t own_tile_y, int32_t target_tile_x, int32_t target_tile_y);
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
    void (*turret_fire)(uint32_t player, uint32_t bldg_index, int32_t target_fine_x,
                        int32_t target_fine_y, int32_t target_elevation, uint32_t last_tick_time_lo,
                        uint32_t last_tick_time_hi, uint8_t fire_kind);
};

const bldg_state_turret_scan_calls   &live_bldg_state_turret_scan_calls();
const bldg_state_turret_attack_calls &live_bldg_state_turret_attack_calls();

namespace detail {

// llm_strat_bldg_state_turret_scan @0x00471ab1. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation.
void bldg_state_turret_scan(const sim_view &v, sim_store &own, const bldg_state_turret_scan_calls &c);

// llm_strat_bldg_state_turret_attack @0x00471e53. REVIEW_REQUIRED=true. See the header banner above.
void bldg_state_turret_attack(const sim_view &v, sim_store &own, const bldg_state_turret_attack_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// committed prototypes (sig_llm_strat_bldg_state_turret_scan/_attack) exactly.
void bldg_state_turret_scan();
void bldg_state_turret_attack();

namespace detail {
} // namespace detail

} // namespace mh::sim
