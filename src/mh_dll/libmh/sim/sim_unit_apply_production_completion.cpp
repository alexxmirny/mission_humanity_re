#include "sim/sim_unit_apply_production_completion.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const apply_production_completion_calls &live_apply_production_completion_calls() {
    static const apply_production_completion_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
        MH_LIBMH_BIND(llm_strat_population_add),
        MH_LIBMH_BIND(llm_strat_unit_housing_count_remove),
    };
    return c;
}

namespace {

// units[player][0].order per-player HEADER-ROW decrement (0x00492c9c, `DEC word ptr`, width WORD) --
// the DECREMENT half of the increment idiom sim_unit_create.h / sim_unit_create_soldier.cpp /
// sim_unit_spawn_on_tile.cpp / sim_unit_recruit.cpp's own headers already document
// (kUnitZeroOrderHeaderIncrement in those files). Named LOCALLY rather than shared or folded into
// sim_order_enqueue.h's UNIT_STATE_STOP_TO_DEFAULT, matching every one of those siblings' own caution
// that the numeric coincidence (both are 1) is not proof of shared enum semantics.
inline constexpr uint16_t kUnitZeroOrderHeaderDecrement = 1;

} // namespace

namespace detail {

void unit_apply_production_completion(const sim_view &v, sim_store &own,
                                      const apply_production_completion_calls &c, uint32_t player,
                                      int32_t unit_proto_id) {
    // 0x00492c08/0x00492c0b: `player`/`unit_proto_id` land in locals; `player` is re-truncated to its
    // low 16 bits at every one of its four uses in the original (MOVZX word ptr) -- hoisted once here,
    // matching sim_bldg_grant_type_resources.cpp's/sim_unit_refund.cpp's own single-truncation
    // precedent for a value that never changes across the body.
    const uint32_t player16 = player & 0xffffu;

    // ---- the resource-cost loop (0x00492c15-0x00492c62) ------------------------------------------
    // `v.cfg_units[unit_proto_id].resource[i]`, id-read-THEN-bound-check, exactly like sim_bldg_grant_
    // type_resources.cpp's identical shape (see the header banner for the address-arithmetic
    // confirmation that this is the same `cfg_units`/`resource` array). Reproduced as an unbounded
    // `for(;;)` with two ordered breaks -- the `i==7` OOB read this shape could reach is provably
    // inert by the same general argument sim_bldg_grant_type_resources.h's/sim_bldg_pay_costs.h's own
    // headers give for their identical loop.
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = (int32_t)v.cfg_units[unit_proto_id].resource[i].id; // 0x00492c24
        if (resource_id == 0)                                                           // 0x00492c2d/0x00492c31: UNDEFINED
            break;
        if (!(i < CFG_RESOURCE_SLOTS)) // 0x00492c33/0x00492c37: bound check, evaluated SECOND
            break;

        const int32_t val = v.cfg_units[unit_proto_id].resource[i].val; // 0x00492c4a
        c.resource_add((int32_t)player16, resource_id, val);            // 0x00492c57
    }

    // ---- population-add, gated on soldier_count != 0 (0x00492c64-0x00492c86) ----------------------
    const int32_t soldier_count = v.cfg_units[unit_proto_id].soldier_count; // 0x00492c6b
    if (soldier_count != 0) {                                               // 0x00492c74/0x00492c78
        c.population_add((uint16_t)player16, soldier_count);                // 0x00492c81
    }

    // ---- housing release, unconditional (0x00492c86-0x00492c92) -----------------------------------
    // `unit_proto_id` passed FULL (no truncation), `player` truncated -- see the header banner's
    // confirmation of this callee's real (player, unit_proto_id) parameter roles.
    c.housing_count_remove((int32_t)player16, unit_proto_id); // 0x00492c8d

    // ---- the header-row decrement (0x00492c92-0x00492ca3) ------------------------------------------
    // units[player][0].order -= 1 -- slot 0, the roster's reserved per-player header row, NOT the
    // caller's own unit's order field. See the header banner's HEADER-ROW DECREMENT section for the
    // full base-address derivation (units base 0x00dd8c48 + field offset 0x4 == `order`).
    unit &roster_header = own.unit_at(player16, 0);
    roster_header.order = (uint16_t)(roster_header.order - kUnitZeroOrderHeaderDecrement);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_apply_production_completion(uint32_t player, int32_t unit_proto_id) {
    sim_state st = state();
    detail::unit_apply_production_completion(st.read, st.own, live_apply_production_completion_calls(),
                                             player, unit_proto_id);
}


} // namespace mh::sim
