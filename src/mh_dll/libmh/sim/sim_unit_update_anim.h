//
// sim/sim_unit_update_anim.h -- the damage-smoke animation tick for the CURRENT unit (RI-SIM / SIM1A).
//
// One function: llm_strat_unit_update_anim @0x0047dd56 (0x127 bytes), `void __watcall(void)` -- no
// parameters. Operates entirely through the _G_LLM_STRAT_CUR_UNIT global cursor: whatever the caller
// last pointed it at is the unit this tick advances. No callees (the only CALL in the body is the
// inert utils_assert_stack_capacity prologue, translator-brief rule 6 -- omitted).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_update_anim @0x0047dd56.
//
// GATE (0x0047dd6e-0x0047dd84): no-op unless cfg Unit[cur.unit_proto_id].dmg_smoke_enabled != 0 (a
// plain int32_t flag at mh_cfg_final_struct_Unit offset 0x107).
//
// BODY (all through the one CUR_UNIT record): drains a local time budget (game_clock delta since the
// unit's last tick, `local_time`) against the CACHED Anim[dmg_smoke_anim_id + 1].time -- cached ONCE
// before the loop and never refreshed even though dmg_smoke_anim_id changes inside the loop; see the
// .cpp, this is a genuine original quirk (translator-brief rule 10), not a translation shortcut.
// Each iteration either:
//   (a) the budget fits inside the cached frame time: subtract it from dmg_smoke_anim_timer and stop
//       (local_time = 0), or
//   (b) it doesn't: advance dmg_smoke_anim_id by Anim[id+1].next, or -- if .next == 0 -- restart it
//       from a per-damage-level base read via the POP_STATS out-of-declared-bounds walk (see the
//       .cpp's PRESERVE note; this is the task's flagged original quirk, reproduced verbatim), then
//       subtract the cached frame time from the budget and keep draining.
//
// Two FCOMP/JNC|JBE branches are NaN-aware in a way the exported .c's plain `<`/`<=` is NOT -- see
// the .cpp for the derivation (same idiom sim_unit_passive_engage.cpp documents for its own FCOMP/
// JNC shape).
void unit_update_anim(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed __watcall(void) shape
// (sig_llm_strat_unit_update_anim).
void unit_update_anim();

namespace detail {
} // namespace detail

} // namespace mh::sim
