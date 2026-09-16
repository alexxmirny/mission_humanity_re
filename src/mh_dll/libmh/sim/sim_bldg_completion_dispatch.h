#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// DECLARED NEED 3: sim/'s own binding of the map resource plane. `mh::ai::ai_state.h` already binds
// this region (RID_RESOURCES, read-only) as `map_resources`; duplicated here per sim_state.h's own
// established mh/sim-does-not-depend-on-mh/ai rule (see e.g. `mine`/`cfg_planet` above it for the same
// posture). `sim_view::resources` and `sim_store::resources_at()` below are both DECLARED NEEDS --
// this alias only prepares the TYPE this file's code assumes those will use.
using map_resources = mh::game::mh_map_resources;

// ---- llm_strat_bldg_completion_dispatch's callees (all ORIGINAL, indirected through `calls`) -------
struct bldg_completion_dispatch_calls {
    uint32_t (*mine_scan_deposit_slot)(uint8_t slot_index, uint32_t player, int32_t building_index);
    void (*population_add)(uint16_t player, int32_t count);
    void (*resource_add)(int32_t player, int32_t resource_index, int32_t amount);
    void (*load_base_layout_dmp)(int32_t player, char *dmp_path);
    // DECLARED NEED 8 (see header banner): missing overload, written as if it exists.
    int32_t (*utils_sprintf__vssii)(void *dst, const char *format, const char *a0, const char *a1, int32_t a2,
                                    int32_t a3);
    int32_t (*bldg_uses_workers)(uint32_t player, int32_t building_index);
    int32_t (*bldg_unassign_workers)(uint16_t player, uint32_t building_index, uint32_t count);
    int32_t (*bldg_assign_workers)(uint32_t player, uint32_t building_id, int32_t count);
    void (*bldg_set_staffed_flag)(uint16_t player, int32_t building_index);
    void (*bldg_clear_staffed_flag)(uint16_t player, uint32_t building_id);
    void (*game_UpdateProgress)(uint16_t plr, uint16_t inv);
    void (*bldg_register_online)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                                 double anim_dur);
    void (*bldg_link_to_network_if_adjacent)(uint16_t player, uint32_t index);
    void (*prod_deliver_arrivals)();
    int32_t (*unit_spawn_docked)(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot);
    int32_t (*unit_add_docked)(uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot);
    int32_t (*unit_type_group_index)(int32_t unit_id);
    int32_t (*reason_to_housing_bldg)(uint32_t param_1, int32_t param_2);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    int32_t (*w_sprintf__vsss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1,
                               const wchar_t *a2);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    void (*unit_apply_production_completion)(uint32_t player, int32_t unit_proto_id);
    void (*ai_notify_unit_lifecycle)(uint16_t player_, uint16_t unit_type, uint32_t unit_id, uint32_t param_4);
    int32_t (*invasion_chance_roll)(int32_t building_completed);
};

const bldg_completion_dispatch_calls &live_bldg_completion_dispatch_calls();

namespace detail {

// llm_strat_bldg_completion_dispatch @0x004795dd. See the header banner above for the full
// derivation; param_3/param_4 are accepted (matching the committed prototype) but never read --
// CONFIRMED dead, not merely carried for signature compatibility.
void bldg_completion_dispatch(const sim_view &v, sim_store &own, const bldg_completion_dispatch_calls &c,
                              uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                              double param_5);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_completion_dispatch_calls(). Matches the
// committed prototype (addr/mh_export.gen.h's sig_llm_strat_bldg_completion_dispatch) exactly.
void bldg_completion_dispatch(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                              double param_5);

namespace detail {
} // namespace detail

} // namespace mh::sim
