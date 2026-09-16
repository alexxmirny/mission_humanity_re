#include "sim/sim_unit_apply_production_completion.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-default, non-symmetric player/proto ids -- kept apart per test so a swapped-argument
// translation cannot pass by accident. MAX_PLAYERS==8, so every player id here stays 0..7; cfg_units
// is sized 100 in the fixture, so every unit_proto_id here stays well under that.
constexpr uint32_t PLAYER       = 3;
constexpr uint32_t PLAYER_OTHER = 6;

// ---- recorder --------------------------------------------------------------------------------------
// Captureless lambdas convert to the three plain function pointers `apply_production_completion_calls`
// declares -- same technique sim_prod_completion_selftest.cpp's own recorders use.
struct ev2 {
    int32_t a, b;
};
struct ev3 {
    int32_t a, b, c;
};

struct apc_recorder {
    std::vector<ev3> resource_add;         // (player, resource_id, val), in call order
    std::vector<ev2> population_add;       // (player, count)
    std::vector<ev2> housing_count_remove; // (player, unit_proto_id)
    void             reset() { *this = apc_recorder{}; }
};
apc_recorder g_apc;

const apply_production_completion_calls &rec_calls() {
    static const apply_production_completion_calls c = {
        [](int32_t player, int32_t resource_id, int32_t amount) {
            g_apc.resource_add.push_back({player, resource_id, amount});
        },
        [](uint16_t player, int32_t count) {
            g_apc.population_add.push_back({(int32_t)player, count});
        },
        [](int32_t player, int32_t unit_proto_id) {
            g_apc.housing_count_remove.push_back({player, unit_proto_id});
        },
    };
    return c;
}

// ==== the resource-cost loop, id-then-bound order (0x00492c15-0x00492c62) ==========================

// A sentinel at slot 0 must stop the walk before a single resource_add fires -- and before slot 0's
// OWN `.val` is even read (the id check breaks first, per the header's ordering).
void test_resource_loop_sentinel_at_slot_zero_stops_immediately() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO            = 11;
    f.cfg_units[PROTO].resource[0].id  = 0;     // UNDEFINED sentinel, slot 0
    f.cfg_units[PROTO].resource[0].val = 12345; // must never be read -- id check breaks first
    f.cfg_units[PROTO].soldier_count   = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)g_apc.resource_add.size(), 0u,
          "loop: id==0 sentinel at slot 0 -> zero resource_add calls");
    ck_eq((uint32_t)g_apc.housing_count_remove.size(), 1u,
          "loop-empty case: housing release still fires (unconditional)");
}

// A sentinel MID-list must stop the walk exactly there, in order, without touching entries past it --
// even when a later slot (4) is deliberately seeded non-zero, proving the break is real and not just
// "ran out of non-zero data".
void test_resource_loop_sentinel_mid_list_stops_and_preserves_order() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO            = 22;
    f.cfg_units[PROTO].resource[0]     = {5, 17};
    f.cfg_units[PROTO].resource[1]     = {9, 23};
    f.cfg_units[PROTO].resource[2]     = {13, 31};
    f.cfg_units[PROTO].resource[3].id  = 0;   // sentinel -- stop here
    f.cfg_units[PROTO].resource[3].val = 909; // must never be read
    f.cfg_units[PROTO].resource[4].id  = 41;  // must never be VISITED -- the walk already broke at 3
    f.cfg_units[PROTO].resource[4].val = 43;
    f.cfg_units[PROTO].soldier_count   = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)g_apc.resource_add.size(), 3u,
          "loop: sentinel at slot 3 -> exactly 3 resource_add calls (slots 0,1,2)");
    ck(g_apc.resource_add.size() == 3 && g_apc.resource_add[0].a == (int32_t)PLAYER &&
           g_apc.resource_add[0].b == 5 && g_apc.resource_add[0].c == 17,
       "loop[0]: resource_add(player, id=5, val=17)");
    ck(g_apc.resource_add.size() == 3 && g_apc.resource_add[1].a == (int32_t)PLAYER &&
           g_apc.resource_add[1].b == 9 && g_apc.resource_add[1].c == 23,
       "loop[1]: resource_add(player, id=9, val=23)");
    ck(g_apc.resource_add.size() == 3 && g_apc.resource_add[2].a == (int32_t)PLAYER &&
           g_apc.resource_add[2].b == 13 && g_apc.resource_add[2].c == 31,
       "loop[2]: resource_add(player, id=13, val=31)");
}

// FINDING (matching sim_unit_refund_selftest.cpp's own precedent for the identical cfg_units/resource
// shape): the id READ happens BEFORE the i<CFG_RESOURCE_SLOTS(7) bound check, so seven fully-populated
// slots refund exactly seven -- the walk does NOT stop because it luckily hit a zero id at slot 7, it
// stops because the BOUND check (evaluated second, 0x00492c33/0x00492c37) fails. `resource_2[0]` --
// the exact memory `resource[7]` lands on once the array's declared 7 slots are exhausted (0x1a7 + 7*8
// == 0x1df == offsetof(resource_2), per the header's own address arithmetic) -- is seeded NON-ZERO on
// purpose: an 8th resource_add must NOT fire even though the id read at the OOB slot is non-zero,
// proving the bound (not a sentinel) is what stops it.
void test_resource_loop_seven_full_slots_stopped_by_bound_not_sentinel() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO   = 33;
    const int32_t     ids[7]  = {61, 62, 63, 64, 65, 66, 67};
    const int32_t     vals[7] = {71, 79, 83, 89, 97, 101, 103};
    for (int32_t i = 0; i < 7; ++i) f.cfg_units[PROTO].resource[i] = {(uint32_t)ids[i], vals[i]};
    // The resource[7] over-read target: non-zero on purpose (see comment above).
    f.cfg_units[PROTO].resource_2[0].id  = 0x4321;
    f.cfg_units[PROTO].resource_2[0].val = 999999;
    f.cfg_units[PROTO].soldier_count     = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)g_apc.resource_add.size(), 7u,
          "loop FINDING: seven full slots -> exactly seven resource_add calls (the i<7 bound stops the "
          "walk, NOT a zero id happening to land at the resource_2[0] over-read)");
    for (int32_t i = 0; i < 7; ++i) {
        ck(g_apc.resource_add[(size_t)i].a == (int32_t)PLAYER && g_apc.resource_add[(size_t)i].b == ids[i] &&
               g_apc.resource_add[(size_t)i].c == vals[i],
           "loop[i]: resource_add(player, id, val) matches slot i in order");
    }
}

// ==== the population-add gate, soldier_count != 0 (0x00492c64-0x00492c86) ==========================

void test_population_add_fires_when_soldier_count_nonzero() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 44;
    f.cfg_units[PROTO].resource[0].id = 0;  // keep the loop quiet -- this test is about the pop gate only
    f.cfg_units[PROTO].soldier_count  = 37; // distinct non-default, non-symmetric with PLAYER(3)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)g_apc.population_add.size(), 1u,
          "pop gate: soldier_count(37) != 0 -> population_add called exactly once");
    ck(g_apc.population_add.size() == 1 && g_apc.population_add[0].a == (int32_t)PLAYER &&
           g_apc.population_add[0].b == 37,
       "pop gate: population_add(player, count=cfg_units[proto].soldier_count=37)");
}

void test_population_add_skipped_when_soldier_count_zero() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 55;
    f.cfg_units[PROTO].resource[0].id = 0;
    f.cfg_units[PROTO].soldier_count  = 0; // gate closed -- no else branch per the header

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)g_apc.population_add.size(), 0u,
          "pop gate: soldier_count==0 -> population_add NOT called (falls straight through)");
}

// ==== the housing release, unconditional (0x00492c86-0x00492c92) ===================================

void test_housing_release_unconditional_even_when_every_other_branch_is_quiet() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 66;
    f.cfg_units[PROTO].resource[0].id = 0; // loop quiet
    f.cfg_units[PROTO].soldier_count  = 0; // pop gate quiet

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)g_apc.housing_count_remove.size(), 1u,
          "housing release: fires exactly once regardless of the loop/pop-gate outcomes");
    ck(g_apc.housing_count_remove.size() == 1 && g_apc.housing_count_remove[0].a == (int32_t)PLAYER &&
           g_apc.housing_count_remove[0].b == PROTO,
       "housing release: housing_count_remove(player=truncated-16-bit, unit_proto_id=FULL, no "
       "truncation) -- see sim_unit_housing_count.h's own (player, unit_proto_id) role pinning");
}

// ==== the header-row decrement (0x00492c92-0x00492ca3), CORRECTED reading ==========================
// units[player][0].order -= 1 via a bare 16-bit `DEC`, NOT the Ghidra draft's symbolic-pointer-
// subtraction misreading. Three properties below jointly rule the draft's reading back out: the value
// actually decreases by 1, ONLY the target player's slot-0 record is touched, and it wraps like a real
// unsigned DEC (0 -> 0xffff) rather than saturating or leaving the field untouched.

void test_header_row_decrement_basic_minus_one() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 12;
    f.cfg_units[PROTO].resource[0].id = 0;
    f.cfg_units[PROTO].soldier_count  = 0;
    f.u(PLAYER, 0).order              = 250; // distinct, non-default

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)f.u(PLAYER, 0).order, 249u,
          "header-row decrement: units[player][0].order 250 -> 249 (plain -1, not a pointer-arithmetic "
          "no-op)");
}

void test_header_row_decrement_wraps_on_zero_like_a_bare_dec() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 13;
    f.cfg_units[PROTO].resource[0].id = 0;
    f.cfg_units[PROTO].soldier_count  = 0;
    f.u(PLAYER, 0).order              = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)f.u(PLAYER, 0).order, 0xffffu,
          "header-row decrement: order==0 wraps to 0xffff -- the bare x86 DEC's real unsigned-16 "
          "wraparound, which a symbolic 'subtract from the pointer' misreading could not reproduce");
}

void test_header_row_decrement_targets_only_that_players_slot_zero() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 14;
    f.cfg_units[PROTO].resource[0].id = 0;
    f.cfg_units[PROTO].soldier_count  = 0;
    f.u(PLAYER, 0).order              = 80; // WILL be decremented
    f.u(PLAYER, 1).order              = 80; // a real unit's own order -- must NOT be touched
    f.u(PLAYER_OTHER, 0).order        = 80; // a different player's header row -- must NOT be touched

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), PLAYER, PROTO);

    ck_eq((uint32_t)f.u(PLAYER, 0).order, 79u, "header-row decrement: units[player][0].order decremented");
    ck_eq((uint32_t)f.u(PLAYER, 1).order, 80u,
          "header-row decrement: units[player][1].order (a real unit slot, not the header row) untouched");
    ck_eq((uint32_t)f.u(PLAYER_OTHER, 0).order, 80u,
          "header-row decrement: a DIFFERENT player's own slot-0 header row untouched -- the row "
          "multiplier (UNITS_PER_PLAYER stride) uses ONLY `player`, matching the header's derivation");
}

// ==== regression: player is truncated to its low 16 bits at EVERY one of its four uses =============
// Same class as sim_prod_completion_selftest.cpp's own test_pc_dirty_high_bits_masked_regression --
// garbage bits above bit 15 must be masked off before player reaches any of the three calls OR the
// header-row decrement's own index arithmetic (which would otherwise index wildly out of the fixture's
// `units` vector and be caught by ASan, so a pass here is a real, load-bearing check).
void test_player_dirty_high_bits_masked_at_all_four_use_sites() {
    sim_fixture f;
    g_apc.reset();
    constexpr int32_t PROTO           = 77;
    f.cfg_units[PROTO].resource[0]    = {91, 93}; // exactly one resource_add to observe the masked arg on
    f.cfg_units[PROTO].resource[1].id = 0;
    f.cfg_units[PROTO].soldier_count  = 17;
    f.u(PLAYER, 0).order              = 55; // will be decremented to 54 at the MASKED player's slot

    const uint32_t player_dirty = 0x00040000u | PLAYER; // low16 == PLAYER(3), garbage ONLY above bit 15

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_apply_production_completion(v, own, rec_calls(), player_dirty, PROTO);

    ck(g_apc.resource_add.size() == 1 && g_apc.resource_add[0].a == (int32_t)PLAYER,
       "dirty player bits: resource_add's player arg masked to low 16 bits");
    ck(g_apc.population_add.size() == 1 && g_apc.population_add[0].a == (int32_t)PLAYER,
       "dirty player bits: population_add's player arg masked to low 16 bits");
    ck(g_apc.housing_count_remove.size() == 1 && g_apc.housing_count_remove[0].a == (int32_t)PLAYER,
       "dirty player bits: housing_count_remove's player arg masked to low 16 bits");
    ck_eq((uint32_t)f.u(PLAYER, 0).order, 54u,
          "dirty player bits: the header-row decrement lands on the MASKED player's slot 0 (55 -> 54), "
          "not on whatever units[player_dirty][0] would resolve to");
}

} // namespace

void run_unit_apply_production_completion_tests() {
    test_resource_loop_sentinel_at_slot_zero_stops_immediately();
    test_resource_loop_sentinel_mid_list_stops_and_preserves_order();
    test_resource_loop_seven_full_slots_stopped_by_bound_not_sentinel();

    test_population_add_fires_when_soldier_count_nonzero();
    test_population_add_skipped_when_soldier_count_zero();

    test_housing_release_unconditional_even_when_every_other_branch_is_quiet();

    test_header_row_decrement_basic_minus_one();
    test_header_row_decrement_wraps_on_zero_like_a_bare_dec();
    test_header_row_decrement_targets_only_that_players_slot_zero();

    test_player_dirty_high_bits_masked_at_all_four_use_sites();
}

} // namespace mh::sim::test
