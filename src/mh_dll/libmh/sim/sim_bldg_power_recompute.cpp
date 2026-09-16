//
// sim/sim_bldg_power_recompute.cpp -- see sim_bldg_power_recompute.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_power_recompute_00491260.asm), with the FDIVRP operand order and
// the FCOMP/SAHF clamp direction re-derived from raw x87/EFLAGS semantics rather than trusted from
// the Ghidra .c draft -- see the header for the full derivation.
//
#include "sim/sim_bldg_power_recompute.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const power_recompute_calls &live_power_recompute_calls() {
    static const power_recompute_calls gc = {
        MH_LIBMH_BIND(llm_strat_refresh_all_buildings),
    };
    return gc;
}

namespace detail {

void power_recompute(sim_store &own, const power_recompute_calls &c, uint16_t player) {
    // ONE reference, read AND written through it for the whole function -- never re-fetched
    // (0x0049127b-0x0049130f all address off the same `player*0x18` row).
    power_stats &ps = own.power_stats_at(player);

    // ratio = (generated+1)/(consumed+1) (0x0049127b-0x004912ac). See the header: FDIVRP's "reverse"
    // un-reverses the FILD push order, so this is a plain numerator/denominator division, not the
    // operand-order trap it looks like from the raw mnemonic.
    ps.ratio = (double)(ps.generated + 1) / (double)(ps.consumed + 1);

    // Clamp (0x004912b9-0x004912e1): if (ratio > 1.0) ratio = 1.0. The asm's "then" arm stores the
    // literal IEEE-754 double bit pattern for 1.0 as two dword writes -- `ratio = 1.0;` is
    // bit-identical, no separate constant needed.
    if (ps.ratio > 1.0) ps.ratio = 1.0;

    // Latch AFTER the ratio computation, through the SAME `ps` reference, in this order
    // (0x004912e1-0x0049130f): prev_generated first, then prev_consumed.
    ps.prev_generated = ps.generated;
    ps.prev_consumed  = ps.consumed;

    // Unconditional tail call (0x00491315-0x0049131e), through the indirected callee -- a real sim/AI
    // sibling migration-set function, not reimplemented here (translator brief rule 3).
    c.refresh_all_buildings((uint32_t)player);
}

} // namespace detail

// ---- the public surface ----------------------------------------------------------------------

void power_recompute(uint16_t player) {
    sim_state st = state();
    detail::power_recompute(st.own, live_power_recompute_calls(), player);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
