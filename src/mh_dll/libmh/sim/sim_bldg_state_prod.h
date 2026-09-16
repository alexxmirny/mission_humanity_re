#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_prod_pick_next's callees ---------------------------------------------------
struct bldg_state_prod_pick_next_calls {
    int32_t (*prod_try_start_unit)(uint32_t player, int32_t unit_type);
    void (*ai_notify_unit_lifecycle)(uint16_t player, uint16_t unit_type, uint32_t unit_id, uint32_t param_4);
};

const bldg_state_prod_pick_next_calls &live_bldg_state_prod_pick_next_calls();

// ---- llm_strat_bldg_state_prod_working's callees -------------------------------------------------------
struct bldg_state_prod_working_calls {
    void (*completion_dispatch)(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                                double param_5);
    void (*snd_play)(int32_t sound_id, int32_t volume);
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    uint32_t (*set_event)(uint32_t type);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_prod_working_calls &live_bldg_state_prod_working_calls();

// ---- llm_strat_bldg_state_prod_blocked_notify's callees --------------------------------------------------
struct bldg_state_prod_blocked_notify_calls {
    int32_t (*reason_to_housing_bldg)(uint32_t reason, int32_t player_race);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    int32_t (*w_sprintf__vsss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1,
                               const wchar_t *a2);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
};

const bldg_state_prod_blocked_notify_calls &live_bldg_state_prod_blocked_notify_calls();

// llm_strat_bldg_state_prod_retry_wait makes no calls at all -- no `calls` struct needed.

namespace detail {

// llm_strat_bldg_state_prod_pick_next @0x00473a20. No parameters (void(void), committed prototype).
void bldg_state_prod_pick_next(const sim_view &v, sim_store &own, const bldg_state_prod_pick_next_calls &c);

// llm_strat_bldg_state_prod_working @0x00473ca8. FOUR real parameters (param_1/param_2 dead, param_3/
// param_4 forwarded verbatim to completion_dispatch -- see the header derivation above).
void bldg_state_prod_working(const sim_view &v, sim_store &own, const bldg_state_prod_working_calls &c,
                             uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);

// llm_strat_bldg_state_prod_blocked_notify @0x00474139. No parameters (void(void), committed
// prototype).
void bldg_state_prod_blocked_notify(const sim_view &v, sim_store &own,
                                    const bldg_state_prod_blocked_notify_calls &c);

// llm_strat_bldg_state_prod_retry_wait @0x0047430b. No parameters (void(void), committed prototype).
void bldg_state_prod_retry_wait(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// originals' committed prototypes exactly (addr/mh_export.gen.h's sig_llm_strat_bldg_state_*).
void bldg_state_prod_pick_next();
void bldg_state_prod_working(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
void bldg_state_prod_blocked_notify();
void bldg_state_prod_retry_wait();

namespace detail {
} // namespace detail

} // namespace mh::sim
