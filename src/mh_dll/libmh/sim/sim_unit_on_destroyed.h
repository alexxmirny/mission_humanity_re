#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_SP, SESSION_MP_LOCKSTEP, EVENT_INFO_REFRESH
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER, UNIT_TYPE_H_HELI_MOTHER

namespace mh::lockstep {
struct overlay_hoist_ops; // LIB-ABI stage E -- lockstep/overlay_hoist.h
}

namespace mh::sim {

// This function's own literal operands with no backing Ghidra enum (checked docs/structs.md and
// mh_structs.gen.h for a text-id / sound-id / outcome-code domain and found none -- same "no enum
// exists" situation sim_unit_recruit.h's own RECRUIT_ERR_* codes document, so named locally per
// rule 17a's fallback rather than left as bare literals scattered through the .cpp).
//
// The two G_TEXT_PTRS text ids llm_ui_print_queue_text_id is called with: a race-dependent
// "<race> commander lost" line (0x69 for _G_LLM_STRAT_PLAYER_RACE==1, 0xb1 otherwise), followed
// UNCONDITIONALLY by a second, race-independent line (0x7e).
inline constexpr int32_t MOTHER_LOST_TEXT_ID_RACE1  = 0x69;
inline constexpr int32_t MOTHER_LOST_TEXT_ID_OTHER  = 0xb1;
inline constexpr int32_t MOTHER_LOST_TEXT_ID_SUFFIX = 0x7e;
inline constexpr int32_t MOTHER_LOST_SOUND_ID       = 0xa6;
inline constexpr int32_t MOTHER_LOST_SOUND_VOLUME   = 100;
inline constexpr uint8_t MOTHER_LOST_OUTCOME_CODE   = 4;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as sim_unit_recruit.h / sim_bldg_finish_order.h: a direct
// mh::call:: inside a detail:: body reaches into the live game image, which makes the body
// untestable by net_selftest.exe simtest. All ten are ORIGINAL functions outside this batch -- none
// need reimplementing here; live_unit_on_destroyed_calls() is the only binder. Signatures copied
// verbatim from addr/mh_calls.gen.h (checked against this function's own register-order use in the
// .cpp, not assumed).
struct unit_on_destroyed_calls {
    void (*prod_unbind_planet)(int32_t player, int32_t planet_slot);
    void (*prod_shuttle_slot_release)(int32_t player, int32_t slot);
    void (*unit_unlink_tile)(uint32_t unit_player, uint16_t unit_index);
    void (*population_remove)(uint32_t player, int32_t count);
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius);
    void (*unit_ctrlgroup_leave)(uint32_t unit_index);
    uint32_t (*set_event)(uint32_t type);
    void (*ui_print_queue_text_id)(int32_t text_id);
    void (*snd_play)(int32_t sound_id, int32_t volume);
    int32_t (*ui_outcome_dialog)(uint8_t outcome);
    // LIB-ABI stage E hoist ops (lockstep/overlay_hoist.h) -- tail member, null-skipped by
    // suites, which is what keeps the R3b panel hoist out of the offline arm where there is
    // no game process and therefore no panel state to keep consistent.
    const mh::lockstep::overlay_hoist_ops *hoist;
};

const unit_on_destroyed_calls &live_unit_on_destroyed_calls();

namespace detail {

// llm_strat_unit_on_destroyed @0x004877bc. See the header hazards above for the full derivation.
void unit_on_destroyed(const sim_view &v, sim_store &own, const unit_on_destroyed_calls &c,
                       uint16_t player, uint32_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_on_destroyed_calls(). Matches the
// original's committed __watcall(AX,EDX) shape (sig_llm_strat_unit_on_destroyed).
void unit_on_destroyed(uint16_t player, uint32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
