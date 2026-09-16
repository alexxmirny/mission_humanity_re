//
// sim_unit_change_proto_and_energy_selftest.cpp -- `simtest` cases for
// llm_strat_unit_change_proto_and_energy @0x00489521 (sim/sim_unit_change_proto_and_energy.h/.cpp,
// SIM1-G1).
//
// SCOPE: this function has NO conditional gates and NO loop -- it is three unconditional steps in a
// straight line, so "both sides of every gate" reduces to exercising the two arithmetic ops across
// their interesting ranges plus the one outward call:
//   1. (0x00489540-0x00489556) `unit_proto_id += proto_delta` via `ADD word ptr [...],AX` -- a
//      SAME-WIDTH 16-bit wraparound add (the destination is a 16-bit memory operand and the source
//      is AX, the low 16 bits of the spilled BX param; the instruction is a word op regardless of
//      what garbage sits in EAX's upper 16 bits). Covered across: an in-range add (T1), an in-range
//      subtract (T2), a zero delta that must not corrupt a nonzero seed (T3), a positive-direction
//      wrap past 0xffff (T4), and a negative-direction wrap past 0x0000 (T5) -- NONE of these clamp
//      or saturate; there is no CMP/Jcc anywhere near this block, so any clamping in the .cpp would
//      be a fabrication.
//   2. (0x0048955d-0x0048957f) `energy += energy_delta` -- a plain x87 `FLD`/`FADD double ptr
//      [EBP+0x8]`/`FSTP`, both operands `double`, no rounding/clamping. Covered by T1/T2/T3/T6
//      (positive, negative, zero, and a case that drives energy negative -- again, no clamp).
//   3. (0x00489582-0x0048958b) `llm_strat_unit_soldiers_start_walk_anim(player, unit_idx)` -- an
//      unconditional single call, `MOV EDX,[EBP-0x14]` (unit_idx) then `MOVZX EAX,[EBP-0x10]`
//      (player) feeding the __watcall EAX/EDX slots in that order. Covered by every case (exact
//      args, not swapped -- player and unit_idx are chosen distinct so a swap is observable) and,
//      in T1c, that the call observes the roster ALREADY updated (the two field writes at
//      0x00489556/0x00489579 both precede the call at 0x00489586 in straight-line order, with no
//      branch that could reorder them).
// Also covered: the fourth parameter (`unused`, ECX) is genuinely dead per the header's derivation
// (the prologue at 0x00489537-0x0048953d only spills EAX/EDX/EBX; ECX is never read) -- T7 proves it
// by varying `unused` alone and asserting identical output. And non-corruption (T8): every other
// field on the touched unit, a neighbouring index of the same player, and a different player at the
// same index all read back exactly as seeded.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_change_proto_and_energy_00489521.asm -- every assertion below cites
// the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_change_proto_and_energy.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorder for the single outward callee --------------------------------------------------
// Snapshots the touched unit's unit_proto_id/energy AT CALL TIME (via g_active_fx), so a case can
// prove the two field writes are already committed before the call fires -- not merely that the
// call happens at all.
sim_fixture *g_active_fx = nullptr;

struct WalkAnimCall {
    uint32_t player;
    int32_t  index;
    uint16_t proto_id_at_call; // snapshot of units[player][index].unit_proto_id when this fired
    double   energy_at_call;   // snapshot of units[player][index].energy when this fired
};
std::vector<WalkAnimCall> g_walk_anim_calls;

void rec_unit_soldiers_start_walk_anim(uint32_t player, int32_t unit_index) {
    WalkAnimCall c;
    c.player = player;
    c.index  = unit_index;
    if (g_active_fx != nullptr) {
        const unit &u      = g_active_fx->u((int32_t)player, unit_index);
        c.proto_id_at_call = u.unit_proto_id;
        c.energy_at_call   = u.energy;
    } else {
        c.proto_id_at_call = 0;
        c.energy_at_call   = 0.0;
    }
    g_walk_anim_calls.push_back(c);
}

const unit_change_proto_and_energy_calls g_calls = {
    &rec_unit_soldiers_start_walk_anim,
};

void reset_observations() { g_walk_anim_calls.clear(); }

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// each run so a wrong-index write lands somewhere observable. Distinct player AND distinct index
// from every main/neighbour slot used below (all of those stay < player 5, index 50).
constexpr uint16_t GUARD_PLAYER = 7;
constexpr int32_t  GUARD_INDEX  = 90;

void seed_guard_slot(sim_fixture &fx) {
    unit &g          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id  = 4242;
    g.energy         = 8888.5;
    g.state          = 0x11;
    g.order          = 0x22;
    g.path_slot_id   = 0x33;
    g.pending_damage = 13.75;
    g.rotation_clock = 55.5;
}

// ---- fixture seeding -------------------------------------------------------------------------
struct Seed {
    uint16_t player = 3;
    int32_t  index  = 41;

    uint16_t start_proto_id = 1000;
    int16_t  proto_delta    = 250;

    double start_energy = 12.5;
    double energy_delta = 7.25;

    int32_t unused = 0;

    // Sentinel-seeded neighbouring fields on the SAME unit -- must read back unchanged (this
    // function's writes are only unit_proto_id and energy).
    uint16_t state          = 0x66;
    uint16_t order          = 0x77;
    uint8_t  path_slot_id   = 0x88;
    double   pending_damage = 44.25;
    double   rotation_clock = 99.125;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.start_proto_id;
    u.energy         = s.start_energy;
    u.state          = s.state;
    u.order          = s.order;
    u.path_slot_id   = s.path_slot_id;
    u.pending_damage = s.pending_damage;
    u.rotation_clock = s.rotation_clock;

    // Neighbouring slots this call must never touch: same player, next index; and a different
    // player at the SAME index -- both seeded with their own distinct sentinels.
    unit &neighbor_same_player         = fx.u(s.player, s.index + 1);
    neighbor_same_player.unit_proto_id = 5151;
    neighbor_same_player.energy        = 1234.5;

    unit &neighbor_same_index         = fx.u(s.player + 1, s.index);
    neighbor_same_index.unit_proto_id = 6161;
    neighbor_same_index.energy        = 4321.5;

    g_active_fx = &fx;
    reset_observations();

    sim_store own = fx.store();
    detail::unit_change_proto_and_energy(own, g_calls, s.player, s.index, s.proto_delta, s.unused,
                                         s.energy_delta);
}

} // namespace

void run_unit_change_proto_and_energy_tests() {
    sim_fixture fx;

    // =============================================================================================
    // T1 -- basic in-range add on both fields (0x00489540-0x00489556 proto_id, 0x0048955d-0x00489579
    // energy), plus the outward call with EXACT, non-swapped args (player=3 != unit_idx=41), plus
    // (T1c) that the call observes the roster ALREADY updated -- the writes at 0x00489556/0x00489579
    // precede the call at 0x00489586 with no branch to reorder them.
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 1000;
        s.proto_delta    = 250;
        s.start_energy   = 12.5;
        s.energy_delta   = 7.25;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.unit_proto_id, 1250u,
              "T1a: unit_proto_id == 1000+250 (0x00489540 MOVZX/0x00489544 IMUL row/0x00489556 ADD word)");
        ck_eq_d(u.energy, 19.75, "T1b: energy == 12.5+7.25 (0x00489570 FLD/0x00489576 FADD/0x00489579 FSTP)");

        ck(g_walk_anim_calls.size() == 1 &&
               g_walk_anim_calls[0].player == 3 && g_walk_anim_calls[0].index == 41,
           "T1: unit_soldiers_start_walk_anim(player=3, unit_idx=41) fires exactly once, args NOT "
           "swapped (0x0048957f MOV EDX,unit_idx / 0x00489582 MOVZX EAX,player / 0x00489586 CALL)");

        ck(g_walk_anim_calls[0].proto_id_at_call == 1250 && g_walk_anim_calls[0].energy_at_call == 19.75,
           "T1c: at call time the roster already shows the POST-update proto_id/energy -- the two "
           "field writes (0x00489556, 0x00489579) precede the call (0x00489586) in straight-line "
           "order, no branch can reorder them");
    }

    // =============================================================================================
    // T2 -- in-range SUBTRACT on both fields (negative proto_delta, negative energy_delta), still
    // within each field's natural range (no wrap involved here -- that is T4/T5's job).
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 1000;
        s.proto_delta    = -100;
        s.start_energy   = 50.0;
        s.energy_delta   = -12.25;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.unit_proto_id, 900u,
              "T2a: unit_proto_id == 1000-100 (0x00489556 ADD word with a negative AX operand)");
        ck_eq_d(u.energy, 37.75, "T2b: energy == 50.0-12.25 (0x00489576 FADD with a negative operand)");
    }

    // =============================================================================================
    // T3 -- ZERO delta on both fields. This is a real separator, not a no-op case: a translation
    // that mistakenly ASSIGNED instead of ACCUMULATED (`unit_proto_id = proto_delta` /
    // `energy = energy_delta`) would zero out a nonzero seed here; the true `+=` semantics leave the
    // seed exactly unchanged.
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 785;
        s.proto_delta    = 0;
        s.start_energy   = 42.75;
        s.energy_delta   = 0.0;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.unit_proto_id, 785u,
              "T3a: proto_delta==0 leaves unit_proto_id==785 unchanged (accumulate, not assign)");
        ck_eq_d(u.energy, 42.75, "T3b: energy_delta==0.0 leaves energy==42.75 unchanged (accumulate, not assign)");
    }

    // =============================================================================================
    // T4 -- 16-bit wraparound, POSITIVE direction: unit_proto_id starts near 0xffff and the add
    // carries past it. `ADD word ptr [...],AX` (0x00489556) wraps mod 0x10000 with NO clamp/
    // saturation (there is no CMP/Jcc anywhere in this function) -- a translation that clamped at
    // 0xffff instead of wrapping would disagree here.
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 65500; // 0xffdc
        s.proto_delta    = 100;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        // (65500 + 100) mod 65536 == 64
        ck_eq((uint32_t)u.unit_proto_id, 64u,
              "T4: unit_proto_id wraps 65500+100 -> 64 mod 0x10000 (0x00489556 ADD word, unclamped -- "
              "NOT saturated at 0xffff)");
    }

    // =============================================================================================
    // T5 -- 16-bit wraparound, NEGATIVE direction: unit_proto_id starts small and a negative delta
    // underflows past 0x0000. Same instruction, same "no clamp" property -- a translation that
    // clamped at 0 instead of wrapping would disagree here.
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 10;
        s.proto_delta    = -50;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        // (10 - 50) mod 65536 == 65496 (0xffd8)
        ck_eq((uint32_t)u.unit_proto_id, 65496u,
              "T5: unit_proto_id underflows 10-50 -> 65496 mod 0x10000 (0x00489556 ADD word, unclamped "
              "-- NOT saturated at 0)");
    }

    // =============================================================================================
    // T6 -- energy driven negative. The x87 FADD at 0x00489576 is a plain add with no clamp/floor;
    // a translation that floored energy at 0 (confusing this HP-like stat's usual apply_damage-side
    // clamping with THIS function, which has none) would disagree here.
    // =============================================================================================
    {
        Seed s;
        s.player       = 3;
        s.index        = 41;
        s.start_energy = 5.0;
        s.energy_delta = -20.0;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        ck_eq_d(u.energy, -15.0,
                "T6: energy == 5.0-20.0 == -15.0, unclamped (0x00489576 FADD has no companion CMP/"
                "MAX -- this function does not floor energy at 0)");
    }

    // =============================================================================================
    // T7 -- the fourth parameter (`unused`, ECX) is genuinely dead. The prologue (0x00489537-
    // 0x0048953d) only spills EAX/EDX/EBX to the stack; ECX is never read again anywhere in the
    // function body. Two runs identical except for `unused` must produce identical results.
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 500;
        s.proto_delta    = 17;
        s.start_energy   = 3.0;
        s.energy_delta   = 1.5;

        s.unused = 0;
        seed_and_run(fx, s);
        const uint16_t proto_a  = fx.u(s.player, s.index).unit_proto_id;
        const double   energy_a = fx.u(s.player, s.index).energy;

        s.unused = (int32_t)0x7fffffff;
        seed_and_run(fx, s);
        const uint16_t proto_b  = fx.u(s.player, s.index).unit_proto_id;
        const double   energy_b = fx.u(s.player, s.index).energy;

        ck(proto_a == proto_b && energy_a == energy_b,
           "T7: `unused` (ECX) has zero effect on unit_proto_id/energy -- the prologue never spills "
           "or reads it (0x00489537-0x0048953d only stores EAX/EDX/EBX)");
        ck(g_walk_anim_calls.size() == 1 && g_walk_anim_calls[0].player == 3 &&
               g_walk_anim_calls[0].index == 41,
           "T7: the outward call's args are unaffected by `unused` too");
    }

    // =============================================================================================
    // T8 -- non-corruption. This function writes exactly two fields (unit_proto_id, energy) on
    // exactly one roster slot (player, unit_idx). Every OTHER field on that unit, a neighbouring
    // index of the same player, a different player at the same index, and the far guard slot must
    // all read back exactly as seeded.
    // =============================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 41;
        s.start_proto_id = 111;
        s.proto_delta    = 22;
        s.start_energy   = 6.0;
        s.energy_delta   = 3.0;
        seed_and_run(fx, s);

        const unit &u = fx.u(s.player, s.index);
        ck(u.state == s.state, "T8: touched unit's state untouched (this function never writes it)");
        ck(u.order == s.order, "T8: touched unit's order untouched");
        ck(u.path_slot_id == s.path_slot_id, "T8: touched unit's path_slot_id untouched");
        ck(u.pending_damage == s.pending_damage, "T8: touched unit's pending_damage untouched");
        ck(u.rotation_clock == s.rotation_clock, "T8: touched unit's rotation_clock untouched");

        const unit &nbr_same_player = fx.u(s.player, s.index + 1);
        ck(nbr_same_player.unit_proto_id == 5151 && nbr_same_player.energy == 1234.5,
           "T8: neighbouring index (same player, index+1) untouched");

        const unit &nbr_same_index = fx.u(s.player + 1, s.index);
        ck(nbr_same_index.unit_proto_id == 6161 && nbr_same_index.energy == 4321.5,
           "T8: neighbouring player (player+1, same index) untouched");

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 4242 && g.energy == 8888.5 && g.state == 0x11 && g.order == 0x22 &&
               g.path_slot_id == 0x33 && g.pending_damage == 13.75 && g.rotation_clock == 55.5,
           "T8: far guard slot untouched");
    }
}

} // namespace mh::sim::test
