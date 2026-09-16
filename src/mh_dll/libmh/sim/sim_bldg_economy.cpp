//
// sim/sim_bldg_economy.cpp -- see sim_bldg_economy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_{add_storage_capacity,power_generate,add_population_housing,power_consume,
// add_unit_capacity_vehicles,add_unit_capacity_soldiers}_*.asm), cross-checked against Ghidra's own
// already-typed decompile (all six are already fully RE'd with named fields -- see the header).
//
#include "sim/sim_bldg_economy.h"

#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_SHUTTLE/_H_SHUTTLE
#include "fp/x87.h"                // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

namespace {

// utils_math_trunc @0x004d0596 (ST0-in/ST0-out, x87-register-only -- not stack-passable, hence not a
// mh::call:: stub). Every call site in this file is an ORDINARY call, not compiler-inlined -- reproduced
// as this TU's own copy of the sim_bldg_completion_dispatch.cpp / sim_bldg_mother_reelect_primary.cpp /
// sim_bldg_power_network_recompute.cpp precedent's exact instruction sequence (each TU keeps its own
// copy; this is not a new shared helper -- see the header). FISTP width confirmed 32-bit at every call
// site in this file (opcode `db5de8`/`db98c04fbf00`, ModRM reg field 3 -> DB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

// llm_strat_add_storage_capacity @0x004701bf.
void add_storage_capacity(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb    = v.cfg_buildings[b.building_id];
    storage_stats      &stats = own.storage_stats_at(*v.cur_player);
    for (int32_t d = 0; d < 10; ++d) stats.cap_accum[d] += cb.capacity[d];

    if (b.shuttle_slot != 0) {
        const prod_shuttle_slot &slot =
            v.prod_shuttle_slots[*v.cur_player * PROD_SHUTTLE_SLOTS_PER_PLAYER + b.shuttle_slot];
        for (int32_t d = 0; d < 10; ++d) stats.cap_accum[d] -= slot.resources_reserved[d];
    }
}

// llm_strat_power_generate @0x004702af. Read-modify-write of power_stats[player].generated -- see
// header for the extraout_EAX store-target derivation (already settled).
void power_generate(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb    = v.cfg_buildings[b.building_id];
    power_stats        &stats = own.power_stats_at(*v.cur_player);
    stats.generated           = trunc_to_int32((double)cb.electric_power * b.efficiency + (double)stats.generated);
}

// llm_strat_add_population_housing @0x00470323. Three writes, then a shuttle-transfer exclusion -- see
// header for the full derivation (colony_hp_sum uses the BUILDING's own energy, colony_hp_max_sum uses
// the CFG max energy -- two independent values, not a duplicate).
void add_population_housing(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb    = v.cfg_buildings[b.building_id];
    pop_stats          &stats = own.population_at(*v.cur_player);

    stats.housing_accum += cb.worker_count;
    stats.colony_hp_sum += trunc_to_int32(b.energy);
    stats.colony_hp_max_sum += trunc_to_int32(cb.energy);

    if (b.shuttle_slot != 0 && cb.type != BUILDING_TYPE_A_SHUTTLE && cb.type != BUILDING_TYPE_H_SHUTTLE) {
        const prod_shuttle_slot &slot =
            v.prod_shuttle_slots[*v.cur_player * PROD_SHUTTLE_SLOTS_PER_PLAYER + b.shuttle_slot];
        stats.housing_accum -= slot.passengers_reserved;
    }
}

// llm_strat_power_consume @0x00470446. UNCONDITIONAL -- no online_state gate (confirmed absent from
// the .asm, unlike every other function in this file).
void power_consume(const sim_view &v, sim_store &own) {
    const building     &b  = own.cur_building();
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    own.power_stats_at(*v.cur_player).consumed += cb.electric_power;
}

// llm_strat_add_unit_capacity_vehicles @0x0047048e. See the header's plate-fix note: this feeds
// cap_accum_vehicles (the vehicle/"GARAGE" quarters, cfg type 7/0x1b), not barracks.
void add_unit_capacity_vehicles(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb = v.cfg_buildings[b.building_id];
    own.unit_housing_at(*v.cur_player).cap_accum_vehicles += cb.unit_housing_capacity;
}

// llm_strat_add_unit_capacity_soldiers @0x004704e2. Feeds cap_accum_soldiers (the soldier/"BARRACKS"
// quarters, cfg type 8/0x1c).
void add_unit_capacity_soldiers(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb = v.cfg_buildings[b.building_id];
    own.unit_housing_at(*v.cur_player).cap_accum_soldiers += cb.unit_housing_capacity;
}

// llm_strat_add_unit_capacity_planes @0x00470536. Byte-for-byte the same shape as _vehicles/_soldiers
// (see header) -- feeds cap_accum_planes (the airfield).
void add_unit_capacity_planes(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb = v.cfg_buildings[b.building_id];
    own.unit_housing_at(*v.cur_player).cap_accum_planes += cb.unit_housing_capacity;
}

// llm_strat_add_unit_capacity_helis @0x0047058a. Byte-for-byte the same shape as _vehicles/_soldiers
// (see header) -- feeds cap_accum_helis (the helipad).
void add_unit_capacity_helis(const sim_view &v, sim_store &own) {
    building &b = own.cur_building();
    if (b.online_state == 0) return;

    const cfg_building &cb = v.cfg_buildings[b.building_id];
    own.unit_housing_at(*v.cur_player).cap_accum_helis += cb.unit_housing_capacity;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void add_storage_capacity() {
    sim_state st = state();
    detail::add_storage_capacity(st.read, st.own);
}
void power_generate() {
    sim_state st = state();
    detail::power_generate(st.read, st.own);
}
void add_population_housing() {
    sim_state st = state();
    detail::add_population_housing(st.read, st.own);
}
void power_consume() {
    sim_state st = state();
    detail::power_consume(st.read, st.own);
}
void add_unit_capacity_vehicles() {
    sim_state st = state();
    detail::add_unit_capacity_vehicles(st.read, st.own);
}
void add_unit_capacity_soldiers() {
    sim_state st = state();
    detail::add_unit_capacity_soldiers(st.read, st.own);
}
void add_unit_capacity_planes() {
    sim_state st = state();
    detail::add_unit_capacity_planes(st.read, st.own);
}
void add_unit_capacity_helis() {
    sim_state st = state();
    detail::add_unit_capacity_helis(st.read, st.own);
}


} // namespace mh::sim
