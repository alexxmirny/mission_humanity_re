//
// sim/sim_order_issue_0xf_adjacent.h -- the paired "issue order 0xf/0x36 at an adjacent tile" helper
// (RI-SIM / SIM1-G1). Translated from the DISASSEMBLY:
//
//   llm_strat_order_issue_0xf_adjacent_by_offset @0x0046a19d (0x84 bytes)
//     tmp/decomp_sim/llm_strat_order_issue_0xf_adjacent_by_offset_0046a19d.asm
//   llm_strat_order_issue_0xf_adjacent_enqueue   @0x0046a325 (0xdd bytes)
//     tmp/decomp_sim/llm_strat_order_issue_0xf_adjacent_enqueue_0046a325.asm
//
// PAIRED, SOLE-CALLER RELATIONSHIP: `by_offset`'s entire job is to add a caller-supplied (dx,dy)
// offset (wrapped by the map's power-of-two width/height mask) to the unit's own tile, then forward
// to `enqueue` at the resulting absolute tile -- confirmed by the .asm at 0x0046a20b-0x0046a215,
// which loads (new_x, new_y, unit_idx, player) into (EBX, ECX, EDX, AX) and falls straight through to
// `enqueue`'s own entry point (0x0046a325), no other call in between. `by_offset`'s own sole caller is
// `llm_strat_unit_queue_advance_search` (batch context: the queue-advance mechanism issuing this order
// for whichever unit in the search chain found a free neighbor tile) -- not general pathfinding.
//
// THE "unaff_EBX artifact" NOTE ON BOTH PLATES IS STALE (batch context correction,
// 2026-07-08): re-derived from the .asm, all four `by_offset` params (player/EAX, unit_idx/EDX,
// dx/EBX, dy/ECX) and all four `enqueue` params (player/AX, unit_idx/EDX, target_x/EBX, target_y/ECX)
// are stored cleanly to the stack at entry and used from there -- no ambient/unaff_ read anywhere in
// either body. Trust the committed `__mh_watcall_ecx_ebx_volatile` prototype.
//
// `by_offset`'s call into `enqueue` is a DIRECT C++ call to this file's own
// `detail::order_issue_0xf_adjacent_enqueue`, NOT routed through `calls`/`mh::call::` -- per the batch
// brief, since both functions are translated together in this one TU (Law 4: one function in, one
// function out, but a same-TU sole-caller edge stays a direct call, matching every other paired
// TU in this codebase, e.g. sim_unit_queue_advance.h's search/advance pair). `enqueue`'s own FOUR
// outward callees (order_scratch_reset/_set_field, order_enqueue, unit_notify_status) plus the tile-
// adjacency test (tiles_adjacent) are all ORIGINAL functions outside this batch, already committed in
// addr/mh_calls.gen.h -- indirected through the `order_issue_0xf_adjacent_calls` table below for
// offline testability (net_selftest.exe simtest), the same reason every sim/ TU with outward calls
// does this (see sim_order_enqueue.h's header note, the closest sibling domain -- this pair is
// logically a two-function slice of that same order-enqueue family, just kept in its own TU since it
// was translated as an isolated batch item).
//
// THE GATE (`enqueue`'s own body, both plates): `Unit[unit.unit_proto_id].move_op_code == 0xf` --
// 0xf is the ground-move class tag (see mh_cfg_final_struct_Unit::move_op_code's own field comment in
// addr/mh_structs.gen.h: "==0xf (ground) by llm_unit_recruit, llm_strat_order_issue_0xf_adjacent,
// llm_strat_bldg_completion_dispatch"). Field offset/type cross-checked directly: the .asm's
// `IMUL EAX,unit_proto_id,0x23f; CMP byte[EAX+0xe4a183],0xf` resolves to Unit-array-base + 0xeb, and
// `static_assert(offsetof(mh_cfg_final_struct_Unit, move_op_code) == 0xeb, ...)` confirms both the
// offset and the byte width. Then, only if that class test passes, `llm_strat_tiles_adjacent(target_x,
// target_y, unit.x, unit.y)` gates the enqueue itself -- both are early-return guards with nothing in
// between (no side effect straddles either test).
//
// `enqueue`'s own field reads (`units[player][unit_idx].x/.y`, `.unit_proto_id`) all cross-checked
// byte-for-byte against addr/mh_structs.gen.h's static_asserts (x@0x84, y@0x85, unit_proto_id@0x2) --
// no phantom/undefined field anywhere in either body.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// `enqueue`'s five outward callees, indirected for offline testability -- see the header note above.
// Signatures copied verbatim from addr/mh_calls.gen.h.
struct order_issue_0xf_adjacent_calls {
    int32_t (*tiles_adjacent)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x00498934
    void (*order_scratch_reset)();                                             // @0x00466812
    void (*order_scratch_set_field)(int32_t index, int32_t value);             // @0x0046685d
    int32_t (*order_enqueue)(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                             uint16_t order_code); // @0x00466094
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
};

const order_issue_0xf_adjacent_calls &live_order_issue_0xf_adjacent_calls();

namespace detail {

// llm_strat_order_issue_0xf_adjacent_enqueue @0x0046a325. Gate (move_op_code==0xf, then
// tiles_adjacent(target,unit) != 0) -> stage (target_x,target_y) into order scratch fields 0/1,
// enqueue order 0xf/0x36 tagged unit-owned (owner_and_kind = player | ORDER_KIND_UNIT, i.e. | 0x80 --
// sim_order_enqueue.h's shared tag constant, reused here the same way sim_unit_state_move_walker.cpp
// reuses that header's UNIT_STATE_* constants), notify(player, unit_idx, 0). No-op on either gate
// failing.
void order_issue_0xf_adjacent_enqueue(const sim_view &v, const order_issue_0xf_adjacent_calls &c,
                                      uint16_t player, int32_t unit_idx, int32_t target_x,
                                      int32_t target_y);

// llm_strat_order_issue_0xf_adjacent_by_offset @0x0046a19d. new_x = width_mask & (unit.x + dx),
// new_y = height_mask & (unit.y + dy); forwards directly to order_issue_0xf_adjacent_enqueue above
// (same TU, direct call -- see header note).
void order_issue_0xf_adjacent_by_offset(const sim_view &v, const order_issue_0xf_adjacent_calls &c,
                                        uint32_t player, int32_t unit_idx, int32_t dx, int32_t dy);

} // namespace detail

// Live wrappers: the logic applied to state().read and live_order_issue_0xf_adjacent_calls().
// Signatures match the committed __watcall shapes in addr/mh_calls.gen.h / addr/mh_export.gen.h
// exactly.
void order_issue_0xf_adjacent_enqueue(uint16_t player, int32_t unit_idx, int32_t target_x,
                                      int32_t target_y);
void order_issue_0xf_adjacent_by_offset(uint32_t param_1, int32_t param_2, int32_t a2, int32_t param_4);

namespace detail {
} // namespace detail

} // namespace mh::sim
