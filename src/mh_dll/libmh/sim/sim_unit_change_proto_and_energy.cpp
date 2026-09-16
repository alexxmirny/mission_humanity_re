//
// sim/sim_unit_change_proto_and_energy.cpp -- see sim_unit_change_proto_and_energy.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_change_proto_and_energy_00489521.asm); the
// exported .c draft agrees with the assembly here (two roster field writes via re-derived
// row/column IMUL strides, then one outward call), so this is a straight transcription.
//
#include "sim/sim_unit_change_proto_and_energy.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_change_proto_and_energy_calls &live_unit_change_proto_and_energy_calls() {
    static const unit_change_proto_and_energy_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_soldiers_start_walk_anim),
    };
    return c;
}

namespace detail {

void unit_change_proto_and_energy(sim_store &own, const unit_change_proto_and_energy_calls &c,
                                  uint16_t player, int32_t unit_idx, int16_t proto_delta,
                                  int32_t unused, double energy_delta) {
    (void)unused; // the fourth (ECX) parameter is genuinely dead in the original -- see the header.

    unit &u = own.unit_at(static_cast<uint32_t>(player), unit_idx);

    // 0x00489540-0x0048955d: unit_proto_id += proto_delta, a same-width 16-bit wraparound add
    // (`ADD word ptr [...],AX`) -- NOT widened to int before truncating back.
    u.unit_proto_id = static_cast<uint16_t>(static_cast<uint16_t>(u.unit_proto_id) +
                                            static_cast<uint16_t>(proto_delta));

    // 0x0048955d-0x0048957f: energy += energy_delta, plain x87 double add.
    u.energy = u.energy + energy_delta;

    // 0x00489582-0x0048958b: refresh the mounted-soldier walk animation.
    c.unit_soldiers_start_walk_anim(static_cast<uint32_t>(player), unit_idx);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_change_proto_and_energy(uint16_t player, int32_t unit_idx, int16_t proto_delta,
                                  int32_t unused, double energy_delta) {
    sim_state st = mh::sim::state();
    detail::unit_change_proto_and_energy(st.own, live_unit_change_proto_and_energy_calls(), player,
                                         unit_idx, proto_delta, unused, energy_delta);
}


} // namespace mh::sim
