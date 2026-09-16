//
// sim/sim_target_release_ref.h -- llm_strat_target_release_ref, the attack-commit/release
// bookkeeping helper (RI-SIM / SIM1F).
//
// One function: llm_strat_target_release_ref @0x004dac44 (0x1bf bytes). Called with mode 0 to
// COMMIT an attacker's weapon-damage estimate onto its current target's "incoming threat" ledger
// (a unit's incoming_threat_damage or a building's incoming_damage_tally, chosen by the packed
// target_ref's 0x40 "is a building" bit), and with mode 1 to RELEASE that commitment (undo the
// ledger addition and clear the attacker's own commit-flag bits and cached damage estimate). Modes
// 2 and 3 (and any other low-nibble value) are a no-op in the original -- the jump table's third and
// fourth entries both land directly on the epilogue. sim_unit_target_tracking.cpp/
// sim_weapon_damage_calc.cpp already call this function (unmigrated) with mode==3 for that reason.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct target_release_ref_calls {
    // llm_strat_unit_estimate_weapon_damage @0x004d32aa -- already translated + verified (SIM1E,
    // sim/sim_weapon_damage_calc.cpp). Bound directly to mh::call:: (matches its committed
    // `uint32_t(int32_t player, int32_t unit_index, uint32_t target_ref, int32_t target_index)`
    // signature exactly).
    uint32_t (*estimate_weapon_damage)(int32_t player, int32_t unit_index, uint32_t target_ref,
                                       int32_t target_index);
};

const target_release_ref_calls &live_target_release_ref_calls();

namespace detail {

// llm_strat_target_release_ref @0x004dac44.
void target_release_ref(const sim_view &v, sim_store &own, const target_release_ref_calls &c,
                        uint32_t player_idx, int32_t unit_idx, uint32_t mode);

} // namespace detail

// Live wrapper: the logic applied to state() and live_target_release_ref_calls(). Matches the
// original's committed __mh_watcall_ebx_volatile(EAX,EDX,EBX) shape.
void target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode);

namespace detail {


} // namespace detail

} // namespace mh::sim
