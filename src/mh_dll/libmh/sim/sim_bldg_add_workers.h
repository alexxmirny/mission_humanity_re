//
// sim/sim_bldg_add_workers.h -- llm_strat_bldg_add_workers @0x00491916 (0x262 bytes), translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_bldg_add_workers_00491916.asm). The Ghidra .c draft's own
// reject condition (the `if (((iVar3==0 && state!=CONSTRUCTION) && state!=CHARGE_STEP) &&
// (state!=UPGRADING && state!=DISMANTLING)) { return 0; }` chain) reads correctly once traced against
// the asm's jump chain (0x00491941-0x004919c9) -- it is an AND of five negatives, i.e. the no-op path
// is taken ONLY when uses_workers() returned 0 AND the state matches none of the four gated states;
// `uses_workers() != 0` alone is enough to fall straight through to the execute path regardless of
// state (see the header's own walk below).
//
// The low-level worker-count mutator wrapped by llm_strat_bldg_assign_workers (mirror of
// llm_strat_bldg_remove_workers, not translated in this slice -- see the closure note in
// tmp/decomp/_CONTEXT_sim1b_slice4.md, callees stay original even for same-batch siblings): gates on
// llm_strat_bldg_uses_workers(player, building_id) OR the building's own state, picks a worker-count
// CAP depending on that state, clamps `count` so `current_workers + count` never exceeds the cap
// (count CAN go negative -- current_workers already at/above cap is not clamped to 0, this function
// mirrors llm_strat_bldg_remove_workers for the opposite direction), applies the clamped count to
// `current_workers` as a 16-bit truncating add, unconditionally refreshes the building, then sets the
// staffed flag if `current_workers` is now nonzero. Returns the (possibly clamped) count actually
// applied, or 0 on the early no-op.
//
// ---- THE GATE (0x00491941-0x004919c9) ----------------------------------------------------------
//
// `CALL llm_strat_bldg_uses_workers; TEST EAX,EAX; JNZ 0x00491962` -- if uses_workers() != 0, the jump
// chain (0x00491962 -> 0x00491981 -> 0x004919a1 -> 0x004919c0) lands DIRECTLY at 0x004919ce, the
// execute path, without ever reading `state` in this block. Only when uses_workers() == 0 does the
// code fall through into the four-state CMP chain (CONSTRUCTION/CHARGE_STEP/UPGRADING/DISMANTLING,
// each a `CMP ...; JNZ <next check>` -- a match on ANY of them also lands at 0x004919ce via the same
// jump chain); falling off the end of all four checks (0x004919be JNZ 0x004919c2) is the ONLY way to
// reach the reject block at 0x004919c2 (`local_1c = 0; return`). So: reject iff uses_workers()==0 AND
// state is none of the four; execute otherwise.
//
// ---- THE CAP (0x004919ce-0x00491ad1) ------------------------------------------------------------
//
// A second, independent four/five-way state check (re-reads `state` fresh -- it is not the same
// branch outcome as the gate above, since the gate can be satisfied by uses_workers()!=0 with `state`
// being anything):
//   CONSTRUCTION / CHARGE_STEP / DISMANTLING (0x004919ce-0x00491a55, three CMPs sharing one target
//     block at 0x00491a27) -> cfg_buildings[buildings[player][building_id].building_id].builder_count
//   UPGRADING (0x00491a55-0x00491aa6)                                          -> cfg_buildings[
//     cfg_buildings[buildings[player][building_id].building_id].upgrade_index].builder_count -- ONE
//     level of indirection through the UPGRADE TARGET's own cfg record, not the building's own id.
//   anything else, i.e. uses_workers()!=0 and none of the above four matched (0x00491aa8-0x00491ad1)
//     -> cfg_buildings[buildings[player][building_id].building_id].worker_count (0xd9eed8, a
//     DIFFERENT field from builder_count at 0xd9eedc -- do not conflate them, per
//     sim_bldg_finish_order.cpp's own note on the same two fields).
//
// ---- THE CLAMP + APPLY (0x00491ad1-0x00491b67) ----------------------------------------------------
//
// `current_workers` (uint16_t @+0x31, mh_structs.gen.h) is zero-extended and added to `count` as a
// 32-bit sum, compared SIGNED against the cap (JLE skips the clamp): if `current_workers + count >
// cap`, `count` is overwritten with `cap - current_workers` -- this CAN be negative (current_workers
// already at/above cap), and that is preserved, not floored at 0. The (possibly clamped) `count` is
// then added to `current_workers` via a NATIVE 16-BIT `ADD word ptr [...],AX` -- i.e. the truncation
// to 16 bits happens on `count` before the add, not on the 32-bit sum after it (the two give the same
// low-16-bit result either way, since addition mod 2^16 does not care which width the intermediate
// was computed at, but the C++ below mirrors the instruction's own shape rather than relying on that
// equivalence). llm_strat_refresh_building is then called UNCONDITIONALLY; only after that call does
// the code re-read `current_workers` (a fresh load, since refresh_building is an external call that
// could in principle write it) to decide whether to call llm_strat_bldg_set_staffed_flag. The return
// value is the (possibly clamped) `count`.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three external callees this closure reaches, all ORIGINAL and already committed in
// addr/mh_calls.gen.h -- indirected for offline testability, matching
// sim_bldg_population_layoff_workers.h's `population_layoff_workers_calls` shape.
struct add_workers_calls {
    // llm_strat_bldg_uses_workers @0x004988b0. EAX=player (widened to uint32_t, per the committed
    // prototype), EDX=building_id.
    int32_t (*uses_workers)(uint32_t player, int32_t building_id);
    // llm_strat_refresh_building @0x004705de. EAX=player (uint16_t), EDX=building_index.
    void (*refresh_building)(uint16_t player, int32_t building_index);
    // llm_strat_bldg_set_staffed_flag @0x004966a4. EAX=player (uint16_t), EDX=building_index.
    void (*set_staffed_flag)(uint16_t player, int32_t building_index);
};

const add_workers_calls &live_add_workers_calls();

namespace detail {

// llm_strat_bldg_add_workers @0x00491916. See the header banner above for the derivation.
int32_t add_workers(const sim_view &v, sim_store &own, const add_workers_calls &gc, uint16_t player,
                    uint32_t building_id, int32_t count);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (addr/mh_calls.gen.h's
// `llm_strat_bldg_add_workers(uint16_t player, uint32_t building_id, int32_t count)` and
// addr/mh_export.gen.h's `sig_llm_strat_bldg_add_workers`) -- plain __watcall(EAX=ushort player,
// EDX=uint building_id, EBX=int count), per the .asm header.

int32_t add_workers(uint16_t player, uint32_t building_id, int32_t count);

namespace detail {
} // namespace detail

} // namespace mh::sim
