//
// tact/tact_squad_assault_resolve.h -- TACT1A batch A: the tactical excursion's strategic-side EXIT.
//
//   llm_strat_squad_assault_resolve @0x0044d81d (0x23c)
//
// Resolves a ground squad assault staged via the squad blackboard globals
// (_G_LLM_SQUAD_BB_SCAN_PLAYER/TARGET_OWNER/TARGET_BUILDING_IDX/TARGET_ENERGY_PCT) against the
// per-soldier hit tally in _G_LLM_SQUAD_STATUS: sum each attacked unit's soldiers' energy_pct,
// convert to an absolute energy loss, and either kill the unit (loss <= 0: remove_from_map +
// teardown, bump units_lost_total), accumulate pending_damage (loss > 0 and the resulting energy
// delta is still positive), or do NOTHING AT ALL (loss > 0 but the delta is <= 0 -- a real
// no-op arm the original's control flow reaches directly, not merely an unreachable corner); then
// apply the same energy-loss formula to the target building's pending_damage. Called exactly once, from
// llm_tact_mission_end_return_to_strategic's map-load-succeeded arm (TACT1A batch A) -- the mode
// boundary itself, not tactical-owned code: region_ownership.json already classifies this function
// "Strategic sim/economy" (it writes ONLY sim-owned regions -- units/buildings/_G_LLM_STRAT_PLAYERS
// -- never a tact-owned one), so this translation reads its own blackboard view (tact_view, four
// scalars this function is the sole reader of) but performs every WRITE through mh::sim::state()'s
// own sim_store, the legitimate owner of those regions. Calling the sim domain's public state()
// accessor from a tact-domain TU is not a W3 violation: W3 only restricts CONSTRUCTING a sim_store
// outside sim_state.cpp/sim_fixture, and state() is the ONE sanctioned public entry point, used here
// exactly as any other caller would use it.
//
// STRUCTURALLY UN-SHADOW-ARMABLE TODAY, not merely deferred: its only two direct calls
// (llm_strat_unit_remove_from_map, llm_strat_unit_teardown) are themselves two of the seven
// TACT-CUT2-ungated `effectful` shared callees this function transitively reaches (BFS over
// call_graph_no_crt.json against tact_shared_callees.json, 2026-08-26 -- GetResourseFilePtr,
// llm_input_key_dequeue, llm_gfx_apply_window_resolution, llm_snd_play_matching_sample,
// llm_fatal_cleanup, plus the two direct calls themselves). Arming this entry under shadow would
// double-fire a real unit-removal/teardown cascade. So BOTH outward calls are indirected via a
// `_calls` struct (this codebase's sim/ pattern, tact precedent tact_unit_cmd_teleport_jump_tick.h)
// and the dispatch logic is proven OFFLINE -- which never executes either real callee, sidestepping
// the hazard rather than merely tolerating it.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "tact/tact_state.h"

namespace mh::tact {

// The two outward calls this function makes, indirected for offline testability -- same shape as
// tact_unit_cmd_teleport_jump_tick_calls.
struct squad_assault_resolve_calls {
    void (*unit_remove_from_map)(uint16_t player,
                                 uint32_t unit_idx); // llm_strat_unit_remove_from_map @0x00487252
    void (*unit_teardown)(uint32_t player,
                          uint16_t unit_idx); // llm_strat_unit_teardown @0x00487ba5
};

const squad_assault_resolve_calls &live_squad_assault_resolve_calls();

namespace detail {

// llm_strat_squad_assault_resolve @0x0044d81d.
//
// 1. @0x0044d835-0x0044d844: cache scan_player = _G_LLM_SQUAD_BB_SCAN_PLAYER; loop cursor = 0.
// 2. @0x0044d844-0x0044d982: while cursor < 64 (TACT_SQUAD_STATUS_SLOTS):
//    - unit_slot_index == 0 -> empty slot, cursor += 1, next.
//    - else: sum this unit's soldier_count consecutive _G_LLM_SQUAD_STATUS[cursor].energy_pct
//      entries (cursor advances once per soldier, UNCHECKED against the 64-slot bound -- the
//      original does not clamp it either, @0x0044d89b-0x0044d8c2), then
//      loss = (sum * Unit[proto].energy) / (Unit[proto].soldier_count * POWER_PERCENT_SCALE). A
//      GENUINE 3-WAY BRANCH (a 2-way collapse here was a real bug caught by reimpl-verify,
//      2026-08-26): loss <= 0 -> kill: remove_from_map, teardown,
//      players[scan_player].units_lost_total[planet_index] += 1. loss > 0 -> delta = unit.energy -
//      loss; delta > 0 -> unit.pending_damage += delta; delta <= 0 -> TRUE NO-OP (neither the kill
//      nor the pending_damage arm fires -- the original's JNC @0x0044d959 jumps straight to the
//      tail, past both blocks).
// 3. @0x0044d987-0x0044da4f: after the loop, target_owner/target_building_idx (re-read fresh here --
//    the original re-reads them every LOOP ITERATION too, harmlessly, since nothing in the loop body
//    writes either global) select the target building; pct = round(building.energy *
//    ENERGY_TO_PERCENT_SCALE / Building[building_id].energy); if TARGET_ENERGY_PCT < pct,
//    building.pending_damage += building.energy - (TARGET_ENERGY_PCT * Building[building_id].energy /
//    PERCENT_TO_ABS_SCALE).
void squad_assault_resolve(const tact_view &tv, tact_store &own, const mh::sim::sim_view &sim_read,
                           mh::sim::sim_store &sim_own, const squad_assault_resolve_calls &c);

} // namespace detail

void squad_assault_resolve();


} // namespace mh::tact
