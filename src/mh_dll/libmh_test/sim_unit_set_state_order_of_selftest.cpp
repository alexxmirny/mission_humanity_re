//
// sim_unit_set_state_order_of_selftest.cpp -- `simtest` cases for
// llm_strat_unit_set_state_order_of @0x00486913 (sim/sim_unit_set_state_order_of.h/.cpp, SIM1-G1).
//
// SCOPE: this is a straight-line, branch-free, callee-free (besides the inert Watcom stack probe)
// function -- two 16-bit stores into the same roster slot, each preceded by a FRESH row/col IMUL
// (0x00486934 IMUL EAX,player,0x5b04 / 0x0048693b IMUL EDX,unit_index,0xe9 / 0x00486942 ADD EDX,EAX
// for the .state store at 0x00486947; then the SAME calculation redone at 0x0048694e/0x00486955/
// 0x0048695c for the .order store at 0x00486961). There is no gate and no loop, so what a test here
// must pin is PRECISION, not branch coverage:
//   T1 -- the row/col addressing lands at (player, unit_index) exactly, args not swapped (0x00486934/
//         0x0048693b -- EAX gets player*0x5b04, EDX gets unit_index*0xe9, in that order).
//   T2 -- `state` feeds `.state` and `param` feeds `.order`, not swapped (0x00486944 MOV EAX,[state]/
//         0x00486947 store to +0xdd8c4e=.state; 0x0048695e MOV EAX,[param]/0x00486961 store to
//         +0xdd8c4c=.order).
//   T3 -- both stores are exactly 16 bits wide (`MOV word ptr [...],AX`), no truncation and no spill
//         into the neighbouring fields (.unit_proto_id just below .order, .activity_clock just above
//         .state).
//   T4 -- the addressing holds at the far edge of the roster (player=MAX_PLAYERS-1,
//         index=UNITS_PER_PLAYER-1), not just near player=0/index=0 where a wrong stride constant can
//         accidentally agree with the right one.
//   T5 -- non-corruption: every other field of the touched slot, and every neighbouring roster slot
//         this call must not address, read back exactly as seeded.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_set_state_order_of_00486913.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_set_state_order_of.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// A sentinel fill so a wrong-slot write (T1's whole point) lands somewhere observable rather than on
// top of a zeroed default that would look "correct" by accident.
void seed_slot(unit &u, uint16_t proto, uint16_t order, uint16_t state, double clock) {
    u.unit_above[0]  = 0xAB;
    u.unit_above[1]  = 0xCD;
    u.unit_proto_id  = proto;
    u.order          = order;
    u.state          = state;
    u.activity_clock = clock;
    u.rotation_clock = clock + 1.0;
    u.energy         = clock + 2.0;
}

} // namespace

void run_unit_set_state_order_of_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- row/col addressing: player=3, unit_index=7 (distinct, nonzero, non-symmetric) must land at
    // u(3,7) and NOWHERE ELSE -- in particular NOT at u(7,3) (the args-swapped mistake), which a
    // player==index test could never distinguish. 0x00486934 IMUL EAX,player,0x5b04 / 0x0048693b IMUL
    // EDX,unit_index,0xe9 / 0x00486942 ADD EDX,EAX (recomputed identically at 0x0048694e-0x0048695c for
    // the second store).
    // =================================================================================================
    {
        fx.reset();
        // Seeded values are DISTINCT from what the call below writes (state=0x1111, param=0x2222) --
        // if the seeded .order (below) equalled the new param value, a translation that left .order
        // unwritten would still pass the check by accident.
        seed_slot(fx.u(3, 7), 111, 0x9999, 0x3333, 10.0);
        seed_slot(fx.u(7, 3), 222, 0x4444, 0x5555, 20.0); // the args-swapped destination -- must survive
        seed_slot(fx.u(0, 0), 333, 0x6666, 0x7777, 30.0); // guard slot at the origin

        sim_store own = fx.store();
        detail::unit_set_state_order_of(own, /*player=*/3, /*unit_index=*/7, /*state=*/(int16_t)0x1111,
                                        /*param=*/(int16_t)0x2222);

        const unit &target = fx.u(3, 7);
        ck_eq(target.state, 0x1111, "T1: u(3,7).state written at the correct slot (0x00486947 store)");
        ck_eq(target.order, 0x2222, "T1: u(3,7).order written at the correct slot (0x00486961 store)");

        const unit &swapped = fx.u(7, 3);
        ck(swapped.state == 0x5555 && swapped.order == 0x4444,
           "T1: u(7,3) (the args-swapped slot) is untouched -- player/unit_index were not swapped "
           "(0x00486934/0x0048693b operand order)");
        const unit &origin = fx.u(0, 0);
        ck(origin.state == 0x7777 && origin.order == 0x6666,
           "T1: u(0,0) untouched by a write addressed at (3,7)");
    }

    // =================================================================================================
    // T2 -- field assignment: `state` feeds .state, `param` feeds .order -- not swapped. Uses two
    // DISTINCT, non-symmetric values so a translation that swapped the two stores would disagree here.
    // 0x00486944 MOV EAX,[EBP-0x10]=state / 0x00486947 MOV word ptr [EDX+0xdd8c4e]=.state,AX; then
    // 0x0048695e MOV EAX,[EBP-0xc]=param / 0x00486961 MOV word ptr [EDX+0xdd8c4c]=.order,AX.
    // =================================================================================================
    {
        fx.reset();
        seed_slot(fx.u(1, 2), 10, 0xBEEF, 0xBEEF, 5.0); // pre-seeded EQUAL so only the call's own writes
                                                        // can produce the asymmetry T2 checks for

        sim_store own = fx.store();
        detail::unit_set_state_order_of(own, 1, 2, /*state=*/(int16_t)0x0AAA, /*param=*/(int16_t)0x0BBB);

        const unit &u = fx.u(1, 2);
        ck_eq(u.state, 0x0AAA, "T2: state(0x0AAA) lands in .state, not .order (0x00486947 store)");
        ck_eq(u.order, 0x0BBB, "T2: param(0x0BBB) lands in .order, not .state (0x00486961 store)");
    }

    // =================================================================================================
    // T3 -- exact 16-bit width, no truncation, no spill into the neighbouring fields. Both `MOV word
    // ptr [...],AX` stores are 2 bytes: .order@+0x4/+0x5, .state@+0x6/+0x7, with .unit_proto_id@+0x2/
    // +0x3 immediately BELOW .order and .activity_clock@+0x8..+0xf immediately ABOVE .state. Negative
    // int16 inputs (-1, -2) exercise the full bit pattern (0xFFFF/0xFFFE) rather than a low value that
    // would still look right after an accidental 8-bit truncation.
    // =================================================================================================
    {
        fx.reset();
        seed_slot(fx.u(4, 9), 0x9999, 0x1234, 0x5678, 42.5);

        sim_store own = fx.store();
        detail::unit_set_state_order_of(own, 4, 9, /*state=*/(int16_t)-1, /*param=*/(int16_t)-2);

        const unit &u = fx.u(4, 9);
        ck_eq(u.state, 0xFFFFu, "T3: state=-1 stores as the exact 16-bit pattern 0xFFFF (0x00486947)");
        ck_eq(u.order, 0xFFFEu, "T3: param=-2 stores as the exact 16-bit pattern 0xFFFE (0x00486961)");
        ck_eq(u.unit_proto_id, 0x9999,
              "T3: .unit_proto_id (just below .order) untouched -- the .order store is exactly 2 bytes");
        ck_eq_d(u.activity_clock, 42.5,
                "T3: .activity_clock (just above .state) untouched -- the .state store is exactly 2 bytes");
    }

    // =================================================================================================
    // T4 -- addressing holds at the far edge of the roster (player=MAX_PLAYERS-1=7,
    // index=UNITS_PER_PLAYER-1=99), not just near the origin where a wrong stride constant (e.g. an
    // off-by-one in the 0xe9/0x5b04 multipliers) can accidentally still land in-bounds.
    // =================================================================================================
    {
        fx.reset();
        seed_slot(fx.u(7, 99), 1, 0x1111, 0x2222, 1.0);
        seed_slot(fx.u(6, 99), 2, 0x3333, 0x4444, 2.0); // adjacent player, same index -- must survive
        seed_slot(fx.u(7, 98), 3, 0x5555, 0x6666, 3.0); // same player, adjacent index -- must survive

        sim_store own = fx.store();
        detail::unit_set_state_order_of(own, 7, 99, /*state=*/(int16_t)0x7A7A, /*param=*/(int16_t)0x7B7B);

        const unit &target = fx.u(7, 99);
        ck_eq(target.state, 0x7A7A, "T4: last-slot (7,99) .state written correctly at the roster edge");
        ck_eq(target.order, 0x7B7B, "T4: last-slot (7,99) .order written correctly at the roster edge");

        const unit &nbr_player = fx.u(6, 99);
        ck(nbr_player.state == 0x4444 && nbr_player.order == 0x3333,
           "T4: neighbouring player row (6,99) untouched");
        const unit &nbr_index = fx.u(7, 98);
        ck(nbr_index.state == 0x6666 && nbr_index.order == 0x5555,
           "T4: neighbouring index in the same player row (7,98) untouched");
    }

    // =================================================================================================
    // T5 -- full non-corruption: every OTHER field of the touched slot, and a set of untouched
    // neighbouring slots, read back exactly as seeded. This function has no callee that could own a
    // side effect elsewhere (only the inert stack probe), so anything beyond the two 16-bit stores
    // moving is a translation bug.
    // =================================================================================================
    {
        fx.reset();
        seed_slot(fx.u(2, 5), 0xCAFE, 0x1010, 0x2020, 99.25);
        seed_slot(fx.u(2, 6), 0xF00D, 0x3030, 0x4040, 88.5);  // guard: same player, next index
        seed_slot(fx.u(1, 5), 0xD00D, 0x5050, 0x6060, 77.75); // guard: same index, prev player

        sim_store own = fx.store();
        detail::unit_set_state_order_of(own, 2, 5, /*state=*/(int16_t)0x0C0C, /*param=*/(int16_t)0x0D0D);

        const unit &u = fx.u(2, 5);
        ck_eq(u.state, 0x0C0C, "T5: .state updated on the target slot");
        ck_eq(u.order, 0x0D0D, "T5: .order updated on the target slot");
        ck(u.unit_above[0] == 0xAB && u.unit_above[1] == 0xCD, "T5: .unit_above untouched");
        ck_eq(u.unit_proto_id, 0xCAFE, "T5: .unit_proto_id untouched");
        ck_eq_d(u.activity_clock, 99.25, "T5: .activity_clock untouched");
        ck_eq_d(u.rotation_clock, 100.25, "T5: .rotation_clock untouched");
        ck_eq_d(u.energy, 101.25, "T5: .energy untouched");

        const unit &nbr_index = fx.u(2, 6);
        ck(nbr_index.state == 0x4040 && nbr_index.order == 0x3030 && nbr_index.unit_proto_id == 0xF00D,
           "T5: neighbouring slot (2,6) fully untouched");
        const unit &nbr_player = fx.u(1, 5);
        ck(nbr_player.state == 0x6060 && nbr_player.order == 0x5050 && nbr_player.unit_proto_id == 0xD00D,
           "T5: neighbouring slot (1,5) fully untouched");
    }
}

} // namespace mh::sim::test
