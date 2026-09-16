#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// llm_strat_bldg_anim_state_helipad's 4 callees, injectable so the offline oracle can mock them instead
// of running their real (unboundedly-reaching, in unit_takeoff_finalize's case) bodies -- see the
// header's "NOT RIG-ARMABLE" derivation. The live wrapper binds these to the real mh::call:: stubs.
struct bldg_anim_state_helipad_calls {
    void (*online_helipad_h_or_misc)(int16_t player, int32_t building_index, uint32_t param_3,
                                     uint32_t param_4, double anim_dur);
    void (*unit_takeoff_finalize)(uint32_t player, uint32_t unit_index);
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode);
    void (*path_free_slot)(uint16_t player, int32_t unit_index);
};
const bldg_anim_state_helipad_calls &live_bldg_anim_state_helipad_calls();

namespace detail {

void bldg_anim_state_barracks_garage_a(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                       uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx);
void bldg_anim_state_vehicles_h(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx);
void bldg_anim_state_soldiers_h(const sim_view &v, sim_store &own, uint32_t unused_eax, uint32_t unused_edx,
                                uint32_t unused_ebx, uint32_t unused_ecx);
void bldg_anim_state_helipad(const sim_view &v, sim_store &own, const bldg_anim_state_helipad_calls &c,
                             uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                             uint32_t unused_ecx);
void bldg_anim_state_airfield_a(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx);
void bldg_anim_state_shuttle_a(const sim_view &v, sim_store &own, uint32_t unused_eax, uint32_t unused_edx,
                               uint32_t unused_ebx, uint32_t unused_ecx);
void bldg_anim_state_shuttle_h(const sim_view &v, sim_store &own, uint32_t unused_eax, uint32_t unused_edx,
                               uint32_t unused_ebx, uint32_t unused_ecx);
void bldg_anim_state_online_toggle(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                   uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx);

} // namespace detail

// Live wrappers -- match each original's committed prototype exactly (addr/mh_export.gen.h's
// sig_llm_strat_bldg_anim_state_*).
void bldg_anim_state_barracks_garage_a(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                       uint32_t unused_ecx);
void bldg_anim_state_vehicles_h(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                uint32_t unused_ecx);
void bldg_anim_state_soldiers_h(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                uint32_t unused_ecx);
void bldg_anim_state_helipad(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                             uint32_t unused_ecx);
void bldg_anim_state_airfield_a(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                uint32_t unused_ecx);
void bldg_anim_state_shuttle_a(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                               uint32_t unused_ecx);
void bldg_anim_state_shuttle_h(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                               uint32_t unused_ecx);
void bldg_anim_state_online_toggle(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                   uint32_t unused_ecx);

namespace detail {
} // namespace detail

} // namespace mh::sim
