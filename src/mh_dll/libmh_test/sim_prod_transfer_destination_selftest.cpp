//
// sim_prod_transfer_destination_selftest.cpp -- `simtest` offline oracle for
// llm_strat_prod_set_transfer_destination @0x0048efcc (sim/resid/sim_prod_transfer_destination.h/.cpp,
// RI-SIM / sim_resid batch E).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification. Expected
// behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_prod_set_transfer_destination_0048efcc.asm), not the .c draft:
//
//   0x0048efeb  MOVZX off the stored dword parameter -- the player index is masked to 16 bits
//     EVERYWHERE this body indexes the shuttle roster.
//   0x0048efff  CMP status,0xc8 -- the IN-TRANSIT arm; 0x0048f1b7 CMP status,0xc9 -- ARRIVED;
//     anything else falls to 0x0048f1d3-0x0048f237 and returns -1 having written NOTHING.
//   0x0048f052-0x0048f087  origin_planet == 0 -> the leg's origin is the cached interpolated
//     position, read from the NEXT SLOT's dead leading pad (prev_slot_transit_x/_y).
//   0x0048f030-0x0048f04d  origin_planet != 0 -> Planets[origin_planet].coordinate_x/_y.
//   0x0048f08a-0x0048f0c4  the lerp TARGET is Planets[slot.dest_planet] -- the slot's OLD
//     destination, still unchanged at this point -- NOT the dest_planet parameter.
//   0x0048f0c7-0x0048f0ca  progress = prod_transfer_progress(prod_slot), the RAW slot parameter,
//     never the player-composite index.
//   0x0048f0d2-0x0048f107  trunc(origin + (dest-origin)*progress) per axis, FISTP dword, ROUND
//     TOWARD ZERO (utils_math_trunc's RC=11) -- a negative result truncates UP, unlike floor().
//   0x0048f10a-0x0048f13c  the fresh position is written back into that same NEXT-slot pad.
//   0x0048f142-0x0048f155  origin_planet = 0.
//   0x0048f15e-0x0048f1aa  distance = planet_distance(new position, Planets[dest_planet PARAM]).
//   0x0048f1c1-0x0048f22b  ARRIVED arm: distance = planet_distance(Planets[origin_planet],
//     Planets[dest_planet PARAM]) -- origin_planet is NOT cleared on this arm.
//   0x0048f23c-0x0048f2e6  common tail: dest_planet = (int16_t)param; travel_duration =
//     Building[Unit[type_ref_id].equivalent].velocity * distance; travel_duration_copy re-reads it;
//     status = 0xc8; return 1.
//
#include <cstring> // memcmp -- the whole-record "wrote nothing" compares

#include "sim/resid/sim_prod_transfer_destination.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- mocks --------------------------------------------------------------------------------------

struct DistCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DistCall> g_dist_calls;
double                g_dist_return = 0.0;
double                stub_planet_distance(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_dist_calls.push_back({x1, y1, x2, y2});
    return g_dist_return;
}

std::vector<int32_t> g_progress_calls;
double               g_progress_return = 0.0;
double               stub_prod_transfer_progress(int32_t slot) {
    g_progress_calls.push_back(slot);
    return g_progress_return;
}

const prod_transfer_destination_calls g_calls = {
    &stub_planet_distance,
    &stub_prod_transfer_progress,
};

// RECORDS only. The two return values are mock CONFIGURATION and arrive as run() parameters, so a
// case's own setup cannot be reset out from under it -- the failure mode that made T6 of
// sim_invasion_due_check_selftest.cpp fail against a correct body (2026-08-31).
void reset_calls() {
    g_dist_calls.clear();
    g_progress_calls.clear();
}

int32_t run(sim_fixture &fx, uint32_t player_idx, int32_t prod_slot, int32_t dest_planet, double distance,
            double progress) {
    reset_calls();
    g_dist_return     = distance;
    g_progress_return = progress;
    sim_store own     = fx.store();
    return detail::prod_set_transfer_destination(fx.view(), own, g_calls, player_idx, prod_slot, dest_planet);
}

// A whole-record byte compare, so "wrote nothing" is asserted over EVERY field rather than the
// handful a case happens to name.
bool slot_bytes_equal(const prod_shuttle_slot &a, const prod_shuttle_slot &b) {
    return memcmp(&a, &b, sizeof(prod_shuttle_slot)) == 0;
}

// The cfg tables the common tail reads: Unit[type_ref_id].equivalent -> Building[...].velocity.
void seed_speed_chain(sim_fixture &fx, uint16_t type_ref_id, int32_t equivalent, double velocity) {
    fx.cfg_units[type_ref_id].equivalent  = equivalent;
    fx.cfg_buildings[equivalent].velocity = velocity;
}

void seed_planet(sim_fixture &fx, int32_t idx, int32_t x, int32_t y) {
    fx.cfg_planets[idx].coordinate_x = (uint32_t)x;
    fx.cfg_planets[idx].coordinate_y = (uint32_t)y;
}

} // namespace

void run_prod_set_transfer_destination_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- NOT AN ACTIVE TRANSFER (status is neither 0xc8 nor 0xc9): return -1 and write NOTHING.
    // The whole 0x320-byte record AND its neighbour (the cross-slot cache) are byte-compared, so any
    // stray tail write -- dest_planet, status, travel_duration, the cache -- shows up here.
    // Every status value that borders the two live ones is exercised.
    // =================================================================================================
    {
        const int16_t dead_statuses[] = {0, 1, (int16_t)0xc7, (int16_t)0xca, (int16_t)-1};
        for (int16_t status : dead_statuses) {
            fx.reset();
            seed_planet(fx, 4, 700, 800);
            seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/2.0);

            sim_store          own    = fx.store();
            prod_shuttle_slot &slot   = own.prod_shuttle_slot_at(2, 5);
            prod_shuttle_slot &next   = own.prod_shuttle_slot_at(2, 6);
            slot.status               = status;
            slot.origin_planet        = 1;
            slot.dest_planet          = 2;
            slot.type_ref_id          = 3;
            slot.travel_duration      = 123.0;
            slot.travel_duration_copy = 456.0;
            next.prev_slot_transit_x  = 11;
            next.prev_slot_transit_y  = 22;

            const prod_shuttle_slot before_slot = slot;
            const prod_shuttle_slot before_next = next;

            const int32_t ret = run(fx, /*player_idx=*/2, /*prod_slot=*/5, /*dest_planet=*/4,
                                    /*distance=*/9.0, /*progress=*/0.5);

            ck_eq((uint32_t)ret, (uint32_t)-1, "T1: inactive status returns -1, 0x0048f1d3-0x0048f237");
            ck(slot_bytes_equal(slot, before_slot), "T1: the slot record is byte-identical -- no tail write");
            ck(slot_bytes_equal(next, before_next), "T1: the NEXT slot's cache pad is byte-identical too");
            ck(g_dist_calls.empty(), "T1: planet_distance never called on the inactive arm");
            ck(g_progress_calls.empty(), "T1: prod_transfer_progress never called on the inactive arm");
        }
    }

    // =================================================================================================
    // T2 -- ARRIVED (0xc9): distance measured origin_planet -> the NEW destination PARAMETER, then the
    // common tail commits. origin_planet is NOT cleared on this arm, and the cross-slot cache is not
    // touched. Planets[old dest], Planets[origin] and Planets[new dest] all carry DISTINCT coordinates
    // so swapping any pair is visible.
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 1, 100, 200); // origin_planet
        seed_planet(fx, 2, 300, 400); // the slot's OLD dest -- must NOT be read on this arm
        seed_planet(fx, 4, 500, 600); // the NEW dest parameter
        seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/2.5);

        sim_store          own   = fx.store();
        prod_shuttle_slot &slot  = own.prod_shuttle_slot_at(2, 5);
        prod_shuttle_slot &next  = own.prod_shuttle_slot_at(2, 6);
        slot.status              = (int16_t)0xc9;
        slot.origin_planet       = 1;
        slot.dest_planet         = 2;
        slot.type_ref_id         = 3;
        next.prev_slot_transit_x = 11;
        next.prev_slot_transit_y = 22;

        const int32_t ret = run(fx, /*player_idx=*/2, /*prod_slot=*/5, /*dest_planet=*/4,
                                /*distance=*/8.0, /*progress=*/0.5);

        ck_eq((uint32_t)ret, 1u, "T2: arrived arm returns 1, 0x0048f2e6");
        ck(g_progress_calls.empty(), "T2: prod_transfer_progress NOT called on the arrived arm");
        ck(g_dist_calls.size() == 1, "T2: planet_distance called exactly once, 0x0048f21e");
        if (g_dist_calls.size() == 1) {
            ck_eq(g_dist_calls[0].x1, 100u, "T2: distance arg 1 = Planets[origin_planet].coordinate_x");
            ck_eq(g_dist_calls[0].y1, 200u, "T2: distance arg 2 = Planets[origin_planet].coordinate_y");
            ck_eq(g_dist_calls[0].x2, 500u, "T2: distance arg 3 = Planets[dest_planet PARAM].coordinate_x");
            ck_eq(g_dist_calls[0].y2, 600u, "T2: distance arg 4 = Planets[dest_planet PARAM].coordinate_y");
        }
        ck_eq((uint32_t)(int32_t)slot.dest_planet, 4u,
              "T2: dest_planet committed to the new destination, 0x0048f278");
        ck_eq((uint32_t)(int32_t)slot.origin_planet, 1u,
              "T2: origin_planet NOT cleared on the arrived arm (the clear at 0x0048f142 is transit-only)");
        ck_eq_d(slot.travel_duration, 2.5 * 8.0,
                "T2: travel_duration = Building[Unit[type_ref_id].equivalent].velocity * distance, "
                "0x0048f282-0x0048f2a5");
        ck_eq_d(slot.travel_duration_copy, 2.5 * 8.0,
                "T2: travel_duration_copy re-reads the stored value, 0x0048f2ab-0x0048f2c4");
        ck_eq((uint32_t)(int32_t)slot.status, 0xc8u, "T2: status set back to 0xc8 (active transfer), 0x0048f2ca");
        ck_eq((uint32_t)next.prev_slot_transit_x, 11u, "T2: cross-slot cache X untouched on the arrived arm");
        ck_eq((uint32_t)next.prev_slot_transit_y, 22u, "T2: cross-slot cache Y untouched on the arrived arm");
    }

    // =================================================================================================
    // T3 -- IN TRANSIT (0xc8), origin_planet != 0: the lerp runs from Planets[origin_planet] toward
    // Planets[slot.dest_planet] -- the slot's OLD destination -- and the RESULT is what the distance
    // call measures against the NEW destination parameter. Every table entry is distinct, so reading
    // the new dest as the lerp target (or the old one as the distance target) fails here.
    //
    // Arithmetic is exact: origin (100,200), old dest (300,-40), progress 0.25 ->
    //   x = 100 + (300-100)*0.25 = 150
    //   y = 200 + (-40-200)*0.25 = 140
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 1, 100, 200); // origin_planet
        seed_planet(fx, 2, 300, -40); // the slot's OLD dest -- the LERP TARGET
        seed_planet(fx, 4, 500, 600); // the NEW dest parameter -- the DISTANCE target
        seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/0.5);

        sim_store          own    = fx.store();
        prod_shuttle_slot &slot   = own.prod_shuttle_slot_at(2, 5);
        prod_shuttle_slot &next   = own.prod_shuttle_slot_at(2, 6);
        prod_shuttle_slot &after  = own.prod_shuttle_slot_at(2, 7);
        slot.status               = (int16_t)0xc8;
        slot.origin_planet        = 1;
        slot.dest_planet          = 2;
        slot.type_ref_id          = 3;
        next.prev_slot_transit_x  = 11; // must be OVERWRITTEN, not read, on this arm
        next.prev_slot_transit_y  = 22;
        after.prev_slot_transit_x = 77; // slot+2: must stay untouched -- pins the +1 offset
        after.prev_slot_transit_y = 88;

        const int32_t ret = run(fx, /*player_idx=*/2, /*prod_slot=*/5, /*dest_planet=*/4,
                                /*distance=*/6.0, /*progress=*/0.25);

        ck_eq((uint32_t)ret, 1u, "T3: transit arm returns 1");
        ck(g_progress_calls.size() == 1, "T3: prod_transfer_progress called exactly once, 0x0048f0c7");
        if (g_progress_calls.size() == 1) {
            ck_eq((uint32_t)g_progress_calls[0], 5u,
                  "T3: progress is keyed on the RAW prod_slot, not the player-composite index, 0x0048f0ca");
        }
        ck_eq((uint32_t)next.prev_slot_transit_x, 150u,
              "T3: cache X = trunc(100 + (300-100)*0.25), 0x0048f0d2-0x0048f120");
        ck_eq((uint32_t)next.prev_slot_transit_y, 140u,
              "T3: cache Y = trunc(200 + (-40-200)*0.25), 0x0048f0ee-0x0048f13c");
        ck_eq((uint32_t)after.prev_slot_transit_x, 77u,
              "T3: slot+2 cache X untouched -- the pad is the NEXT slot's, +1 exactly");
        ck_eq((uint32_t)after.prev_slot_transit_y, 88u, "T3: slot+2 cache Y untouched");
        ck_eq((uint32_t)(int32_t)slot.origin_planet, 0u, "T3: origin_planet cleared, 0x0048f142-0x0048f155");
        ck(g_dist_calls.size() == 1, "T3: planet_distance called exactly once, 0x0048f1aa");
        if (g_dist_calls.size() == 1) {
            ck_eq(g_dist_calls[0].x1, 150u,
                  "T3: distance arg 1 is the FRESH interpolated X, not a planet coordinate");
            ck_eq(g_dist_calls[0].y1, 140u, "T3: distance arg 2 is the FRESH interpolated Y");
            ck_eq(g_dist_calls[0].x2, 500u, "T3: distance arg 3 = Planets[dest_planet PARAM].coordinate_x");
            ck_eq(g_dist_calls[0].y2, 600u, "T3: distance arg 4 = Planets[dest_planet PARAM].coordinate_y");
        }
        ck_eq((uint32_t)(int32_t)slot.dest_planet, 4u, "T3: dest_planet committed after the distance call");
        ck_eq_d(slot.travel_duration, 0.5 * 6.0, "T3: travel_duration = velocity * distance");
        ck_eq_d(slot.travel_duration_copy, 0.5 * 6.0, "T3: travel_duration_copy mirrors it");
        ck_eq((uint32_t)(int32_t)slot.status, 0xc8u, "T3: status stays/returns to 0xc8");
    }

    // =================================================================================================
    // T4 -- IN TRANSIT, origin_planet == 0: the leg's origin comes from the NEXT slot's cache pad, and
    // the same pad is then overwritten. Planets[0] is seeded with a DECOY so a translation that read
    // Planets[origin_planet] unconditionally (i.e. Planets[0]) computes a different answer and fails.
    //
    // origin (10,20) from the cache, old dest (30,60), progress 0.5 -> x = 20, y = 40.
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 0, -9999, -9999); // DECOY: Planets[0] must never be read as the origin here
        seed_planet(fx, 2, 30, 60);       // the slot's OLD dest -- the lerp target
        seed_planet(fx, 4, 500, 600);     // the NEW dest parameter
        seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/1.0);

        sim_store          own   = fx.store();
        prod_shuttle_slot &slot  = own.prod_shuttle_slot_at(2, 5);
        prod_shuttle_slot &next  = own.prod_shuttle_slot_at(2, 6);
        slot.status              = (int16_t)0xc8;
        slot.origin_planet       = 0;
        slot.dest_planet         = 2;
        slot.type_ref_id         = 3;
        next.prev_slot_transit_x = 10;
        next.prev_slot_transit_y = 20;

        const int32_t ret = run(fx, /*player_idx=*/2, /*prod_slot=*/5, /*dest_planet=*/4,
                                /*distance=*/3.0, /*progress=*/0.5);

        ck_eq((uint32_t)ret, 1u, "T4: transit arm (cached origin) returns 1");
        ck_eq((uint32_t)next.prev_slot_transit_x, 20u,
              "T4: cache X = trunc(10 + (30-10)*0.5) -- origin READ FROM THE CACHE, 0x0048f052-0x0048f087");
        ck_eq((uint32_t)next.prev_slot_transit_y, 40u, "T4: cache Y = trunc(20 + (60-20)*0.5)");
        ck(g_dist_calls.size() == 1, "T4: planet_distance called exactly once");
        if (g_dist_calls.size() == 1) {
            ck_eq(g_dist_calls[0].x1, 20u, "T4: distance arg 1 is the recomputed X");
            ck_eq(g_dist_calls[0].y1, 40u, "T4: distance arg 2 is the recomputed Y");
        }
        ck_eq((uint32_t)(int32_t)slot.origin_planet, 0u, "T4: origin_planet still 0");
    }

    // =================================================================================================
    // T5 -- TRUNCATION TOWARD ZERO, not floor. The lerp lands on a NEGATIVE fraction on both axes:
    //   x: 0 + (-10 - 0)*0.25 = -2.5 -> trunc -2   (floor would give -3)
    //   y: 0 + (-30 - 0)*0.25 = -7.5 -> trunc -7   (floor would give -8)
    // This is the only case that separates utils_math_trunc's RC=11 from a plain floor/rint.
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 1, 0, 0);
        seed_planet(fx, 2, -10, -30);
        seed_planet(fx, 4, 500, 600);
        seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/1.0);

        sim_store          own  = fx.store();
        prod_shuttle_slot &slot = own.prod_shuttle_slot_at(2, 5);
        prod_shuttle_slot &next = own.prod_shuttle_slot_at(2, 6);
        slot.status             = (int16_t)0xc8;
        slot.origin_planet      = 1;
        slot.dest_planet        = 2;
        slot.type_ref_id        = 3;

        run(fx, /*player_idx=*/2, /*prod_slot=*/5, /*dest_planet=*/4, /*distance=*/1.0, /*progress=*/0.25);

        ck_eq((uint32_t)next.prev_slot_transit_x, (uint32_t)-2,
              "T5: -2.5 truncates TOWARD ZERO to -2 (floor would be -3), utils_math_trunc RC=11 @0x0048f0e6");
        ck_eq((uint32_t)next.prev_slot_transit_y, (uint32_t)-7,
              "T5: -7.5 truncates TOWARD ZERO to -7 (floor would be -8), 0x0048f102");
    }

    // =================================================================================================
    // T6 -- the 16-BIT PLAYER MASK (0x0048efeb and every sibling read). player_idx carries garbage in
    // its high half; the body must index player 3 and leave every other player's slots alone.
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 1, 100, 200);
        seed_planet(fx, 4, 500, 600);
        seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/2.0);

        sim_store          own    = fx.store();
        prod_shuttle_slot &target = own.prod_shuttle_slot_at(3, 5);
        prod_shuttle_slot &decoy  = own.prod_shuttle_slot_at(4, 5);
        target.status             = (int16_t)0xc9;
        target.origin_planet      = 1;
        target.dest_planet        = 2;
        target.type_ref_id        = 3;
        decoy.status              = (int16_t)0xc9;
        decoy.origin_planet       = 1;
        decoy.dest_planet         = 2;
        decoy.type_ref_id         = 3;

        const prod_shuttle_slot before_decoy = decoy;

        const int32_t ret = run(fx, /*player_idx=*/0x12340003u, /*prod_slot=*/5, /*dest_planet=*/4,
                                /*distance=*/4.0, /*progress=*/0.5);

        ck_eq((uint32_t)ret, 1u, "T6: masked player index still resolves an active transfer");
        ck_eq((uint32_t)(int32_t)target.dest_planet, 4u,
              "T6: player 3's slot was the one committed -- the mask is 0xffff, 0x0048efeb");
        ck(slot_bytes_equal(decoy, before_decoy),
           "T6: player 4's slot is byte-identical -- no neighbouring player touched");
    }

    // =================================================================================================
    // T7 -- the dest_planet PARAMETER, not the slot's old value, selects the distance target, and it
    // is committed verbatim through the 16-bit store at 0x0048f278.
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 1, 100, 200);
        seed_planet(fx, 7, 111, 222);  // the slot's OLD dest -- a decoy for the distance target
        seed_planet(fx, 30, 700, 800); // the parameter
        seed_speed_chain(fx, /*type_ref_id=*/3, /*equivalent=*/9, /*velocity=*/1.0);

        sim_store          own  = fx.store();
        prod_shuttle_slot &slot = own.prod_shuttle_slot_at(1, 2);
        slot.status             = (int16_t)0xc9;
        slot.origin_planet      = 1;
        slot.dest_planet        = 7;
        slot.type_ref_id        = 3;

        run(fx, /*player_idx=*/1, /*prod_slot=*/2, /*dest_planet=*/30, /*distance=*/1.0, /*progress=*/0.0);

        ck_eq((uint32_t)(int32_t)slot.dest_planet, 30u, "T7: dest_planet committed verbatim, 0x0048f278");
        ck(g_dist_calls.size() == 1, "T7: planet_distance called exactly once");
        if (g_dist_calls.size() == 1) {
            ck_eq(g_dist_calls[0].x2, 700u,
                  "T7: the distance target is Planets[30], the parameter -- not the old dest 7");
            ck_eq(g_dist_calls[0].y2, 800u, "T7: ... y likewise");
        }
    }

    // =================================================================================================
    // T8 -- the SPEED CHAIN is two hops, and both must be the ones the .asm reads:
    //   Unit[slot.type_ref_id].equivalent -> Building[that].velocity.
    // Decoys are planted at Building[type_ref_id] and Unit[equivalent] so a one-hop or swapped-table
    // translation reads a DIFFERENT velocity and the product changes.
    // =================================================================================================
    {
        fx.reset();
        seed_planet(fx, 1, 100, 200);
        seed_planet(fx, 4, 500, 600);
        fx.cfg_units[6].equivalent    = 11;   // the real chain: Unit[6] -> Building[11]
        fx.cfg_buildings[11].velocity = 3.0;  // the real velocity
        fx.cfg_buildings[6].velocity  = 99.0; // decoy: Building[type_ref_id] -- a missed hop
        fx.cfg_units[11].equivalent   = 6;    // decoy: the reverse chain

        sim_store          own  = fx.store();
        prod_shuttle_slot &slot = own.prod_shuttle_slot_at(0, 0);
        slot.status             = (int16_t)0xc9;
        slot.origin_planet      = 1;
        slot.dest_planet        = 2;
        slot.type_ref_id        = 6;

        run(fx, /*player_idx=*/0, /*prod_slot=*/0, /*dest_planet=*/4, /*distance=*/7.0, /*progress=*/0.0);

        ck_eq_d(slot.travel_duration, 3.0 * 7.0,
                "T8: velocity comes from Building[Unit[type_ref_id].equivalent], 0x0048f24f-0x0048f2a5");
        ck_eq_d(slot.travel_duration_copy, 3.0 * 7.0, "T8: the copy field mirrors the same product");
    }
}

} // namespace mh::sim::test
