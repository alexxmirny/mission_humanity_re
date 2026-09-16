#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PRODUCTION (0x01) / BUILDING_TYPE_H_PRODUCTION (0x15)
#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_upgrading's callees ---------------------------------------------------
struct bldg_state_upgrading_calls {
    void (*ai_queue_release_order)(int32_t player, int32_t building_index, int32_t mode);
    void (*snd_play)(int32_t sound_id, int32_t volume);
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    void (*completion_dispatch)(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                                double param_5);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_upgrading_calls &live_bldg_state_upgrading_calls();

// ---- llm_strat_bldg_state_researching's callees ------------------------------------------------------
struct bldg_state_researching_calls {
    void (*snd_play)(int32_t sound_id, int32_t volume);
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    void (*completion_dispatch)(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                                double param_5);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_researching_calls &live_bldg_state_researching_calls();

namespace detail {

// llm_strat_bldg_state_upgrading @0x00472c42. void(void), no parameters (committed prototype).
void bldg_state_upgrading(const sim_view &v, sim_store &own, const bldg_state_upgrading_calls &c);

// llm_strat_bldg_state_researching @0x00472f3c. void(void), no parameters (committed prototype).
void bldg_state_researching(const sim_view &v, sim_store &own, const bldg_state_researching_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// originals' committed prototypes exactly (addr/mh_export.gen.h's sig_llm_strat_bldg_state_*).
void bldg_state_upgrading();
void bldg_state_researching();

namespace detail {
} // namespace detail

} // namespace mh::sim
