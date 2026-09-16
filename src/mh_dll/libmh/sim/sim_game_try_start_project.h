//
// sim/sim_game_try_start_project.h -- validate + charge the cost of starting a research project at a
// lab building (RI-SIM / SIM1F):
//
//   game_TryStartProject @0x00492cac (0x189 bytes), `int32_t game_TryStartProject(uint16_t player,
//   int32_t b_idx, uint32_t project_id)` (committed prototype, addr/mh_export.gen.h's
//   sig_game_TryStartProject / addr/mh_calls.gen.h's mh::call::game_TryStartProject).
//
// Translated from the DISASSEMBLY (tmp/decomp/game_TryStartProject_00492cac.asm). The Ghidra .c draft
// (tmp/decomp/game_TryStartProject_00492cac.c) reads correctly for every branch/loop and is cited
// below only as corroboration, not as the source of truth -- its `player_resources[(uVar2 & 0xffff) *
// 10 + cVar1]` / `Building[...]` / `Projects[...]` raw indexing is exactly what the sim_view/sim_state
// helpers below replace.
//
// ---- GATE 1: THE INVENTION PREREQUISITE (0x00492ccb-0x00492cf7) --------------------------------------
// `progress[player][Projects[project_id].invention].available == 0` -> return 0x13 immediately, no
// further work. Byte-stride cross-check: `IMUL EAX,player,0x384` (900 = PROGRESS_ROW_COUNT(300) *
// sizeof(mh_game_progress)(3)) plus `LEA EDX,[invention + invention*2]` (invention*3) reproduces
// exactly `progress_of(v, player, invention)`'s own `player*300+row` indexing at the struct's real
// 3-byte stride -- same helper sim_bldg_pay_costs.cpp's `llm_bldg_pay_build_cost` already uses for the
// identical gate shape.
//
// ---- GATE 2: THE BUILDING'S project_type MUST MATCH (0x00492cfc-0x00492d3d) ---------------------------
// `Building[buildings[player][b_idx].building_id].project_type == Projects[project_id].type` -> proceed
// to the resource walk; else -> return 0x14. `buildings[player][b_idx].building_id` is
// `building_of(v, player, b_idx).building_id` (sim_state.h's existing helper, same shape
// sim_bldg_pay_costs.cpp's `bldg_pay_cycle_inputs` already uses to go from a roster instance to its cfg
// type). `project_type` is `int32_t` (dword read, offset 0x5e5, confirmed against mh_structs.gen.h's
// static_assert) -- NOT the byte-width `type`/`invention` fields nearby on the same struct.
//
// ---- THE RESOURCE-AFFORDABILITY LOOP (0x00492d3d-0x00492e23): IDENTICAL SHAPE TO sim_bldg_pay_costs --
// Two passes over `Projects[project_id].resource[0..6]` ((id,val) pairs, cfg_t_project_id-indexed cfg
// data), walking with the SAME id-read-before-bound-check order and the SAME error-accumulation rule
// sim_bldg_pay_costs.h documents in full (first shortage sets `resource_id + 0x89`; any later shortage
// collapses the accumulator to the bare sentinel `0x89`; the scan never short-circuits). Pass 1
// (0x00492d4b-0x00492dbf) only accumulates; pass 2 (0x00492dcd-0x00492e23) runs ONLY if pass 1 finished
// with the accumulator still 0, and actually calls `game_SpendResource(player, resource_id, val)` per
// slot -- an ORIGINAL callee, called via `mh::call::` directly per this slice's own context note (rule:
// `game_TryStartProject` and `llm_cfg_apply_project_resources` call `game_SpendResource`/
// `llm_resource_add` respectively, and NEITHER callee is on the effectful-outward-call list nor
// translated in this closure, so both go through `mh::call::` like any other untranslated sibling --
// NOT indirected through a `_calls` struct, unlike sim_bldg_pay_costs.cpp's `pay_costs_calls`, because
// this slice's context explicitly calls out the ordinary-mh::call:: route for this exact pair of
// callees). `player_resources[player][resource_id]` is `player_resource_of(v, player, resource_id)`
// (sim_state.h's existing helper, same one sim_bldg_pay_costs.cpp uses).
//
// `player` is truncated to its low 16 bits at EVERY use site (roster index, progress index,
// player_resources index, and the game_SpendResource call itself -- 0x00492cdb/0x00492d00/0x00492d80/
// 0x00492e12, all `MOVZX ..., word ptr [player]`), even though the committed prototype's parameter is
// already `uint16_t player` (so the mask is a no-op for a conforming caller, but the raw export thunk
// widens through EAX -- see sim_prod_shuttle_complete.cpp's precedent comment on why every read site
// still masks explicitly rather than trusting the parameter type alone).
//
// Returns `int32_t`: 0 = success (all costs charged); 0x13 = invention not researched; 0x14 =
// project_type mismatch; otherwise a resource-shortage reason code (`resource_id + 0x89`, or the bare
// `0x89` sentinel once a second resource is also short) -- same code family
// sim_bldg_pay_costs.h documents, and this function's own caller
// (`llm_strat_order_queue_dispatch`'s Table-A start-project case, see mh_structs.gen.h's
// `active_project_id` field comment) reads these codes back.
//
// ---- FIELDS: ALREADY NAMED / BOUND, NOT BYTE OFFSETS ---------------------------------------------------
// mh_cfg_final_struct_Project: invention@0x0, type@0xc, resource[7]@0x10 (sizeof 0xd0, static_assert'd).
// mh_cfg_final_struct_Building: project_type@0x5e5 (int32_t, static_assert'd). mh_game_progress:
// available@0x0 (sizeof 3, static_assert'd -- the byte-stride source for gate 1's IMUL constants above).
//
// ---- STATE ALREADY BOUND THIS SLICE (sim_state.h, checked before writing this file) -------------------
// sim_view::cfg_projects (new this slice), sim_view::progress / progress_of(), sim_view::cfg_buildings,
// sim_view::player_resources / player_resource_of(), sim_state.h's building_of(). No fresh
// declared_needs -- every field this function touches was pre-bound by the conductor per
// tmp/decomp/_CONTEXT_SIM1F_2.md.
//
// ---- CALLEES ---------------------------------------------------------------------------------------
// game_SpendResource (0x00497f94, already committed in mh_calls.gen.h) -- the resource-charge call in
// pass 2. Indirected through this TU's `try_start_project_calls` struct (bound to
// `mh::call::game_SpendResource` in `live_try_start_project_calls()`) so the offline oracle can stub
// it; behaviour-identical in production (the pointer targets the same live-image callee). See the
// `try_start_project_calls` comment below and sim_bldg_pay_costs.cpp's `pay_costs_calls` precedent for
// the identical shape/posture.
//
// ---- WHAT THIS DOES NOT TOUCH ----------------------------------------------------------------------
// No writes to any roster (`units`/`buildings`/`labs`/etc.) and no writes to `progress` itself --
// player_resources is written only through game_SpendResource, exactly like sim_bldg_pay_costs.cpp's
// pair. No floats anywhere in this function (every comparison and every array walk is integer).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra enum (rule 17a fallback) -----------
inline constexpr int32_t PROJECT_ERR_INVENTION_NOT_RESEARCHED = 0x13;
inline constexpr int32_t PROJECT_ERR_TYPE_MISMATCH            = 0x14;
inline constexpr int32_t PROJECT_ERR_RESOURCE_SHORTAGE_BASE   = 0x89; // + resource_id on the first
                                                                      // shortage; the bare sentinel on
                                                                      // any later one -- see the
                                                                      // header banner.

// The one outward call this function makes, indirected through a `_calls` struct for the SAME reason
// as every other sim TU's (sim_bldg_pay_costs.cpp's `pay_costs_calls`, sim_invasion.cpp's
// `invasion_calls`): a direct `mh::call::` inside a `detail::` body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest (the public wrapper / mh::call:: target
// resolves via `state()` -> a real stock VA that is unmapped offline, crashing before any assertion).
// The pointer points at the same callee in production, so this is behaviour-identical there -- it only
// adds a stub seam for the offline oracle. Signature matches game_SpendResource's committed prototype
// (addr/mh_calls.gen.h) exactly.
struct try_start_project_calls {
    void (*spend_resource)(int32_t player, int32_t resource_id, int32_t amount); // game_SpendResource @0x00497f94
};

const try_start_project_calls &live_try_start_project_calls();

namespace detail {

// game_TryStartProject @0x00492cac. See the header banner above for the full derivation. Matches the
// original's int32_t(player, b_idx, project_id) signature; the resource-charge call in pass 2 is routed
// through `c` (see try_start_project_calls above).
int32_t game_try_start_project(const sim_view &v, const try_start_project_calls &c, uint32_t player,
                               int32_t b_idx, uint32_t project_id);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (sig_game_TryStartProject) exactly.

int32_t game_try_start_project(uint16_t player, int32_t b_idx, uint32_t project_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
