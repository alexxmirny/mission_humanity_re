#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

void add_storage_capacity(const sim_view &v, sim_store &own);
void power_generate(const sim_view &v, sim_store &own);
void add_population_housing(const sim_view &v, sim_store &own);
void power_consume(const sim_view &v, sim_store &own);
void add_unit_capacity_vehicles(const sim_view &v, sim_store &own);
void add_unit_capacity_soldiers(const sim_view &v, sim_store &own);
void add_unit_capacity_planes(const sim_view &v, sim_store &own);
void add_unit_capacity_helis(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers -- match each original's committed void(void) prototype exactly.
void add_storage_capacity();
void power_generate();
void add_population_housing();
void power_consume();
void add_unit_capacity_vehicles();
void add_unit_capacity_soldiers();
void add_unit_capacity_planes();
void add_unit_capacity_helis();

namespace detail {
} // namespace detail

} // namespace mh::sim
