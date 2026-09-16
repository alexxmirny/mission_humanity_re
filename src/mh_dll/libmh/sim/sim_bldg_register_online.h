#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_register_online's 7 out-of-scope callees ---------------------------------------
struct register_online_calls {
    void (*online_helipad_h_or_misc)(int16_t player, int32_t building_index, uint32_t param_3,
                                     uint32_t param_4, double anim_dur);
    void (*online_airfield_a)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                              double anim_dur);
    void (*online_airfield_h)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                              double anim_dur);
    void (*online_shuttle_a)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                             double anim_dur);
    void (*online_shuttle_h)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                             double anim_dur);
    void (*online_port_a)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                          double anim_dur);
    void (*online_port_h)(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                          double anim_dur);
};

const register_online_calls &live_register_online_calls();

namespace detail {

// llm_strat_bldg_online_default @0x004749ed. `player` is a committed `int32_t` (unlike its four
// siblings below) -- see the header banner.
void bldg_online_default(const sim_view &v, sim_store &own, int32_t player, int32_t building_index,
                         uint32_t param_3, uint32_t param_4, double anim_dur);

// llm_strat_bldg_online_barracks_garage_a @0x00474bfc.
void bldg_online_barracks_garage_a(const sim_view &v, sim_store &own, int16_t player,
                                   int32_t building_index, uint32_t param_3, uint32_t param_4,
                                   double anim_dur);

// llm_strat_bldg_online_vehicles_h @0x00474d4a. `player` is a committed `int32_t` (same note as
// online_default).
void bldg_online_vehicles_h(const sim_view &v, sim_store &own, int32_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur);

// llm_strat_bldg_online_soldiers_h @0x00474f33.
void bldg_online_soldiers_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur);

// llm_strat_bldg_online_helipad_a @0x004750e5. The ONLY one of the five that writes
// `online_state = 2` instead of `1` -- see the header banner.
void bldg_online_helipad_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                           uint32_t param_3, uint32_t param_4, double anim_dur);

// llm_strat_bldg_register_online @0x004747d6. The type-range dispatcher; see the header banner for
// the full type -> target mapping. `param_3`/`param_4` are accepted (matching the committed
// prototype) but never read anywhere in this function's own body.
void bldg_register_online(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                          uint32_t param_3, uint32_t param_4, double anim_dur,
                          const register_online_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() (and, for the dispatcher, live_register_online_calls()).
// Match the committed prototypes (addr/mh_export.gen.h's sig_llm_strat_bldg_register_online /
// sig_llm_strat_bldg_online_default / _online_barracks_garage_a / _online_barracks_h / _online_garage_h /
// _online_helipad_a) exactly.
void bldg_online_default(int32_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                         double anim_dur);
void bldg_online_barracks_garage_a(int16_t player, int32_t building_index, uint32_t param_3,
                                   uint32_t param_4, double anim_dur);
void bldg_online_vehicles_h(int32_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur);
void bldg_online_soldiers_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur);
void bldg_online_helipad_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                           double anim_dur);
void bldg_register_online(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                          double anim_dur);

namespace detail {
} // namespace detail

} // namespace mh::sim
