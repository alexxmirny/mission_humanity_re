//
// sim/sim_bldg_population_layoff_workers.h -- llm_strat_population_layoff_workers @0x00491689
// (0x13c bytes), translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_population_layoff_workers_00491689.asm).
//
// Shrinks player `player`'s population by `worker_count`: first drains
// _G_LLM_STRAT_POP_STATS[player].human directly (clamped at 0 -- any excess of `worker_count` over
// `.human` becomes the remaining deficit), then, for any remaining deficit, walks
// buildings[player][] starting one slot past the PERSISTENT round-robin cursor
// (_G_LLM_STRAT_POP_STATS[player].layoff_cursor, wrapping 1->99->1, never touching slot 0 -- the
// roster's count/header row), laying off one worker at a time via the ORIGINAL
// llm_strat_bldg_remove_workers wherever a slot is both OCCUPIED (building_id != 0) and STAFFED
// (current_workers != 0), until the deficit reaches zero. Finishes by calling the ORIGINAL
// llm_strat_bldg_notify_state_change(player, cursor) UNCONDITIONALLY -- even when the roster loop
// never ran at all (deficit already 0 after the direct `.human` drain), in which case `cursor` is
// whatever `.layoff_cursor` held BEFORE this call: a STALE value, preserved verbatim, not "the last
// building touched" (there may have been none).
//
// THE ROSTER LOOP IS DELIBERATELY UNBOUNDED (0x00491712-0x004917ab): the only exit test is
// `deficit != 0`. There is no iteration cap and no re-check that any building anywhere is still
// staffed. If the caller asks to lay off more workers than the player actually employs, the
// original spins forever cycling the cursor around the 99-slot ring, repeatedly calling
// remove_workers on slots that can no longer pay (each such call is a no-op, returning 0, so the
// deficit never shrinks). Reproduced exactly -- no safety cap added, per the conductor's brief.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two external callees this closure reaches, both ORIGINAL and already committed in
// addr/mh_calls.gen.h -- indirected for offline testability, matching
// sim_bldg_refresh_all_buildings.h's `refresh_all_buildings_calls` shape.
struct population_layoff_workers_calls {
    // llm_strat_bldg_remove_workers @0x004917c5. EAX=player, EDX=building slot INDEX (the generated
    // header's own parameter name says "building_id", but the value actually passed at this call
    // site is the roster slot/cursor, per the .asm's `MOV EDX, [local_24]`), EBX=count (always 1
    // here). Returns the number of workers actually removed (0 if the slot could not pay).
    uint32_t (*remove_workers)(uint16_t player, uint32_t building_index, uint32_t count);
    // llm_strat_bldg_notify_state_change @0x00470c5c. EAX=player, EDX=building slot index (the
    // round-robin cursor, not necessarily a slot that was ever touched -- see the header banner).
    void (*notify_state_change)(uint16_t player, uint32_t building_index);
};

const population_layoff_workers_calls &live_population_layoff_workers_calls();

namespace detail {

// llm_strat_population_layoff_workers @0x00491689. See the header banner above for the derivation.
void population_layoff_workers(const sim_view &v, sim_store &own,
                               const population_layoff_workers_calls &gc, int32_t player,
                               int32_t worker_count);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the COMMITTED prototype (addr/mh_calls.gen.h's own
// `llm_strat_population_layoff_workers(int32_t player, int32_t worker_count)`) -- plain int, not
// ushort/uint, even though `player` is masked to its low 16 bits for every internal use
// (0x004916a6's MOVZX, re-applied at every subsequent row-address/callee use).

void population_layoff_workers(int32_t player, int32_t worker_count);

namespace detail {
} // namespace detail

} // namespace mh::sim
