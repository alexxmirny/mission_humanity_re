//
// sim_bldg_anim_state_port_selftest.cpp -- `simtest` cases for TWO of SIM1-BLDGCB batch H's
// verification-debt rows (batch H closing slice, 2026-08-23):
//   llm_strat_bldg_anim_state_port   @0x004786f5
//   llm_strat_bldg_anim_state_port_h @0x00478bfe
// Both sim/sim_bldg_anim_state_port.h/.cpp. Both are shadow-armed (extra_regions:["buildings"], the
// same manual declaration as sim_bldg_anim_tick.h's family) but got 0 calls over a 15000-step all-AI
// soak -- the run's only player is a base-race human-converted AI ("_solo" lane, one ALLAI VERIFY
// line), which never queued/completed a port/landing-pad building in that short a game. UNLIKE the
// online-state family (sim_bldg_anim_state_online.h), NEITHER function here calls anything (see the
// production header's "CALLEES: NONE for either function" note) -- both are fully self-contained, so
// this oracle covers EVERY branch with no callee-forwarding residue, i.e. full T1 coverage rather than
// the online family's T2 (contrast sim_bldg_anim_state_online_a_selftest.cpp's scope note).
//
// EVERY EXPECTED VALUE DERIVED FROM tmp/decomp_sim/llm_strat_bldg_anim_state_port_004786f5.asm and
// tmp/decomp_sim/llm_strat_bldg_anim_state_port_h_00478bfe.asm (cross-checked against
// sim_bldg_anim_state_port.h's own derivation, not re-derived here). Both slots scale elapsed time by
// `efficiency` (unlike llm_strat_bldg_anim_state_helipad's slot 1) -- efficiency is set to a
// non-identity value (4.0) in every case so a wrongly-dropped/added scale factor changes the result.
//
// One-switch-iteration rig, same shape as sim_bldg_anim_state_online_a_selftest.cpp's
// bga_arm_one_switch_iteration: anim[slot]=FRAME, Anim[FRAME+1]={time=10,next=N}, anim_dur[slot] chosen
// so elapsed=(game_clock-anim_dur[slot])*efficiency lands in (10,20] -- exactly one over-time
// iteration (reaching either the chain-follow or the switch/restart), then the remainder is spent by
// the loop's own `elapsed <= time` arm, ending the loop deterministically.
//
#include "sim/sim_bldg_anim_state_port.h"

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

constexpr int32_t PLAYER    = 3;
constexpr int32_t BLDG      = 5;
constexpr uint8_t SUB_ID    = 2;
constexpr int32_t DOOR_UNIT = 7; // nonzero unit index for the docked unit
constexpr int32_t FRAME     = 40;

// llm_strat_unit_state values, confirmed against Ghidra's own enum (see sim_bldg_anim_state_port.h).
constexpr uint16_t UNIT_STATE_TAKEOFF_TAXI = 0x27;
constexpr uint16_t UNIT_STATE_LANDING      = 0x16;
constexpr uint16_t UNIT_STATE_DOCK_TAXI_IN = 0x2a;
constexpr uint16_t UNIT_STATE_OTHER        = 0x1f; // neither of the three named states above

sim_fixture &common_setup(sim_fixture &f, uint16_t online_state) {
    f.reset();
    f.view_cur_player  = PLAYER;
    f.view_cur_index   = BLDG;
    f.cur_building_ptr = &f.b(PLAYER, BLDG);
    building &b        = *f.cur_building_ptr;
    b.sub_id           = SUB_ID;
    b.efficiency       = 4.0; // non-identity -- a dropped/added scale would change every result below
    b.online_state     = online_state;
    f.game_clock       = 100.0;
    return f;
}

// Arms slot `slot` for exactly one over-time ("switch"/restart-reaching) iteration: elapsed =
// (100 - anim_dur[slot]) * 4.0 = 15, in (10,20].
void arm_one_iteration(sim_fixture &f, int32_t slot, int32_t frame_next) {
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, slot, FRAME);
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = frame_next;
    b.anim_dur[slot]              = 96.25; // elapsed = (100-96.25)*4 = 15
}

// ================================================================================================
// port_h @0x00478bfe -- slot 1 (fixed), NO `anim[1]>0` pre-guard, restart_idx=2 (the generic slot+1
// convention -- CONTRAST port's own slot 1 below, which restarts to index 6 instead).
// ================================================================================================

void test_port_h_time_zero_is_a_whole_function_noop() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 1, FRAME);
    f.anim_frames[FRAME + 1].time = 0.0;  // triggers the early `if (time==0.0) return;`
    b.anim_dur[1]                 = 55.0; // sentinel
    const sim_view v              = f.view();
    sim_store      s              = f.store();

    detail::bldg_anim_state_port_h(v, s);

    ck(b.anim_dur[1] == 55.0, "port_h: time==0.0 -> early return, anim_dur[1] stays sentinel 55.0 @0x00478bfe-region time==0 gate");
    ck(anim_slot_get(b, 1) == FRAME, "port_h: time==0.0 -> anim[1] unchanged");
}

void test_port_h_chain_follow_advances_by_next() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0);
    arm_one_iteration(f, /*slot=*/1, /*frame_next=*/7); // nonzero -> chain-follow, restart never reached
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_port_h(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 1) == FRAME + 7,
       "port_h: chain-follow -- anim[1] = FRAME + Anim[...].next (40+7=47)");
}

void test_port_h_chain_end_restarts_to_cfg_anim2() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0);
    arm_one_iteration(f, /*slot=*/1, /*frame_next=*/0); // next==0 -> restart
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 2, 202);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_port_h(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 1) == 202,
       "port_h: chain-end restart -- anim[1] = cfg.anim[2] (the generic slot+1 convention, restart_idx=2)");
    ck(f.b(PLAYER, BLDG).anim_dur[1] == 98.75,
       "port_h: remainder spend -- anim_dur[1] = 100 - (15-10)/4.0 = 98.75 (scaled by efficiency)");
}

// ================================================================================================
// port @0x004786f5 -- TWO INDEPENDENT SLOTS: slot 1 (idle, gated by `anim[1]>0`, restarts to
// cfg.anim[6] -- NOT the generic slot+1 convention) and slot 3 (pad door, no pre-guard, chain end runs
// the online-state door switch).
// ================================================================================================

// ---- slot 1: the `anim[1]>0` gate and its restart_idx=6 deviation -----------------------------------

void test_port_slot1_not_gated_when_anim1_zero() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 1, 0); // NOT > 0 -> slot 1 block entirely skipped
    b.anim_dur[1] = 12.0;   // sentinel -- must survive untouched
    // slot 3 made a no-op so this case isolates slot 1's own gate.
    f.anim_frames[1].time = 0.0; // anim_slot_get(b,3)==0 by default -> Anim[0+1] == Anim[1]
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(b.anim_dur[1] == 12.0, "port: anim[1]<=0 -> slot 1 block skipped entirely, anim_dur[1] untouched @0x00478722-0x00478729");
}

void test_port_slot1_gated_runs_and_restarts_to_cfg_anim6() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 1, FRAME); // > 0 -> gate passes
    f.anim_frames[FRAME + 1].time = 10.0;
    f.anim_frames[FRAME + 1].next = 0; // restart
    b.anim_dur[1]                 = 96.25;
    cfg_building &cb              = f.cfg_buildings[b.building_id];
    cfg_anim_slot_set(cb, 6, 606);
    cfg_anim_slot_set(cb, 2, 202); // sentinel -- must NOT be picked (that would be the generic slot+1)
    // slot 3 made a no-op so this case isolates slot 1's own restart index.
    f.anim_frames[1].time = 0.0;
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(b, 1) == 606,
       "port: slot1 chain-end restarts to cfg.anim[6] @0x004787fb, a genuine deviation from the "
       "generic slot+1(=2) convention port_h's own slot 1 uses -- NOT 202");
}

// ---- slot 3: no pre-guard, chain-follow, and the 4-state + default door switch -----------------------

// Arms slot 3 for one over-time iteration while leaving slot 1 a no-op (anim[1]==0 -> gate fails).
void arm_slot3_one_iteration(sim_fixture &f, int32_t frame_next) {
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 1, 0); // slot 1 gate fails -> isolates slot 3
    arm_one_iteration(f, /*slot=*/3, frame_next);
}

void test_port_slot3_no_pre_guard_runs_even_with_anim3_zero() {
    sim_fixture f;
    common_setup(f, /*online_state=*/1);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 1, 0);
    anim_slot_set(b, 3, 0); // anim[3]==0, UNLIKE slot1 there is no `>0` pre-guard on slot 3
    f.anim_frames[0 + 1].time = 10.0;
    f.anim_frames[0 + 1].next = 0;
    b.anim_dur[3]             = 96.25;
    cfg_building &cb          = f.cfg_buildings[b.building_id];
    cfg_anim_slot_set(cb, 2, 909);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = 0; // idle_or_other=true
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(b, 3) == 909,
       "port: slot3 has NO `anim[3]>0` pre-guard (unlike slot1) -- runs and reaches the switch even "
       "starting from anim[3]==0 @0x00478878-0x004788b4 has no comparable JLE-skip");
}

void test_port_slot3_chain_follow_advances_by_next() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0);
    arm_slot3_one_iteration(f, /*frame_next=*/9); // nonzero -> chain-follow, switch never reached
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == FRAME + 9,
       "port: slot3 chain-follow -- anim[3] = FRAME + Anim[...].next (40+9=49) @0x004788fa-0x00478930");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0u, "port: slot3 chain-follow never reaches the switch -- online_state unchanged");
}

void test_port_slot3_state1_idle_or_other_true_door_empty() {
    sim_fixture f;
    common_setup(f, /*online_state=*/1);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 2, 212);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = 0; // idle_or_other: door_unit==0
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 212, "port: state1 idle_or_other (door empty) -> anim[3] = cfg.anim[2] @0x004789de-0x00478a02");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "port: state1 idle_or_other -> online_state UNCHANGED (stays 1)");
}

void test_port_slot3_state1_idle_or_other_true_door_occupied_other_state() {
    sim_fixture f;
    common_setup(f, /*online_state=*/1);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 2, 213);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_OTHER; // neither TAKEOFF_TAXI nor LANDING
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 213,
       "port: state1 idle_or_other (door occupied but state is neither TAKEOFF_TAXI(0x27) nor "
       "LANDING(0x16)) -> anim[3] = cfg.anim[2] @0x004789b2-0x004789da");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "port: state1 idle_or_other (occupied/other-state) -> online_state UNCHANGED");
}

void test_port_slot3_state1_busy_takeoff_taxi_transitions_to_3() {
    sim_fixture f;
    common_setup(f, /*online_state=*/1);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 4, 414);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_TAKEOFF_TAXI;
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 414, "port: state1 busy (door occupied, state==TAKEOFF_TAXI) -> anim[3] = cfg.anim[4]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "port: state1 busy -> online_state -> 3 @0x004789e0-0x00478a13");
}

void test_port_slot3_state1_busy_landing_transitions_to_3() {
    sim_fixture f;
    common_setup(f, /*online_state=*/1);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 4, 415);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_LANDING; // the OTHER busy state
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 415, "port: state1 busy via LANDING(0x16) (not just TAKEOFF_TAXI) -> anim[3] = cfg.anim[4]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "port: state1 busy via LANDING -> online_state -> 3");
}

// state2's boolean is reproduced LITERALLY from the original (`not_taxi_in && door_unit!=0`), not
// algebraically simplified -- see the production header. Three variants below pin all three ways the
// two clauses combine (door empty; occupied+DOCK_TAXI_IN; occupied+other), which a wrong simplification
// dropping either clause could get wrong.
void test_port_slot3_state2_door_empty_transitions_to_4() {
    sim_fixture f;
    common_setup(f, /*online_state=*/2);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 525);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = 0; // door empty
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 525, "port: state2, door empty -> anim[3] = cfg.anim[5] @0x00478a92-0x00478ae4");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "port: state2, door empty -> online_state -> 4");
}

void test_port_slot3_state2_docked_taxi_in_transitions_to_4() {
    sim_fixture f;
    common_setup(f, /*online_state=*/2);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 5, 526);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_DOCK_TAXI_IN;
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 526,
       "port: state2, docked unit IS taxiing in (DOCK_TAXI_IN=0x2a) -> anim[3] = cfg.anim[5] "
       "(not_taxi_in=false, so (not_taxi_in && door_unit!=0)=false -> the ELSE arm) @0x00478a70-0x00478a90");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "port: state2, docked+taxi-in -> online_state -> 4");
}

void test_port_slot3_state2_docked_other_state_no_state_change() {
    sim_fixture f;
    common_setup(f, /*online_state=*/2);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 623);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_OTHER; // not DOCK_TAXI_IN
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 623,
       "port: state2, docked+NOT taxi-in -> not_taxi_in=true AND door_unit!=0 -> anim[3] = cfg.anim[3] "
       "(the THEN arm) @0x00478abc-0x00478ade");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "port: state2, docked+other-state -> online_state UNCHANGED (stays 2)");
}

void test_port_slot3_state3_unconditional_to_2() {
    sim_fixture f;
    common_setup(f, /*online_state=*/3);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 3, 730);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 730, "port: state3 -> anim[3] = cfg.anim[3] @0x00478b1b-0x00478b3d");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "port: state3 -> online_state -> 2, unconditionally");
}

void test_port_slot3_state4_unconditional_to_1() {
    sim_fixture f;
    common_setup(f, /*online_state=*/4);
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_slot_set(cb, 2, 841);
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(f.b(PLAYER, BLDG), 3) == 841, "port: state4 -> anim[3] = cfg.anim[2] @0x00478b50-0x00478b72");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "port: state4 -> online_state -> 1, unconditionally");
}

void test_port_slot3_default_state_no_case_leaves_anim_and_state_unchanged() {
    sim_fixture f;
    common_setup(f, /*online_state=*/0); // 0 has no case in the switch
    arm_slot3_one_iteration(f, /*frame_next=*/0);
    building &b = f.b(PLAYER, BLDG);
    anim_slot_set(b, 3, FRAME); // re-set: arm_one_iteration already set this, kept explicit for clarity
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_port(v, s);

    ck(anim_slot_get(b, 3) == FRAME,
       "port: default (online_state=0, no matching case) -> anim[3] left AT THE CURRENT FRAME (no "
       "chain-follow, no restart) @0x00478bb3-0x00478bb6, reproduced as `default: break;`");
    ck_eq((uint32_t)b.online_state, 0u, "port: default -> online_state unchanged");
}

} // namespace

void run_bldg_anim_state_port_tests() {
    test_port_h_time_zero_is_a_whole_function_noop();
    test_port_h_chain_follow_advances_by_next();
    test_port_h_chain_end_restarts_to_cfg_anim2();

    test_port_slot1_not_gated_when_anim1_zero();
    test_port_slot1_gated_runs_and_restarts_to_cfg_anim6();

    test_port_slot3_no_pre_guard_runs_even_with_anim3_zero();
    test_port_slot3_chain_follow_advances_by_next();
    test_port_slot3_state1_idle_or_other_true_door_empty();
    test_port_slot3_state1_idle_or_other_true_door_occupied_other_state();
    test_port_slot3_state1_busy_takeoff_taxi_transitions_to_3();
    test_port_slot3_state1_busy_landing_transitions_to_3();
    test_port_slot3_state2_door_empty_transitions_to_4();
    test_port_slot3_state2_docked_taxi_in_transitions_to_4();
    test_port_slot3_state2_docked_other_state_no_state_change();
    test_port_slot3_state3_unconditional_to_2();
    test_port_slot3_state4_unconditional_to_1();
    test_port_slot3_default_state_no_case_leaves_anim_and_state_unchanged();
}

} // namespace mh::sim::test
