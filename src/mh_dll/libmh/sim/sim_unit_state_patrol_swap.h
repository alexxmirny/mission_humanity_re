//
// sim/sim_unit_state_patrol_swap.h -- unit PATROL_SWAP state handler (RI-SIM / SIM1-G1). Translated
// from the DISASSEMBLY (tmp/decomp/llm_strat_unit_state_patrol_swap_0047e2e2.asm), not from the
// Ghidra .c draft -- the two agree on shape here, but every field access below was independently
// cross-checked against addr/mh_structs.gen.h's own static_assert'd offsets (see below), matching this
// codebase's usual "asm is the spec" posture rather than trusting the draft's field names on faith.
//
// One function: llm_strat_unit_state_patrol_swap @0x0047e2e2 (0x9e bytes), `void __watcall
// llm_strat_unit_state_patrol_swap(void)` -- NO PARAMETERS, ambient "current unit" tick context
// (_G_LLM_STRAT_CUR_UNIT), same zero-arg ambient-context shape as
// sim_unit_state_die_explode.h's sibling and the rest of this batch (SIM1-G1's _CONTEXT.md Sect. 1).
//
// ---- SHAPE (read the .cpp for address-by-address derivation) --------------------------------------
//   1. (0x0047e2fa-0x0047e356) swap goal<->home on the current unit, FOUR single-byte field copies,
//      in this exact order (order matters -- goal picks up home's OLD value before home is
//      overwritten from x/y; each read/write pair re-derives the CUR_UNIT pointer independently in
//      the asm rather than caching a base, same idiom sim_unit_state_die_explode.cpp documents):
//        goal_x = home_x   (0x0047e2fa-0x0047e311, dest[+0x86] = src[+0x88])
//        goal_y = home_y   (0x0047e311-0x0047e328, dest[+0x87] = src[+0x89])
//        home_x = x        (0x0047e328-0x0047e33f, dest[+0x88] = src[+0x84])
//        home_y = y        (0x0047e33f-0x0047e356, dest[+0x89] = src[+0x85])
//   2. (0x0047e356-0x0047e376) llm_strat_unit_set_state_order(Unit[cur_unit.unit_proto_id].move_op_arg,
//      PATROL_SWAP=0x10). Param order: EAX=move_op_arg (param_1, zero-extended byte->ushort),
//      EDX=0x10 (param_2) -- `MOV EDX,0x10` sits BEFORE the move_op_arg computation in the asm, and
//      llm_strat_unit_set_state_order's committed signature is (uint16_t param_1, uint16_t param_2)
//      bound EAX/EDX (addr/mh_calls.gen.h), so param_1=move_op_arg, param_2=0x10 unambiguously.
//
// Does NOT touch _G_LLM_STRAT_TICK_BUDGET anywhere in the 0x9e-byte body (no reference to its address
// in the listing) -- unlike most of this batch's handlers, this one spends no per-tick time budget.
//
// ---- FIELDS: NAMED, NOT BYTE OFFSETS (all static_assert'd in addr/mh_structs.gen.h) ----------------
// mh_map_object_unit: x@0x84, y@0x85, goal_x@0x86, goal_y@0x87, home_x@0x88, home_y@0x89,
// unit_proto_id@0x2 (confirms the .h's own field-offset comment: "the game's own tooltip labels the
// 0x86/0x87 pair 'X2 Y2'" -- goal_x/goal_y).
// mh_cfg_final_struct_Unit: move_op_arg@0xec, uint8_t (sandwiched between move_op_code@0xeb and
// default_op_code@0xed, so exactly one byte wide -- matches the asm's single-byte MOVZX read).
//
// PATROL_SWAP=0x10 is the SAME llm_strat_unit_state member sim_unit_idle_state.cpp's own
// UNIT_STATE_PATROL_SWAP already names (that file's cited source: "Ghidra enum dump 2026-08-08,
// get-data-type-by-string llm_strat_unit_state"). Reused BY VALUE here, not by #include, per that
// TU's own established precedent of not sharing sim/ constants across files this way.
//
// ---- CALLEE (ORIGINAL, indirected through the `calls` struct like every other sim/ TU) -------------
// llm_strat_unit_set_state_order(uint16_t move_op_arg, uint16_t order_code) @0x00486657 -- writes the
// current unit's own .order/.state fields (an ORIGINAL roster-writing call that overlaps this
// function's own goal/home writes on the SAME record, exactly the situation
// sim_unit_state_die_explode.cpp's shadow arm reasons through for its own overlapping callees); fires
// for real under shadow (live_calls(), not stubbed). Arm-readiness is the conductor's call.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// llm_strat_unit_state member PATROL_SWAP=0x10 -- see sim_unit_idle_state.cpp's own
// UNIT_STATE_PATROL_SWAP for the Ghidra enum dump this value is read off. Suffixed _ORDER here (vs.
// that TU's UNIT_STATE_PATROL_SWAP) only to avoid an ODR name collision if a future TU includes both
// headers -- same value, same domain.
inline constexpr uint16_t UNIT_STATE_PATROL_SWAP_ORDER = 0x10;

// ---- the outward call -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. Signature
// copied verbatim from addr/mh_calls.gen.h.
struct unit_state_patrol_swap_calls {
    void (*unit_set_state_order)(uint16_t move_op_arg,
                                 uint16_t order_code); // llm_strat_unit_set_state_order @0x00486657
};

const unit_state_patrol_swap_calls &live_unit_state_patrol_swap_calls();

namespace detail {

// llm_strat_unit_state_patrol_swap @0x0047e2e2. Zero-arg, ambient cur_unit, matching the original's
// void(void) signature.
void unit_state_patrol_swap(const sim_view &v, sim_store &own, const unit_state_patrol_swap_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_patrol_swap_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_patrol_swap) exactly.
void unit_state_patrol_swap();

namespace detail {
} // namespace detail

} // namespace mh::sim
