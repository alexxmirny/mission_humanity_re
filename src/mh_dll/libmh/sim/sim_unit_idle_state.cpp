//
// sim/sim_unit_idle_state.cpp -- see sim_unit_idle_state.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_is_idle_or_patrolling_004ee4c2.asm), not from Ghidra's C draft: the two
// agree on the logic (the draft's `!(order!=P && state!=S && state!=H)` is De Morgan's dual of the
// three-CMP short-circuit chain below), so this is a case where the draft's PLATE was worth
// re-deriving from the listing (as asked) but turned out correct, not a case where it lied.
//
// The base/index arithmetic (`IMUL EAX,player,0x5b04` / `IMUL EAX,unit_index,0xe9` / ADD, then
// `[EAX + 0xdd8c4c]` / `[EAX + 0xdd8c4e]`) is the SAME row/item stride sim_state.h documents for
// `units` (UNITS_PER_PLAYER row stride 0x5b04, per-record stride 0xe9) added to the `units` global's
// own base address 0x00dd8c48 (addr/mh_regions.gen.h's RID_UNITS: `{"units", 0x00dd8c48u, ...}`) --
// 0xdd8c4c - 4 == 0xdd8c48, confirming the two CMPs read units[player][unit_index] at byte offsets
// +0x4 and +0x6, i.e. the `order` and `state` fields (addr/mh_structs.gen.h:
// mh_map_object_unit::order @+0x4, ::state @+0x6). unit_of() below reproduces the exact same
// arithmetic; no raw offset appears here.
//
#include "sim/sim_unit_idle_state.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// llm_strat_unit_state members this predicate tests (order field compared to one value, state field
// to two others). Values from sim_order_enqueue.h's own "Ghidra enum dump 2026-08-08
// (get-data-type-by-string llm_strat_unit_state)" -- duplicated here rather than shared by #include,
// per this file's header note. NOT reconciled against sim_unit_state_predicates.h's contrary claim
// that the same field domain is "not backed by any enum in this tree" -- both values match this
// function's own assembly either way (a bare literal or a named one compare identically), so nothing
// here depends on which claim is right; flagged in the translation report instead of picked between.
inline constexpr uint16_t UNIT_STATE_STOP_TO_DEFAULT = 0x01;
inline constexpr uint16_t UNIT_STATE_PATROL_SWAP     = 0x10;
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE    = 0x2e;

} // namespace

namespace detail {

int32_t unit_is_idle_or_patrolling(const sim_view &v, int32_t player, int32_t unit_index) {
    const unit &u = unit_of(v, (uint32_t)player, unit_index);

    // 0x004ee4e1/0x004ee4e9: order == PATROL_SWAP -> return 1.
    // 0x004ee4eb/0x004ee4f3: state == STOP_TO_DEFAULT -> return 1.
    // 0x004ee4f5/0x004ee4fd: state == HOVER_ENGAGE -> return 1, else (JNZ 0x004ee508) return 0.
    if (u.order == UNIT_STATE_PATROL_SWAP || u.state == UNIT_STATE_STOP_TO_DEFAULT ||
        u.state == UNIT_STATE_HOVER_ENGAGE) {
        return 1;
    }
    return 0;
}

} // namespace detail

int32_t unit_is_idle_or_patrolling(int32_t player, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_is_idle_or_patrolling(v, player, unit_index);
}


} // namespace mh::sim
