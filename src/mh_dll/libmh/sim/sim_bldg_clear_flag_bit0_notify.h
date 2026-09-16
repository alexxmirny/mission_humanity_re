//
// sim/sim_bldg_clear_flag_bit0_notify.h -- llm_strat_bldg_clear_flag_bit0_notify @0x0049663d (0x67
// B), translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_clear_flag_bit0_notify_0049663d.asm), NOT from the Ghidra .c draft
// (whose plate hedges "(unverified: bit0 meaning)" -- see below for why that hedge is dropped here).
//
//   void __watcall llm_strat_bldg_clear_flag_bit0_notify(ushort player, int building_index)
//   -- EAX=player, EDX=building_index (committed prototype, addr/mh_export.gen.h's
//   sig_llm_strat_bldg_clear_flag_bit0_notify: `void(__cdecl*)(uint16_t, int32_t)`).
//
// TRIVIAL BODY, THREE STEPS, ALL LITERAL FROM THE ASM:
//   1. 0x0049666d-0x00496673: read buildings[player][building_index].built_flags (uint8_t @+0x4),
//      AND 0xfe (clear bit0).
//   2. 0x00496676-0x00496689: re-derive the SAME row/slot address a second time (independent IMUL
//      pair, not a cached pointer -- Watcom's usual idiom, reproduced as a single read-modify-write
//      through one `building &` reference below since the two address computations are provably the
//      same slot) and store the masked byte back.
//   3. 0x0049668f-0x00496696: CALL llm_strat_bldg_notify_ui(player, building_index) unconditionally
//      -- EDX=building_index loaded first, then EAX=player (re-read via MOVZX from the same stack
//      slot the entry stashed it in), matching the committed mh::call:: signature
//      (uint16_t player, uint32_t b_Index).
//
// WHAT BIT0 MEANS, STATED PLAINLY (not "(unverified)"): docs/structs.md documents
// mh_map_object_building::built_flags bit0 as "connected/reached" -- the SAME flag this slice's
// llm_strat_bldg_power_network_recompute / llm_strat_bldg_propagate_network_connectivity (both also
// translated this slice, see the batch's _CONTEXT file) set via the ORIGINAL
// llm_bldg_set_connected_flag as they walk the power-network graph. This function is the graph's
// "mark unreached" primitive: clear the connected bit for one building slot, then tell the UI so it
// can redraw. Nothing about this function's own asm resolves that meaning on its own -- it is the
// batch context (the callers being the connectivity walk, and the pre-existing docs/structs.md field
// comment) that grounds it, so the name and this comment carry that citation rather than re-deriving
// bit0 from scratch and re-hedging it.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (llm_strat_bldg_notify_ui @0x00470bdd, already
// committed in mh_calls.gen.h -- not part of this migration slice, stays original), indirected for
// offline testability like every other sim/ TU's `_calls` struct.
struct clear_flag_bit0_notify_calls {
    // llm_strat_bldg_notify_ui @0x00470bdd. EAX=player (uint16_t), EDX=b_Index (uint32_t) --
    // matches mh_calls.gen.h's own signature for this callee exactly.
    void (*notify_ui)(uint16_t player, uint32_t b_index);
};

const clear_flag_bit0_notify_calls &live_clear_flag_bit0_notify_calls();

namespace detail {

// llm_strat_bldg_clear_flag_bit0_notify @0x0049663d. See the header banner above.
void clear_flag_bit0_notify(sim_store &own, const clear_flag_bit0_notify_calls &c, uint16_t player,
                            int32_t building_index);

} // namespace detail

// Live wrapper: the logic applied to state().own / live_clear_flag_bit0_notify_calls(). Matches the
// committed prototype (sig_llm_strat_bldg_clear_flag_bit0_notify) exactly.
void clear_flag_bit0_notify(uint16_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
