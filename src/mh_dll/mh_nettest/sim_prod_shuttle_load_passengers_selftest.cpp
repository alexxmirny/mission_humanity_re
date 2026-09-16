#include "sim/sim_prod_shuttle_load_passengers.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct ev2 {
    int32_t a, b; // (player, count)
};

// One recorder for the single outward call (population_remove) -- same captureless-lambda-to-function-
// pointer shape as sim_prod_completion_selftest.cpp's g_pc/g_da/g_sa.
struct lp_recorder {
    std::vector<ev2> population_remove;
    void             reset() { *this = lp_recorder{}; }
};
lp_recorder g_lp;

const prod_shuttle_load_passengers_calls &rec_lp_calls() {
    static const prod_shuttle_load_passengers_calls c = {
        [](uint32_t player, int32_t count) {
            g_lp.population_remove.push_back({(int32_t)player, count});
        },
    };
    return c;
}

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)slot];
}

// ==== the human_transport==0 short-circuit (0x0048e64a-0x0048e657) =================================
// No transport capacity at all -- return 0 immediately, no slot record ever touched (not even the
// avail-net-of-reserved read), no population_remove call.
void test_human_transport_zero_short_circuits() {
    sim_fixture f;
    g_lp.reset();
    constexpr uint16_t PLAYER = 2;
    constexpr int32_t  BLDG   = 6;
    constexpr int32_t  SLOT   = 3;

    f.b(PLAYER, BLDG).shuttle_slot               = SLOT;
    f.b(PLAYER, BLDG).building_id                = 40;
    f.cfg_buildings[40].human_transport          = 0;  // the short-circuit trigger
    slot_of(f, PLAYER, SLOT).passengers_reserved = 55; // must survive untouched -- the slot is never read
    f.population[PLAYER].human                   = 77; // must survive untouched -- no population_remove call

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_shuttle_load_passengers(v, own, rec_lp_calls(), PLAYER, BLDG, 999);

    ck_eq(r, 0u, "human_transport==0: returns 0");
    ck_eq((uint32_t)g_lp.population_remove.size(), 0u,
          "human_transport==0: population_remove NOT called");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 55u,
          "human_transport==0: passengers_reserved untouched (slot never read on this path)");
    ck_eq((uint32_t)f.population[PLAYER].human, 77u,
          "human_transport==0: population.human untouched");
}

// ==== clamp stage 1: avail-net-of-reserved is the binding constraint (0x0048e65c-0x0048e675) =======
// avail = human_transport - passengers_reserved. Set this to the smallest of the three limits so it
// alone binds: transport_cap(50) - reserved(47) = 3, well under both population(1000) and cap(5000).
void test_clamp_avail_net_of_reserved_binds() {
    sim_fixture f;
    g_lp.reset();
    constexpr uint16_t PLAYER = 4;
    constexpr int32_t  BLDG   = 8;
    constexpr int32_t  SLOT   = 6;

    f.b(PLAYER, BLDG).shuttle_slot               = SLOT;
    f.b(PLAYER, BLDG).building_id                = 41;
    f.cfg_buildings[41].human_transport          = 50;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 47;   // avail = 50 - 47 = 3
    f.population[PLAYER].human                   = 1000; // not binding (3 < 1000)

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_shuttle_load_passengers(v, own, rec_lp_calls(), PLAYER, BLDG, 5000 /* not binding */);

    ck_eq(r, 3u, "avail-net-of-reserved binds: returns 3 (50-47)");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 50u,
          "avail-net-of-reserved binds: passengers_reserved bumped 47 -> 50 (+avail)");
    ck((g_lp.population_remove.size() == 1 && g_lp.population_remove[0].a == PLAYER &&
        g_lp.population_remove[0].b == 3),
       "avail-net-of-reserved binds: population_remove(player=4, count=3)");
    ck_eq((uint32_t)f.population[PLAYER].human, 1000u,
          "avail-net-of-reserved binds: population.human field itself untouched directly -- the decrement "
          "happens only via the outward population_remove call");
}

// ==== clamp stage 2: population is the binding constraint (0x0048e678-0x0048e69a) ===================
// avail starts huge (transport_cap=2000, reserved=0), then gets clamped down to the small idle
// population(8) -- well under the untruncated cap(6000).
void test_clamp_population_binds() {
    sim_fixture f;
    g_lp.reset();
    constexpr uint16_t PLAYER = 5;
    constexpr int32_t  BLDG   = 9;
    constexpr int32_t  SLOT   = 1;

    f.b(PLAYER, BLDG).shuttle_slot               = SLOT;
    f.b(PLAYER, BLDG).building_id                = 42;
    f.cfg_buildings[42].human_transport          = 2000; // huge -- not the binding limit
    slot_of(f, PLAYER, SLOT).passengers_reserved = 0;    // avail starts at 2000
    f.population[PLAYER].human                   = 8;    // the binding limit

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_shuttle_load_passengers(v, own, rec_lp_calls(), PLAYER, BLDG, 6000 /* not binding */);

    ck_eq(r, 8u, "population binds: returns 8 (idle population, well under transport_cap and cap)");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 8u,
          "population binds: passengers_reserved bumped 0 -> 8");
    ck((g_lp.population_remove.size() == 1 && g_lp.population_remove[0].a == PLAYER &&
        g_lp.population_remove[0].b == 8),
       "population binds: population_remove(player=5, count=8)");
}

// ==== clamp stage 3: the requested cap is the binding constraint (0x0048e69a-0x0048e6aa) ============
// avail after the first two clamps is 800 (transport_cap=900, reserved=100, human=850 -- both well
// above 800), then the small requested cap(37) clamps it down further.
void test_clamp_requested_cap_binds() {
    sim_fixture f;
    g_lp.reset();
    constexpr uint16_t PLAYER = 6;
    constexpr int32_t  BLDG   = 2;
    constexpr int32_t  SLOT   = 8;

    f.b(PLAYER, BLDG).shuttle_slot               = SLOT;
    f.b(PLAYER, BLDG).building_id                = 43;
    f.cfg_buildings[43].human_transport          = 900;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 100; // avail after stage 1 = 800
    f.population[PLAYER].human                   = 850; // not binding (800 <= 850, stage 2 no-op)

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_shuttle_load_passengers(v, own, rec_lp_calls(), PLAYER, BLDG, 37);

    ck_eq(r, 37u, "requested cap binds: returns 37 (the smallest of 800/850/37)");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 137u,
          "requested cap binds: passengers_reserved bumped 100 -> 137 (+37)");
    ck((g_lp.population_remove.size() == 1 && g_lp.population_remove[0].a == PLAYER &&
        g_lp.population_remove[0].b == 37),
       "requested cap binds: population_remove(player=6, count=37)");
}

// ==== avail==0 returns 0 without calling population_remove or bumping the slot (0x0048e6aa) ========
// transport_cap == passengers_reserved exactly -> avail = 0. Nothing embarked, no side effects at all.
void test_avail_zero_no_population_remove_no_bump() {
    sim_fixture f;
    g_lp.reset();
    constexpr uint16_t PLAYER = 1;
    constexpr int32_t  BLDG   = 3;
    constexpr int32_t  SLOT   = 2;

    f.b(PLAYER, BLDG).shuttle_slot               = SLOT;
    f.b(PLAYER, BLDG).building_id                = 44;
    f.cfg_buildings[44].human_transport          = 300;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 300; // avail = 300 - 300 = 0
    f.population[PLAYER].human                   = 500; // irrelevant -- avail is already 0

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_shuttle_load_passengers(v, own, rec_lp_calls(), PLAYER, BLDG, 100);

    ck_eq(r, 0u, "avail==0: returns 0");
    ck_eq((uint32_t)g_lp.population_remove.size(), 0u, "avail==0: population_remove NOT called");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 300u,
          "avail==0: passengers_reserved NOT bumped -- stays exactly at the pre-existing reservation");
}

// ==== the `cap` parameter is masked to its low 16 bits at the one point of use (0x0048e69a-0x0048e6a7)
// cap = 0x00230014 -- upper 16 bits (0x0023) are pure garbage, low 16 bits are 0x0014 = 20. If the
// translation used the full 32-bit value as a signed int (a truncation bug), 0x00230014 (2,293,780)
// would NOT bind against avail=900, and the result would incorrectly be 900. The header banner (see
// "THE `cap` PARAMETER IS TRUNCATED TO 16 BITS AT ITS ONLY USE") says the ONLY comparison reads just
// the low 16 bits, so the correct result is 20.
void test_cap_truncated_to_16_bits_at_point_of_use() {
    sim_fixture f;
    g_lp.reset();
    constexpr uint16_t PLAYER = 3;
    constexpr int32_t  BLDG   = 1;
    constexpr int32_t  SLOT   = 5;

    f.b(PLAYER, BLDG).shuttle_slot               = SLOT;
    f.b(PLAYER, BLDG).building_id                = 45;
    f.cfg_buildings[45].human_transport          = 1000;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 0;   // avail after stage 1 = 1000
    f.population[PLAYER].human                   = 900; // avail after stage 2 = 900 (binding, before cap)

    constexpr uint32_t CAP_DIRTY_HIGH_BITS = 0x00230014u; // low16 = 0x14 = 20

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_shuttle_load_passengers(v, own, rec_lp_calls(), PLAYER, BLDG, CAP_DIRTY_HIGH_BITS);

    ck_eq(r, 20u,
          "cap dirty high bits: only the low 16 bits (20) are compared -- NOT the full 32-bit value "
          "(2293780), which would not have clamped avail(900) at all");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 20u,
          "cap dirty high bits: passengers_reserved bumped 0 -> 20, matching the truncated cap");
    ck((g_lp.population_remove.size() == 1 && g_lp.population_remove[0].a == PLAYER &&
        g_lp.population_remove[0].b == 20),
       "cap dirty high bits: population_remove(player=3, count=20)");
}

} // namespace

void run_prod_shuttle_load_passengers_tests() {
    test_human_transport_zero_short_circuits();
    test_clamp_avail_net_of_reserved_binds();
    test_clamp_population_binds();
    test_clamp_requested_cap_binds();
    test_avail_zero_no_population_remove_no_bump();
    test_cap_truncated_to_16_bits_at_point_of_use();
}

} // namespace mh::sim::test
