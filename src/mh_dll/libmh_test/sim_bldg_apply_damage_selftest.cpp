//
// sim_bldg_apply_damage_selftest.cpp -- `simtest` cases for llm_strat_bldg_apply_damage
// (sim/sim_bldg_apply_damage.h/.cpp) @0x004710ba, SIM1E -- the per-building damage-application state
// transition (top guard, unconditional damage subtraction, lethal-drop arm, unconditional tail).
//
// WHY THIS FILE IS THE ONLY EVIDENCE: llm_strat_bldg_state_destroyed (this function's lethal-arm
// callee) reaches the UI/dialog/chat-input/net-send cluster and runs FOR REAL under a shadow arm,
// double-firing all of it -- this function is permanently DO-NOT-ARM at runtime.
//
// SCOPE (honest, not exhaustive): this file covers the TOP GUARD (both no-op states), the exact
// LETHAL-DROP COMPARISON SENSE (strictly-survives / exactly-boundary / strictly-dies, plus the
// explicit clamp-to-0.0 on overshoot), the HQ ENERGY CREDIT (-1.0 to buildings[player][0].energy,
// proven to land on slot 0 of the OWNING player and nowhere else -- a neighbour slot and the next
// player's HQ are seeded with distinct sentinels and asserted untouched in every lethal case), the
// unconditional post-guard tail (charge-pips/refresh/notify-UI, which fires in BOTH the survive and
// lethal arms with no re-check of state in between), and full call order + argument tuples for every
// outward call via one shared trace.
//
// IT DOES NOT COVER bldg_main_base_damage_mult or the bldg_lost_feedback_cooldown_mother/_other pair.
// Those two boot constants are NOT read anywhere in this function's disassembly -- they belong to a
// DIFFERENT function, llm_strat_bldg_kill_credit (0x0044c6f1 / 0x0044c8dc / 0x0044c9cd; see
// sim_state.h's comment on that boot-constant run). Checked directly against the .asm below before
// writing this file; there is no FMUL-by-0.0001 and no cycle_progress/last_tick_time cooldown stamp
// anywhere in llm_strat_bldg_apply_damage's 0x109 bytes.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_apply_damage_004710ba.asm) -- NOT from the .cpp body. Full shape, every
// address cited so this file stays auditable:
//
//   0x004710d2-0x004710e8 (TOP GUARD): if state==DESTROYED(2) [CMP word[cur+0xd],2 @0x004710d7; JZ
//     0x004710dc taken] OR state==RUBBLE_SIGHT_DECAY(4) [CMP word[cur+0xd],4 @0x004710e3; JNZ
//     0x004710e8 NOT taken, i.e. falls straight into the same return] -- JMP straight to the epilogue
//     (LAB_004710ea -> 0x004711b9), skipping EVERY later instruction, including the unconditional
//     tail below. Zero outward calls, zero writes.
//   0x004710ef-0x00471116 (UNCONDITIONAL once past the guard): energy = energy - pending_damage
//     [FLD double[cur+0x21] (pending_damage); FSUBR double[cur+0x19] (energy) @0x004710fa-0x00471100
//     -- FSUBR computes mem_operand - ST(0), i.e. energy - pending_damage, stored back to energy; NOT
//     the reverse]; pending_damage = 0.0 [two dword-zero stores @0x00471108/0x0047110f, Watcom's
//     plain double-zero idiom, not two int32 writes].
//   0x0047111b-0x00471123 (THE LETHAL-DROP GATE, the single highest-value thing in this file): FLDZ;
//     FCOMP double[cur+0x19] (energy); FNSTSW AX; SAHF; JC 0x00471180 -- FCOMP sets CF=1 iff
//     ST(0)(0.0) < the energy operand, and JC (taken) SKIPS the lethal arm. So the lethal arm runs
//     when NOT(0.0 < energy), i.e. energy<=0.0 -- energy EXACTLY 0.0 (damage exactly equal to the
//     pre-damage energy) takes the LETHAL side, not the survive side (0.0 is not strictly < 0.0).
//   0x00471125-0x0047117b (LETHAL ARM, fallthrough -- no label, reached only by NOT taking the JC):
//     energy = 0.0 [two dword-zero stores @0x0047112a/0x00471131 -- an explicit CLAMP, distinct from
//     whatever negative value the subtraction actually produced on overshoot]; buildings[cur_player]
//     [0].energy += DAT_005012d4 [MOVZX EAX,cur_player @0x00471138; IMUL EAX,EAX,0x6aa4 @0x0047113f
//     (the per-player building-row stride, BUILDINGS_PER_PLAYER*sizeof(building)); FLD
//     double[EAX+0xc3d2b9] (that player's building slot 0's energy, the row-stride offset already
//     folded into the base constant); FADD double[0x005012d4] (DAT_005012d4 = -1.0, read-memory
//     confirmed); FSTP back @0x00471145-0x00471151 -- a per-death DECREMENT to the HQ/slot-0 record,
//     not a positive refund]; state = DESTROYED(2) [MOV word[cur+0xd],2 @0x0047115c]; then CALL
//     llm_strat_ai_notify_object_removed(flags=(cur_player|0x40) zero-extended, object_index=cur_index,
//     hard_remove=0) [XOR EBX,EBX @0x00471162; MOVZX EDX,cur_index @0x00471164; MOV AX,cur_player
//     @0x0047116b; OR AL,0x40 @0x00471171; MOVZX EAX,AX @0x00471173; CALL @0x00471176]; CALL
//     llm_strat_bldg_state_destroyed() [@0x0047117b, no visible args].
//   0x00471180-0x004711b9 (TAIL, UNCONDITIONAL): reached either by the JC that skipped the lethal arm
//     entirely OR by falling straight out of the lethal arm above (no re-check of state in between,
//     and no jump back out -- straight fallthrough) -- CALL
//     llm_strat_bldg_update_charge_pips(cur_player, cur_index) [@0x0047118e], CALL
//     llm_strat_refresh_building(cur_player, cur_index) [@0x004711a1], CALL
//     llm_strat_bldg_notify_ui(cur_player, cur_index) [@0x004711b4].
//
#include "sim/sim_bldg_apply_damage.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "ai/ai_state.h" // mh::ai::REF_BLDG_BIT -- the 0x40 flag bit OR'd into cur_player @0x00471171
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 5 callees ------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (5, one per bldg_apply_damage_calls member) --------------------------
struct NotifyRemovedCall {
    uint32_t flags;
    uint32_t object_index;
    int32_t  hard_remove;
};
std::vector<NotifyRemovedCall> g_notify_removed;
void                           rec_ai_notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    tr("ai_notify_object_removed");
    g_notify_removed.push_back({flags, object_index, hard_remove});
}

int  g_state_destroyed_calls = 0;
void rec_bldg_state_destroyed() {
    tr("bldg_state_destroyed");
    ++g_state_destroyed_calls;
}

// Shared shape for the three tail calls -- all three take (player, index) in that order.
struct PlayerIndexCall {
    uint16_t player;
    uint32_t index;
};
std::vector<PlayerIndexCall> g_charge_pips;
void                         rec_bldg_update_charge_pips(uint16_t player, uint32_t building_id) {
    tr("bldg_update_charge_pips");
    g_charge_pips.push_back({player, building_id});
}

std::vector<PlayerIndexCall> g_refresh;
void                         rec_refresh_building(uint16_t p_id, int32_t b_id) {
    tr("refresh_building");
    g_refresh.push_back({p_id, static_cast<uint32_t>(b_id)});
}

std::vector<PlayerIndexCall> g_notify_ui;
void                         rec_bldg_notify_ui(uint16_t player, uint32_t b_index) {
    tr("bldg_notify_ui");
    g_notify_ui.push_back({player, b_index});
}

const bldg_apply_damage_calls g_calls = {
    &rec_ai_notify_object_removed,
    &rec_bldg_state_destroyed,
    &rec_bldg_update_charge_pips,
    &rec_refresh_building,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_notify_removed.clear();
    g_state_destroyed_calls = 0;
    g_charge_pips.clear();
    g_refresh.clear();
    g_notify_ui.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player = 2;
    int32_t  index  = 5;         // nonzero -- distinct from the HQ slot (0) death credits, so a
                                 // slot mix-up (crediting the dying building instead of slot 0)
                                 // is detectable.
    uint16_t state          = 1; // "alive": neither DESTROYED(2) nor RUBBLE_SIGHT_DECAY(4)
    double   energy         = 100.0;
    double   pending_damage = 30.0;

    double hq_energy_seed       = 500.0; // buildings[player][0].energy -- the death-credit target
    double neighbor_energy_seed = 250.0; // buildings[player][1].energy -- must stay untouched
    double other_hq_energy_seed = 999.0; // buildings[other_player][0].energy -- must stay untouched
};

uint16_t other_player_of(const Seed &s) { return static_cast<uint16_t>((s.player + 1) % MAX_PLAYERS); }

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    fx.b(s.player, 0).energy           = s.hq_energy_seed;
    fx.b(s.player, 1).energy           = s.neighbor_energy_seed;
    fx.b(other_player_of(s), 0).energy = s.other_hq_energy_seed;

    building &b      = fx.b(s.player, s.index);
    b.state          = s.state;
    b.energy         = s.energy;
    b.pending_damage = s.pending_damage;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = static_cast<uint16_t>(s.index);

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_apply_damage(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_apply_damage_tests() {
    printf("-- llm_strat_bldg_apply_damage --\n");
    sim_fixture fx;

    // =================================================================================================
    // A -- THE TOP GUARD: an already-DESTROYED or RUBBLE_SIGHT_DECAY building takes NO further damage
    // processing at all -- the "nothing happens" arm a busy soak never isolates (0x004710d2-0x004710e8).
    // =================================================================================================
    {
        Seed s;
        s.state          = BLDG_STATE_DESTROYED; // 2
        s.energy         = 42.0;
        s.pending_damage = 17.0; // nonzero sentinel -- would be consumed if the guard failed to fire
        seed_and_run(fx, s);

        ck(trace_eq({}), "A1: state==DESTROYED(2) -- top guard fires ZERO outward calls (0x004710d7/dc)");
        ck_eq_d(fx.b(s.player, s.index).energy, s.energy, "A1: energy left untouched by the guard return");
        ck_eq_d(fx.b(s.player, s.index).pending_damage, s.pending_damage, "A1: pending_damage left untouched");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_DESTROYED),
              "A1: state left untouched (still DESTROYED)");
        ck_eq_d(fx.b(s.player, 0).energy, s.hq_energy_seed,
                "A1: HQ (slot 0) energy untouched -- lethal arm never reached");
        ck_eq_d(fx.b(s.player, 1).energy, s.neighbor_energy_seed, "A1: neighbor slot untouched");
        ck_eq_d(fx.b(other_player_of(s), 0).energy, s.other_hq_energy_seed, "A1: other player's HQ untouched");
    }
    {
        Seed s;
        s.player         = 3;
        s.index          = 6;
        s.state          = BLDG_STATE_RUBBLE_SIGHT_DECAY; // 4
        s.energy         = 8.0;
        s.pending_damage = 9.0;
        seed_and_run(fx, s);

        ck(trace_eq({}), "A2: state==RUBBLE_SIGHT_DECAY(4) -- top guard fires ZERO outward calls (0x004710e3/e8)");
        ck_eq_d(fx.b(s.player, s.index).energy, s.energy, "A2: energy left untouched");
        ck_eq_d(fx.b(s.player, s.index).pending_damage, s.pending_damage, "A2: pending_damage left untouched");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_RUBBLE_SIGHT_DECAY),
              "A2: state left untouched (still RUBBLE_SIGHT_DECAY)");
        ck_eq_d(fx.b(s.player, 0).energy, s.hq_energy_seed, "A2: HQ energy untouched");
    }

    // =================================================================================================
    // B -- SURVIVE ARM: damage strictly less than remaining energy. energy -= pending_damage lands
    // strictly positive; the FCOMP/JC gate (0x0047111b-0x00471123) takes JC and SKIPS the lethal arm.
    // Only the unconditional tail fires (0x00471180-0x004711b9); no HQ credit, no state change.
    // =================================================================================================
    {
        Seed s;
        s.state          = 1;
        s.energy         = 100.0;
        s.pending_damage = 30.0; // strictly < energy -> survives with energy == 70.0 exactly
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).energy, 70.0,
                "B1: energy -= pending_damage == 100.0-30.0 exactly (0x004710fa-0x00471100)");
        ck_eq_d(fx.b(s.player, s.index).pending_damage, 0.0,
                "B1: pending_damage zeroed unconditionally (0x00471108/0x0047110f)");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(s.state),
              "B1: state UNCHANGED -- lethal arm not taken");

        ck(g_notify_removed.empty(), "B1: ai_notify_object_removed does NOT fire on the survive arm");
        ck_eq(static_cast<uint32_t>(g_state_destroyed_calls), 0u, "B1: bldg_state_destroyed does NOT fire on the survive arm");
        ck_eq_d(fx.b(s.player, 0).energy, s.hq_energy_seed, "B1: HQ energy untouched -- no death, no credit");

        ck(trace_eq({"bldg_update_charge_pips", "refresh_building", "bldg_notify_ui"}),
           "B1: survive-arm call order -- only the unconditional tail, in order (0x0047118e/a1/b4)");
        ck(g_charge_pips.size() == 1 && g_charge_pips[0].player == s.player &&
               g_charge_pips[0].index == static_cast<uint32_t>(s.index),
           "B1: bldg_update_charge_pips(cur_player, cur_index)");
        ck(g_refresh.size() == 1 && g_refresh[0].player == s.player &&
               g_refresh[0].index == static_cast<uint32_t>(s.index),
           "B1: refresh_building(cur_player, cur_index)");
        ck(g_notify_ui.size() == 1 && g_notify_ui[0].player == s.player &&
               g_notify_ui[0].index == static_cast<uint32_t>(s.index),
           "B1: bldg_notify_ui(cur_player, cur_index)");
    }
    // B2 -- a small strictly-positive remainder, pinning the STRICT (not >=) sense of the gate from
    // the other direction: energy just barely above zero still takes the survive arm.
    {
        Seed s;
        s.energy         = 0.5;
        s.pending_damage = 0.0; // energy stays 0.5 -- strictly > 0.0
        seed_and_run(fx, s);
        ck_eq_d(fx.b(s.player, s.index).energy, 0.5, "B2: energy == 0.5 (unchanged, pending_damage was 0.0)");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(s.state),
              "B2: state unchanged -- 0.5 > 0.0 survives");
        ck(g_notify_removed.empty() && g_state_destroyed_calls == 0,
           "B2: no lethal calls when energy stays strictly positive");
    }

    // =================================================================================================
    // C -- THE LETHAL BOUNDARY: damage EXACTLY equal to remaining energy -> energy lands at EXACTLY
    // 0.0, and the FCOMP/JC gate takes the LETHAL side (JC is NOT taken when ST(0)(0.0) is not
    // strictly < energy, i.e. when energy<=0.0 -- equality falls through, it does not survive). This
    // is the single highest-value case in this file: a boundary-wrong translation passes every
    // non-exact test.
    // =================================================================================================
    {
        Seed s;
        s.energy         = 50.0;
        s.pending_damage = 50.0; // exact boundary: energy - pending_damage == 0.0 exactly
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).energy, 0.0, "C1: energy == 50.0-50.0 == 0.0 exactly");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_DESTROYED),
              "C1: BOUNDARY -- energy==0.0 takes the LETHAL arm (JC not taken; 0.0 is not < 0.0) -- "
              "state=DESTROYED(2) @0x0047115c");

        ck(g_notify_removed.size() == 1, "C1: ai_notify_object_removed fires exactly once on the boundary");
        if (g_notify_removed.size() == 1) {
            const auto &n = g_notify_removed[0];
            ck_eq(n.flags, static_cast<uint32_t>(s.player | mh::ai::REF_BLDG_BIT),
                  "C1: flags == cur_player | REF_BLDG_BIT(0x40) (0x0047116b-0x00471173)");
            ck_eq(n.object_index, static_cast<uint32_t>(s.index), "C1: object_index == cur_index (0x00471164)");
            ck_eq(static_cast<uint32_t>(n.hard_remove), 0u, "C1: hard_remove == 0 (XOR EBX,EBX @0x00471162)");
        }
        ck_eq(static_cast<uint32_t>(g_state_destroyed_calls), 1u, "C1: bldg_state_destroyed fires exactly once (0x0047117b)");

        ck_eq_d(fx.b(s.player, 0).energy, s.hq_energy_seed - 1.0,
                "C1: HQ credit -- buildings[player][0].energy += DAT_005012d4(-1.0) (0x00471145-0x00471151)");
        ck_eq_d(fx.b(s.player, 1).energy, s.neighbor_energy_seed,
                "C1: neighbor slot (player,1) untouched -- credit lands ONLY on slot 0");
        ck_eq_d(fx.b(other_player_of(s), 0).energy, s.other_hq_energy_seed,
                "C1: other player's HQ untouched -- credit lands ONLY on the OWNING player's row");

        ck(trace_eq({"ai_notify_object_removed", "bldg_state_destroyed", "bldg_update_charge_pips", "refresh_building",
                     "bldg_notify_ui"}),
           "C1: lethal-arm call order -- notify/state_destroyed BEFORE the unconditional tail, no re-check "
           "of state between them");
    }

    // =================================================================================================
    // D -- OVERSHOOT: damage strictly GREATER than remaining energy drives the subtraction negative,
    // but the lethal arm EXPLICITLY re-zeroes energy (0x0047112a/0x00471131) rather than leaving the
    // negative intermediate -- a translation that skips this clamp (since <=0.0 is already "dead")
    // would leave energy == -30.0, which this pins as wrong.
    // =================================================================================================
    {
        Seed s;
        s.energy         = 50.0;
        s.pending_damage = 80.0; // -> -30.0 before the clamp
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).energy, 0.0,
                "D1: energy CLAMPED to exactly 0.0, not left at the negative intermediate -30.0 "
                "(0x0047112a/0x00471131)");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_DESTROYED),
              "D1: state = DESTROYED(2) -- dies on overshoot");
        ck_eq(static_cast<uint32_t>(g_state_destroyed_calls), 1u, "D1: bldg_state_destroyed fires once");
        ck_eq_d(fx.b(s.player, 0).energy, s.hq_energy_seed - 1.0, "D1: HQ credit applied on overshoot death too");
    }

    // =================================================================================================
    // E -- ALREADY AT ZERO ENERGY ON ENTRY, ZERO PENDING DAMAGE: the gate only checks the RESULTING
    // energy, not whether any damage was actually applied this tick -- a building sitting at exactly
    // 0.0 energy dies on the very next call even with pending_damage==0.0. Distinct from the top-guard
    // "already dead" arm (A) -- here state has NOT yet been set to DESTROYED, so the guard at
    // 0x004710d7/e3 lets it through, and it is THIS call that kills it. This is the "nothing happens
    // going IN, but it still dies" case a busy soak drowns out alongside A's true no-op.
    // =================================================================================================
    {
        Seed s;
        s.energy         = 0.0;
        s.pending_damage = 0.0;
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).energy, 0.0, "E1: energy == 0.0-0.0 == 0.0 (no-op subtraction)");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_DESTROYED),
              "E1: energy already 0.0 on entry -- STILL takes the lethal arm (the gate checks the RESULT, "
              "not the delta)");
        ck_eq(static_cast<uint32_t>(g_state_destroyed_calls), 1u,
              "E1: bldg_state_destroyed fires -- a zero-damage tick can still kill");
        ck_eq_d(fx.b(s.player, 0).energy, s.hq_energy_seed - 1.0, "E1: HQ credit still applied");
    }

    // =================================================================================================
    // F -- PLAYER 0 / HQ-INDEX ARITHMETIC: the same lethal path with cur_player==0, ruling out a
    // translation that special-cased player 0 (IMUL player,0x6aa4 == 0 at player 0, so the HQ address
    // IS the row base with no offset -- a plausible off-by-a-multiply bug this isolates).
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 4;
        s.energy         = 10.0;
        s.pending_damage = 10.0; // boundary again, at player 0
        seed_and_run(fx, s);

        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_DESTROYED),
              "F1: dies at the boundary for player 0 too");
        ck_eq_d(fx.b(0, 0).energy, s.hq_energy_seed - 1.0,
                "F1: buildings[0][0].energy credited (player*0x6aa4 == 0 -> row base, no offset bug)");
        ck_eq_d(fx.b(s.player, 1).energy, s.neighbor_energy_seed, "F1: neighbor slot (player 0, index 1) untouched");
        ck_eq_d(fx.b(other_player_of(s), 0).energy, s.other_hq_energy_seed, "F1: player 1's HQ untouched");
    }
}

} // namespace mh::sim::test
