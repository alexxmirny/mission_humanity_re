#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PRODUCTION (0x01) / BUILDING_TYPE_H_PRODUCTION (0x15)
#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_construction's callees ---------------------------------------------------
struct bldg_state_construction_calls {
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    void (*snd_play)(int32_t sound_id, int32_t volume);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    void (*bldg_construction_complete)(uint32_t player, uint32_t building_index, uint32_t param_3,
                                       uint32_t param_4);
    void (*ai_notify_bldg_constructed)(uint32_t player, uint32_t x_b, uint32_t param_3, uint32_t building_id,
                                       uint32_t y_b, uint32_t param_6);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_construction_calls &live_bldg_state_construction_calls();

// ---- llm_strat_bldg_state_charge_gate's callees ------------------------------------------------------
struct bldg_state_charge_gate_calls {
    int32_t (*bldg_pay_cycle_inputs)(uint32_t player, uint32_t b_index);
    void (*ai_queue_release_order)(int32_t player, int32_t building_index, int32_t mode);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    void (*completion_dispatch)(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                                double param_5);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
};

const bldg_state_charge_gate_calls &live_bldg_state_charge_gate_calls();

// ---- llm_strat_bldg_state_charge_step's callees -------------------------------------------------------
struct bldg_state_charge_step_calls {
    void (*bldg_update_charge_pips)(uint16_t player, uint32_t building_id);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_charge_step_calls &live_bldg_state_charge_step_calls();

namespace detail {

// llm_strat_bldg_state_construction @0x004724af. See the header derivation above -- FOUR real
// parameters (param_1/param_2 dead, param_3/param_4 forwarded, sometimes overwritten first; see the
// REGISTER-REUSE HAZARD note).
void bldg_state_construction(const sim_view &v, sim_store &own, const bldg_state_construction_calls &c,
                             uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);

// llm_strat_bldg_state_charge_gate @0x00472674. No parameters (void(void), committed prototype).
void bldg_state_charge_gate(const sim_view &v, sim_store &own, const bldg_state_charge_gate_calls &c);

// llm_strat_bldg_state_charge_step @0x00472979. No parameters (void(void), committed prototype).
void bldg_state_charge_step(const sim_view &v, sim_store &own, const bldg_state_charge_step_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// originals' committed prototypes exactly (addr/mh_export.gen.h's sig_llm_strat_bldg_state_*).
void bldg_state_construction(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
void bldg_state_charge_gate();
void bldg_state_charge_step();

namespace detail {
} // namespace detail

} // namespace mh::sim
