//
// sim/sim_bldg_remove_workers.h -- llm_strat_bldg_remove_workers @0x004917c5 (0x151 bytes),
// translated from the DISASSEMBLY (tmp/decomp/llm_strat_bldg_remove_workers_004917c5.asm). The
// Ghidra .c draft agrees field-for-field once cross-checked against the asm (same posture as
// sim_bldg_population_layoff_workers.cpp's own note), but the asm remains the authority.
//
// The low-level worker-count mutator wrapped by llm_strat_bldg_unassign_workers and called directly
// by llm_strat_population_layoff_workers (both callers external to this function's own body; the
// latter is ALSO in this batch as sim_bldg_population_layoff_workers.cpp, which already indirects
// this very function through its own `remove_workers` call-table member -- this file is that
// function's real implementation, still reached by the sibling only through mh::call:: per policy).
// Mirror of llm_strat_bldg_add_workers (sim_bldg_add_workers.cpp/.h, same batch) for the opposite
// direction: clamps `count` down to `current_workers` (never lets the subtraction go negative),
// applies it as a 16-bit truncating SUBTRACT, unconditionally refreshes the building, then -- only
// when `current_workers` is now exactly zero -- clears the staffed flag IF the building still "uses
// workers" or its state is one of CONSTRUCTION/CHARGE_STEP/UPGRADING/DISMANTLING. Returns the
// (clamped) count actually removed.
//
// ---- THE CLAMP (0x004917e4-0x00491820) -----------------------------------------------------------
//
// `MOVZX EAX, current_workers; CMP EAX, count; JGE 0x00491820` -- current_workers is zero-extended
// (always >= 0) and compared SIGNED against `count`. If current_workers >= count, the clamp is
// skipped entirely (count is used as given). Only when current_workers < count does the code
// re-read current_workers and OVERWRITE count with it -- i.e. `if (current_workers < count) count =
// current_workers;`, so `count` never exceeds what is actually staffed and the subtraction below can
// never drive `current_workers` negative. Read carefully: the clamp direction is "cap count at
// current_workers", not the reverse.
//
// ---- THE SUBTRACT (0x00491820-0x00491836) --------------------------------------------------------
//
// `SUB word ptr [current_workers], AX` -- a NATIVE 16-BIT truncating subtract: only the low 16 bits
// of the (already-clamped) `count` participate, matching the Ghidra draft's own
// `*puVar1 = *puVar1 - (short)count;`. Reproduced with an explicit int16_t cast, same idiom
// sim_bldg_add_workers.cpp's ADD side uses.
//
// ---- THE REFRESH + STAFFED-FLAG CLEAR (0x0049183d-0x00491905) -------------------------------------
//
// llm_strat_refresh_building(player, building_id) runs UNCONDITIONALLY. `current_workers` is then
// RE-READ (a fresh load -- refresh_building is an external call that could in principle write it,
// same discipline sim_bldg_add_workers.cpp's set_staffed_flag gate uses). Only when that fresh read
// is exactly 0 does the function call llm_strat_bldg_uses_workers(player, building_id); the clear
// happens if THAT returns nonzero, OR building.state (re-read again) is CONSTRUCTION(0x64),
// CHARGE_STEP(0x6a), UPGRADING(0x82), or DISMANTLING(0x6b) -- a plain OR of five conditions gated
// behind the single `current_workers == 0` outer test (the asm's five-way jump chain
// 0x00491864-0x004918f9 all lands at the same clear_staffed_flag call site; falling through all five
// checks lands at 0x00491905, skipping the call). Return value is the clamped `count`, computed
// BEFORE the refresh/clear tail -- neither of those calls can change it.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three external callees this closure reaches, all ORIGINAL and already committed in
// addr/mh_calls.gen.h -- indirected for offline testability and per the task hazard note: all three
// (llm_strat_refresh_building, llm_strat_bldg_uses_workers, llm_strat_bldg_clear_staffed_flag) are
// ALSO being translated as sibling units in this same batch, so this file calls them through
// mh::call:: bound here rather than into their detail:: bodies directly -- same precedent
// sim_bldg_population_layoff_workers.h already set for indirecting `remove_workers` itself.
struct remove_workers_calls {
    // llm_strat_refresh_building @0x004705de. EAX=player (uint16_t), EDX=building_id.
    void (*refresh_building)(uint16_t player, int32_t building_id);
    // llm_strat_bldg_uses_workers @0x004988b0. EAX=player (widened to uint32_t, per the committed
    // prototype), EDX=building_id.
    int32_t (*uses_workers)(uint32_t player, int32_t building_id);
    // llm_strat_bldg_clear_staffed_flag @0x0049670b. EAX=player (uint16_t), EDX=building_id
    // (uint32_t per the committed prototype -- addr/mh_calls.gen.h).
    void (*clear_staffed_flag)(uint16_t player, uint32_t building_id);
};

const remove_workers_calls &live_remove_workers_calls();

namespace detail {

// llm_strat_bldg_remove_workers @0x004917c5. See the header banner above for the derivation.
uint32_t remove_workers(const sim_view &v, sim_store &own, const remove_workers_calls &gc,
                        uint16_t player, uint32_t building_id, uint32_t count);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (addr/mh_calls.gen.h's own
// `llm_strat_bldg_remove_workers(uint16_t player, uint32_t building_id, uint32_t count)`) -- plain
// __watcall(EAX=ushort player, EDX=uint building_id, EBX=uint count), per the .asm header.

uint32_t remove_workers(uint16_t player, uint32_t building_id, uint32_t count);

namespace detail {
} // namespace detail

} // namespace mh::sim
