#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The 8 shared economy primitives every done_<kind> below composes from (see the header banner's
// rationale for one shared struct instead of 12 near-duplicate ones). All 8 are `void __watcall
// (void)` originals, not yet translated -- bound to their mh::call:: stubs in live_bldg_done_calls().
struct bldg_done_calls {
    void (*add_population_housing)();
    void (*power_generate)();
    void (*add_storage_capacity)();
    void (*power_consume)();
    void (*add_unit_capacity_soldiers)();
    void (*add_unit_capacity_vehicles)();
    void (*add_unit_capacity_planes)();
    void (*add_unit_capacity_helis)();
};

const bldg_done_calls &live_bldg_done_calls();

namespace detail {

// llm_strat_done_default @0x0046ff2c. No parameters, no callees -- the whole body (besides the inert
// stack probe) is empty. See the header banner: this is the table's intentional "do nothing" default.
void done_default();

void done_mother(const bldg_done_calls &c);
void done_plant(const bldg_done_calls &c);
void done_colony(const bldg_done_calls &c);
void done_quarters_vehicles(const bldg_done_calls &c);
void done_quarters_soldiers(const bldg_done_calls &c);
void done_airfield(const bldg_done_calls &c);
void done_helipad(const bldg_done_calls &c);
void done_mine(const bldg_done_calls &c);
void done_production(const bldg_done_calls &c);
void done_lab(const bldg_done_calls &c);
void done_turret(const bldg_done_calls &c);

// llm_strat_done_shuttle @0x0047014f. No parameters, no callees -- same "no bonus" no-op class as
// done_default (see the batch-H second-slice banner above).
void done_shuttle();

void done_silos(const bldg_done_calls &c);
void done_relay(const bldg_done_calls &c);
void done_port(const bldg_done_calls &c);

} // namespace detail

// Live wrappers -- match each original's committed void(void) prototype exactly.
void done_default();
void done_mother();
void done_plant();
void done_colony();
void done_quarters_vehicles();
void done_quarters_soldiers();
void done_airfield();
void done_helipad();
void done_mine();
void done_production();
void done_lab();
void done_turret();
void done_shuttle();
void done_silos();
void done_relay();
void done_port();

namespace detail {
} // namespace detail

} // namespace mh::sim
