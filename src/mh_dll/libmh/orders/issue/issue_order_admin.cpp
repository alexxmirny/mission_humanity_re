//
// orders/issue/issue_order_admin.cpp -- the admin/debug-console order wrappers (RI-ORDERS / O4-0,
// O4A-C sweep, batch C unit `order_admin`). See the header for the per-function evidence; this file
// is the transcription. DERIVED FROM THE .asm, not the .c drafts (tmp/decomp_orders_issue/
// llm_strat_order_{grant_resource,collect_available_projects,recheck_projects,recheck_buildings,
// recheck_planet_system_all_players,credit_conquest_kills,fow_reveal_full}_*.asm).
//
// THE DISPATCH ARGUMENT SHAPE, stated once: all seven load ECX=order_code, EBX=param0 (equal to
// order_code at every site here), then either MOVZX the player/owner half-word straight into EDX
// with no OR at all (six of the seven), or the `AND EAX,0xf` low-nibble form (fow_reveal_full only).
// EAX (unit_id) is a literal 0 at every site -- none of these rows target a building or unit slot;
// they are player- or session-scoped admin orders. `s.dispatch(unit_id, player, op_code, arg)` maps
// op_code to the record's param0 and arg to order_code, per order_sink's own comment.
//
#include "orders/issue/issue_order_admin.h"

namespace mh::orders::issue {

// ---- llm_strat_order_grant_resource @0x0046f537 --------------------------------------------------
void detail::order_grant_resource(const issue_view &, const order_sink &s, const issue_calls &,
                                  uint32_t player, int32_t resource_type, int32_t amount) {
    s.scratch_reset();
    s.scratch_set_field(3, resource_type);
    s.scratch_set_field(2, amount);
    s.dispatch(0, (uint16_t)player, 0xed, 0xed);
}

// ---- llm_strat_order_collect_available_projects @0x0046f687 --------------------------------------
void detail::order_collect_available_projects(const issue_view &, const order_sink &s,
                                              const issue_calls &, uint32_t         side) {
    s.dispatch(0, (uint16_t)side, 0xee, 0xee);
}

// ---- llm_strat_order_recheck_projects @0x0046f6fb -------------------------------------------------
void detail::order_recheck_projects(const issue_view &, const order_sink &s, const issue_calls &,
                                    uint32_t side) {
    s.dispatch(0, (uint16_t)side, 0xf0, 0xf0);
}

// ---- llm_strat_order_recheck_buildings @0x0046f76f -------------------------------------------------
void detail::order_recheck_buildings(const issue_view &, const order_sink &s, const issue_calls &,
                                     uint32_t side) {
    s.dispatch(0, (uint16_t)side, 0xef, 0xef);
}

// ---- llm_strat_order_recheck_planet_system_all_players @0x0046f7e3 --------------------------------
// No parameter -- the owner half-word comes from v.player_side (RID_PLAYERSIDE) instead.
void detail::order_recheck_planet_system_all_players(const issue_view &v, const order_sink &s,
                                                     const issue_calls &) {
    s.dispatch(0, *v.player_side, 0xf1, 0xf1);
}

// ---- llm_strat_order_credit_conquest_kills @0x0046f852 --------------------------------------------
void detail::order_credit_conquest_kills(const issue_view &, const order_sink &s, const issue_calls &,
                                         uint32_t side) {
    s.dispatch(0, (uint16_t)side, 0xfa, 0xfa);
}

// ---- llm_strat_order_fow_reveal_full @0x0046fce0 ---------------------------------------------------
// The one row in this unit whose owner is `player_id & 0xf` rather than `(uint16_t)side` -- no kind
// nibble, low nibble of the FULL dword only (AND EAX,0xf @0x0046fd1a, not a 16-bit narrow first).
void detail::order_fow_reveal_full(const issue_view &, const order_sink &s, const issue_calls &,
                                   uint32_t player_id) {
    s.scratch_reset();
    s.scratch_set_field(2, player_id);
    s.dispatch(0, (uint16_t)(player_id & 0xfu), 0xf9, 0xf9);
}

// ---- the public forms ------------------------------------------------------------------------------

void order_grant_resource(uint32_t player, int32_t resource_type, int32_t amount) {
    detail::order_grant_resource(live_view(), live_sink(), live_calls(), player, resource_type,
                                 amount);
}
void order_collect_available_projects(uint32_t side) {
    detail::order_collect_available_projects(live_view(), live_sink(), live_calls(), side);
}
void order_recheck_projects(uint32_t side) {
    detail::order_recheck_projects(live_view(), live_sink(), live_calls(), side);
}
void order_recheck_buildings(uint32_t side) {
    detail::order_recheck_buildings(live_view(), live_sink(), live_calls(), side);
}
void order_recheck_planet_system_all_players() {
    detail::order_recheck_planet_system_all_players(live_view(), live_sink(), live_calls());
}
void order_credit_conquest_kills(uint32_t side) {
    detail::order_credit_conquest_kills(live_view(), live_sink(), live_calls(), side);
}
void order_fow_reveal_full(uint32_t player_id) {
    detail::order_fow_reveal_full(live_view(), live_sink(), live_calls(), player_id);
}

} // namespace mh::orders::issue
