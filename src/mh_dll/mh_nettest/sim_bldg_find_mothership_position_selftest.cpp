//
// sim_bldg_find_mothership_position_selftest.cpp -- `simtest` cases for
// llm_strat_bldg_find_mothership_position (sim/sim_bldg_find_mothership_position.h/.cpp), batch B.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_find_mothership_position_0048fdd0.asm), not read off the .cpp:
//
//   HEADCOUNT (0x0048fdef-0x0048fe04): remaining = trunc(buildings[player][0].energy) -- slot 0 is
//   the roster's SENTINEL/count slot, never itself a candidate.
//
//   LOOP BOUND (0x0048fe0b-0x0048fe17), checked at the TOP of every iteration, TWO ANDed conditions,
//   either ending the walk: `slot < 100` (JGE 0x0048fe17) and `remaining != 0` (JNZ 0x0048fe24, else
//   falls into the same exit at 0x0048fe17). Slot cursor starts at 1 (0x0048fe04 inits it to 1 via
//   the loop-var slot; slot 0 was already consumed as the count field).
//
//   ENERGY GATE (0x0048fe34-0x0048fe3f): FLDZ; FCOMP energy; FNSTSW AX; SAHF; JNC 0x0048fed8 (skip).
//   JNC (no jump = fallthrough = "alive") fires on CF=0, i.e. NOT(0.0 > energy), i.e. `!(energy <=
//   0.0)` -- matching sim_bldg_mother_reelect_primary.cpp's identical idiom, NOT the Ghidra draft's
//   `0.0 < energy` (which would treat a boundary energy==0.0 the same way here -- both forms agree at
//   exactly 0.0 -- but disagrees on NaN; S7 below exercises the NaN case since it costs nothing).
//   On skip (JNC not taken -> JC to 0x0048fed8): NO decrement, NO type check -- straight to the next
//   slot (0x0048fed8 -> 0x0048fe1c: INC slot, loop back).
//
//   DECREMENT (0x0048fe45-0x0048fe48): once alive, `remaining` is decremented UNCONDITIONALLY, BEFORE
//   the type gate below -- so a live NON-mother slot still consumes one unit of budget.
//
//   TYPE GATE (0x0048fe4b-0x0048fe95): cfg_buildings[building_id].type == BUILDING_TYPE_A_MOTHER
//   (0x06, checked first) OR == BUILDING_TYPE_H_MOTHER (0x1a, checked second, only if the first
//   missed). Neither this gate nor the energy gate above ever tests building_id != 0 -- occupancy is
//   implied ENTIRELY by energy > 0 (an empty slot has energy == 0.0 from being memset, so it self-
//   excludes via the energy gate, not via a building_id check) -- confirmed by grepping the whole
//   asm body for any CMP against building_id itself: there is none.
//
//   MATCH (0x0048fe97-0x0048fecd): writes `*out_x = (uint32_t)building.x`, `*out_y =
//   (uint32_t)building.y` (both MOVZX BYTE, zero-extended -- x/y are uint8_t fields) and returns 1,
//   WITHOUT visiting any further slot (straight fallthrough into the write-and-return tail from
//   either type-match branch, then JMP 0x0048fee4 past the exhausted-path assignment).
//
//   EXHAUSTED (0x0048fedd, reached only via the LAB_0048fe17 loop-bound exit): result = 0; *out_x and
//   *out_y are NEVER referenced on this path (EDX/EBX, the out-pointers, are not touched between
//   0x0048fedd and the RET) -- the not-found path leaves the caller's buffers UNTOUCHED, not zeroed.
//
#include "sim/sim_bldg_find_mothership_position.h"

#include <limits>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER / BUILDING_TYPE_H_MOTHER
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint32_t kSentinel = 0x0badf00d;

// Returns the function's own result; out_x/out_y are seeded to kSentinel by the caller-visible
// helper below so an untouched-on-no-match claim is actually checked, not assumed.
uint32_t run_find(sim_fixture &fx, int32_t player, uint32_t &out_x, uint32_t &out_y) {
    out_x = kSentinel;
    out_y = kSentinel;
    return detail::bldg_find_mothership_position(fx.view(), player, &out_x, &out_y);
}

} // namespace

void run_bldg_find_mothership_position_tests() {
    sim_fixture fx;
    uint32_t    out_x = kSentinel, out_y = kSentinel;

    // ---- S1: basic match, and the decrement-before-type-check ORDER doesn't change the outcome when
    // it doesn't need to -- slot 1 is a live NON-mother (consumes budget, no match), slot 2 is the
    // live MOTHER that gets found. count=2 covers exactly both slots. x != y so a coordinate swap is
    // caught. ------------------------------------------------------------------------------------
    fx.reset();
    fx.b(0, 0).energy         = 2.0;
    fx.b(0, 1).energy         = 1.0;
    fx.b(0, 1).building_id    = 10;
    fx.cfg_buildings[10].type = 3; // not a mother type
    fx.b(0, 2).energy         = 1.0;
    fx.b(0, 2).building_id    = 11;
    fx.b(0, 2).x              = 37;
    fx.b(0, 2).y              = 91;
    fx.cfg_buildings[11].type = BUILDING_TYPE_A_MOTHER;
    ck_eq(run_find(fx, 0, out_x, out_y), 1, "S1: mother found at slot 2 after a live non-mother at slot 1");
    ck_eq(out_x, 37, "S1: out_x == slot 2's x");
    ck_eq(out_y, 91, "S1: out_y == slot 2's y");

    // ---- S2: the COUNT gates the walk -- two live non-mother slots exhaust remaining=2 before a
    // mother placed at slot 3 is ever visited. The loop-bound check happens at the TOP of the next
    // iteration (0x0048fe0b), so slot 3 is never entered at all: return 0, out params untouched. -----
    fx.reset();
    fx.b(0, 0).energy         = 2.0;
    fx.b(0, 1).energy         = 1.0;
    fx.b(0, 1).building_id    = 10;
    fx.cfg_buildings[10].type = 3;
    fx.b(0, 2).energy         = 1.0;
    fx.b(0, 2).building_id    = 12;
    fx.cfg_buildings[12].type = 4;
    fx.b(0, 3).energy         = 1.0; // would be alive+mother if ever reached
    fx.b(0, 3).building_id    = 13;
    fx.b(0, 3).x              = 55;
    fx.b(0, 3).y              = 66;
    fx.cfg_buildings[13].type = BUILDING_TYPE_A_MOTHER;
    ck_eq(run_find(fx, 0, out_x, out_y), 0, "S2: budget exhausted by slots 1-2 before reaching slot 3's mother");
    ck_eq(out_x, kSentinel, "S2: out_x left untouched on the no-match path");
    ck_eq(out_y, kSentinel, "S2: out_y left untouched on the no-match path");

    // ---- S3: energy<=0.0 (boundary: EXACTLY 0.0) is skipped WITHOUT decrementing remaining and
    // WITHOUT even reaching the type gate -- slot 1 is a mother-typed building with energy==0.0 and
    // must NOT be matched (would report slot 1's coords if the gate were wrong); slot 2 is the real
    // live mother, still reachable because slot 1 never touched the count=1 budget. -----------------
    fx.reset();
    fx.b(0, 0).energy         = 1.0;
    fx.b(0, 1).energy         = 0.0; // boundary: <= 0.0, must be skipped entirely
    fx.b(0, 1).building_id    = 14;
    fx.b(0, 1).x              = 12;
    fx.b(0, 1).y              = 34;
    fx.cfg_buildings[14].type = BUILDING_TYPE_A_MOTHER; // would match if the gate were wrong
    fx.b(0, 2).energy         = 1.0;
    fx.b(0, 2).building_id    = 15;
    fx.b(0, 2).x              = 78;
    fx.b(0, 2).y              = 21;
    fx.cfg_buildings[15].type = BUILDING_TYPE_A_MOTHER;
    ck_eq(run_find(fx, 0, out_x, out_y), 1, "S3: zero-energy slot 1 skipped, budget untouched, slot 2 matches");
    ck_eq(out_x, 78, "S3: out_x == slot 2's x, NOT slot 1's (12)");
    ck_eq(out_y, 21, "S3: out_y == slot 2's y, NOT slot 1's (34)");

    // ---- S4: the SECOND subtype, BUILDING_TYPE_H_MOTHER(0x1a), matches too (checked second in the
    // asm, only reached once A_MOTHER's CMP misses). ------------------------------------------------
    fx.reset();
    fx.b(0, 0).energy         = 1.0;
    fx.b(0, 1).energy         = 1.0;
    fx.b(0, 1).building_id    = 16;
    fx.b(0, 1).x              = 5;
    fx.b(0, 1).y              = 200;
    fx.cfg_buildings[16].type = BUILDING_TYPE_H_MOTHER;
    ck_eq(run_find(fx, 0, out_x, out_y), 1, "S4: H_MOTHER (0x1a) matches");
    ck_eq(out_x, 5, "S4: out_x == the H_MOTHER's x");
    ck_eq(out_y, 200, "S4: out_y == the H_MOTHER's y");

    // ---- S5: a live NON-mother type never matches -- it still consumes the one unit of budget
    // (count=1), so the walk correctly returns 0 rather than continuing to look further (there is
    // nothing further to look at here, but this proves the decrement-then-miss path terminates the
    // walk via the SAME loop-bound check S2 exercises, not some separate "found nothing of this type,
    // keep going forever" bug). ------------------------------------------------------------------
    fx.reset();
    fx.b(0, 0).energy         = 1.0;
    fx.b(0, 1).energy         = 1.0;
    fx.b(0, 1).building_id    = 17;
    fx.cfg_buildings[17].type = 9; // neither 0x06 nor 0x1a
    ck_eq(run_find(fx, 0, out_x, out_y), 0, "S5: a live non-mother type never matches");
    ck_eq(out_x, kSentinel, "S5: out_x untouched");
    ck_eq(out_y, kSentinel, "S5: out_y untouched");

    // ---- S6: player-row isolation -- player 0's row has a live non-mother at slot 1 (so it walks,
    // finds nothing), player 1's row has a live MOTHER at the SAME slot index. Scanning player 0 must
    // not see player 1's mother, and scanning player 1 must read its OWN row correctly. --------------
    fx.reset();
    fx.b(0, 0).energy         = 1.0;
    fx.b(0, 1).energy         = 1.0;
    fx.b(0, 1).building_id    = 18;
    fx.cfg_buildings[18].type = 2; // not a mother
    fx.b(1, 0).energy         = 1.0;
    fx.b(1, 1).energy         = 1.0;
    fx.b(1, 1).building_id    = 19;
    fx.b(1, 1).x              = 44;
    fx.b(1, 1).y              = 88;
    fx.cfg_buildings[19].type = BUILDING_TYPE_A_MOTHER;
    ck_eq(run_find(fx, 0, out_x, out_y), 0, "S6: player 0's own row has no mother (isolation from player 1)");
    ck_eq(out_x, kSentinel, "S6: player 0 out_x untouched");
    ck_eq(out_y, kSentinel, "S6: player 0 out_y untouched");
    ck_eq(run_find(fx, 1, out_x, out_y), 1, "S6: player 1's own row IS found when scanning player 1");
    ck_eq(out_x, 44, "S6: player 1 out_x == its own building's x");
    ck_eq(out_y, 88, "S6: player 1 out_y == its own building's y");

    // ---- S7: NaN energy is treated as ALIVE -- the asm's gate is `!(energy <= 0.0)` (FLDZ/FCOMP/
    // FNSTSW/SAHF/JNC), and an unordered compare (NaN involved) sets the same flags as "not less",
    // so JNC falls through to "alive" exactly as it does for a genuine positive energy. This is the
    // case that would fail if the gate were instead written `0.0 < energy` (false for NaN either way,
    // so that form would WRONGLY skip this slot). ---------------------------------------------------
    fx.reset();
    fx.b(0, 0).energy         = 1.0;
    fx.b(0, 1).energy         = std::numeric_limits<double>::quiet_NaN();
    fx.b(0, 1).building_id    = 20;
    fx.b(0, 1).x              = 9;
    fx.b(0, 1).y              = 99;
    fx.cfg_buildings[20].type = BUILDING_TYPE_A_MOTHER;
    ck_eq(run_find(fx, 0, out_x, out_y), 1, "S7: NaN energy is treated as alive (matches !(energy<=0.0))");
    ck_eq(out_x, 9, "S7: out_x == the NaN-energy mother's x");
    ck_eq(out_y, 99, "S7: out_y == the NaN-energy mother's y");
}

} // namespace mh::sim::test
