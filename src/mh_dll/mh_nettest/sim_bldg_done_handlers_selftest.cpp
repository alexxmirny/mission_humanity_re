#include "sim/sim_bldg_done_handlers.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER = 3;

void seed_stats(sim_fixture &fx) {
    pop_stats &pop       = fx.population[PLAYER];
    pop.pop_total        = 111;
    pop.workers_employed = 22;
    pop.human_in_field   = 33;
    pop.human            = 44;
    pop.housing_prev     = 55;
    pop.housing_accum    = 66;

    power_stats &pw   = fx.power_stats_rows[PLAYER];
    pw.generated      = 771;
    pw.consumed       = 882;
    pw.prev_generated = 993;

    storage_stats &st = fx.storage_stats_rows[PLAYER];
    for (int i = 0; i < 10; ++i) {
        st.cap_accum[i] = 1000 + i;
        st.cap_prev[i]  = 2000 + i;
    }

    housing_stats &h     = fx.unit_housing[PLAYER];
    h.used_vehicles      = 5;
    h.used_soldiers      = 6;
    h.used_planes        = 7;
    h.used_helis         = 8;
    h.cap_prev_vehicles  = 15;
    h.cap_prev_soldiers  = 16;
    h.cap_prev_planes    = 17;
    h.cap_prev_helis     = 18;
    h.cap_accum_vehicles = 25;
    h.cap_accum_soldiers = 26;
}

// ---- llm_strat_done_port's mock `bldg_done_calls` -- records COUNT per callee so the test can prove
// add_storage_capacity fires EXACTLY once and none of its 7 siblings fires at all (call PRESENCE +
// EXCLUSIVITY, the entire observable behaviour of a straight-line, no-branch body).
struct done_port_call_log {
    int  add_population_housing_calls     = 0;
    int  power_generate_calls             = 0;
    int  add_storage_capacity_calls       = 0;
    int  power_consume_calls              = 0;
    int  add_unit_capacity_soldiers_calls = 0;
    int  add_unit_capacity_vehicles_calls = 0;
    int  add_unit_capacity_planes_calls   = 0;
    int  add_unit_capacity_helis_calls    = 0;
    void reset() { *this = done_port_call_log{}; }
};
done_port_call_log g_done_port_log;

const bldg_done_calls &mock_done_port_calls() {
    static const bldg_done_calls c = {
        []() { g_done_port_log.add_population_housing_calls++; },
        []() { g_done_port_log.power_generate_calls++; },
        []() { g_done_port_log.add_storage_capacity_calls++; },
        []() { g_done_port_log.power_consume_calls++; },
        []() { g_done_port_log.add_unit_capacity_soldiers_calls++; },
        []() { g_done_port_log.add_unit_capacity_vehicles_calls++; },
        []() { g_done_port_log.add_unit_capacity_planes_calls++; },
        []() { g_done_port_log.add_unit_capacity_helis_calls++; },
    };
    return c;
}

} // namespace

void run_bldg_done_handlers_tests() {
    sim_fixture fx;
    fx.reset();
    seed_stats(fx);

    const pop_stats     pop_before = fx.population[PLAYER];
    const power_stats   pw_before  = fx.power_stats_rows[PLAYER];
    const storage_stats st_before  = fx.storage_stats_rows[PLAYER];
    const housing_stats h_before   = fx.unit_housing[PLAYER];

    // T1 -- llm_strat_done_default @0x0046ff2c: no arguments, no return, no store/view of any kind
    // (see sim_bldg_done_handlers.h's `void done_default();` -- unlike its 11 siblings it takes no
    // `bldg_done_calls` either, because the disassembly shows no CALL besides the inert prologue).
    mh::sim::detail::done_default();

    ck(std::memcmp(&fx.population[PLAYER], &pop_before, sizeof(pop_stats)) == 0,
       "done_default@0x0046ff2c: the seeded player's pop_stats block is byte-identical after the "
       "call -- the whole body besides the inert prologue is PUSH/POP/RET, no write of any kind");
    ck(std::memcmp(&fx.power_stats_rows[PLAYER], &pw_before, sizeof(power_stats)) == 0,
       "done_default@0x0046ff2c: the seeded player's power_stats block is byte-identical after the "
       "call");
    ck(std::memcmp(&fx.storage_stats_rows[PLAYER], &st_before, sizeof(storage_stats)) == 0,
       "done_default@0x0046ff2c: the seeded player's storage_stats block is byte-identical after the "
       "call");
    ck(std::memcmp(&fx.unit_housing[PLAYER], &h_before, sizeof(housing_stats)) == 0,
       "done_default@0x0046ff2c: the seeded player's unit_housing_stats block is byte-identical "
       "after the call");

    // T2 -- llm_strat_done_shuttle @0x0047014f: no arguments, no return, no store/view of any kind --
    // same class as done_default (BLDG_DONE_FUNCS table's "no completion bonus" default installed for
    // the shuttle building type too). Re-seed distinct sentinels first: done_default's call above
    // wrote nothing, so the blocks still hold pop_before/pw_before/etc, but re-seed explicitly so this
    // case does not silently depend on that.
    seed_stats(fx);
    const pop_stats     pop_before2 = fx.population[PLAYER];
    const power_stats   pw_before2  = fx.power_stats_rows[PLAYER];
    const storage_stats st_before2  = fx.storage_stats_rows[PLAYER];
    const housing_stats h_before2   = fx.unit_housing[PLAYER];

    mh::sim::detail::done_shuttle();

    ck(std::memcmp(&fx.population[PLAYER], &pop_before2, sizeof(pop_stats)) == 0,
       "done_shuttle@0x0047014f: the seeded player's pop_stats block is byte-identical after the "
       "call -- the whole body besides the inert prologue is PUSH/POP/RET, no write of any kind");
    ck(std::memcmp(&fx.power_stats_rows[PLAYER], &pw_before2, sizeof(power_stats)) == 0,
       "done_shuttle@0x0047014f: the seeded player's power_stats block is byte-identical after the "
       "call");
    ck(std::memcmp(&fx.storage_stats_rows[PLAYER], &st_before2, sizeof(storage_stats)) == 0,
       "done_shuttle@0x0047014f: the seeded player's storage_stats block is byte-identical after the "
       "call");
    ck(std::memcmp(&fx.unit_housing[PLAYER], &h_before2, sizeof(housing_stats)) == 0,
       "done_shuttle@0x0047014f: the seeded player's unit_housing_stats block is byte-identical "
       "after the call");

    // T3 (verification-debt drain) -- llm_strat_done_port @0x00470198:
    // add_storage_capacity() ONLY (0x004701b0). Shadow-armed but 0 rig calls across two soak runs (see
    // the file banner) -- this oracle is the evidence until a scenario reaches it for real, so it is
    // graded T2 in the ledger, not T1, per the "never round up" rule (the rig might still reach it).
    g_done_port_log.reset();
    mh::sim::detail::done_port(mock_done_port_calls());
    ck_eq((uint32_t)g_done_port_log.add_storage_capacity_calls, 1u,
          "done_port@0x00470198: add_storage_capacity called exactly once @0x004701b0");
    ck_eq((uint32_t)g_done_port_log.add_population_housing_calls, 0u,
          "done_port: add_population_housing NOT called");
    ck_eq((uint32_t)g_done_port_log.power_generate_calls, 0u, "done_port: power_generate NOT called");
    ck_eq((uint32_t)g_done_port_log.power_consume_calls, 0u, "done_port: power_consume NOT called");
    ck_eq((uint32_t)g_done_port_log.add_unit_capacity_soldiers_calls, 0u,
          "done_port: add_unit_capacity_soldiers NOT called");
    ck_eq((uint32_t)g_done_port_log.add_unit_capacity_vehicles_calls, 0u,
          "done_port: add_unit_capacity_vehicles NOT called");
    ck_eq((uint32_t)g_done_port_log.add_unit_capacity_planes_calls, 0u,
          "done_port: add_unit_capacity_planes NOT called");
    ck_eq((uint32_t)g_done_port_log.add_unit_capacity_helis_calls, 0u,
          "done_port: add_unit_capacity_helis NOT called");
}

} // namespace mh::sim::test
