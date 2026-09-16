//
// orders/issue/issue_order_player.h -- the `order_player` unit's per-player admin order wrappers
// (RI-ORDERS / O4A-C).
//
// Two wrappers: unit `order_player` of the O4A-C sweep
// (tmp/decomp_orders_issue/_UNIT_order_player.md). Both are UI-rooted -- neither appears in
// The order matrix's `issue_domains_rooted` set, so the domain's soak/sp rig vehicles never
// call them -- so THIS UNIT DECLARES NO SHADOW SITE. Both are proven OFFLINE against
// order_issue_golden.gen.h instead.
//
// Neither reads the `buildings`/`units` regions, so there is no local `bldg_at()`/`unit_at()`
// helper here (unlike issue_bldg_orders.cpp) -- the .cpp is flat.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_order_set_player_relation @0x0046f8c6. Order 0xf4/0xf4. Unguarded, no OR'd kind nibble
// -- the owner is the plain 16-bit MOVZX of `player` (`MOVZX EDX,word ptr [player]`), and the
// dispatch's own unit_index is a literal 0: this order targets a player-to-player relation, not an
// object. Scratch: field 2 = opponent_idx, field 3 = relation_value (zero-extended from the
// original's byte parameter). `v` and `c` are unused -- named away.
void order_set_player_relation(const issue_view &, const order_sink &s, const issue_calls &,
                               uint32_t player, int32_t opponent_idx, uint8_t relation_value);

// llm_strat_order_set_player_control_mode @0x0046f97e. Order 0xf5/0xf5, unit_index a literal 0,
// owner the plain 16-bit MOVZX of `player` (no kind nibble). Scratch: field 2 = target_player,
// field 3 = zero-extended `set_human`. See the .cpp for the two things that make this row unusual
// among the sweep's wrappers: it both READS and WRITES `v.player_control_mask` (its only accessor
// in this domain), and it formats a debug line into `v.text_tmp` via `c.sprintf_ii` on every call --
// a dead store nothing in this domain reads back, transcribed anyway because it is what the
// original does.
void order_set_player_control_mode(const issue_view &v, const order_sink &s, const issue_calls &c,
                                   uint32_t player, int32_t target_player, uint8_t set_human);

} // namespace detail

void order_set_player_relation(uint32_t player, int32_t opponent_idx, uint8_t relation_value);
void order_set_player_control_mode(uint32_t player, int32_t target_player, uint8_t set_human);

} // namespace mh::orders::issue
