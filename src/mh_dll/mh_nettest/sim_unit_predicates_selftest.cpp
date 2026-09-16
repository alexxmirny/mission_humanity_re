//
// sim_unit_predicates_selftest.cpp -- `simtest` cases for the nine zero-callee unit predicates
// (sim/sim_unit_state_predicates.{h,cpp}, sim/sim_unit_type_predicates.{h,cpp}), SIM1A.
//
// All nine are pure reads with no callees beyond the inert stack-capacity probe, so every case here
// drives detail:: directly over a fixture -- no recording-calls struct needed, unlike
// sim_unit_passive_engage_selftest.cpp.
//
#include "sim/sim_unit_state_predicates.h"
#include "sim/sim_unit_type_predicates.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- llm_strat_unit_attack_target_is_dead @0x004d4226 --------------------------------------------
void test_unit_attack_target_is_dead() {
    sim_fixture    f;
    const sim_view v = f.view();

    // ATTACK_UNIT (state == 0x1a): dead iff target.energy - target.pending_damage <= 0.0.
    f.u(2, 5).state          = 0x1a;
    f.u(2, 5).target_ref     = 3; // owner nibble 3, no building/unit-class bits set
    f.u(2, 5).target_index   = 7;
    f.u(3, 7).energy         = 10.0;
    f.u(3, 7).pending_damage = 4.0;
    ck(detail::unit_attack_target_is_dead(v, 2, 5) == 0,
       "attack_target_is_dead: ATTACK_UNIT, energy(10) - pending(4) > 0 -> alive (0)");

    f.u(3, 7).pending_damage = 10.0;
    ck(detail::unit_attack_target_is_dead(v, 2, 5) == 1,
       "attack_target_is_dead: ATTACK_UNIT, energy(10) - pending(10) == 0 -> dead (STRICT <=, not <)");

    f.u(3, 7).pending_damage = 20.0;
    ck(detail::unit_attack_target_is_dead(v, 2, 5) == 1,
       "attack_target_is_dead: ATTACK_UNIT, negative diff -> dead (1)");

    // ORDER field alone also selects the ATTACK_UNIT arm, when state does not.
    f.u(2, 5).state = 0;
    f.u(2, 5).order = 0x1a;
    ck(detail::unit_attack_target_is_dead(v, 2, 5) == 1,
       "attack_target_is_dead: .order == ATTACK_UNIT also selects the ATTACK_UNIT arm");

    // ATTACK_BUILDING (state == 0x1c): dead iff the target tile's occupancy `building` field is 0.
    f.u(4, 1).state         = 0x1c;
    f.u(4, 1).order         = 0;
    f.u(4, 1).target_fine_x = 32 * 10; // tile (10, 20), positive coords -- plain /32
    f.u(4, 1).target_fine_y = 32 * 20;
    f.t(10, 20).building    = 0;
    ck(detail::unit_attack_target_is_dead(v, 4, 1) == 1,
       "attack_target_is_dead: ATTACK_BUILDING, tile has no building -> dead (1)");
    f.t(10, 20).building = 77;
    ck(detail::unit_attack_target_is_dead(v, 4, 1) == 0,
       "attack_target_is_dead: ATTACK_BUILDING, tile has a building -> alive (0)");

    // Negative fine coords: truncating /32, matching the asm's SAR/SHL/SBB/SAR idiom (fine=-1 -> 0,
    // not floor(-1/32)=-1).
    f.u(4, 1).target_fine_x = -1;
    f.u(4, 1).target_fine_y = 0;
    f.t(0, 0).building      = 0;
    ck(detail::unit_attack_target_is_dead(v, 4, 1) == 1,
       "attack_target_is_dead: fine_x=-1 truncates to tile 0 (C's `/32`, not floor), no building -> dead");

    // Neither state nor order matches -> 0 ("not dead", nothing to abandon).
    f.u(5, 2).state = 5;
    f.u(5, 2).order = 6;
    ck(detail::unit_attack_target_is_dead(v, 5, 2) == 0,
       "attack_target_is_dead: neither state nor order is ATTACK_UNIT/ATTACK_BUILDING -> 0");
}

// ---- llm_strat_unit_state_is_in_transit @0x004d431e -----------------------------------------------
void test_unit_state_is_in_transit() {
    sim_fixture    f;
    const sim_view v = f.view();

    struct {
        uint16_t    val;
        bool        member;
        const char *why;
    } cases[] = {
        {0xa, true, "GROUP_MARSHAL"},
        {0xf, true, "MOVE_WALKER"},
        {0x11, true, "MOVE_PATH"},
        {0x12, true, "MOVE_PATH_12"},
        {0x20, true, "EXIT_STORAGE_BEGIN"},
        {0x22, true, "EXIT_WAIT"},
        {0x24, true, "ENTER_STORAGE_BEGIN"},
        {0x2b, true, "PARKED_2B"},
        {0xb, false, "GROUP_STEP (exclusive bound, not a member)"},
        {0x13, false, "IDLE_SCATTER (exclusive bound)"},
        {0x23, false, "EXIT_CANCEL (exclusive bound)"},
        {0x2c, false, "MOVE_PATH_PLANE (exclusive bound)"},
        {0, false, "0 is not in the set"},
    };
    for (auto &c : cases) {
        f.u(1, 1).state = c.val;
        f.u(1, 1).order = 0; // keep order out of the set so only .state is under test
        ck(detail::unit_state_is_in_transit(v, 1, 1) == (c.member ? 1 : 0),
           c.why);
    }

    // .order alone also selects membership, when .state does not.
    f.u(1, 1).state = 0;
    f.u(1, 1).order = 0xf; // MOVE_WALKER
    ck(detail::unit_state_is_in_transit(v, 1, 1) == 1,
       "state_is_in_transit: .order in the set also returns 1 (state checked first, order settles it)");
}

// ---- llm_strat_unit_is_idle_or_parked @0x004d4452 -------------------------------------------------
void test_unit_is_idle_or_parked() {
    sim_fixture    f;
    const sim_view v = f.view();

    struct {
        uint16_t    val;
        bool        member;
        const char *why;
    } cases[] = {
        {0x1f, true, "PARKED"},
        {0x22, true, "EXIT_WAIT"},
        {0x24, true, "ENTER_STORAGE_BEGIN"},
        {0x2b, true, "PARKED_2B"},
        {0x1e, false, "just below PARKED"},
        {0x23, false, "EXIT_CANCEL, the gap between the two ranges"},
        {0x2c, false, "just above PARKED_2B"},
    };
    for (auto &c : cases) {
        f.u(2, 2).state = c.val;
        f.u(2, 2).order = 0;
        ck(detail::unit_is_idle_or_parked(v, 2, 2) == (c.member ? 1 : 0), c.why);
    }

    // .order alone also selects membership -- this is the branch the exported .c draft's PLATE PROSE
    // describes wrong (see sim_unit_state_predicates.h); the CODE (and this test) follow the asm.
    f.u(2, 2).state = 0;
    f.u(2, 2).order = 0x24;
    ck(detail::unit_is_idle_or_parked(v, 2, 2) == 1,
       "is_idle_or_parked: .order in the set also returns 1 (OR over state/order, not AND)");
}

// ---- the five packed-ref type predicates + the plain accessor -------------------------------------
//
// All five share the same building-bit (0x40) and owner-nibble (0xf) convention on their first
// argument; one shared setup helper avoids repeating the ref-packing arithmetic five times.
void test_unit_type_predicates() {
    sim_fixture    f;
    const sim_view v = f.view();

    // is_aircraft: true for plane/heli/heli-mother/heli-cargo/heli-shuttle (both race variants).
    f.u(3, 9).unit_proto_id = 40;
    f.cfg_units[40].type    = UNIT_TYPE_A_PLANE;
    ck(detail::is_aircraft(v, /*unit_ref=*/3, 9) == 1, "is_aircraft: A_PLANE -> true");
    f.cfg_units[40].type = UNIT_TYPE_H_HELI_CARGO;
    ck(detail::is_aircraft(v, 3, 9) == 1, "is_aircraft: H_HELI_CARGO -> true");
    f.cfg_units[40].type = UNIT_TYPE_A_GROUND;
    ck(detail::is_aircraft(v, 3, 9) == 0, "is_aircraft: A_GROUND -> false");
    // The building bit (0x40) short-circuits before any roster/cfg read at all.
    ck(detail::is_aircraft(v, /*unit_ref=*/3 | 0x40, 9) == 0,
       "is_aircraft: building bit (0x40) set -> false immediately, no roster read");

    // is_plane: A_PLANE/H_PLANE only -- narrower than is_aircraft.
    f.cfg_units[40].type = UNIT_TYPE_A_PLANE;
    ck(detail::is_plane(v, 3, 9) == 1, "is_plane: A_PLANE -> true");
    f.cfg_units[40].type = UNIT_TYPE_A_HELI;
    ck(detail::is_plane(v, 3, 9) == 0, "is_plane: A_HELI -> false (is_plane is narrower than is_aircraft)");

    // is_heli: A_HELI/H_HELI only.
    f.cfg_units[40].type = UNIT_TYPE_H_HELI;
    ck(detail::is_heli(v, 3, 9) == 1, "is_heli: H_HELI -> true");
    f.cfg_units[40].type = UNIT_TYPE_A_HELI_MOTHER;
    ck(detail::is_heli(v, 3, 9) == 0, "is_heli: A_HELI_MOTHER -> false (is_heli excludes the mother variant)");

    // is_ground: A_GROUND/H_GROUND/A_WALKER/H_WALKER.
    f.cfg_units[40].type = UNIT_TYPE_A_WALKER;
    ck(detail::is_ground(v, 3, 9) == 1, "is_ground: A_WALKER -> true");
    f.cfg_units[40].type = UNIT_TYPE_H_GROUND;
    ck(detail::is_ground(v, 3, 9) == 1, "is_ground: H_GROUND -> true");
    f.cfg_units[40].type = UNIT_TYPE_A_PLANE;
    ck(detail::is_ground(v, 3, 9) == 0, "is_ground: A_PLANE -> false");

    // is_soldier: reads cfg_unit::ai_unit (a DIFFERENT field from .type).
    f.cfg_units[40].ai_unit = AI_UNIT_SOLDIER;
    f.cfg_units[40].type    = UNIT_TYPE_A_GROUND; // .type deliberately irrelevant to is_soldier
    ck(detail::is_soldier(v, 3, 9) == 1, "is_soldier: ai_unit == SOLDIER -> true regardless of .type");
    f.cfg_units[40].ai_unit = 0;
    ck(detail::is_soldier(v, 3, 9) == 0, "is_soldier: ai_unit != SOLDIER -> false");
    ck(detail::is_soldier(v, 3 | 0x40, 9) == 0, "is_soldier: building bit set -> false immediately");

    // get_ai_group_index: PLAIN player index, NOT a packed/masked ref -- see the file header.
    f.u(6, 2).ai_group_index = 0xffff;
    ck(detail::get_ai_group_index(v, 6, 2) == 0xffffu,
       "get_ai_group_index: 0xffff sentinel (no group) round-trips");
    f.u(6, 2).ai_group_index = 3;
    ck(detail::get_ai_group_index(v, 6, 2) == 3u, "get_ai_group_index: reads unit::ai_group_index (+0xd8) verbatim");
}

} // namespace

void run_unit_predicates_tests() {
    test_unit_attack_target_is_dead();
    test_unit_state_is_in_transit();
    test_unit_is_idle_or_parked();
    test_unit_type_predicates();
}

} // namespace mh::sim::test
