//
// orders/issue/issue_order_admin.h -- the admin/debug-console order wrappers (RI-ORDERS / O4-0,
// O4A-C sweep, batch C unit `order_admin`). Seven functions, each a thin dispatch(0, player, code,
// code) call with param0 == order_code at every site -- the "constant triple, unit_id always 0"
// shape the pilot's `bldg_order_activate` established, minus any kind nibble (these are player-
// targeted admin/debug orders, not building- or unit-targeted ones). See
// tmp/decomp_orders_issue/_UNIT_order_admin.md.
//
// THIS UNIT DECLARES NO SHADOW SITE. Every row is UI-rooted, debug-rooted, or has no order_matrix
// entry at all (per the unit spec), so the domain's soak/sp rig vehicles never reach any of them --
// an arm would read ZERO CALLS. All seven are proven OFFLINE against `order_issue_golden.gen.h`
// instead (each has exactly one golden site).
//
// Every `detail::` form takes (view, sink, calls) as its first three parameters, always, even though
// none of these seven bodies calls a member of `issue_calls` -- issue_state.h's rule, fixed by the
// O4A-C sweep.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_order_grant_resource @0x0046f537 (91 B). void __mh_watcall_ebx_volatile(uint player,
// int resource_type, int amount) -- EAX/EDX/EBX. Golden site: order 0xed.
//
// scratch_reset(), THEN scratch_set_field(3, resource_type) BEFORE scratch_set_field(2, amount) --
// the original loads EDX(=resource_type)/EAX=3 first, EBX(=amount)/EAX=2 second (0x0046f55b-
// 0x0046f573). Owner is `(uint16_t)player`, no kind OR'd in at all (MOVZX EDX,word [player] straight
// into the dispatch call, 0x0046f57f); unit_id is a literal 0 (XOR EAX,EAX, 0x0046f583).
void order_grant_resource(const issue_view &v, const order_sink &s, const issue_calls &c,
                          uint32_t player, int32_t resource_type, int32_t amount);

// llm_strat_order_collect_available_projects @0x0046f687 (58 B). void __watcall(uint side) -- EAX.
// Golden site: order 0xee. Unconditional, no scratch. unit_id 0, owner (uint16_t)side, no kind.
void order_collect_available_projects(const issue_view &v, const order_sink &s, const issue_calls &c,
                                      uint32_t side);

// llm_strat_order_recheck_projects @0x0046f6fb (58 B). void __watcall(uint side) -- EAX. Golden
// site: order 0xf0. Same shape as collect_available_projects, different code.
void order_recheck_projects(const issue_view &v, const order_sink &s, const issue_calls &c,
                            uint32_t side);

// llm_strat_order_recheck_buildings @0x0046f76f (58 B). void __watcall(uint side) -- EAX. Golden
// site: order 0xef. Same shape again.
void order_recheck_buildings(const issue_view &v, const order_sink &s, const issue_calls &c,
                             uint32_t side);

// llm_strat_order_recheck_planet_system_all_players @0x0046f7e3 (58 B). void __watcall(void) -- NO
// parameter. Golden site: order 0xf1. The owner comes from v.player_side (RID_PLAYERSIDE,
// MOVZX EDX,word [0x00e58354] @0x0046f805) rather than an argument -- the one row in this unit that
// reads the view.
void order_recheck_planet_system_all_players(const issue_view &v, const order_sink &s,
                                             const issue_calls &c);

// llm_strat_order_credit_conquest_kills @0x0046f852 (58 B). void __watcall(uint side) -- EAX.
// Golden site: order 0xfa. Same constant-triple shape as the three recheck/collect wrappers above.
void order_credit_conquest_kills(const issue_view &v, const order_sink &s, const issue_calls &c,
                                 uint32_t side);

// llm_strat_order_fow_reveal_full @0x0046fce0 (83 B). void __watcall(uint player_id) -- EAX. Golden
// site: order 0xf9. scratch_reset() THEN scratch_set_field(2, player_id) (0x0046fcfb-0x0046fd08).
// Owner is `player_id & 0xf` (AND EAX,0xf @0x0046fd1a, NOT `& 0xffff` like every other row here) --
// the "no kind at all, low nibble only" shape the context doc calls out. unit_id is a literal 0.
void order_fow_reveal_full(const issue_view &v, const order_sink &s, const issue_calls &c,
                           uint32_t player_id);

} // namespace detail

void order_grant_resource(uint32_t player, int32_t resource_type, int32_t amount);
void order_collect_available_projects(uint32_t side);
void order_recheck_projects(uint32_t side);
void order_recheck_buildings(uint32_t side);
void order_recheck_planet_system_all_players();
void order_credit_conquest_kills(uint32_t side);
void order_fow_reveal_full(uint32_t player_id);

} // namespace mh::orders::issue
