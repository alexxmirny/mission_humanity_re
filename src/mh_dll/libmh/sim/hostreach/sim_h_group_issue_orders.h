#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI_CARGO / UNIT_TYPE_H_HELI_CARGO (already named here)
#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls both functions reach, indirected for offline (net_selftest simtest) testability
// like every other sim/ TU. Neither function writes sim state directly -- everything below is either
// a pure read of `v` or an outward call -- so there is no sim_store parameter anywhere in this file.
struct group_issue_orders_calls {
    // ---- OWNED rows (already-translated sim bodies), bound via MH_LIBMH_BIND in the .cpp ----------
    uint8_t (*select_weapon)(uint16_t player, int32_t unit_index,
                             uint32_t target_mask); // llm_strat_unit_select_weapon @0x0048ba9e
    void (*order_attack_target)(uint32_t player, int32_t unit_idx, uint32_t target_side,
                                int32_t  target_unit_idx,
                                uint32_t weapon_idx); // llm_strat_unit_order_attack_target @0x0046af1e
    void (*order_attack_target_alt)(uint32_t player, int32_t unit_idx, uint32_t target_side,
                                    int32_t  target_unit_idx,
                                    uint32_t weapon_idx); // _target_alt @0x0046b599
    void (*order_attack_unit)(uint32_t player, int32_t unit_idx, uint32_t target_side,
                              int32_t  target_unit_idx,
                              uint32_t weapon_idx); // llm_strat_unit_order_attack_unit @0x0046bc14
    void (*order_attack_building_reposition)(uint32_t player, int32_t unit_idx, uint32_t target_side,
                                             int32_t  target_bldg_idx,
                                             uint32_t weapon_idx); // @0x0046bf9c
    void (*order_exit_storage)(uint32_t player, uint32_t unit_idx, int32_t a2, uint32_t param_4,
                               uint32_t param_5);     // llm_strat_unit_order_exit_storage @0x0046a6b9
    int32_t (*unit_state_is_boarding)(int32_t state); // llm_unit_state_is_boarding @0x004967ce

    // ---- FRONTIER originals (never reimplemented here), bound via mh::call:: in the .cpp -----------
    void (*race_alert_sound_emit)(); // llm_strat_race_alert_sound_emit @0x00425a6e
    void (*group_order_ack_voice)(); // llm_strat_group_order_ack_voice @0x004259b2
};

const group_issue_orders_calls &live_group_issue_orders_calls();

namespace detail {

// llm_strat_group_issue_attack_order @0x00444894. See the header banner for the selector-chain,
// modifier-key-bit, and bVar1-artifact derivations. `param_1` and `player` are the committed
// (unrenamed -- Ghidra never recovered semantic names) prototype parameter names; from the call
// shape (an id compared against a per-group member and forwarded as the order's target index) they
// most likely carry a target unit/building id and the order's owning-side tag respectively, but
// that reading is NOT confirmed against any other evidence, so the names are left as committed.
void group_issue_attack_order(const sim_view &v, const group_issue_orders_calls &c, uint32_t param_1,
                              uint32_t player, uint16_t selector);

// llm_strat_group_issue_enter_building_order @0x004451d2. Issues llm_strat_unit_order_exit_storage on
// every _G_LLM_STRAT_CTRL_GROUPS[0] member that is not a currently-boarding cargo-heli, forwarding
// param_1/param_2/param_3 positionally (register trace: EBX/ECX/stack -- see the .cpp).
void group_issue_enter_building_order(const sim_view &v, const group_issue_orders_calls &c,
                                      uint32_t param_1, uint32_t param_2, uint32_t param_3);

} // namespace detail

// Public wrappers. Signatures match sig_llm_strat_group_issue_attack_order /
// sig_llm_strat_group_issue_enter_building_order (addr/mh_export.gen.h) EXACTLY -- these are the
// promotion seam the conductor's promoted_arm adapter forwards to.
void group_issue_attack_order(uint32_t param_1, uint32_t player, uint16_t selector);
void group_issue_enter_building_order(uint32_t param_1, uint32_t param_2, uint32_t param_3);

} // namespace mh::sim
