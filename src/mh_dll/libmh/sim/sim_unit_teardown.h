#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_MP_LOCKSTEP, EVENT_INFO_REFRESH
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_WALKER/A_HELI/A_HELI_MOTHER/UNDEFINED
#include "sim/sim_order_enqueue.h"        // UNIT_TYPE_A_PLANE

namespace mh::sim {

// UNIT_TYPE_H_HELI_MOTHER is NOT re-declared here -- it already lives at `mh::sim` scope via
// sim_unit_type_predicates.h's own UNIT_TYPE_A_HELI_MOTHER/H_HELI_MOTHER pair (both pulled in by the
// include above), same as sim_unit_on_destroyed.h's identical mother-ship-type check reuses them.

// unit::state == 4, this function's own literal (0x00487f41's EBX operand). See the DECLARED NEED
// above: no backing Ghidra enum, so this is a commented magic number naming the plate's own
// "corpse_fow_decay" reading, not an invented enum.
inline constexpr int16_t UNIT_TEARDOWN_STATE_CORPSE_FOW_DECAY = 4;

// The slot-[0] live-unit accumulator step. CONDUCTOR-RESOLVED (read via ReVA read-memory, not
// inferred): DAT_0050148a is the IEEE-754 double bytes `00 00 00 00 00 00 F0 BF`, i.e. EXACTLY -1.0
// -- the negative mirror of llm_strat_unit_init_record's own +1.0 step on the identical
// units[player][0].energy slot. Added to that slot only when THIS unit's own energy was > 0.0 right
// before being zeroed (see the .cpp for the exact x87 FCOMP gating). Do not re-derive; this value is
// resolved, not guessed.
inline constexpr double UNIT_TEARDOWN_LIVE_ACCUM_STEP = -1.0; // DAT_0050148a

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as sim_unit_on_destroyed.h / sim_unit_remove_from_map.h: a direct
// mh::call:: inside a detail:: body reaches into the live game image, making the body untestable by
// net_selftest.exe simtest. All seven are ORIGINAL functions outside this batch -- none need
// reimplementing here; live_unit_teardown_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .cpp).
struct unit_teardown_calls {
    void (*unit_ctrlgroup_leave)(uint32_t unit_index);
    uint32_t (*set_event)(uint32_t type);
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode);
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode);
    void (*unit_set_state_of)(int32_t player, int32_t unit_index, int16_t state);
    int32_t (*player_teardown_hook_stub)(int32_t player_index);
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index, int32_t hard_remove);
};

const unit_teardown_calls &live_unit_teardown_calls();

namespace detail {

// llm_strat_unit_teardown @0x00487ba5. See the header derivation above for the full 10-step shape and
// the two threshold-order/phantom-field hazards.
void unit_teardown(const sim_view &v, sim_store &own, const unit_teardown_calls &c, uint32_t player,
                   uint16_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_teardown_calls(). Matches the original's
// committed __watcall(EAX,EDX) shape (sig_llm_strat_unit_teardown).
void unit_teardown(uint32_t player, uint16_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
