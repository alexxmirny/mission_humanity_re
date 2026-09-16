#include "sim/sim_unit_create.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_IDLE_SCATTER -- see the anon-namespace note below
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_create_calls &live_unit_create_calls() {
    static const unit_create_calls c = {
        MH_LIBMH_BIND(map_unit_Add),
        MH_LIBMH_BIND(map_unit_PutOnMap),
        MH_LIBMH_BIND(llm_strat_population_add),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
    };
    return c;
}

namespace {

// units[player][0].order per-player HEADER-ROW increment (0x004638e0, `INC word ptr`, width WORD).
// At least the FOURTH independent occurrence of this exact idiom in the sim closure
// (sim_unit_create_soldier.cpp's kUnitZeroOrderHeaderIncrement, llm_unit_recruit.h's own hazard
// note, sim_unit_spawn_on_tile.cpp's own copy, now this one) -- named LOCALLY rather than shared or
// folded into sim_order_enqueue.h's UNIT_STATE_STOP_TO_DEFAULT, matching every sibling's caution
// that the numeric coincidence (both are 1, confirmed here by the bare `INC` opcode -- not an `ADD
// ..., <imm>` -- so the value is unambiguously 1) is not proof of shared enum semantics.
inline constexpr uint16_t kUnitZeroOrderHeaderIncrement = 1;

} // namespace

namespace detail {

uint32_t create_unit(const sim_view &v, sim_store &own, const unit_create_calls &c, uint32_t x,
                     uint32_t y, uint16_t unit_proto_id, uint16_t player, uint8_t is_ship) {
    // 0x00463881-0x00463897: is_ship_offset = (is_ship == 1) ? 9 : 0 -- an EQUALITY test against the
    // literal 1 (`CMP byte ptr [EBP+8],0x1`), not a truthiness test. Ships are confined to roster
    // slots 1..90, everything else to 1..99 -- see the header hazard.
    const int32_t is_ship_offset = (is_ship == 1) ? 9 : 0;

    // 0x0046389e-0x004638b3: the free-slot scan. `(100 - is_ship_offset) <= slot` is the failure
    // exit (return 0 below, no wraparound, no reuse of slot 0); otherwise probe
    // units[player][slot].unit_proto_id == 0 and stop there, else advance.
    int32_t slot = 1;
    for (;;) {
        if (100 - is_ship_offset <= slot) return 0;
        if (unit_of(v, (uint32_t)player, slot).unit_proto_id == 0) break;
        ++slot;
    }

    // 0x004638d6-0x004638e6: the roster header-row increment, UNCONDITIONAL once a free slot is
    // found, BEFORE map::unit::Add -- targets units[player][0], NOT units[player][slot]. See the
    // anon-namespace comment on why STOP_TO_DEFAULT's value (1) is kept as its own local constant
    // rather than reused from sim_order_enqueue.h.
    unit &roster_header = own.unit_at((uint32_t)player, 0);
    roster_header.order = (uint16_t)(roster_header.order + kUnitZeroOrderHeaderIncrement);

    // 0x004638e7-0x004638f6: map::unit::Add (ORIGINAL, via `c`) -- stamps
    // units[player][slot].unit_proto_id (and whatever else it owns internally). Register order per
    // addr/mh_calls.gen.h: EAX=slot, EDX=unit_proto_id, EBX=player.
    c.unit_add((uint32_t)slot, (int32_t)unit_proto_id, player);

    unit &u = own.unit_at((uint32_t)player, (uint32_t)slot);

    // 0x00463913-0x0046392f: STATE written first, then ORDER -- both UNIT_STATE_IDLE_SCATTER (0x13),
    // reused verbatim from sim_order_enqueue.h per naming rule 17a (same Ghidra llm_strat_unit_state
    // enum member sim_order_enqueue.h already names). See the header hazard: this is IDLE_SCATTER,
    // NOT STOP_TO_DEFAULT -- do not conflate with the header-row increment above.
    u.state = UNIT_STATE_IDLE_SCATTER;
    u.order = UNIT_STATE_IDLE_SCATTER;

    // 0x0046392f-0x00463968: elevation, re-derived from Unit[proto].elevation using the record's OWN
    // (just-written) unit_proto_id, not the `unit_proto_id` PARAMETER -- see the header hazard on
    // why every subsequent cfg lookup in this function re-reads u.unit_proto_id off the record.
    u.elevation = v.cfg_units[u.unit_proto_id].elevation;

    // 0x0046396e-0x00463982: map::unit::PutOnMap (ORIGINAL, via `c`) -- x/y BOTH truncated to a
    // byte here (matching unit::x/y's uint8_t storage), unlike the FULL-width x/y the FoW call
    // below receives -- see the header hazard. Register order: EAX=player, EDX=slot, EBX=x, ECX=y.
    c.unit_put_on_map(player, (uint16_t)slot, (uint8_t)x, (uint8_t)y);

    // 0x00463983-0x004639aa: the population gate, `Unit[proto].human != 0` alone (no soldier_count
    // condition, unlike create_soldier's sibling gate) -- see the header hazard. `placed_proto` is
    // re-derived from u.unit_proto_id again (the same record read map::unit::Add already performed;
    // nothing writes unit_proto_id again between the elevation stamp and here, so re-reading is
    // value-identical to the original's own repeated memory loads).
    const cfg_unit &placed_proto = v.cfg_units[u.unit_proto_id];
    if (placed_proto.human != 0) {
        // 0x004639b0-0x004639df: llm_strat_population_add (ORIGINAL, via `c`), then the two
        // _G_LLM_STRAT_POP_STATS[player] bookkeeping writes (direct sim_store accessor, same shape
        // create_soldier.cpp's own population branch uses). Order: human -=, THEN human_in_field +=
        // (matches the asm's own SUB-then-ADD order at 0x00463a0c / 0x00463a3f).
        c.population_add(player, placed_proto.human);
        own.population_at(player).human -= placed_proto.human;
        own.population_at(player).human_in_field += placed_proto.human;
    }

    // 0x00463a45-0x00463a76: map_fow_UpdateFoWPlus (ORIGINAL, via `c`), ALWAYS -- runs whether or
    // not the population branch above fired (LAB_00463a45 is the join point for both paths). x/y
    // are the FULL untruncated parameters here (unlike unit_put_on_map above) -- see the header
    // hazard. Register order: EAX=player, EDX=x, EBX=y, ECX=sight.
    c.fow_update_plus(player, x, y, (uint8_t)placed_proto.sight);

    // 0x00463a7b-0x00463aa1: llm_strat_ai_notify_unit_lifecycle (ORIGINAL, via `c`), ALWAYS, kind=4
    // ("unit appeared" -- same bare literal every sibling creation primitive uses; no generated C++
    // enum exists for this domain). Re-reads u.unit_proto_id off the record again (not the
    // `unit_proto_id` parameter) for the unit_type argument. Register order: EAX=player,
    // EDX=unit_proto_id, EBX=slot, ECX=kind.
    c.ai_notify_unit_lifecycle(player, u.unit_proto_id, (uint32_t)slot, 4u);

    return (uint32_t)slot;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t unit_create(uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player, uint8_t is_ship) {
    sim_state st = state();
    return detail::create_unit(st.read, st.own, live_unit_create_calls(), x, y, unit_proto_id, player,
                               is_ship);
}


} // namespace mh::sim
