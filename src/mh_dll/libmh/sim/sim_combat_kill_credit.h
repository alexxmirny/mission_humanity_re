#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls shared by bldg_kill_credit / unit_kill_credit ------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest. ONE
// struct for both functions (bldg_kill_credit simply never touches `math_scale_pct`) rather than two
// near-identical structs, since both are translated in this same file.
struct kill_credit_calls {
    double (*math_scale_pct)(double value, int32_t pct); // llm_math_scale_pct @0x0044b3b3
    void (*snd_play)(int32_t sound_id, int32_t volume);  // llm_snd_play @0x00425233
    void (*ui_print_queue_text_id)(int32_t text_id);     // llm_ui_print_queue_text_id @0x0049653d
    void (*ai_bldg_register_visible_building)(int32_t victim_index, uint32_t victim_ref,
                                              uint32_t aggressor_unit_index, uint32_t aggressor_ref,
                                              int32_t victim_destroyed); // @0x004db22f
};

const kill_credit_calls &live_kill_credit_calls();

namespace detail {

// llm_strat_bldg_kill_credit @0x0044c67f. See the header banner above.
void bldg_kill_credit(const sim_view &v, sim_store &own, const kill_credit_calls &c,
                      uint32_t victim_player, int32_t victim_building_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index);

// llm_strat_unit_kill_credit @0x0044cac9. See the header banner above.
void unit_kill_credit(const sim_view &v, sim_store &own, const kill_credit_calls &c,
                      uint32_t victim_player, int32_t victim_unit_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index);

// llm_strat_apply_area_damage @0x0044c3fb. Calls bldg_kill_credit/unit_kill_credit above directly
// (same TU, same `v`/`own`/`c`), per this batch's _CONTEXT.md.
void apply_area_damage(const sim_view &v, sim_store &own, const kill_credit_calls &c, int32_t x,
                       int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                       uint32_t owner_filter_zeroed, uint32_t killer_info, int32_t killer_unit_index);

} // namespace detail

// Live wrappers: the logic applied to state() and live_kill_credit_calls(). Signatures match the
// committed __watcall shapes already in addr/mh_calls.gen.h exactly.
void bldg_kill_credit(uint32_t victim_player, int32_t victim_building_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index);
void unit_kill_credit(uint32_t victim_player, int32_t victim_unit_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index);
void apply_area_damage(int32_t x, int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                       uint32_t owner_filter_zeroed, uint32_t killer_info, int32_t killer_unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
