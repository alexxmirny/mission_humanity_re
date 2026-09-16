//
// sim/sim_bldg_done_handlers.cpp -- see sim_bldg_done_handlers.h for the whole rationale (the shared
// calls struct, the extra_regions per site, why done_default has no shadow arm). Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_done_*.asm) -- every body is a straight-line CALL sequence,
// so the CALL ORDER below is copied verbatim from each .asm and is the entire translation.
//
#include "sim/sim_bldg_done_handlers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_done_calls &live_bldg_done_calls() {
    static const bldg_done_calls c = {
        MH_LIBMH_BIND(llm_strat_add_population_housing),
        MH_LIBMH_BIND(llm_strat_power_generate),
        MH_LIBMH_BIND(llm_strat_add_storage_capacity),
        MH_LIBMH_BIND(llm_strat_power_consume),
        MH_LIBMH_BIND(llm_strat_add_unit_capacity_soldiers),
        MH_LIBMH_BIND(llm_strat_add_unit_capacity_vehicles),
        MH_LIBMH_BIND(llm_strat_add_unit_capacity_planes),
        MH_LIBMH_BIND(llm_strat_add_unit_capacity_helis),
    };
    return c;
}

namespace detail {

// llm_strat_done_default @0x0046ff2c: no calls, no writes -- see the header banner.
void done_default() {}

// llm_strat_done_mother @0x0046ff4e: 0x0046ff66 add_population_housing, 0x0046ff6b power_generate,
// 0x0046ff70 add_storage_capacity.
void done_mother(const bldg_done_calls &c) {
    c.add_population_housing();
    c.power_generate();
    c.add_storage_capacity();
}

// llm_strat_done_plant @0x0046ff7f: 0x0046ff97 power_generate.
void done_plant(const bldg_done_calls &c) { c.power_generate(); }

// llm_strat_done_colony @0x0046ffa6: 0x0046ffbe add_population_housing, 0x0046ffc3 power_consume.
void done_colony(const bldg_done_calls &c) {
    c.add_population_housing();
    c.power_consume();
}

// llm_strat_done_quarters_vehicles @0x0046ffd2: 0x0046ffea add_unit_capacity_vehicles,
// 0x0046ffef power_consume.
void done_quarters_vehicles(const bldg_done_calls &c) {
    c.add_unit_capacity_vehicles();
    c.power_consume();
}

// llm_strat_done_quarters_soldiers @0x0046fffe: 0x00470016 add_unit_capacity_soldiers,
// 0x0047001b power_consume.
void done_quarters_soldiers(const bldg_done_calls &c) {
    c.add_unit_capacity_soldiers();
    c.power_consume();
}

// llm_strat_done_airfield @0x0047002a: 0x00470042 add_unit_capacity_planes, 0x00470047 power_consume.
void done_airfield(const bldg_done_calls &c) {
    c.add_unit_capacity_planes();
    c.power_consume();
}

// llm_strat_done_helipad @0x00470056: 0x0047006e add_unit_capacity_helis, 0x00470073 power_consume.
void done_helipad(const bldg_done_calls &c) {
    c.add_unit_capacity_helis();
    c.power_consume();
}

// llm_strat_done_mine @0x00470082: 0x0047009a add_storage_capacity, 0x0047009f power_consume.
void done_mine(const bldg_done_calls &c) {
    c.add_storage_capacity();
    c.power_consume();
}

// llm_strat_done_production @0x004700ae: 0x004700c6 power_consume.
void done_production(const bldg_done_calls &c) { c.power_consume(); }

// llm_strat_done_lab @0x004700d5: 0x004700ed power_consume.
void done_lab(const bldg_done_calls &c) { c.power_consume(); }

// llm_strat_done_turret @0x004700fc: 0x00470114 power_consume.
void done_turret(const bldg_done_calls &c) { c.power_consume(); }

// llm_strat_done_shuttle @0x0047014f: no calls, no writes -- see the header banner. Same class as
// done_default.
void done_shuttle() {}

// llm_strat_done_silos @0x00470123: 0x0047013b add_storage_capacity, 0x00470140 power_consume.
void done_silos(const bldg_done_calls &c) {
    c.add_storage_capacity();
    c.power_consume();
}

// llm_strat_done_relay @0x00470171: 0x00470189 power_consume.
void done_relay(const bldg_done_calls &c) { c.power_consume(); }

// llm_strat_done_port @0x00470198: 0x004701b0 add_storage_capacity.
void done_port(const bldg_done_calls &c) { c.add_storage_capacity(); }

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void done_default() { detail::done_default(); }
void done_mother() { detail::done_mother(live_bldg_done_calls()); }
void done_plant() { detail::done_plant(live_bldg_done_calls()); }
void done_colony() { detail::done_colony(live_bldg_done_calls()); }
void done_quarters_vehicles() { detail::done_quarters_vehicles(live_bldg_done_calls()); }
void done_quarters_soldiers() { detail::done_quarters_soldiers(live_bldg_done_calls()); }
void done_airfield() { detail::done_airfield(live_bldg_done_calls()); }
void done_helipad() { detail::done_helipad(live_bldg_done_calls()); }
void done_mine() { detail::done_mine(live_bldg_done_calls()); }
void done_production() { detail::done_production(live_bldg_done_calls()); }
void done_lab() { detail::done_lab(live_bldg_done_calls()); }
void done_turret() { detail::done_turret(live_bldg_done_calls()); }
void done_shuttle() { detail::done_shuttle(); }
void done_silos() { detail::done_silos(live_bldg_done_calls()); }
void done_relay() { detail::done_relay(live_bldg_done_calls()); }
void done_port() { detail::done_port(live_bldg_done_calls()); }


} // namespace mh::sim
