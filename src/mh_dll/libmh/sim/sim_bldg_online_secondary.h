#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim::detail {

void bldg_online_helipad_h_or_misc(const sim_view &v, sim_store &own, int16_t player,
                                   int32_t building_index, uint32_t param_3, uint32_t param_4,
                                   double anim_dur);
void bldg_online_airfield_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur);
void bldg_online_airfield_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur);
void bldg_online_shuttle_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                           uint32_t param_3, uint32_t param_4, double anim_dur);
void bldg_online_shuttle_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                           uint32_t param_3, uint32_t param_4, double anim_dur);
void bldg_online_port_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                        uint32_t param_3, uint32_t param_4, double anim_dur);
void bldg_online_port_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                        uint32_t param_3, uint32_t param_4, double anim_dur);

} // namespace mh::sim::detail

namespace mh::sim {

// Live wrappers: the logic applied to state(). Match the committed prototypes
// (addr/mh_export.gen.h's sig_llm_strat_bldg_online_helipad_h_or_misc / _airfield_a / _airfield_h /
// _shuttle_a / _shuttle_h / _port_a / _port_h) exactly.
void bldg_online_helipad_h_or_misc(int16_t player, int32_t building_index, uint32_t param_3,
                                   uint32_t param_4, double anim_dur);
void bldg_online_airfield_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur);
void bldg_online_airfield_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur);
void bldg_online_shuttle_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                           double anim_dur);
void bldg_online_shuttle_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                           double anim_dur);
void bldg_online_port_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                        double anim_dur);
void bldg_online_port_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                        double anim_dur);

namespace detail {
} // namespace detail

} // namespace mh::sim
