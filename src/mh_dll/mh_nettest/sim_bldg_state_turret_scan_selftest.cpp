//
// sim_bldg_state_turret_scan_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_turret_scan
// (sim/sim_bldg_state_turret.h/.cpp, detail::bldg_state_turret_scan), SIM1-G4 building_tick
// machinery -- the turret's idle-scan/target-acquire state handler.
//
// SCOPE: this file covers ONLY llm_strat_bldg_state_turret_scan @0x00471ab1. Its sibling
// llm_strat_bldg_state_turret_attack @0x00471e53 (same TU, REVIEW_REQUIRED=true, its own tracking
// loop + fire gate) is NOT exercised here -- it needs its own selftest.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_turret_scan_00471ab1.asm), cross-checked against the .cpp/.h's
// own per-line address citations -- NOT read off the .cpp body alone:
//
//   `do { ... } while (tick_budget > 0.0 && !found_target)` (bottom-tested @0x00471dbf, always runs
//   >=1 pass). Each pass: if tick_budget < per_shot_cost, drain the remainder into last_tick_time and
//   zero tick_budget (0x00471d9a-0x00471db5), done. Else: advance the idle-scan sweep (aim_heading/
//   aim_step_dir, wrap 25->1 / 0->24, 0x00471b37-0x00471bf9), unconditionally decrement
//   acquire_retry_counter (0x00471bf9-0x00471c0c), and -- only when (acquire_retry_counter % 5) == 0
//   OR pending_damage > 0.0 (0x00471c25-0x00471c49) -- call turret_acquire_target: found -> stamp
//   counter_ref/counter_target_slot, state=TURRET_ATTACK(0x7b), notify_ui, break (0x00471c66-
//   0x00471cbd); not found -> if counter==0, flip aim_step_dir's sign and reseed BOTH
//   acquire_retry_seed ((old%40)+30) and acquire_retry_counter from that new value
//   (0x00471cfc-0x00471d80), else clear the loop-PERSISTENT has_pending_damage local (0x00471d82).
//   Then tick_budget -= per_shot_cost unconditionally (0x00471d89-0x00471d98). After the loop:
//   cur_building->anim[sprite_quantity - 1] = Building[bid].anim[sprite_quantity] + aim_heading - 1
//   (0x00471dd6-0x00471e49) -- the WRITE index and the READ index DIFFER, and there are TWO separate
//   -1s: the store's is folded into its 0x8f displacement (anim is at +0x93, so 0x8f == 0x93 - 4) and
//   the value's is the DEC EBX @0x00471e42. See TS-H for how reading only the mnemonics got this
//   backwards for two weeks.
//
#include "sim/sim_bldg_state_turret.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- turret_acquire_target: call recorder + return-value knobs ---------------------------------
struct AcquireCall {
    uint32_t player;
    int32_t  building_index;
};
std::vector<AcquireCall> g_acquire_calls;
uint32_t                 g_acquire_found       = 0;
uint32_t                 g_acquire_target_ref  = 0;
uint32_t                 g_acquire_target_slot = 0;

uint32_t rec_turret_acquire_target(uint32_t player, int32_t building_index, uint32_t *out_ref, uint32_t *out_slot) {
    g_acquire_calls.push_back({player, building_index});
    *out_ref  = g_acquire_target_ref;
    *out_slot = g_acquire_target_slot;
    return g_acquire_found;
}

// ---- bldg_notify_ui: call recorder ---------------------------------------------------------------
struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    g_notify_calls.push_back({player, index});
}

const bldg_state_turret_scan_calls g_calls = {
    &rec_turret_acquire_target,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_acquire_calls.clear();
    g_notify_calls.clear();
    g_acquire_found       = 0;
    g_acquire_target_ref  = 0;
    g_acquire_target_slot = 0;
}

// Same frame_at() shape the .cpp uses internally (frame_at/set_frame_at), re-derived locally for
// this test's own read-back checks -- not shared cross-TU, per this project's convention.
int32_t anim_slot(const uint8_t (&anim)[48], int32_t slot) {
    int32_t v;
    std::memcpy(&v, &anim[slot * 4], sizeof(v));
    return v;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 2;
    int32_t  index   = 5;
    uint16_t cfg_row = 9;
    uint8_t  sub_id  = 4; // turret slot = turrets[player][cur_building->sub_id], NOT building index

    double per_shot_cost  = 5.0;
    double tick_budget    = 5.0;
    double last_tick_time = 50.0;
    double pending_damage = 0.0;

    int32_t aim_heading           = 10;
    int32_t aim_step_dir          = 1;
    int32_t acquire_retry_seed    = 999;
    int32_t acquire_retry_counter = 8; // decrements to 7, 7%5==2 -- gate does NOT fire by default

    uint8_t sprite_quantity          = 6;
    int32_t cfg_anim_value           = 7000; // Building[cfg_row].anim[sprite_quantity]
    int32_t bldg_anim_prev_value     = 3000; // cur_building->anim[sprite_quantity] BEFORE the call
    int32_t bldg_anim_neighbor_value = 4000; // cur_building->anim[sprite_quantity-1] BEFORE the call

    uint32_t acquire_found       = 0;
    uint32_t acquire_target_ref  = 0;
    uint32_t acquire_target_slot = 0;

    uint16_t initial_state = 0xBEEF; // sentinel, distinct from TURRET_ATTACK(0x7b)
};

turret &turret_of(sim_fixture &fx, const Seed &s) {
    return fx.turrets[(size_t)s.player * (size_t)TURRETS_PER_PLAYER + (size_t)s.sub_id];
}

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.sub_id         = s.sub_id;
    b.pending_damage = s.pending_damage;
    b.last_tick_time = s.last_tick_time;
    b.state          = s.initial_state;
    std::memset(b.anim, 0, sizeof(b.anim));
    {
        int32_t prevv = s.bldg_anim_prev_value;
        std::memcpy(&b.anim[(size_t)s.sprite_quantity * 4], &prevv, sizeof(prevv));
        if (s.sprite_quantity > 0) {
            int32_t neighborv = s.bldg_anim_neighbor_value;
            std::memcpy(&b.anim[(size_t)(s.sprite_quantity - 1) * 4], &neighborv, sizeof(neighborv));
        }
    }

    cfg_building &cb   = fx.cfg_buildings[s.cfg_row];
    cb.per_shot_cost   = s.per_shot_cost;
    cb.sprite_quantity = s.sprite_quantity;
    std::memset(cb.anim, 0, sizeof(cb.anim));
    {
        int32_t av = s.cfg_anim_value;
        std::memcpy(&cb.anim[(size_t)s.sprite_quantity * 4], &av, sizeof(av));
    }

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;
    fx.tick_budget      = s.tick_budget;

    turret &t               = turret_of(fx, s);
    t.aim_heading           = s.aim_heading;
    t.aim_step_dir          = s.aim_step_dir;
    t.acquire_retry_seed    = s.acquire_retry_seed;
    t.acquire_retry_counter = s.acquire_retry_counter;
    t.counter_ref           = 0xCECE; // sentinel, overwritten only on the found arm
    t.counter_target_slot   = -12345; // sentinel, overwritten only on the found arm

    reset_observations();
    g_acquire_found       = s.acquire_found;
    g_acquire_target_ref  = s.acquire_target_ref;
    g_acquire_target_slot = s.acquire_target_slot;

    sim_store own = fx.store();
    detail::bldg_state_turret_scan(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_turret_scan_tests() {
    sim_fixture fx;

    // =================================================================================================
    // TS-A -- insufficient budget on the FIRST pass (tick_budget < per_shot_cost from the start,
    // JC @0x00471b31 into the insufficient-budget arm 0x00471d9a-0x00471db5). The do-while's own
    // bottom-tested condition (0x00471dbf) then reads tick_budget==0 and exits: exactly one pass, no
    // sweep, no acquire attempt -- but the shared animation-frame tail still runs (unconditional).
    // =================================================================================================
    {
        Seed s;
        s.per_shot_cost  = 10.0;
        s.tick_budget    = 4.0; // < per_shot_cost
        s.last_tick_time = 50.0;
        seed_and_run(fx, s);

        ck_eq_d(fx.tick_budget, 0.0,
                "TS-A: tick_budget zeroed by the insufficient-budget arm (0x00471dab-0x00471db5)");
        ck_eq_d(fx.b(s.player, s.index).last_tick_time, 46.0,
                "TS-A: last_tick_time -= remaining tick_budget (0x00471da5-0x00471da8): 50.0-4.0==46.0");
        ck(g_acquire_calls.empty(),
           "TS-A: turret_acquire_target never called -- the sweep/gate block (0x00471b37+) is unreached");
        ck(g_notify_calls.empty(), "TS-A: bldg_notify_ui never called");
        ck_eq((uint32_t)turret_of(fx, s).aim_heading, (uint32_t)s.aim_heading,
              "TS-A: aim_heading UNCHANGED -- the insufficient-budget arm skips the sweep entirely");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.initial_state,
              "TS-A: state UNCHANGED (0x7b is only written on a target-found pass)");
        ck_eq((uint32_t)anim_slot(fx.b(s.player, s.index).anim, s.sprite_quantity - 1),
              (uint32_t)(s.cfg_anim_value + s.aim_heading - 1),
              "TS-A: shared animation-frame tail still fires on the insufficient-budget pass "
              "(0x00471dd6-0x00471e49, unconditional after the loop) -- writing sprite_quantity-1");
    }

    // =================================================================================================
    // TS-B -- idle-scan sweep wrap 25->1 with aim_step_dir==1 (INC + CMP ...,0x19 @0x00471b53-
    // 0x00471ba5). Gate deliberately NOT firing (6%5==1, pending_damage==0) to isolate the sweep.
    // =================================================================================================
    {
        Seed s;
        s.aim_heading           = 24; // 0x18
        s.aim_step_dir          = 1;
        s.acquire_retry_counter = 7; // -> 6, 6%5==1
        s.pending_damage        = 0.0;
        seed_and_run(fx, s);

        ck_eq((uint32_t)turret_of(fx, s).aim_heading, 1u,
              "TS-B: aim_step_dir==1 sweep wraps 25->1 (0x00471b53-0x00471ba5)");
        ck_eq((uint32_t)turret_of(fx, s).acquire_retry_counter, 6u,
              "TS-B: acquire_retry_counter unconditionally decremented (0x00471bf9-0x00471c0c) even "
              "though the gate below does not fire");
        ck(g_acquire_calls.empty(),
           "TS-B: 6%5==1 and pending_damage==0 -- gate does not fire (0x00471c25-0x00471c49)");
    }

    // =================================================================================================
    // TS-C -- idle-scan sweep wrap 0->24 with aim_step_dir!=1 (DEC + CMP ...,0x0 @0x00471ba7-
    // 0x00471bf9).
    // =================================================================================================
    {
        Seed s;
        s.aim_heading           = 1;
        s.aim_step_dir          = -1; // any value != 1 takes the decrement arm
        s.acquire_retry_counter = 7;  // -> 6, 6%5==1 -- gate does not fire
        s.pending_damage        = 0.0;
        seed_and_run(fx, s);

        ck_eq((uint32_t)turret_of(fx, s).aim_heading, 24u,
              "TS-C: aim_step_dir!=1 sweep wraps 0->24 (0x00471ba7-0x00471bf9)");
    }

    // =================================================================================================
    // TS-D -- acquire-attempt gate firing via the mod-5 cadence ALONE (pending_damage==0). Target not
    // found, counter(10)!=0 after the decrement -- the else arm (has_pending_damage=false, 0x00471d82)
    // does not touch the counter/seed.
    // =================================================================================================
    {
        Seed s;
        s.acquire_retry_counter = 11; // -> 10, 10%5==0
        s.pending_damage        = 0.0;
        s.acquire_found         = 0;
        seed_and_run(fx, s);

        ck(g_acquire_calls.size() == 1,
           "TS-D: mod-5 cadence alone fires the gate (IDIV 5 / TEST EDX,EDX / JZ @0x00471c2a-0x00471c49)");
        if (g_acquire_calls.size() == 1)
            ck(g_acquire_calls[0].player == s.player && g_acquire_calls[0].building_index == s.index,
               "TS-D: turret_acquire_target(cur_player, cur_index, ...) (0x00471c4f-0x00471c5d)");
        ck_eq((uint32_t)turret_of(fx, s).acquire_retry_counter, 10u,
              "TS-D: not-found with counter(10)!=0 -- no reseed, counter stays at its post-decrement value");
        ck(g_notify_calls.empty() && (uint32_t)fx.b(s.player, s.index).state == (uint32_t)s.initial_state,
           "TS-D: acquire miss -- no state change / notify_ui");
    }

    // =================================================================================================
    // TS-E -- acquire-attempt gate firing via pending_damage>0.0 on a pass where the mod-5 cadence
    // ALONE would NOT fire (7%5==2) -- proves the OR, not an AND (0x00471aea / 0x00471c3f-0x00471c49).
    // =================================================================================================
    {
        Seed s;
        s.acquire_retry_counter = 8; // -> 7, 7%5==2 -- cadence alone would NOT fire
        s.pending_damage        = 1.0;
        s.acquire_found         = 0;
        seed_and_run(fx, s);

        ck(g_acquire_calls.size() == 1,
           "TS-E: pending_damage>0.0 fires the gate even though 7%5!=0 -- the OR condition, not an AND");
    }

    // =================================================================================================
    // TS-E2 -- has_pending_damage is a loop-PERSISTENT local (0x00471aea/0x00471b13, cleared once at
    // 0x00471d82 and never reset per pass -- the earlier mis-modelling this translation's own header
    // documents correcting). Two passes, cadence never fires either pass (8%5==3, then 7%5==2): pass 1
    // fires via pending_damage, clears the local on its miss; pass 2 must NOT fire again.
    // =================================================================================================
    {
        Seed s;
        s.tick_budget           = 10.0; // two full per_shot_cost=5.0 passes
        s.acquire_retry_counter = 9;    // pass1: ->8 (8%5=3); pass2: ->7 (7%5=2) -- cadence never fires
        s.pending_damage        = 3.0;  // has_pending_damage starts true
        s.acquire_found         = 0;    // both (hypothetical) attempts report not-found
        seed_and_run(fx, s);

        ck(g_acquire_calls.size() == 1,
           "TS-E2: has_pending_damage fires pass 1's attempt then is cleared (0x00471d82) and STAYS "
           "cleared for pass 2 -- only ONE of the two passes attempts an acquire, proving the flag "
           "persists across do-while iterations rather than re-reading b.pending_damage each pass");
        ck_eq((uint32_t)turret_of(fx, s).acquire_retry_counter, 7u,
              "TS-E2: two unconditional decrements (9->8->7), neither miss reseeds (8!=0, 7!=0)");
    }

    // =================================================================================================
    // TS-F -- target FOUND: counter_ref/counter_target_slot stamped, state->TURRET_ATTACK(0x7b),
    // notify_ui fires, and the do-while terminates on found_target rather than on budget exhaustion
    // (tick_budget deliberately left large to prove this).
    // =================================================================================================
    {
        Seed s;
        s.tick_budget           = 100.0; // large -- must NOT be drained to 0
        s.per_shot_cost         = 5.0;
        s.acquire_retry_counter = 6; // -> 5, 5%5==0 -- gate fires
        s.pending_damage        = 0.0;
        s.acquire_found         = 1;
        s.acquire_target_ref    = 0x1BEEF; // > 16 bits: exercises the (uint16_t) truncation
        s.acquire_target_slot   = 424242;
        seed_and_run(fx, s);

        ck_eq_d(fx.tick_budget, 95.0,
                "TS-F: tick_budget -= per_shot_cost fires exactly ONCE (0x00471d89-0x00471d98) then the "
                "loop breaks on found_target -- 100.0-5.0==95.0, NOT drained to 0, proves the break "
                "ended the do-while, not budget exhaustion");
        ck_eq((uint32_t)turret_of(fx, s).counter_ref, 0xBEEFu,
              "TS-F: counter_ref = (uint16_t)target_ref (0x00471c79-0x00471c7c): 0x1BEEF truncates to 0xBEEF");
        ck_eq((uint32_t)turret_of(fx, s).counter_target_slot, 424242u,
              "TS-F: counter_target_slot = (int32_t)target_slot (0x00471c96-0x00471c99)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x7bu,
              "TS-F: cur_building->state = TURRET_ATTACK (0x7b) (0x00471c9f-0x00471ca4)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == s.player &&
               g_notify_calls[0].index == (uint32_t)s.index,
           "TS-F: bldg_notify_ui(cur_player, cur_index) fires on the found arm (0x00471caa-0x00471cb8)");
    }

    // =================================================================================================
    // TS-G -- target NOT found with acquire_retry_counter reaching exactly 0 after the decrement (which
    // also satisfies 0%5==0, so the gate fires one last time): aim_step_dir's sign flips, and BOTH
    // acquire_retry_seed/acquire_retry_counter reseed via (old%40)+30 -- seeded with 45 so the mod-40
    // genuinely changes the value (45 -> (45%40)+30 == 35, not 45 unchanged).
    // =================================================================================================
    {
        Seed s;
        s.acquire_retry_counter = 1; // -> 0
        s.pending_damage        = 0.0;
        s.acquire_found         = 0; // miss
        s.acquire_retry_seed    = 45;
        s.aim_step_dir          = 1;
        seed_and_run(fx, s);

        ck(g_acquire_calls.size() == 1,
           "TS-G: counter reaching exactly 0 still satisfies 0%5==0 -- one last acquire attempt fires "
           "(0x00471c2a-0x00471c49)");
        ck_eq((uint32_t)turret_of(fx, s).aim_step_dir, (uint32_t)-1,
              "TS-G: full-timeout arm flips aim_step_dir's sign (IMUL ...,-1 @0x00471cfc-0x00471d16): 1 -> -1");
        ck_eq((uint32_t)turret_of(fx, s).acquire_retry_seed, 35u,
              "TS-G: acquire_retry_seed = (old%40)+30 (0x00471d2f-0x00471d5b): (45%40)+30 == 35");
        ck_eq((uint32_t)turret_of(fx, s).acquire_retry_counter, 35u,
              "TS-G: acquire_retry_counter re-seeded from that SAME new value (0x00471d61-0x00471d7a)");
    }

    // =================================================================================================
    // TS-H -- the shared animation-frame tail's INDEX ASYMMETRY: the WRITE lands on
    // sprite_quantity-1 while the READ comes from sprite_quantity. Distinct sentinels on both slots,
    // so a wrong index is caught either way it could go wrong.
    //
    // THIS CASE PREVIOUSLY LOCKED IN THE OPPOSITE, WRONG ANSWER, and that is the reason to read the
    // rest of this comment. On 2026-08-22 a reimpl-verify pass concluded the write index carried no
    // -1, changed both turret bodies, and rewrote this case to enforce it -- so the oracle agreed
    // with the defect and could not fail. It took a 30k A/B (SIM-DEEP-DIV) to find: the promoted
    // closure diverged from all-original at step 3215 in `buildings`, and a raw region dump named 3
    // bytes in ONE turret's anim[] -- original [590,0,...], ours [589,590,...]. Same value, one slot
    // too high, slot 0 left stale.
    //
    // The asm, which settles it: `MOV dword ptr [EAX + 0x8f],EBX` (0x00471e43) stores through
    // displacement 0x8f while `anim` is at +0x93 in the record, so 0x8f == 0x93 - 4 IS the write's
    // -1, folded into the displacement. The `DEC EBX` @0x00471e42 is a SECOND, separate -1, the
    // value's. Both exist; the earlier pass found the DEC, correctly called it the value's, and
    // inferred the address had none. An offset folded into a displacement leaves no arithmetic
    // instruction to find -- check displacements against field offsets, not mnemonics.
    // =================================================================================================
    {
        Seed s;
        s.per_shot_cost            = 10.0;
        s.tick_budget              = 4.0; // insufficient-budget shape -- single pass, no sweep, isolates the tail
        s.sprite_quantity          = 5;
        s.cfg_anim_value           = 9001;   // Building.anim[5] -- the slot that MUST be read
        s.bldg_anim_prev_value     = 111111; // cur_building->anim[5] before -- MUST be left alone
        s.bldg_anim_neighbor_value = 555555; // cur_building->anim[4] before -- MUST be overwritten
        s.aim_heading              = 10;
        seed_and_run(fx, s);

        ck_eq((uint32_t)anim_slot(fx.b(s.player, s.index).anim, 4), 9010u,
              "TS-H: WRITE index is sprite_quantity-1 (slot 4), VALUE read from cfg.anim[5](9001) + "
              "aim_heading(10) - 1 == 9010 -- the store's 0x8f displacement is anim(+0x93) - 4");
        ck_eq((uint32_t)anim_slot(fx.b(s.player, s.index).anim, 5), 111111u,
              "TS-H: cur_building->anim[sprite_quantity] (slot 5) is UNTOUCHED -- it is the READ slot "
              "only. Writing here is the 2026-08-22 defect SIM-DEEP-DIV caught at step 3215");
    }
}

} // namespace mh::sim::test
