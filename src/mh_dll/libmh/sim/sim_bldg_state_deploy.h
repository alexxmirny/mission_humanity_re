#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/H_MOTHER/A_SHUTTLE/H_SHUTTLE
#include "sim/sim_state.h"

namespace mh::sim {

// ---- this file's own literal operands, sourced from the Ghidra .c drafts' resolved
// llm_strat_bldg_state enum member names (rule 17a: existing enum, not yet a real C++ type -- see the
// declared_needs note above). NOT re-derived/invented -- transcribed from tmp/decomp_sim's
// llm_strat_bldg_state_deploy_anim_wait_00471239.c / _deploy_start_00471377.c.
inline constexpr uint16_t BLDG_STATE_TO_UNIT          = 0x7c; // Ghidra: TO_UNIT
inline constexpr uint16_t BLDG_STATE_DEPLOY_ANIM_WAIT = 0x84; // Ghidra: DEPLOY_ANIM_WAIT

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
// Signatures copied verbatim from addr/mh_calls.gen.h.
struct bldg_state_land_activate_calls {
    void (*bldg_register_online)(int16_t player, int32_t building_index, uint32_t param_3,
                                 uint32_t param_4, double anim_dur);
    void (*bldg_flush_cargo_hold)(uint32_t player, int32_t building_index);
    int32_t (*prod_unload_cargo_manifest)(uint32_t param_1, int32_t param_2);
};

struct bldg_state_deploy_start_calls {
    void (*bldg_start_liftoff_anim_mother)(uint32_t param_1, int32_t param_2, uint32_t param_3,
                                           uint32_t param_4, uint32_t param_5, uint32_t param_6);
    void (*bldg_start_liftoff_anim_shuttle)(uint32_t param_1, int32_t param_2, uint32_t param_3,
                                            uint32_t param_4, uint32_t param_5, uint32_t param_6);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_land_activate_calls &live_bldg_state_land_activate_calls();
const bldg_state_deploy_start_calls  &live_bldg_state_deploy_start_calls();

namespace detail {

// llm_strat_bldg_state_deploy_anim_wait @0x00471239. No params, no callees -- see the header
// derivation above.
void bldg_state_deploy_anim_wait(sim_store &own);

// llm_strat_bldg_state_land_activate @0x00471288. `param_1`/`param_2` are DEAD register carriers
// (EAX/EDX, overwritten before ever being read -- see the header's hazard note); kept in the
// signature for fidelity to the committed prototype, marked unused. `param_3`/`param_4` (EBX/ECX) are
// forwarded verbatim into llm_strat_bldg_register_online's own param_3/param_4 -- see the header
// derivation for why that forwarding is a real, observed effect and not invented.
void bldg_state_land_activate(const sim_view &v, sim_store &own, uint32_t param_1, uint32_t param_2,
                              uint32_t param_3, uint32_t param_4,
                              const bldg_state_land_activate_calls &c);

// llm_strat_bldg_state_deploy_start @0x00471377. Same param_1/param_2 dead / param_3/param_4
// forwarded shape as land_activate above (forwarded here into whichever start_liftoff_anim_* call
// this function's own type-dispatch selects).
void bldg_state_deploy_start(const sim_view &v, sim_store &own, uint32_t param_1, uint32_t param_2,
                             uint32_t param_3, uint32_t param_4,
                             const bldg_state_deploy_start_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// committed prototypes (sig_llm_strat_bldg_state_*) exactly.
void bldg_state_deploy_anim_wait();
void bldg_state_land_activate(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
void bldg_state_deploy_start(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);

namespace detail {
} // namespace detail

} // namespace mh::sim
