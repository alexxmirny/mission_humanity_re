//
// ai/ai_opponent_relations.h -- per-opponent military/economic assessment (RI-AI / AI1A).
//
// One function: llm_strat_ai_update_opponent_relations, called once per OTHER active player from
// llm_strat_ai_player_tick. It fills *out -- a pointer the CALLER supplies, into the TICKING
// player's own player_data.ai_opponent_assessments[assessed_player] slot, NOT AI-owned scratch --
// with a snapshot of `assessed_player`'s unit/building military strength and weighted
// resource/mine-yield economy.
//
// SIDE EFFECT ON THE ASSESSED PLAYER'S OWN player_data (not the ticking player's). While
// player_data[assessed_player].ai_enabled == 0 (THEIR OWN strategic-AI pipeline is disabled -- see
// ai_state.h's field comment on ai_enabled), this lazily refreshes THEIR OWN score cache first:
// llm_strat_ai_score_build_categories(assessed_player), then
// llm_strat_ai_player_score_tier(assessed_player), whose return is stored into
// player_data[assessed_player].ai_resource_need_score. That destination is reached in the original
// through byte-offset ADJACENCY to ai_intel_seen_count (-0xc), not through its own field name --
// resolved before translation against tools/data/dll_struct_layouts.json / mh_structs.gen.h:
// ai_resource_need_score is at +0x2847c, ai_intel_seen_count at +0x28488, exactly 0xc apart.
//
// THREE PLAYER-IDS NEVER APPEAR HERE: only one, `assessed_player`, both the roster being read AND
// (for the lazy-refresh side effect) the player_data record being written. There is no "ticking
// player" parameter -- the caller (llm_strat_ai_player_tick) already knows which slot of ITS OWN
// ai_opponent_assessments to aim `out` at.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_update_opponent_relations @0x004d74de.
void update_opponent_relations(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t                                       assessed_player,
                               mh::game::mh_llm_strat_ai_opponent_assessment *out);

// llm_strat_ai_unit_weapon_power @0x004d729d (RI-AI batch B, antichain layer 3, 2026-08-06). Sole
// caller is update_opponent_relations above (the four gc.unit_weapon_power(...) call sites feeding
// soldier_power/ground_power/heli_power/plane_power). Sums cfg Weapon.power[player] (the SAME
// per-player power column group_pick_best_weapon_unit reads, cfg_final_struct_Weapon::power @0x92,
// double[9]) over the unit's four weapon slots, gated on the slot's OWN enabled_2 flag -- NOT on
// weapon_id != 0 and NOT on Weapon.target's ground/air bit, unlike the two similarly-shaped functions
// in ai_group_muster_pick.cpp/ai_army_milestone.cpp. Every slot is visited regardless of whether an
// earlier one was empty or disabled (no early-out), matching the original's unconditional 4-iteration
// loop.
//
// THE ACCUMULATION SHAPE DIFFERS FROM ITS LOOKALIKES: the running total is a FLOAT narrowed back to
// float32 after EVERY addition (0x004d72ea-0x004d72f1, FADD double / FSTP float), and truncated to an
// integer via utils_math_trunc exactly ONCE, after the loop (0x004d72fb-0x004d7307) -- not per-slot the
// way weapon_power_add_and_trunc's sibling helper works. Ghidra's own decompile renders this one
// correctly (an ordinary CALL utils_math_trunc, not the inlined artifact the other two show), which is
// why this .cpp does not need the ai_group_muster_pick.h hazard-note treatment.
int32_t unit_weapon_power(const ai_view &v, uint32_t player, uint32_t unit_id);

} // namespace detail

void    update_opponent_relations(uint32_t                                       assessed_player,
                                  mh::game::mh_llm_strat_ai_opponent_assessment *out);
int32_t unit_weapon_power(uint32_t player, uint32_t unit_id);

} // namespace mh::ai
