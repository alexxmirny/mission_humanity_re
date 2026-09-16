//
// sim_combat_conquest_credit_selftest.cpp -- `simtest` cases for
// llm_combat_credit_planet_conquest_kills @0x0049928a (sim/sim_combat_credit_planet_conquest_kills.h)
//
// SCOPE (honest, not exhaustive): the FULL CONTROL-FLOW SHAPE (witness scan -> outer guard -> single-
// enemy scan -> credit-one-enemy-then-return), every branch on both sides (self-skip, dead/absent
// enemy slot, wrong-planet enemy, buildings_alive-vs-units_alive OR short-circuit and its priority,
// no-witness early return, no-alive-enemy), the x87 `energy>0.0` gate at its three sites (witness
// scan, unit-credit loop, building-credit loop) with boundary + NaN coverage, the exact killer_info
// bit pattern, the exact argument tuple of every unit_kill_credit/bldg_kill_credit call, and the
// units-before-buildings / ascending-index call order via one shared trace.
//
// DOES NOT COVER: the internals of llm_strat_unit_kill_credit / llm_strat_bldg_kill_credit themselves
// (covered by sim_weapon_damage_selftest.cpp) -- this file only pins what THIS function passes to
// them and WHEN. Does not exercise `player` values outside 0..MAX_PLAYERS-1 (the only domain the
// fixture's row-major unit/building storage can address, and the only domain the real caller ever
// passes -- see MAX_PLAYERS=8 in sim/sim_state.h), so the killer_info OR/MOVZX bit-pattern is proven
// exact for that domain (0x0049939e OR AL,0x80 / 0x004993a0 MOVZX EAX,AX) but not stress-tested with
// bits above bit 15 (unreachable without corrupting the fixture's own storage).
//
// EVERY EXPECTED VALUE BELOW WAS DERIVED FROM THE RAW DISASSEMBLY
// (tmp/decomp/llm_combat_credit_planet_conquest_kills_0049928a.asm), NOT from the .cpp under test or
// its header banner -- though both were cross-checked against, and agree with, the independent
// derivation below. Every case cites the instruction address(es) it pins. The four-step shape, for
// reference:
//   (1) 0x004992a5-0x004992e6  witness scan: first index 1..99 of `player`'s own units with
//       energy>0.0 (the x87 FLDZ/FCOMP/FNSTSW/SAHF/JNC idiom -- JNC taken == energy<=0.0 skip,
//       fallthrough == found+break).
//   (2) 0x004992ea/0x00499416  outer guard: witness==0 (sentinel, never assigned) -> return NOW.
//   (3) 0x004992f4-0x0049935e  single-enemy scan: first index 0..7 that is != player, ALIVE
//       (status_flags & 0x2), and has buildings_alive[planet]>0 OR units_alive[planet]>0
//       (buildings checked first, short-circuited).
//   (4) 0x00499363-0x0049940f  THAT ONE ENEMY ONLY: credit every energy>0.0 unit (damage 2000.0),
//       then every energy>0.0 building (damage 5000.0), killer_info=(player&0xffff)|0x80,
//       killer_unit_index=the step-(1) witness -- then RETURN (0x0049940f->0x00499416), never
//       resuming the enemy scan for a second match.
//
#include "sim/sim_combat_credit_planet_conquest_kills.h"

#include <cmath>
#include <limits>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;

// ---- shared trace: one combined record per outward call, in call order --------------------------
// A single vector (not two per-callee ones) is deliberate: it is what lets one assertion pin BOTH
// call order (units before buildings, ascending index within each) AND the argument tuple together.
struct CreditCall {
    bool     is_bldg;
    uint32_t victim_player;
    int32_t  victim_index;
    double   damage;
    uint32_t killer_info;
    int32_t  killer_unit_index;
};
std::vector<CreditCall> g_calls;

void rec_unit_kill_credit(uint32_t victim_player, int32_t victim_unit_index, double damage,
                          uint32_t killer_info, int32_t killer_unit_index) {
    g_calls.push_back({false, victim_player, victim_unit_index, damage, killer_info, killer_unit_index});
}
void rec_bldg_kill_credit(uint32_t victim_player, int32_t victim_building_index, double damage,
                          uint32_t killer_info, int32_t killer_unit_index) {
    g_calls.push_back({true, victim_player, victim_building_index, damage, killer_info, killer_unit_index});
}

const combat_credit_planet_conquest_kills_calls &rec_calls() {
    static const combat_credit_planet_conquest_kills_calls c = {
        &rec_unit_kill_credit,
        &rec_bldg_kill_credit,
    };
    return c;
}

// Bounds-safe access, same posture as sim_weapon_damage_selftest.cpp's reg_at/scale_at: a mutation
// that makes the body call FEWER times than a case expects must turn into a FAILED CHECK (a sentinel
// no real call can match), not an out-of-range read.
CreditCall call_at(size_t i) {
    static const CreditCall none = {false, 0xffffffffu, -1, -1.0, 0xffffffffu, -1};
    return i < g_calls.size() ? g_calls[i] : none;
}
size_t n_unit_calls() {
    size_t n = 0;
    for (auto &c : g_calls)
        if (!c.is_bldg) ++n;
    return n;
}
size_t n_bldg_calls() {
    size_t n = 0;
    for (auto &c : g_calls)
        if (c.is_bldg) ++n;
    return n;
}
void clear_calls() { g_calls.clear(); }

// player_profile.status_flags bit1 = ALIVE -- 0x0049931d TEST byte ptr [...+0xcff060],0x2.
constexpr uint32_t ALIVE_BIT = 0x2u;
void               set_alive(sim_fixture &f, uint32_t p, bool alive) {
    if (alive)
        f.profiles[p].status_flags |= ALIVE_BIT;
    else
        f.profiles[p].status_flags &= ~ALIVE_BIT;
}

// Distinct from the fixture's own default planet_index (0), so a hardcoded-0 read is caught.
constexpr int32_t PLANET       = 3;
constexpr int32_t OTHER_PLANET = 5;

// ==== (1) witness scan (0x004992a5-0x004992e6) ======================================================

void test_witness_is_first_positive_energy_unit_and_flows_to_every_call() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;

    // idx1 energy<=0 (skipped); idx2 AND idx3 both energy>0 -- witness must be 2 (FIRST hit), not 3.
    // 0x004992d3-0x004992de is the gate, 0x004992e0/0x004992e3 assigns witness=idx, 0x004992e6 JMPs
    // straight out of the loop (break) -- idx3 is never even reached.
    f.u((int32_t)PLAYER, 1).energy = 0.0;
    f.u((int32_t)PLAYER, 2).energy = 5.0;
    f.u((int32_t)PLAYER, 3).energy = 7.0;

    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 1).energy             = 9.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 1u, "witness-first: exactly one credit call fires");
    ck_eq((uint32_t)call_at(0).killer_unit_index, 2u,
          "witness-first: killer_unit_index == witness == FIRST positive-energy unit (2, not 3) -- "
          "0x004992e0/0x004992e6 breaks the scan on the first hit");
}

void test_witness_scan_upper_bound_index_99_is_reachable() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    // The last index the 1..99 loop visits (0x004992b3 CMP idx,0x64 / JL) -- an off-by-one upper
    // bound would miss it.
    f.u((int32_t)PLAYER, 99).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 1).energy             = 1.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 1u, "witness-upper-bound: index 99 is reachable (inclusive 1..99)");
    ck_eq((uint32_t)call_at(0).killer_unit_index, 99u, "witness-upper-bound: killer_unit_index == 99");
}

void test_witness_scan_energy_boundary() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 0.0;  // exactly at the threshold -- NOT a witness
    f.u((int32_t)PLAYER, 2).energy = -0.5; // just below -- NOT a witness
    f.u((int32_t)PLAYER, 3).energy = 0.5;  // just above -- becomes the witness

    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 4).energy             = 1.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 1u, "witness-boundary: exactly one credit call");
    ck_eq((uint32_t)call_at(0).killer_unit_index, 3u,
          "witness-boundary: witness == index 3 -- energy==0.0 (idx1) and energy<0.0 (idx2) both fail "
          "the 0x004992d3-0x004992de gate (!(energy<=0.0)), energy=0.5 (idx3) passes");
}

void test_witness_scan_nan_energy_is_treated_as_positive_by_the_hardware_gate() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = std::numeric_limits<double>::quiet_NaN();

    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 2).energy             = 1.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    // FCOMP against a NaN sets C0=1 (unordered), the SAME bit pattern as "0.0 < energy" -- so JNC at
    // 0x004992de is NOT taken, and the idiom reads a NaN-energy record as "alive". This is the exact
    // reason the correct spelling is !(energy<=0.0), not energy>0.0 (which is FALSE for NaN under
    // IEEE754 -- a naive translation would disagree with the hardware and fail this case).
    ck_eq((uint32_t)g_calls.size(), 1u,
          "witness-nan: NaN energy at index 1 STILL becomes the witness (x87 unordered -> C0=1, JNC "
          "not taken, same as energy>0.0)");
    ck_eq((uint32_t)call_at(0).killer_unit_index, 1u, "witness-nan: witness == index 1 (the NaN unit)");
}

// ==== (2) outer guard (0x004992ea/0x00499416) =========================================================

void test_no_witness_returns_before_scanning_any_enemy() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    // player has NO unit with energy>0 anywhere in 1..99 (all fixture units default-zeroed).
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 1).energy             = 9.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 0u,
          "no-witness: witness_unit_index stays 0 -> 0x004992ea CMP/JZ 0x00499416 returns "
          "IMMEDIATELY, before the enemy scan (steps 3/4) ever runs, despite a fully qualifying "
          "enemy existing -- this is a REAL early return, not a fall-through to an empty scan");
}

// ==== (3) single-enemy scan (0x004992f4-0x0049935e) ====================================================

void test_two_alive_enemies_only_the_first_is_credited() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY_A = 1, ENEMY_B = 2;
    f.u((int32_t)PLAYER, 1).energy = 1.0; // witness = 1

    set_alive(f, ENEMY_A, true);
    f.profiles[ENEMY_A].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY_A, 5).energy             = 10.0; // exactly one credited unit

    set_alive(f, ENEMY_B, true);
    f.profiles[ENEMY_B].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY_B, 7).energy             = 10.0; // WOULD be credited if the scan ever reached it

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 1u,
          "two-enemies: exactly ONE credit call total -- 0x0049940f->0x00499416 returns the instant "
          "the FIRST matching enemy's two loops finish, never looping back to LAB_004992fb to "
          "consider ENEMY_B even though ENEMY_B fully qualifies");
    ck_eq(call_at(0).victim_player, ENEMY_A, "two-enemies: the credited victim_player is ENEMY_A (1)");
    ck_eq((uint32_t)call_at(0).victim_index, 5u, "two-enemies: victim_index is ENEMY_A's unit 5");
    ck_eq((uint32_t)n_bldg_calls(), 0u, "two-enemies: ENEMY_A has no alive building -- 0 building calls");
}

void test_dead_or_absent_first_enemy_slot_is_skipped() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY_1 = 1, ENEMY_2 = 2;
    f.u((int32_t)PLAYER, 1).energy = 1.0;

    // Slot 1: NOT alive (status_flags bit 0x2 clear) despite an otherwise-matching count -- the
    // 0x0049931d TEST / 0x00499324 JNZ falls through to the skip path (LAB_00499326) without ever
    // reaching the buildings_alive/units_alive check.
    set_alive(f, ENEMY_1, false);
    f.profiles[ENEMY_1].buildings_alive[PLANET] = 1;

    set_alive(f, ENEMY_2, true);
    f.profiles[ENEMY_2].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY_2, 4).energy             = 10.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 1u, "dead-first-slot: exactly one credit call");
    ck_eq(call_at(0).victim_player, ENEMY_2,
          "dead-first-slot: the dead/not-alive slot 1 is skipped -- credit lands on ENEMY_2");
}

void test_enemy_matching_only_on_a_different_planet_is_skipped() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;

    set_alive(f, ENEMY, true);
    // Positive counts ONLY on OTHER_PLANET, zero on the CURRENT planet -- 0x0049932f and 0x00499349
    // both re-read G_PLANET_INDEX fresh, so this pins that BOTH counters are indexed by the SAME
    // current-planet slot, not the enemy's off-planet counts and not a hardcoded planet 0.
    f.profiles[ENEMY].buildings_alive[OTHER_PLANET] = 5;
    f.profiles[ENEMY].units_alive[OTHER_PLANET]     = 5;
    f.profiles[ENEMY].buildings_alive[PLANET]       = 0;
    f.profiles[ENEMY].units_alive[PLANET]           = 0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 0u,
          "wrong-planet: enemy alive+matching only on a DIFFERENT planet index -- 0x00499339/"
          "0x00499353 both test the CURRENT G_PLANET_INDEX (3) slot -- skipped");

    // Same enemy, now ALSO given a positive count on the CURRENT planet -- must now match.
    clear_calls();
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 6).energy             = 3.0;
    sim_view v2                               = f.view();
    detail::combat_credit_planet_conquest_kills(v2, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 1u,
          "wrong-planet: the SAME enemy now matches once its CURRENT-planet buildings_alive is positive");
}

void test_no_alive_enemy_at_all_credits_nothing() {
    sim_fixture f;
    clear_calls();
    f.planet_index                 = PLANET;
    const uint32_t PLAYER          = 0;
    f.u((int32_t)PLAYER, 1).energy = 1.0; // a valid witness is present

    // Every other profile: default not-alive (status_flags memset to 0 by reset()), zero counts
    // everywhere -- the loop-exhausted path 0x00499301->0x00499416 (no enemy in 0..7 ever matches).
    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 0u,
          "no-enemy: no enemy is alive+matching anywhere -- 0x00499301 JMP 0x00499416, nothing credited");
}

void test_self_is_skipped_even_when_it_would_otherwise_match() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 2, REAL_ENEMY = 5;
    f.u((int32_t)PLAYER, 1).energy = 1.0;

    // PLAYER's own profile is alive with a positive buildings_alive[PLANET] -- would satisfy the
    // match test at 0x00499339 if it were ever reached, but 0x00499311 CMP EAX,[player] / 0x00499314
    // JZ 0x00499326 skips index==player UNCONDITIONALLY, before that check is even attempted.
    set_alive(f, PLAYER, true);
    f.profiles[PLAYER].buildings_alive[PLANET] = 1;

    set_alive(f, REAL_ENEMY, true);
    f.profiles[REAL_ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)REAL_ENEMY, 3).energy             = 2.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 1u, "self-skip: exactly one credit call");
    ck_eq(call_at(0).victim_player, REAL_ENEMY,
          "self-skip: the credited enemy is REAL_ENEMY (5), never PLAYER itself (2) -- self-skip at "
          "0x00499311-0x00499314 is unconditional, not gated on alive/counts");
}

void test_match_gate_buildings_alive_alone_suffices() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1; // > 0
    f.profiles[ENEMY].units_alive[PLANET]     = 0; // irrelevant -- short-circuited
    f.u((int32_t)ENEMY, 2).energy             = 3.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 1u,
          "or-buildings: buildings_alive>0 alone (0x00499339 CMP / 0x00499340 JG) matches without "
          "ever consulting units_alive");
}

void test_match_gate_units_alive_alone_suffices_when_buildings_alive_is_not_positive() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 0; // not > 0 -- falls through to the 2nd check
    f.profiles[ENEMY].units_alive[PLANET]     = 1; // > 0
    f.u((int32_t)ENEMY, 2).energy             = 3.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 1u,
          "or-units: buildings_alive<=0 -> 0x00499342-0x0049935a re-checks units_alive; units_alive>0 "
          "(0x0049935a JLE not taken) still matches");
}

void test_match_gate_neither_counter_positive_skips() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1, REAL_ENEMY = 2;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 0;
    f.profiles[ENEMY].units_alive[PLANET]     = 0;
    // A second, genuinely qualifying enemy at a higher index proves the OR-false case merely
    // continues the loop rather than aborting it.
    set_alive(f, REAL_ENEMY, true);
    f.profiles[REAL_ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)REAL_ENEMY, 9).energy             = 3.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 1u, "or-neither: exactly one credit call");
    ck_eq(call_at(0).victim_player, REAL_ENEMY,
          "or-neither: neither counter positive (0x0049935a JLE 0x0049935e) -> skip to next enemy, "
          "which is REAL_ENEMY");
}

void test_enemy_scan_upper_bound_index_7_is_reachable() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 7; // MAX_PLAYERS-1, the last index the loop visits
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 2).energy             = 3.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)g_calls.size(), 1u,
          "enemy-upper-bound: enemy index 7 (MAX_PLAYERS-1) is reached -- 0x004992fb CMP idx,8 / JL "
          "includes 7");
    ck_eq(call_at(0).victim_player, ENEMY, "enemy-upper-bound: credited enemy is index 7");
}

// ==== (4) the credit sweep: gates, order, and the exact argument tuple (0x00499363-0x0049940f) =======

void test_unit_credit_loop_energy_boundary() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;

    f.u((int32_t)ENEMY, 2).energy = 0.0;  // exactly at the threshold -- not credited
    f.u((int32_t)ENEMY, 3).energy = -0.5; // just below -- not credited
    f.u((int32_t)ENEMY, 4).energy = 0.5;  // just above -- credited

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)n_unit_calls(), 1u,
          "unit-boundary: exactly one unit credited -- the 0x0049938a-0x00499395 gate (identical "
          "idiom to the witness scan) excludes energy==0.0 and energy<0.0, includes energy>0.0");
    ck_eq((uint32_t)call_at(0).victim_index, 4u, "unit-boundary: the credited unit is index 4 (energy 0.5)");
}

void test_building_credit_loop_energy_boundary() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    // No alive units for this enemy -- isolates the building loop entirely.

    f.b((int32_t)ENEMY, 2).energy = 0.0;
    f.b((int32_t)ENEMY, 3).energy = -0.5;
    f.b((int32_t)ENEMY, 4).energy = 0.5;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)n_unit_calls(), 0u, "bldg-boundary: no unit has positive energy -- 0 unit calls");
    ck_eq((uint32_t)n_bldg_calls(), 1u,
          "bldg-boundary: exactly one building credited -- the 0x004993e0-0x004993eb gate mirrors "
          "the unit one");
    ck_eq((uint32_t)call_at(0).victim_index, 4u, "bldg-boundary: the credited building is index 4 (energy 0.5)");
}

void test_unit_loop_excludes_index_zero() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;

    f.u((int32_t)ENEMY, 0).energy = 9.0; // index 0 -- MUST be excluded (loop starts at 1, 0x00499363)
    f.u((int32_t)ENEMY, 1).energy = 0.0; // skipped on the energy gate
    f.u((int32_t)ENEMY, 2).energy = 9.0; // credited

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)n_unit_calls(), 1u, "unit-lower-bound: exactly one unit credited");
    ck_eq((uint32_t)call_at(0).victim_index, 2u,
          "unit-lower-bound: index 0 (energy 9.0) is NEVER visited -- 0x00499363 MOV [idx],1 starts "
          "the unit loop at 1");
}

void test_building_loop_excludes_index_zero() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0;
    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    // No alive units -- isolate the building loop.

    f.b((int32_t)ENEMY, 0).energy = 9.0; // index 0 -- MUST be excluded (0x004993b9 starts idx at 1)
    f.b((int32_t)ENEMY, 1).energy = 0.0; // skipped on the energy gate
    f.b((int32_t)ENEMY, 2).energy = 9.0; // credited

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());
    ck_eq((uint32_t)n_bldg_calls(), 1u, "bldg-lower-bound: exactly one building credited");
    ck_eq((uint32_t)call_at(0).victim_index, 2u,
          "bldg-lower-bound: index 0 (energy 9.0) is NEVER visited -- 0x004993b9 MOV [idx],1 starts "
          "the building loop at 1");
}

void test_call_order_units_ascending_then_buildings_ascending() {
    sim_fixture f;
    clear_calls();
    f.planet_index        = PLANET;
    const uint32_t PLAYER = 0, ENEMY = 1;
    f.u((int32_t)PLAYER, 1).energy = 1.0; // witness = 1

    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;

    // Set the alive slots in a scrambled order to prove traversal order comes from the LOOP
    // (ascending index 1..99), not insertion order.
    f.u((int32_t)ENEMY, 7).energy = 1.0;
    f.u((int32_t)ENEMY, 2).energy = 1.0;
    f.u((int32_t)ENEMY, 5).energy = 1.0;
    f.b((int32_t)ENEMY, 9).energy = 1.0;
    f.b((int32_t)ENEMY, 1).energy = 1.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 5u, "call-order: 3 units + 2 buildings = 5 calls total");
    const bool order_ok = g_calls.size() == 5 && !g_calls[0].is_bldg && g_calls[0].victim_index == 2 &&
                          !g_calls[1].is_bldg && g_calls[1].victim_index == 5 &&
                          !g_calls[2].is_bldg && g_calls[2].victim_index == 7 && g_calls[3].is_bldg &&
                          g_calls[3].victim_index == 1 && g_calls[4].is_bldg && g_calls[4].victim_index == 9;
    ck(order_ok,
       "call-order: order is unit(2),unit(5),unit(7),bldg(1),bldg(9) -- ASCENDING index within each "
       "loop (0x0049936a.../0x004993c0... plain 1..99 for-loops, not insertion order) and ALL units "
       "before ANY building (0x004993b9 starts the building loop only after the unit loop at "
       "0x0049936a-0x004993b7 fully exits)");

    bool witness_consistent = true, killer_info_consistent = true;
    for (auto &c : g_calls) {
        if (c.killer_unit_index != 1) witness_consistent = false;
        if (c.killer_info != 0x80u) killer_info_consistent = false; // player=0 -> (0&0xffff)|0x80 = 0x80
    }
    ck(witness_consistent,
       "call-order: killer_unit_index == the SAME witness (1) on every one of the 5 calls -- the "
       "witness is hoisted before the sweep, not re-derived per call");
    ck(killer_info_consistent,
       "call-order: killer_info == 0x80 (player=0 | 0x80) identically on every one of the 5 calls");
}

void test_full_argument_tuple_unit_and_building() {
    sim_fixture f;
    clear_calls();
    f.planet_index = PLANET;
    // player=5 -> killer_info = (5 & 0xffff) | 0x80 = 0x85 (0x0049939e OR AL,0x80 / 0x004993a0 MOVZX
    // EAX,AX) -- a nontrivial value distinct from both a bare 0x80 and a bare player id.
    const uint32_t PLAYER = 5, ENEMY = 2;
    f.u((int32_t)PLAYER, 11).energy = 1.0; // witness = 11

    set_alive(f, ENEMY, true);
    f.profiles[ENEMY].buildings_alive[PLANET] = 1;
    f.u((int32_t)ENEMY, 6).energy             = 1.0;
    f.b((int32_t)ENEMY, 8).energy             = 1.0;

    sim_view v = f.view();
    detail::combat_credit_planet_conquest_kills(v, PLAYER, rec_calls());

    ck_eq((uint32_t)g_calls.size(), 2u, "tuple: one unit call + one building call");

    const CreditCall uc = call_at(0);
    ck(!uc.is_bldg, "tuple: call 0 is the unit credit (units loop runs first)");
    ck_eq(uc.victim_player, ENEMY,
          "tuple: unit call victim_player == enemy index (0x004993ae MOVZX word ptr [enemy])");
    ck_eq((uint32_t)uc.victim_index, 6u, "tuple: unit call victim_index == 6");
    ck_eq_d(uc.damage, 2000.0,
            "tuple: unit damage == 2000.0 exactly (0x409f4000/0x0 double literal, 0x004993a4-0x004993a9)");
    ck_eq(uc.killer_info, 0x85u,
          "tuple: killer_info == (5 & 0xffff) | 0x80 == 0x85 (0x0049939e OR AL,0x80 / 0x004993a0 "
          "MOVZX EAX,AX)");
    ck_eq((uint32_t)uc.killer_unit_index, 11u, "tuple: unit call killer_unit_index == witness (11)");

    const CreditCall bc = call_at(1);
    ck(bc.is_bldg, "tuple: call 1 is the building credit (buildings loop runs second)");
    ck_eq(bc.victim_player, ENEMY, "tuple: building call victim_player == enemy index");
    ck_eq((uint32_t)bc.victim_index, 8u, "tuple: building call victim_index == 8");
    ck_eq_d(bc.damage, 5000.0,
            "tuple: building damage == 5000.0 exactly (0x40b38800/0x0 double literal, "
            "0x004993fa-0x004993ff)");
    ck_eq(bc.killer_info, 0x85u, "tuple: building call killer_info == the same 0x85 as the unit call");
    ck_eq((uint32_t)bc.killer_unit_index, 11u,
          "tuple: building call killer_unit_index == witness (11), same as the unit call");
}

} // namespace

void run_combat_conquest_credit_tests() {
    printf("-- llm_combat_credit_planet_conquest_kills --\n");

    test_witness_is_first_positive_energy_unit_and_flows_to_every_call();
    test_witness_scan_upper_bound_index_99_is_reachable();
    test_witness_scan_energy_boundary();
    test_witness_scan_nan_energy_is_treated_as_positive_by_the_hardware_gate();

    test_no_witness_returns_before_scanning_any_enemy();

    test_two_alive_enemies_only_the_first_is_credited();
    test_dead_or_absent_first_enemy_slot_is_skipped();
    test_enemy_matching_only_on_a_different_planet_is_skipped();
    test_no_alive_enemy_at_all_credits_nothing();
    test_self_is_skipped_even_when_it_would_otherwise_match();
    test_match_gate_buildings_alive_alone_suffices();
    test_match_gate_units_alive_alone_suffices_when_buildings_alive_is_not_positive();
    test_match_gate_neither_counter_positive_skips();
    test_enemy_scan_upper_bound_index_7_is_reachable();

    test_unit_credit_loop_energy_boundary();
    test_building_credit_loop_energy_boundary();
    test_unit_loop_excludes_index_zero();
    test_building_loop_excludes_index_zero();
    test_call_order_units_ascending_then_buildings_ascending();
    test_full_argument_tuple_unit_and_building();
}

} // namespace mh::sim::test
