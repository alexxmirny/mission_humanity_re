//
// orders/issue/issue_group_formation_move.h -- the AI group formation mover (RI-ORDERS / O4B).
//
//   llm_strat_ai_group_move_formation_rotating @0x004d7da9 (0x79 bytes)
//
// WHY AN `llm_strat_ai_*` FUNCTION IS IN THIS DOMAIN. It is an order-issue wrapper wearing the AI
// name prefix, and the `ai` ledger held it for the prefix alone. Its whole body stamps two
// ORDER-PIPELINE bytes on each unit it moves and hands that unit to a move-order enqueue; nothing in
// it is AI policy. The two bytes settle it: `order_status_flags` (+0xe3) and `order_notify_status`
// (+0xe8) are SIM-written and AI-READ -- llm_strat_unit_notify_status writes both at six sites
// (libmh/sim/sim_unit_notify.cpp), sim_target_release_ref and sim_storage_scrap write the flags, and the
// only AI touches anywhere in the tree are reads (ai_group_task_predicates.cpp's readiness switch,
// ai_group_move_helpers.cpp's 0x80 test). Homing it in `ai` would have meant a mutable `units` in
// `ai_store` -- the R2 hole ai_state.h names in as many words. The re-home is recorded in
// tools/data/migration/ai.json (`select.owned_elsewhere`) and adopted by this domain's `names` seed.
//
// IT IS A CONSUMER, NOT A HARVESTER. Its caller -- llm_strat_ai_group_task_advance_to_anchor,
// already translated at libmh/ai/ai_group_task_formation.cpp -- resets
// _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_COUNT and appends every group member's id into
// _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST, then calls this. The list IS the argument; that protocol
// is documented from the publishing side in ai_group_task_formation.h. This is why an order-issue
// wrapper reads two AI-owned regions (issue_view::ai_group_scratch_list / _count).
//
// NO GENERATED GOLDEN CASE, and the reason is structural rather than an omission. Its sink is
// llm_strat_unit_order_move_enqueue (@0x0046a0d5), one level ABOVE the container;
// gen_order_issue_golden.py's roots are llm_strat_order_dispatch and llm_strat_order_enqueue, so the
// abstract interpreter records ZERO sites for this wrapper. Its emission is observed instead through
// `issue_calls::unit_order_move_enqueue` by a hand-authored `net_selftest issuetest` recorder, which
// is why the row is a T2 -- the same tier, for the same reason, as the domain's other no-golden-site
// rows. This header is the spec those cases are written against.
//
// NO SHADOW SITE, deliberately. The row IS rig-reachable (it is AI-rooted, so an all-AI soak enters
// it), but its only caller's site -- shadow_ai_c7/c8 on group_task_advance_to_anchor -- is already
// armed and its declared closure CONTAINS this function, so arming both would make the original arm
// re-enter the inner site. One owner per entry. Proven offline; if a later session wants live-game
// evidence for this row it must disarm the caller's site first.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_ai_group_move_formation_rotating @0x004d7da9.
// void __watcall (game_t_Player player, int unused_param2, int unused_param3, int target_x,
//                 int target_y) -- EAX/EDX/EBX/ECX/Stack[0x4].
//
// `unused_param2` / `unused_param3` are genuinely dead, verified rather than assumed: EDX is never
// read before the loop overwrites it, and EBX is overwritten by the first instruction of the loop
// body (`IMUL EBX,...` @0x004d7dc4). The caller passes the group's live CENTROID in them
// (ai_group_task_formation.cpp @0x004eb00e), so the values are meaningful at the call site and
// ignored here. The committed prototype's names are carried through verbatim rather than renamed on
// a guess -- and they are kept as PARAMETERS, not dropped, because the shape is the original's ABI.
//
// FOR EACH ENTRY of the published scratch list, in order (0x004d7dc4-0x004d7e02):
//   units[player][id].order_notify_status = 1            0x004d7dd5
//   units[player][id].order_status_flags |= 0x40         0x004d7ddd
//   c.unit_order_move_enqueue(player16, id, target_x, target_y, seq)   0x004d7dfd
// where `seq` is v.order_seq_id_by_player[player] read FRESH inside the loop (MOVZX @0x004d7de5) --
// so every member of one formation is stamped with the SAME id, the tail bump below being the only
// writer. The two byte stores land on different fields, so their relative order is unobservable;
// they are written in listing order anyway.
//
// THE LOOP BOUND IS RE-READ EVERY ITERATION (`CMP EDI,[_..SCRATCH_COUNT]` @0x004d7e03, not a cached
// length) and the compare is UNSIGNED (JC, not JL). Both are reproduced: a callee that changed the
// count mid-walk would be seen by the original, and a negative count would terminate the loop
// immediately under JC where a signed compare would run it.
//
// THE TAIL RUNS UNCONDITIONALLY -- including when the list is EMPTY and no order was issued
// (0x004d7e0b-0x004d7e1d): bump v.order_seq_id_by_player[player], and bump it AGAIN if that wrapped
// to 0. It is a byte, so the second bump fires exactly on the 0xff -> 0 wrap; 0 is never left as a
// live seq id. Same skip-zero stamp as issue_group_orders.cpp's, but reached on every call rather
// than only when something moved.
void group_move_formation_rotating(const issue_view &v, const order_sink &s, const issue_calls &c,
                                   uint32_t player, int32_t unused_param2, int32_t unused_param3,
                                   int32_t target_x, int32_t target_y);

} // namespace detail

// live_view()/live_sink()/live_calls() applied to the above.
void group_move_formation_rotating(uint32_t player, int32_t unused_param2, int32_t unused_param3,
                                   int32_t target_x, int32_t target_y);

} // namespace mh::orders::issue
