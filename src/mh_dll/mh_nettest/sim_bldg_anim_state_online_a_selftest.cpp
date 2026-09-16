//
// sim_bldg_anim_state_online_a_selftest.cpp -- `simtest` cases for FOUR of SIM1-BLDGCB batch H's
// verification-debt rows:
// llm_strat_bldg_anim_state_barracks_garage_a @0x00476627 (batch H, 2026-08-23)
// llm_strat_bldg_anim_state_airfield_a @0x00477881 (batch H, 2026-08-23)
//   llm_strat_bldg_anim_state_shuttle_a         @0x00477d9e (batch H closing slice, 2026-08-23)
//   llm_strat_bldg_anim_state_shuttle_h         @0x0047815f (batch H closing slice, 2026-08-23)
// All four sim/sim_bldg_anim_state_online.h/.cpp. All are shadow-armed (extra_regions:["buildings"],
// same manual declaration as their siblings) but got 0 calls under an all-AI soak (barracks_garage_a/
// airfield_a: 15000 then 40000 steps @1000%; shuttle_a/shuttle_h: 15000 steps @1000%, co-armed with the
// rest of batch H's closing-slice rows) -- SIM1-BLDGCB records the root cause for
// the first two as scenario shape (the soak lane is "_solo", a 1v1 with no alien-race AI player at all,
// so an A-race-only building callback structurally cannot fire regardless of step count); shuttle_a/_h
// most likely need a completed shuttle-bay building the same short game never queued (their "_h"-named
// -- wait, shuttle_h has no non-alien sibling; contrast vehicles_h/soldiers_h, which DID get real coverage
// in the very same run that left barracks_garage_a/airfield_a at 0 calls).
//
// SCOPE, HONESTLY NARROWER THAN THEIR SIBLING FILES -- read this before trusting a green run as full
// coverage: unlike llm_strat_bldg_anim_state_helipad (sim_bldg_anim_state_helipad_selftest.cpp), none of
// these four take an injectable `*_calls` struct -- their state-0 arm calls
// `mh::call::llm_strat_bldg_online_{barracks_garage_a,airfield_a,shuttle_a,shuttle_h}` DIRECTLY, a fixed
// VA that only exists inside the real game process (see e.g. sim_path_slot_dist_selftest.cpp's own note
// on why an offline oracle can never call a bare `mh::call::` target). This file therefore covers states
// 1-4 (plus shuttle_a/_h's extra states 10/11), the chain-follow branch and the `time==0.0` early return
// -- EVERY branch except the state-0 call-forward itself, which is NOT exercised here and remains
// unproven until either a real rig run reaches it (a 2v2+ multi-race scenario, or a scenario with a
// completed shuttle bay) or these functions get refactored to take an injectable call, mirroring the
// helipad precedent (not done here -- out of scope for these slices; the rig sites stay armed for the
// day a suitable scenario exists). Graded T2 in the ledger, not T1, for exactly this reason.
//
// CORRECTED 2026-09-06 -- read this before adding a case here. The shuttle_a/shuttle_h fixtures were
// SEEDED FROM THE TRANSLATION, not from the listing: `anim_dur = 96.25` with the comment
// "elapsed = (100-96.25)*4 = 15" bakes in a `* efficiency` factor that neither original body has (both
// use a bare FSUB -- no FMUL/FDIV by building+0x29 anywhere; see sim_bldg_anim_state_online.h's table).
// So when the defect was removed from the bodies, 22 checks here went red and read as evidence AGAINST
// the fix -- an oracle arguing for the bug it was seeded from. The seeds are now 85.0 (elapsed = 15 with
// no scaling) and two new efficiency-independence cases pin the quantity that discriminates. The lesson
// is general: an expected value that had to be COMPUTED to write is only as trustworthy as the formula
// used, and "cross-checked against the .cpp" is not a listing derivation.
//
// shuttle_a/shuttle_h EXPECTED VALUES from tmp/decomp_sim/llm_strat_bldg_anim_state_shuttle_{a,h}_*.asm
// (cross-checked against sim_bldg_anim_state_online.cpp, not re-derived here). Both are IDENTICAL in
// every cfg.anim[] index and state transition (only the fixed slot -- 3 for shuttle_a, 2 for shuttle_h
// -- and the online_<kind> callee differ), but each gets its OWN cases below: same mechanism, different
// instruction addresses, and "identical to its sibling" is exactly the kind of claim worth an
// independent check rather than an assumption. TWO extra terminal states (10, 11) beyond the 0-4 shape
// the other two functions in this file use -- state 10 resets online_state to 0 (no anim write), state
// 11 sets it to 0xc (no anim write); neither is reachable via the 0-4 chain, so each needs its own case.
//
// EVERY EXPECTED VALUE DERIVED FROM tmp/decomp_sim/llm_strat_bldg_anim_state_{barracks_garage_a,
// airfield_a}_*.asm (cross-checked against sim_bldg_anim_state_online.h's own derivation, not
// re-derived here). `time` is read ONCE before the while-loop and stays FIXED for the whole call even
// as the chain-walk changes `anim[slot]` (same as sim_bldg_anim_tick.h's dVar1/dVar2 note) -- every
// case that needs the switch to fire seeds `elapsed` in `(time, 2*time]` so the loop takes EXACTLY ONE
// over-time iteration (reaching the switch) before the remainder-spend arm ends it, so the asserted
// anim value is the switch's result, not overwritten by a later iteration.
//
#include "sim/sim_bldg_anim_state_online.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

inline void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value);
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}
inline int32_t anim_slot_get(const building &b, int32_t slot) {
    return static_cast<int32_t>(load_u32_le(&b.anim[slot * 4]));
}
inline void anim_slot_set(building &b, int32_t slot, int32_t value) {
    store_u32_le(&b.anim[slot * 4], static_cast<uint32_t>(value));
}
inline int32_t cfg_anim_slot_get(const cfg_building &cb, int32_t slot) {
    return static_cast<int32_t>(load_u32_le(&cb.anim[slot * 4]));
}
inline void cfg_anim_slot_set(cfg_building &cb, int32_t slot, int32_t value) {
    store_u32_le(&cb.anim[slot * 4], static_cast<uint32_t>(value));
}

constexpr int32_t PLAYER = 4;
constexpr int32_t BLDG   = 2;
constexpr uint8_t SUB_ID = 3;

void set_door_idle(sim_fixture &f, bool idle) {
    unit_storage &st = f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID];
    if (idle) {
        st.door_mutex_unit   = 0;
        st.door_waiter_count = 0;
    } else {
        st.door_mutex_unit   = 9; // nonzero -> busy
        st.door_waiter_count = 0;
    }
}

// ---- barracks_garage_a: slot fixed at 0. -----------------------------------------------------------

sim_fixture &bga_setup(sim_fixture &f, uint16_t online_state) {
    f.reset();
    f.view_cur_player  = PLAYER;
    f.view_cur_index   = BLDG;
    f.cur_building_ptr = &f.b(PLAYER, BLDG);
    building &b        = *f.cur_building_ptr;
    b.sub_id           = SUB_ID;
    b.efficiency       = 4.0;
    b.online_state     = online_state;
    return f;
}

// One-switch-iteration rig: anim[0]=FRAME, Anim[FRAME+1]={time=10,next=0}, anim_dur[0] chosen so
// elapsed lands in (10,20] -- exactly one over-time iteration, which reaches the switch (next==0).
constexpr int32_t FRAME = 40;
void              bga_arm_one_switch_iteration(sim_fixture &f) {
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 0, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 0;
    b.anim_dur[0]                 = 96.25; // elapsed = (100-96.25)*4 = 15, in (10,20]
    f.game_clock                  = 100.0;
}

void test_bga_time_zero_is_a_whole_function_noop() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 0, FRAME);
    f.anim_frames[FRAME + 1].time = 0.0; // triggers the early `if (time==0.0) return;`
    f.game_clock                  = 100.0;
    b.anim_dur[0]                 = 55.0; // sentinel
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(b.anim_dur[0] == 55.0,
       "barracks_garage_a: time==0.0 -> early return, anim_dur[0] stays the sentinel 55.0");
    ck(anim_slot_get(b, 0) == FRAME, "barracks_garage_a: time==0.0 -> anim[0] unchanged");
}

void test_bga_chain_follow_advances_by_next() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 0, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 7; // nonzero -> chain-follow, switch never reached
    b.anim_dur[0]                 = 96.25;
    f.game_clock                  = 100.0;
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(b, 0) == FRAME + 7,
       "barracks_garage_a: chain-follow -- anim[0] = FRAME + Anim[...].next (40+7=47)");
    ck_eq((uint32_t)b.online_state, 0u, "barracks_garage_a: chain-follow never reaches the switch -- online_state unchanged");
}

void test_bga_state1_door_idle_no_state_change() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/1);
    bga_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 2, 222);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 0) == 222, "barracks_garage_a state1 idle: anim[0] = cfg.anim[2]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "barracks_garage_a state1 idle: online_state UNCHANGED (stays 1)");
}

void test_bga_state1_door_busy_transitions_to_3() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/1);
    bga_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 333);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 0) == 333, "barracks_garage_a state1 busy: anim[0] = cfg.anim[3]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "barracks_garage_a state1 busy: online_state -> 3");
}

void test_bga_state2_door_idle_transitions_to_4() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/2);
    bga_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 4, 444);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 0) == 444, "barracks_garage_a state2 idle: anim[0] = cfg.anim[4]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "barracks_garage_a state2 idle: online_state -> 4");
}

void test_bga_state2_door_busy_no_state_change() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/2);
    bga_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 555);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 0) == 555, "barracks_garage_a state2 busy: anim[0] = cfg.anim[5]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "barracks_garage_a state2 busy: online_state UNCHANGED (stays 2)");
}

void test_bga_state3_unconditional_to_2() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/3);
    bga_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 500);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 0) == 500, "barracks_garage_a state3: anim[0] = cfg.anim[5]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "barracks_garage_a state3: online_state -> 2, unconditionally");
}

void test_bga_state4_unconditional_to_1() {
    sim_fixture f;
    bga_setup(f, /*online_state=*/4);
    bga_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 2, 200);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_barracks_garage_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 0) == 200, "barracks_garage_a state4: anim[0] = cfg.anim[2]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "barracks_garage_a state4: online_state -> 1, unconditionally");
}

// ---- airfield_a: slot = (anim[1]<0)?3:0 selects which frame is TICKED, but every switch branch
// writes the LITERAL anim[3] regardless of slot (see sim_bldg_anim_state_online.h's derivation). -----

sim_fixture &afa_setup(sim_fixture &f, uint16_t online_state) {
    f.reset();
    f.view_cur_player  = PLAYER;
    f.view_cur_index   = BLDG;
    f.cur_building_ptr = &f.b(PLAYER, BLDG);
    building &b        = *f.cur_building_ptr;
    b.sub_id           = SUB_ID;
    b.efficiency       = 4.0;
    b.online_state     = online_state;
    f.game_clock       = 100.0;
    return f;
}

// slot=0 path (anim[1] left at its zeroed default, which is NOT negative): FRAME lives at anim[0].
void afa_arm_one_switch_iteration_slot0(sim_fixture &f) {
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 0, FRAME);
    // anim[1] left 0 (>= 0) -> slot = 0, matching the ticked frame set above.
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 0;
    b.anim_dur[0]                 = 96.25; // elapsed = 15, in (10,20]
}

void test_afa_slot0_state1_door_idle() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/1);
    afa_arm_one_switch_iteration_slot0(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 303);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 303,
       "airfield_a state1 idle: anim[3] = cfg.anim[3] (LITERAL slot 3, not the ticked slot 0) @0x00477356");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "airfield_a state1 idle: online_state UNCHANGED (stays 1)");
}

void test_afa_slot0_state1_door_busy_transitions_to_3() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/1);
    afa_arm_one_switch_iteration_slot0(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 4, 404);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 404, "airfield_a state1 busy: anim[3] = cfg.anim[4]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "airfield_a state1 busy: online_state -> 3");
}

void test_afa_slot0_state2_door_idle_transitions_to_4() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/2);
    afa_arm_one_switch_iteration_slot0(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 505);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 505, "airfield_a state2 idle: anim[3] = cfg.anim[5]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "airfield_a state2 idle: online_state -> 4");
}

void test_afa_slot0_state3_unconditional_to_2() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/3);
    afa_arm_one_switch_iteration_slot0(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 6, 606);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 606, "airfield_a state3: anim[3] = cfg.anim[6]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "airfield_a state3: online_state -> 2, unconditionally");
}

void test_afa_slot0_state4_unconditional_to_1() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/4);
    afa_arm_one_switch_iteration_slot0(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 303);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 303, "airfield_a state4: anim[3] = cfg.anim[3]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "airfield_a state4: online_state -> 1, unconditionally");
}

// slot SELECTION finding: anim[1] < 0 selects slot=3 for the TICKED frame (not slot 0) -- the
// chain-follow branch is the ONE place that uses `slot`, not the literal 3, so this is where the
// selection is actually observable.
void test_afa_negative_anim1_selects_slot3_for_chain_follow() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 1, -5); // negative -> slot = 3
    anim_slot_set(b, 3, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 9; // chain-follow, not the switch
    b.anim_dur[3]                 = 96.25;
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(b, 3) == FRAME + 9,
       "airfield_a FINDING: anim[1]<0 selects slot=3 for the ticked frame -- chain-follow advances "
       "anim[3] (the ONE branch that reads `slot`, not the literal 3) by Anim[...].next (40+9=49)");
}

void test_afa_time_zero_is_a_whole_function_noop() {
    sim_fixture f;
    afa_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 0, FRAME); // anim[1] default 0, not negative -> slot=0
    f.anim_frames[FRAME + 1].time = 0.0;
    b.anim_dur[0]                 = 66.0; // sentinel
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_airfield_a(v, s, 0, 0, 0, 0);

    ck(b.anim_dur[0] == 66.0, "airfield_a: time==0.0 -> early return, anim_dur[0] stays the sentinel 66.0");
}

// ---- shuttle_a: slot fixed at 3. Calls llm_strat_bldg_online_shuttle_a in state 0 (untestable
// offline, see file banner). Addresses from llm_strat_bldg_anim_state_shuttle_a_00477d9e.asm. ----------

sim_fixture &sha_setup(sim_fixture &f, uint16_t online_state) {
    f.reset();
    f.view_cur_player  = PLAYER;
    f.view_cur_index   = BLDG;
    f.cur_building_ptr = &f.b(PLAYER, BLDG);
    building &b        = *f.cur_building_ptr;
    b.sub_id           = SUB_ID;
    b.efficiency       = 4.0;
    b.online_state     = online_state;
    f.game_clock       = 100.0;
    return f;
}

constexpr int32_t SHUTTLE_A_SLOT = 3;
void              sha_arm_one_switch_iteration(sim_fixture &f) {
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, SHUTTLE_A_SLOT, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 0;
    // shuttle_a does NOT scale by efficiency (see the file banner) -- elapsed = 100 - 85 = 15, in
    // (10,20], regardless of b.efficiency. This seed WAS 96.25, computed as (100-96.25)*4 back when the
    // body carried a `* efficiency` round-trip the original has no FMUL for; that made the fixture
    // depend on the defect, so removing it turned 22 checks red and read as a behavioural conflict.
    b.anim_dur[SHUTTLE_A_SLOT] = 85.0;
}

void test_sha_time_zero_is_a_whole_function_noop() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, SHUTTLE_A_SLOT, FRAME);
    f.anim_frames[FRAME + 1].time = 0.0;
    b.anim_dur[SHUTTLE_A_SLOT]    = 44.0; // sentinel
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(b.anim_dur[SHUTTLE_A_SLOT] == 44.0, "shuttle_a: time==0.0 -> early return, anim_dur[3] stays sentinel 44.0");
}

void test_sha_chain_follow_advances_by_next() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, SHUTTLE_A_SLOT, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 5;    // nonzero -> chain-follow, switch never reached
    b.anim_dur[SHUTTLE_A_SLOT]    = 85.0; // elapsed = 15, in (10,20] -- no efficiency scaling
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(b, SHUTTLE_A_SLOT) == FRAME + 5, "shuttle_a: chain-follow -- anim[3] = FRAME + Anim[...].next (40+5=45)");
    ck_eq((uint32_t)b.online_state, 0u, "shuttle_a: chain-follow never reaches the switch -- online_state unchanged");
}

void test_sha_state1_door_idle_no_state_change() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/1);
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 313);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == 313, "shuttle_a state1 idle: anim[3] = cfg.anim[3] @0x00477f65-0x00477f87");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "shuttle_a state1 idle: online_state UNCHANGED (stays 1)");
}

void test_sha_state1_door_busy_transitions_to_3() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/1);
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 4, 414);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == 414, "shuttle_a state1 busy: anim[3] = cfg.anim[4] @0x00477f30-0x00477f52");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "shuttle_a state1 busy: online_state -> 3 @0x00477f5d");
}

void test_sha_state2_door_idle_transitions_to_4() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/2);
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 525);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == 525, "shuttle_a state2 idle: anim[3] = cfg.anim[5] @0x00477fe8-0x0047800a");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "shuttle_a state2 idle: online_state -> 4 @0x00478015");
}

void test_sha_state2_door_busy_no_state_change() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/2);
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 6, 636);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == 636, "shuttle_a state2 busy: anim[3] = cfg.anim[6] @0x0047801d-0x0047803f");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "shuttle_a state2 busy: online_state UNCHANGED (stays 2)");
}

void test_sha_state3_unconditional_to_2() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/3);
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 6, 637);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == 637, "shuttle_a state3: anim[3] = cfg.anim[6] @0x0047804a-0x0047806c");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "shuttle_a state3: online_state -> 2, unconditionally @0x00478077");
}

void test_sha_state4_unconditional_to_1() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/4);
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 314);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == 314, "shuttle_a state4: anim[3] = cfg.anim[3] @0x0047807f-0x004780a1");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "shuttle_a state4: online_state -> 1, unconditionally @0x004780ac");
}

void test_sha_state10_resets_to_zero() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/10);
    sha_arm_one_switch_iteration(f);
    const int32_t  before = anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT);
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0u, "shuttle_a state10: online_state -> 0, unconditionally @0x004780b9");
    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == before, "shuttle_a state10: no anim[3] write (state-only case)");
}

void test_sha_state11_sets_0xc() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/11);
    sha_arm_one_switch_iteration(f);
    const int32_t  before = anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT);
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0xcu, "shuttle_a state11: online_state -> 0xc, unconditionally @0x004780c6");
    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_A_SLOT) == before, "shuttle_a state11: no anim[3] write (state-only case)");
}

// ---- shuttle_h: slot fixed at 2. IDENTICAL cfg.anim[] indices/state transitions to shuttle_a -- only
// the slot and the callee (llm_strat_bldg_online_shuttle_h) differ. Addresses from
// llm_strat_bldg_anim_state_shuttle_h_0047815f.asm. --------------------------------------------------

constexpr int32_t SHUTTLE_H_SLOT = 2;
void              shh_arm_one_switch_iteration(sim_fixture &f) {
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, SHUTTLE_H_SLOT, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 0;
    b.anim_dur[SHUTTLE_H_SLOT]    = 85.0; // elapsed = 15, in (10,20] -- no efficiency scaling (see sha_arm)
}

void test_shh_time_zero_is_a_whole_function_noop() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, SHUTTLE_H_SLOT, FRAME);
    f.anim_frames[FRAME + 1].time = 0.0;
    b.anim_dur[SHUTTLE_H_SLOT]    = 33.0; // sentinel
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(b.anim_dur[SHUTTLE_H_SLOT] == 33.0, "shuttle_h: time==0.0 -> early return, anim_dur[2] stays sentinel 33.0");
}

void test_shh_chain_follow_advances_by_next() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, SHUTTLE_H_SLOT, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 6;
    b.anim_dur[SHUTTLE_H_SLOT]    = 85.0; // elapsed = 15, in (10,20] -- no efficiency scaling
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(b, SHUTTLE_H_SLOT) == FRAME + 6, "shuttle_h: chain-follow -- anim[2] = FRAME + Anim[...].next (40+6=46)");
    ck_eq((uint32_t)b.online_state, 0u, "shuttle_h: chain-follow never reaches the switch -- online_state unchanged");
}

void test_shh_state1_door_idle_no_state_change() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/1);
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 213);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == 213, "shuttle_h state1 idle: anim[2] = cfg.anim[3]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "shuttle_h state1 idle: online_state UNCHANGED (stays 1)");
}

void test_shh_state1_door_busy_transitions_to_3() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/1);
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 4, 224);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == 224, "shuttle_h state1 busy: anim[2] = cfg.anim[4] @0x0047831e");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "shuttle_h state1 busy: online_state -> 3 @0x0047831e");
}

void test_shh_state2_door_idle_transitions_to_4() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/2);
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 235);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == 235, "shuttle_h state2 idle: anim[2] = cfg.anim[5]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "shuttle_h state2 idle: online_state -> 4 @0x004783d6");
}

void test_shh_state2_door_busy_no_state_change() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/2);
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 6, 246);
    set_door_idle(f, /*idle=*/false);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == 246, "shuttle_h state2 busy: anim[2] = cfg.anim[6]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "shuttle_h state2 busy: online_state UNCHANGED (stays 2)");
}

void test_shh_state3_unconditional_to_2() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/3);
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 6, 247);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == 247, "shuttle_h state3: anim[2] = cfg.anim[6]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "shuttle_h state3: online_state -> 2, unconditionally @0x00478438");
}

void test_shh_state4_unconditional_to_1() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/4);
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 214);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == 214, "shuttle_h state4: anim[2] = cfg.anim[3]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "shuttle_h state4: online_state -> 1, unconditionally @0x0047846d");
}

void test_shh_state10_resets_to_zero() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/10);
    shh_arm_one_switch_iteration(f);
    const int32_t  before = anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT);
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0u, "shuttle_h state10: online_state -> 0, unconditionally @0x0047847a");
    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == before, "shuttle_h state10: no anim[2] write (state-only case)");
}

void test_shh_state11_sets_0xc() {
    sim_fixture f;
    sha_setup(f, /*online_state=*/11);
    shh_arm_one_switch_iteration(f);
    const int32_t  before = anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT);
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0xcu, "shuttle_h state11: online_state -> 0xc, unconditionally @0x00478487");
    ck(anim_slot_get(f.b(PLAYER, BLDG), SHUTTLE_H_SLOT) == before, "shuttle_h state11: no anim[2] write (state-only case)");
}

// ---- EFFICIENCY INDEPENDENCE: the check these two bodies were missing ------------------------------
// shuttle_a/shuttle_h compute the credit clock with a BARE FSUB and credit the residual back with a
// BARE FSUB -- no FMUL/FDIV by building+0x29 (efficiency) anywhere in either body (shuttle_a
// 0x00477e0c / 0x0047813c, shuttle_h 0x004781cd / 0x004784fd; contrast airfield_a, which DOES scale:
// FMUL [EAX+0x29] @0x0047790d, FDIV [EDX+0x29] @0x00477bc7).
//
// WHY THIS CASE EXISTS. Every other case in this file asserts anim[slot] and online_state, and in the
// door-idle arm NEITHER of those moves when a `* efficiency` / `/ efficiency` round-trip is present:
// the extra loop iterations re-run the same idle branch and rewrite the same cfg.anim[3] value. The
// round-trip is observable in exactly ONE quantity -- anim_dur[slot] -- which nothing here asserted.
// That is how the defect survived a green `simtest`, and it is the same defect (and the same blind
// spot) as SIM-SAVE-DIV fault 2 in online_toggle. Arithmetic, from the shared seed anim_dur=85,
// game_clock=100, time=10:
//   correct (bare FSUB):   elapsed = 15; one 10-unit iteration; residual 5 credited -> anim_dur = 95.0
//   with `*eff` / `/eff`:  eff=4.0  -> elapsed 60, five over-time iterations, residual 10/4  -> 97.5
//                          eff=0.25 -> elapsed 3.75 <= time immediately, residual 3.75/0.25 -> 85.0
// so the round-trip fails this check at BOTH efficiencies, in OPPOSITE directions -- which is why two
// are asserted rather than one. Mutation-tested both ways 2026-09-06.
//
// AND A TRAP FOR ANYONE RE-SEEDING THESE FIXTURES. Every case in this file is tuned so the loop takes
// EXACTLY ONE over-time iteration. That is not merely tidy -- a seed that buys more iterations makes the
// state-10/state-11 cases CRASH rather than fail: state 10 sets online_state = 0, and the next iteration
// re-enters the switch at case 0, which calls `mh::call::llm_strat_bldg_online_shuttle_a` -- a fixed game
// VA that does not exist in this process (see the SCOPE note in the banner). A crash loses stdout, so it
// reports as "exit 3221225477 (CRASH)" with no indication of which check or which body. That is exactly
// what the round-trip mutant does here, which is why these two cases are registered AHEAD of the state
// cases: they print their clean red before the crash can swallow the run.

void check_sha_residual_at(double eff, const char *what) {
    sim_fixture f;
    sha_setup(f, /*online_state=*/1);
    f.b(PLAYER, BLDG).efficiency = eff;
    sha_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 313);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_a(v, s, 0, 0, 0, 0);

    ck(f.b(PLAYER, BLDG).anim_dur[SHUTTLE_A_SLOT] == 95.0, what);
}

void test_sha_residual_is_not_efficiency_scaled() {
    check_sha_residual_at(4.0, "shuttle_a: residual credit is a BARE FSUB -- anim_dur[3] == 95.0 at efficiency 4.0");
    check_sha_residual_at(0.25, "shuttle_a: residual credit is a BARE FSUB -- anim_dur[3] == 95.0 at efficiency 0.25");
}

void check_shh_residual_at(double eff, const char *what) {
    sim_fixture f;
    sha_setup(f, /*online_state=*/1);
    f.b(PLAYER, BLDG).efficiency = eff;
    shh_arm_one_switch_iteration(f);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 213);
    set_door_idle(f, /*idle=*/true);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_shuttle_h(v, s, 0, 0, 0, 0);

    ck(f.b(PLAYER, BLDG).anim_dur[SHUTTLE_H_SLOT] == 95.0, what);
}

void test_shh_residual_is_not_efficiency_scaled() {
    check_shh_residual_at(4.0, "shuttle_h: residual credit is a BARE FSUB -- anim_dur[2] == 95.0 at efficiency 4.0");
    check_shh_residual_at(0.25, "shuttle_h: residual credit is a BARE FSUB -- anim_dur[2] == 95.0 at efficiency 0.25");
}

} // namespace

void run_bldg_anim_state_online_a_tests() {
    test_bga_time_zero_is_a_whole_function_noop();
    test_bga_chain_follow_advances_by_next();
    test_bga_state1_door_idle_no_state_change();
    test_bga_state1_door_busy_transitions_to_3();
    test_bga_state2_door_idle_transitions_to_4();
    test_bga_state2_door_busy_no_state_change();
    test_bga_state3_unconditional_to_2();
    test_bga_state4_unconditional_to_1();

    test_afa_slot0_state1_door_idle();
    test_afa_slot0_state1_door_busy_transitions_to_3();
    test_afa_slot0_state2_door_idle_transitions_to_4();
    test_afa_slot0_state3_unconditional_to_2();
    test_afa_slot0_state4_unconditional_to_1();
    test_afa_negative_anim1_selects_slot3_for_chain_follow();
    test_afa_time_zero_is_a_whole_function_noop();

    test_sha_time_zero_is_a_whole_function_noop();
    test_sha_chain_follow_advances_by_next();
    // Registered HERE, ahead of the state cases, deliberately -- see its own comment block: under a
    // reintroduced `* efficiency` round-trip the state-10 case CRASHES (extra loop iterations re-enter
    // the switch at case 0, which calls a bare `mh::call::` game VA that does not exist offline), and a
    // crash loses stdout, so a later registration would never get to print its red.
    test_sha_residual_is_not_efficiency_scaled();
    test_sha_state1_door_idle_no_state_change();
    test_sha_state1_door_busy_transitions_to_3();
    test_sha_state2_door_idle_transitions_to_4();
    test_sha_state2_door_busy_no_state_change();
    test_sha_state3_unconditional_to_2();
    test_sha_state4_unconditional_to_1();
    test_sha_state10_resets_to_zero();
    test_sha_state11_sets_0xc();

    test_shh_time_zero_is_a_whole_function_noop();
    test_shh_chain_follow_advances_by_next();
    test_shh_residual_is_not_efficiency_scaled(); // ahead of the state cases -- see the sha note above
    test_shh_state1_door_idle_no_state_change();
    test_shh_state1_door_busy_transitions_to_3();
    test_shh_state2_door_idle_transitions_to_4();
    test_shh_state2_door_busy_no_state_change();
    test_shh_state3_unconditional_to_2();
    test_shh_state4_unconditional_to_1();
    test_shh_state10_resets_to_zero();
    test_shh_state11_sets_0xc();
}

} // namespace mh::sim::test
