//
// sim/sim_bldg_set_connected_flag.h -- llm_bldg_set_connected_flag @0x004965d6 (0x67 B), translated
// from the DISASSEMBLY (tmp/decomp/llm_bldg_set_connected_flag_004965d6.asm), NOT from the Ghidra .c
// draft (whose plate hedges "exact network/connectivity purpose unverified" -- see below for why that
// hedge is dropped here, same resolution sim_bldg_clear_flag_bit0_notify.h's own header records for
// its mirror-image function).
//
//   void __watcall llm_bldg_set_connected_flag(ushort player, int b_index)
//   -- EAX=player (storage=AX:2), EDX=b_index (storage=EDX:4), per the asm header.
//   -- ALREADY COMMITTED in addr/mh_calls.gen.h:
//      `inline void llm_bldg_set_connected_flag(uint16_t player, int32_t b_index)`.
//
// TRIVIAL BODY, TWO STEPS, ALL LITERAL FROM THE ASM:
//   1. 0x004965ed-0x0049660c: read buildings[player][b_index].built_flags (uint8_t @+0x4), OR 0x1
//      (set bit0).
//   2. 0x0049660f-0x00496622: re-derive the SAME row/slot address a second time (independent
//      MOVZX/IMUL/IMUL/ADD pair, not a cached pointer -- Watcom's usual re-derive-rather-than-cache
//      idiom, same as every sibling in this slice) and store the OR'd byte back. Reproduced here as a
//      single read-modify-write through one `building &` reference, exactly as
//      sim_bldg_clear_flag_bit0_notify.cpp does for its own two-address-computation pair.
//   3. 0x00496628-0x0049662f: CALL llm_strat_bldg_notify_ui(player, b_index) UNCONDITIONALLY -- EDX
//      (b_index) loaded first, then EAX (player, re-read via MOVZX from the same stack slot the entry
//      stashed it in), matching mh_calls.gen.h's committed signature (uint16_t player, uint32_t
//      b_Index) -- both operands are read before the CALL, so the load order is not observable here
//      (same non-issue clear_flag_bit0_notify.cpp's own comment notes for its analogous call site).
//
// WHAT BIT0 MEANS, STATED PLAINLY (not "unverified"): docs/structs.md documents
// mh_map_object_building::built_flags bit0 as "connected/reached" -- the SAME flag
// llm_strat_bldg_power_network_recompute / llm_strat_bldg_propagate_network_connectivity /
// llm_strat_bldg_link_to_network_if_adjacent (all translated earlier today) already
// call THIS function on, as the graph's "mark reached" primitive when their classification concludes
// a building is definitely connected (shuttle/port unconditional link, or the power-network recompute's
// final pass). llm_strat_bldg_clear_flag_bit0_notify is the mirror "mark unreached" primitive over the
// same bit and the same notify_ui tail. Nothing about this function's own asm resolves that meaning on
// its own -- it is the batch context (the three existing callers being the connectivity walk, and the
// pre-existing docs/structs.md field comment) that grounds it, so the name and this comment carry that
// citation rather than re-deriving bit0 from scratch and re-hedging it.
//
// ---- SIGNATURE IS PINNED BY EXISTING CALLERS ------------------------------------------------------
// llm_bldg_set_connected_flag is ALREADY referenced as an indirected callee (function-pointer member
// `void (*bldg_set_connected_flag)(uint16_t player, int32_t b_index)`) by
// sim_bldg_link_to_network.h's bldg_link_to_network_calls, sim_bldg_power_network_recompute.h's
// power_network_recompute_calls, and sim_bldg_propagate_network_connectivity.h's own calls struct --
// this translation's public wrapper keeps EXACTLY that shape (`void(uint16_t, int32_t)`), matching
// addr/mh_calls.gen.h's own committed signature for this address, so it stays call-compatible with
// those three existing binders without any of them needing to change.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (llm_strat_bldg_notify_ui @0x00470bdd, already
// committed in mh_calls.gen.h -- not part of this migration slice, stays original), indirected for
// offline testability like every other sim/ TU's `_calls` struct, same shape as
// sim_bldg_clear_flag_bit0_notify.h's `clear_flag_bit0_notify_calls`.
struct bldg_set_connected_flag_calls {
    // llm_strat_bldg_notify_ui @0x00470bdd. EAX=player (uint16_t), EDX=b_Index (uint32_t) -- matches
    // mh_calls.gen.h's own signature for this callee exactly.
    void (*notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_set_connected_flag_calls &live_bldg_set_connected_flag_calls();

namespace detail {

// llm_bldg_set_connected_flag @0x004965d6. See the header banner above.
void bldg_set_connected_flag(sim_store &own, const bldg_set_connected_flag_calls &c, uint16_t player,
                             int32_t b_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Signature matches addr/mh_calls.gen.h's committed prototype for this address EXACTLY --
// `void(uint16_t player, int32_t b_index)` -- required for call-compatibility with the three existing
// function-pointer-typed callers listed above.
void bldg_set_connected_flag(uint16_t player, int32_t b_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
