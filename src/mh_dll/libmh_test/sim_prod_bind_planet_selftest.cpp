//
// sim_prod_bind_planet_selftest.cpp -- `simtest` oracle for llm_strat_prod_bind_planet @0x0048feef
// (sim/sim_prod_bind_planet.h/.cpp, RI-SIM / SIM1D).
//
// arm_ready:false -- the ledger records a 5-sibling, 15000-step all-AI soak that saw ZERO calls to
// this function. This offline oracle is the ONLY evidence this function will ever have; it is
// written as the primary proof, not as a supplement to a shadow arm.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_prod_bind_planet.h):
//   0x0048ff0e-0x0048ff24: guard. EAX = players[player].prod_queue_slot[queue_slot]. If nonzero
//     (already claimed), result=0, jump straight to the epilogue -- NO other store happens on this
//     path (neither prod_queue_slot NOR is_planet_bound is touched).
//   LAB_0048ff2f, 0x0048ff2f-0x0048ff61 (only reached when the guard found the slot free):
//     players[player].prod_queue_slot[queue_slot] = shuttle_slot (0x0048ff3e-0x0048ff41);
//     _G_LLM_PROD_SHUTTLE_SLOTS[player][shuttle_slot].is_planet_bound = 1 (0x0048ff57, address
//     independently computed as player*0x1f18 + shuttle_slot*0x31c, 0x1f18 == 10*0x31c ==
//     PROD_SHUTTLE_SLOTS_PER_PLAYER * sizeof(mh_llm_prod_shuttle_slot)); result=1.
//   Return value: local_10, 0 on the already-occupied path / 1 on the claimed path, returned
//     unconditionally at the epilogue (0x0048ff68/0x0048ff6b/0x0048ff72) -- a plain bool-as-int.
//
// NO *_calls recorder struct exists for this module and none is defined here: the traced body has
// no outward calls besides the inert Watcom stack-capacity probe (translator-brief rule 6), so there
// is nothing to mock -- every check below observes fixture state and the return value directly.
//
// SHIFT-AMOUNT IDENTITY (ledger note, reimpl-verify batched with llm_strat_prod_spawn_arrived_unit):
// the asm's SHL EAX,0x2 / SHL EDX,0x2 (0x0048ff18/0x0048ff39) are the compiler's ordinary
// int32_t[32]-subscript scaling (queue_slot*4), not an algebraic rewrite the translator chose --
// sim_prod_bind_planet.cpp reproduces them as plain `profile.prod_queue_slot[queue_slot]` array
// indexing, so there is no explicit shift/multiply form left in this .cpp to pin as an "identity".
// The batch's flagged identity looks to belong to the sibling function, not this one. The stride
// arithmetic that IS this function's own (player*0x740, player*0x1f18, shuttle_slot*0x31c) is
// stressed instead by T6's high-index boundary case below, which would fail under a wrong stride
// constant even though it cannot fail under a wrong shift/multiply CHOICE that doesn't exist here.
//
#include "sim/sim_prod_bind_planet.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The primary claim-path scenario (T3): every index distinct from every other, so a translation
// that crossed player/queue_slot/shuttle_slot, or wrote through the wrong one of the two regions,
// disagrees with the fixture instead of coincidentally agreeing.
constexpr int32_t PLAYER             = 5;
constexpr int32_t OTHER_PLAYER       = 2;
constexpr int32_t QUEUE_SLOT         = 12;
constexpr int32_t OTHER_QUEUE_SLOT   = 13;
constexpr int32_t SHUTTLE_SLOT       = 7;
constexpr int32_t OTHER_SHUTTLE_SLOT = 8;

} // namespace

void run_prod_bind_planet_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- already occupied, NEGATIVE sentinel (-1). Pins the guard as `!= 0`, not e.g. a sign/`> 0`
    // style test that would misread a negative occupant as "free" and wrongly enter the claim path.
    // =================================================================================================
    {
        fx.reset();
        fx.profiles[PLAYER].prod_queue_slot[QUEUE_SLOT]                                              = -1;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound = 0;

        sim_store own = fx.store();
        int32_t   ret = detail::prod_bind_planet(own, PLAYER, QUEUE_SLOT, SHUTTLE_SLOT);

        ck_eq((uint32_t)ret, 0u, "T1: already-occupied (-1) -> return 0, 0x0048ff1d-0x0048ff26");
        ck_eq((uint32_t)fx.profiles[PLAYER].prod_queue_slot[QUEUE_SLOT], (uint32_t)-1,
              "T1: prod_queue_slot[queue_slot] left UNCHANGED on the already-occupied path (no store), 0x0048ff26-0x0048ff2d");
        ck_eq((uint32_t)fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound,
              0u, "T1: is_planet_bound NOT touched on the already-occupied path (claim block never entered)");
    }

    // =================================================================================================
    // T2 -- already occupied, POSITIVE sentinel (1). Same no-op shape from the other side of 0.
    // =================================================================================================
    {
        fx.reset();
        fx.profiles[PLAYER].prod_queue_slot[QUEUE_SLOT]                                              = 1;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound = 0;

        sim_store own = fx.store();
        int32_t   ret = detail::prod_bind_planet(own, PLAYER, QUEUE_SLOT, SHUTTLE_SLOT);

        ck_eq((uint32_t)ret, 0u, "T2: already-occupied (1) -> return 0, 0x0048ff1d-0x0048ff26");
        ck_eq((uint32_t)fx.profiles[PLAYER].prod_queue_slot[QUEUE_SLOT], 1u,
              "T2: prod_queue_slot[queue_slot] left UNCHANGED on the already-occupied path, 0x0048ff26-0x0048ff2d");
        ck_eq((uint32_t)fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound,
              0u, "T2: is_planet_bound NOT touched on the already-occupied path");
    }

    // =================================================================================================
    // T3 -- claim path, primary evidence. Free slot (0) is claimed: BOTH regions written (the
    // player_profile.prod_queue_slot claim via sim_store::profile_at, and the
    // _G_LLM_PROD_SHUTTLE_SLOTS.is_planet_bound set via sim_store::prod_shuttle_slot_at), and every
    // neighbouring cell in BOTH regions -- a different queue slot on the SAME player, the SAME queue
    // slot on a DIFFERENT player, a different shuttle slot on the SAME player, and the SAME shuttle
    // slot on a DIFFERENT player -- is asserted UNCHANGED, so a wrong index is caught rather than
    // merely a wrong value.
    // =================================================================================================
    {
        fx.reset();
        fx.profiles[PLAYER].prod_queue_slot[QUEUE_SLOT]                                              = 0;   // free
        fx.profiles[PLAYER].prod_queue_slot[OTHER_QUEUE_SLOT]                                        = 999; // neighbour: same player, other slot
        fx.profiles[OTHER_PLAYER].prod_queue_slot[QUEUE_SLOT]                                        = 888; // neighbour: other player, same slot
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound = 0;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + OTHER_SHUTTLE_SLOT].is_planet_bound =
            42; // neighbour: same player, other shuttle slot
        fx.prod_shuttle_slots[OTHER_PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound =
            55; // neighbour: other player, same shuttle slot

        sim_store own = fx.store();
        int32_t   ret = detail::prod_bind_planet(own, PLAYER, QUEUE_SLOT, SHUTTLE_SLOT);

        ck_eq((uint32_t)ret, 1u, "T3: free slot claimed -> return 1, 0x0048ff61-0x0048ff68");
        ck_eq((uint32_t)fx.profiles[PLAYER].prod_queue_slot[QUEUE_SLOT], (uint32_t)SHUTTLE_SLOT,
              "T3: prod_queue_slot[queue_slot] = shuttle_slot, 0x0048ff3e-0x0048ff41");
        ck_eq((uint32_t)fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound,
              1u, "T3: is_planet_bound = 1, 0x0048ff47-0x0048ff57");
        ck_eq((uint32_t)fx.profiles[PLAYER].prod_queue_slot[OTHER_QUEUE_SLOT], 999u,
              "T3: non-corruption -- same player's OTHER queue slot untouched");
        ck_eq((uint32_t)fx.profiles[OTHER_PLAYER].prod_queue_slot[QUEUE_SLOT], 888u,
              "T3: non-corruption -- OTHER player's same queue slot untouched (player*0x740 stride)");
        ck_eq((uint32_t)fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + OTHER_SHUTTLE_SLOT].is_planet_bound,
              42u, "T3: non-corruption -- same player's OTHER shuttle slot untouched (shuttle_slot*0x31c stride)");
        ck_eq((uint32_t)fx.prod_shuttle_slots[OTHER_PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].is_planet_bound,
              55u, "T3: non-corruption -- OTHER player's same shuttle slot untouched (player*0x1f18 stride)");
    }

    // =================================================================================================
    // T4 -- low boundary: player=0, queue_slot=0 (array start), shuttle_slot=1 (nonzero, so the
    // prod_queue_slot write is actually observable -- see the note in T5 on why shuttle_slot=0 alone
    // cannot prove that store fired).
    // =================================================================================================
    {
        fx.reset();
        fx.profiles[0].prod_queue_slot[0]                                            = 0; // free
        fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 1].is_planet_bound = 0;

        sim_store own = fx.store();
        int32_t   ret = detail::prod_bind_planet(own, 0, 0, 1);

        ck_eq((uint32_t)ret, 1u, "T4: low boundary (player=0,queue_slot=0,shuttle_slot=1) -> return 1");
        ck_eq((uint32_t)fx.profiles[0].prod_queue_slot[0], 1u,
              "T4: prod_queue_slot[0] = shuttle_slot(1), array-start index, 0x0048ff3e-0x0048ff41");
        ck_eq((uint32_t)fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 1].is_planet_bound, 1u,
              "T4: is_planet_bound(player=0,slot=1) = 1, 0x0048ff57");
    }

    // =================================================================================================
    // T5 -- low boundary: shuttle_slot=0 (per-player-row start). NOTE: with shuttle_slot=0 the value
    // stored into prod_queue_slot equals the pre-condition value (0) that the guard itself requires,
    // so that particular field's before/after equality cannot by itself prove the store fired --
    // the is_planet_bound check below is this case's real signal, and it is a genuine one (it
    // exercises the shuttle-slot address at its lowest index, player*0x1f18 + 0*0x31c).
    // =================================================================================================
    {
        fx.reset();
        fx.profiles[0].prod_queue_slot[1]                                            = 0; // free
        fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 0].is_planet_bound = 0;

        sim_store own = fx.store();
        int32_t   ret = detail::prod_bind_planet(own, 0, 1, 0);

        ck_eq((uint32_t)ret, 1u, "T5: low boundary (player=0,queue_slot=1,shuttle_slot=0) -> return 1");
        ck_eq((uint32_t)fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 0].is_planet_bound, 1u,
              "T5: is_planet_bound(player=0,slot=0) = 1, per-player-row start, 0x0048ff57");
    }

    // =================================================================================================
    // T6 -- high boundary: player=7 (MAX_PLAYERS-1), queue_slot=31 (last of int32_t prod_queue_slot
    // [32]), shuttle_slot=9 (PROD_SHUTTLE_SLOTS_PER_PLAYER-1). The most stride-sensitive case: a wrong
    // player_profile stride (not 0x740) or a wrong shuttle-slot-per-player count (not 10) or a wrong
    // sizeof(prod_shuttle_slot) (not 0x31c) all show up here, where a low-index case would hide them.
    // Also checks player 0's row/slot are untouched by the largest player offset (0x740*7 / 0x1f18*7).
    // =================================================================================================
    {
        fx.reset();
        fx.profiles[7].prod_queue_slot[31]                                           = 0;   // free
        fx.profiles[0].prod_queue_slot[31]                                           = 321; // neighbour: player 0, same slot
        fx.prod_shuttle_slots[7 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 9].is_planet_bound = 0;
        fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 9].is_planet_bound = 654; // neighbour: player 0, same slot

        sim_store own = fx.store();
        int32_t   ret = detail::prod_bind_planet(own, 7, 31, 9);

        ck_eq((uint32_t)ret, 1u, "T6: high boundary (player=7,queue_slot=31,shuttle_slot=9) -> return 1");
        ck_eq((uint32_t)fx.profiles[7].prod_queue_slot[31], 9u,
              "T6: prod_queue_slot[31] = shuttle_slot(9), array-end index, player*0x740 stride at player=7");
        ck_eq((uint32_t)fx.prod_shuttle_slots[7 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 9].is_planet_bound, 1u,
              "T6: is_planet_bound(player=7,slot=9) = 1, player*0x1f18 + slot*0x31c at the largest offsets");
        ck_eq((uint32_t)fx.profiles[0].prod_queue_slot[31], 321u,
              "T6: non-corruption -- player 0's slot 31 untouched by the player=7 write (stride, not overrun)");
        ck_eq((uint32_t)fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + 9].is_planet_bound, 654u,
              "T6: non-corruption -- player 0's shuttle slot 9 untouched by the player=7 write");
    }
}

} // namespace mh::sim::test
