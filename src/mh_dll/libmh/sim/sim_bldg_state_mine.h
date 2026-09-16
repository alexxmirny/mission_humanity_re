#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_mine_check_deposits's callees ---------------------------------------------
struct bldg_state_mine_check_deposits_calls {
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    void (*snd_play)(int32_t sound_id, int32_t volume);
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_mine_check_deposits_calls &live_bldg_state_mine_check_deposits_calls();

// ---- llm_strat_bldg_state_mine_extracting's callees ---------------------------------------------------
struct bldg_state_mine_extracting_calls {
    void (*bldg_completion_dispatch)(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                                     double param_5);
};

const bldg_state_mine_extracting_calls &live_bldg_state_mine_extracting_calls();

// mine_depleted / mine_rescan_wait have no callees at all -- no `_calls` struct, same "no callees, so
// no _calls table" precedent sim_bldg_state_deploy.cpp / sim_bldg_state_power.h already use.

namespace detail {

// llm_strat_bldg_state_mine_check_deposits @0x004743e1. See the header derivation above.
void bldg_state_mine_check_deposits(const sim_view &v, sim_store &own,
                                    const bldg_state_mine_check_deposits_calls &c);

// llm_strat_bldg_state_mine_extracting @0x0047457a. Four committed params (EAX/EDX/EBX/ECX);
// param_1/param_2 are genuinely dead, param_3/param_4 are forwarded verbatim to
// completion_dispatch -- see the REGISTER FORWARDING note above.
void bldg_state_mine_extracting(const sim_view &v, sim_store &own, const bldg_state_mine_extracting_calls &c,
                                uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);

// llm_strat_bldg_state_mine_depleted @0x0047464e. No parameters, no callees.
void bldg_state_mine_depleted(sim_store &own);

// llm_strat_bldg_state_mine_rescan_wait @0x0047468e. No parameters, no callees.
void bldg_state_mine_rescan_wait(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls() (where one
// exists). Match the originals' committed prototypes exactly (addr/mh_export.gen.h's
// sig_llm_strat_bldg_state_mine_*).
void bldg_state_mine_check_deposits();
void bldg_state_mine_extracting(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
void bldg_state_mine_depleted();
void bldg_state_mine_rescan_wait();

namespace detail {
} // namespace detail

} // namespace mh::sim
