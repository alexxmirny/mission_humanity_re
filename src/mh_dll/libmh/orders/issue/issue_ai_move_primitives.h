//
// orders/issue/issue_ai_move_primitives.h -- the two AI-named move primitives (RI-ORDERS / O4B).
//
//   llm_strat_ai_unit_flag_and_move            @0x004d7e22 (0x49 bytes)
//   llm_strat_ai_group_scatter_to_passable_tile @0x004d7e6b (0xa3 bytes)
//
// WHY TWO `llm_strat_ai_*` FUNCTIONS ARE IN THIS DOMAIN. Same verdict, same evidence and the same
// measurement as issue_group_formation_move.h's: behaviour owns a function, the name prefix does
// not. Both bodies write EXACTLY the two order-pipeline bytes that one writes, at the same offsets
// with the same constants -- `order_notify_status = 1` (unit +0xe8) and `order_status_flags |= 0x40`
// (unit +0xe3) -- and then hand the unit to llm_strat_unit_order_move_enqueue. Those two fields are
// SIM-written and AI-READ (llm_strat_unit_notify_status writes both at six sites,
// libmh/sim/sim_unit_notify.cpp; every AI touch in the tree is a read), so the write is an order-issue
// write. Homing them in `ai` would have needed a mutable `units` in `ai_store` for bodies that do
// nothing but stamp order bits -- the R2 hole ai_state.h names in as many words. Re-homed 2026-08-28
// (ai.json + sim.json `select.owned_elsewhere`, adopted by this domain's `names` seed).
//
// THE EIGHT ROSTER WRITERS THAT STAYED IN `ai` ARE A DIFFERENT CASE and this note exists so the line
// is not re-litigated: they write ai_group_next/_prev/_index (+0xd4/+0xd6/+0xd8) and
// player_data.ai_groups[] -- the AI's OWN doubly-linked group list, which merely lives inside the
// sim's frozen record layout. That is genuine AI state and is AI1D's, under a field-scoped mutable
// window.
//
// `flag_and_move` IS THE DOMAIN'S HIGHEST-TRAFFIC WRAPPER. Ten AI group tasks call it as their move
// primitive (group_rally_formup_worker, group_rally_stragglers, group_redistribute_units,
// group_reposition_members, group_scatter_idle_members, group_scatter_near_point,
// group_scatter_random_worker, group_task_muster_from_pool, group_task_nudge_stragglers,
// unit_move_near_random_point), and `scatter_to_passable_tile` is called by two more.
//
// NEITHER HAS A GENERATED GOLDEN CASE, structurally rather than by omission: their sink is
// llm_strat_unit_order_move_enqueue (@0x0046a0d5), one level ABOVE the container, while
// gen_order_issue_golden.py's roots are llm_strat_order_dispatch / llm_strat_order_enqueue -- so the
// extractor records ZERO sites for both and `recording_sink` never fires. Their emission is observed
// through `issue_calls::unit_order_move_enqueue` by hand-authored `issuetest` recorders instead,
// which is what makes both rows **T2** rather than a shortfall -- the same tier and the same reason
// as this domain's other no-golden-site rows.
//
// NEITHER BUMPS THE ORDER SEQUENCE ID. Both push a literal 0 as the enqueue's fifth argument
// (`PUSH 0x0` @0x004d7e58 / @0x004d7ee9), so unlike issue_group_formation_move they do not touch
// _G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER at all. That region still measures zero external writers.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_ai_unit_flag_and_move @0x004d7e22.
// void __watcall (uint player, int unit_index, int target_x, int target_y)
//                -- EAX / EDX / EBX / ECX.
//
// The whole body, in order:
//   units[player & 0xf][unit_index].order_notify_status = 1     0x004d7e4a
//   units[player & 0xf][unit_index].order_status_flags |= 0x40  0x004d7e51
//   c.unit_order_move_enqueue(player & 0xf, unit_index, target_x, target_y, 0)   0x004d7e60
//
// THE PLAYER IS MASKED TO 4 BITS TWICE, and both are reproduced rather than folded into one: once
// for the record stride (`AND EAX,0xf` @0x004d7e35, before `IMUL EAX,EAX,0x5b04`) and again for the
// value handed to the enqueue (`AND ESI,0xf` @0x004d7e5a, then `MOVZX EAX,SI`). They compute the
// same thing for any player the game can produce; keeping both documents that the original does not
// rely on the caller having masked it.
//
// target_x / target_y are PASSED THROUGH UNTOUCHED -- EBX and ECX are never written between entry
// and the call, so whatever the caller left in them is what the order carries.
void unit_flag_and_move(const issue_view &v, const order_sink &s, const issue_calls &c,
                        uint32_t player, int32_t unit_index, uint32_t target_x, uint32_t target_y);

// llm_strat_ai_group_scatter_to_passable_tile @0x004d7e6b.
// void __mh_watcall_ebx_volatile (uint player, int anchor_x, int anchor_y) -- EAX / EDX / EBX.
//
// For each entry of the published scratch list (the same list issue_group_formation_move consumes,
// filled by the AI caller), spiral outward from the anchor to the next PASSABLE tile and issue a
// move order to it:
//
//   loop head 0x004d7efe:  i < *ai_group_scratch_count, compared UNSIGNED (JC @0x004d7f07)
//   spiral    0x004d7e8d:  x = (anchor_x + spiral[s].dx) & *width_m
//                          y = (anchor_y + spiral[s].dy) & *height_m
//                          ++s
//                          repeat WHILE passable[(x << 8) | y] == 0   (JZ @0x004d7ebe)
//   emit      0x004d7edb:  units[player][list[i]].order_notify_status = 1
//             0x004d7ee2:  units[player][list[i]].order_status_flags |= 0x40
//             0x004d7ef6:  c.unit_order_move_enqueue(player16, list[i], x, y, 0)
//
// THE SPIRAL INDEX IS NOT RESET PER MEMBER, and that is the whole point of the function rather than
// an oversight: `XOR ESI,ESI` runs ONCE at entry (0x004d7e89), outside the loop. So member 0 takes
// the first passable tile, member 1 resumes the spiral from where member 0 stopped, and the group
// ends up spread over distinct tiles. A translation that reset the index per member would pile every
// member onto the same tile and still pass a naive one-member test.
//
// TWO ORIGINAL DEFECTS, BOTH PRESERVED (Law 2):
//   * THE SPIRAL SEARCH IS UNBOUNDED. Nothing compares `s` against the table's live entry count
//     (_G_LLM_STRAT_AI_TILE_SPIRAL_CELL_COUNT) or against any radius; if no passable tile is ever
//     found the original walks off the end of the offsets table and keeps going. Reproduced as
//     written -- do not add a bound.
//   * THE PLAYER IS NOT MASKED HERE. `IMUL EAX,[player],0x5b04` @0x004d7ed1 uses the raw argument,
//     unlike flag_and_move above which masks it to 4 bits. Same family, inconsistent guard; the
//     asymmetry is the original's.
//
// THE PASSABLE TEST IS `!= 0`, i.e. the spiral stops on the first NON-zero cell (`CMP byte ..,0x0` /
// `JZ` back to the top @0x004d7eb6-0x004d7ebe) -- stated explicitly because the plane's own plate
// describes the footprint scanner rejecting cells that read 0 OR 6, and this body does not check 6.
//
// ITS TAIL IS A JMP INTO ANOTHER FUNCTION'S EPILOGUE (`JMP 0x004d70c9` @0x004d7f09, which lands 7
// bytes before the end of llm_strat_ai_group_scatter_random_worker). That is a Watcom TAIL-MERGE of
// two identical epilogues, not a fall-through into other logic: both functions push ECX, ESI, EDI in
// that order, and the shared code is `LEA ESP,[EBP-0xc]; POP EDI; POP ESI; POP ECX; POP EBP; RET`.
// Semantically a plain return, which is why this translates as an ordinary void function.
void group_scatter_to_passable_tile(const issue_view &v, const order_sink &s, const issue_calls &c,
                                    uint32_t player, int32_t anchor_x, int32_t anchor_y);

} // namespace detail

// live_view()/live_sink()/live_calls() applied to the above.
void unit_flag_and_move(uint32_t player, int32_t unit_index, uint32_t target_x, uint32_t target_y);
void group_scatter_to_passable_tile(uint32_t player, int32_t anchor_x, int32_t anchor_y);

} // namespace mh::orders::issue
