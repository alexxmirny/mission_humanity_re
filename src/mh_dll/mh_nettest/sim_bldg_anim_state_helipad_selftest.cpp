#include "sim/sim_bldg_anim_state_online.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int32_t  PLAYER               = 2;
constexpr int32_t  BLDG                 = 4;
constexpr uint8_t  SUB_ID               = 6;
constexpr int32_t  DOOR_UNIT            = 9; // nonzero unit index for the docked unit
constexpr uint16_t UNIT_STATE_TAKEOFF   = 0x15;
constexpr uint16_t UNIT_STATE_PARKED    = 0x1f;
constexpr uint16_t UNIT_STATE_PARKED_28 = 0x28;
constexpr uint16_t UNIT_STATE_PARKED_2B = 0x2b;
constexpr int32_t  FRAME_IDX            = 40; // arbitrary Anim[] index used as anim[1]'s value

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
int32_t anim1_get(building &b) { return static_cast<int32_t>(load_u32_le(&b.anim[1 * 4])); }
void    anim1_set(building &b, int32_t v) { store_u32_le(&b.anim[1 * 4], static_cast<uint32_t>(v)); }
int32_t cfg_anim_get(cfg_building &cb, int32_t slot) {
    return static_cast<int32_t>(load_u32_le(&cb.anim[slot * 4]));
}
void cfg_anim_set(cfg_building &cb, int32_t slot, int32_t v) {
    store_u32_le(&cb.anim[slot * 4], static_cast<uint32_t>(v));
}

struct call_log {
    int      online_calls  = 0;
    int16_t  online_player = 0;
    int32_t  online_index  = 0;
    uint32_t online_ebx = 0, online_ecx = 0;
    double   online_clock = 0.0;

    int      takeoff_calls  = 0;
    uint32_t takeoff_player = 0, takeoff_unit = 0;

    int      release_calls  = 0;
    uint32_t release_player = 0;
    int32_t  release_unit   = 0;
    uint32_t release_mode   = 0;

    int      free_calls  = 0;
    uint16_t free_player = 0;
    int32_t  free_unit   = 0;

    void reset() { *this = call_log{}; }
};
call_log g_log;

const bldg_anim_state_helipad_calls &mock_calls() {
    static const bldg_anim_state_helipad_calls c = {
        [](int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
           double anim_dur) -> void {
            g_log.online_calls++;
            g_log.online_player = player;
            g_log.online_index  = building_index;
            g_log.online_ebx    = param_3;
            g_log.online_ecx    = param_4;
            g_log.online_clock  = anim_dur;
        },
        [](uint32_t player, uint32_t unit_index) -> void {
            g_log.takeoff_calls++;
            g_log.takeoff_player = player;
            g_log.takeoff_unit   = unit_index;
        },
        [](uint32_t player_idx, int32_t unit_idx, uint32_t mode) -> void {
            g_log.release_calls++;
            g_log.release_player = player_idx;
            g_log.release_unit   = unit_idx;
            g_log.release_mode   = mode;
        },
        [](uint16_t player, int32_t unit_index) -> void {
            g_log.free_calls++;
            g_log.free_player = player;
            g_log.free_unit   = unit_index;
        },
    };
    return c;
}

// Common setup: cur_building = buildings[PLAYER][BLDG], anim[1]=FRAME_IDX with a `time`-only Anim[]
// entry (next=0, forcing the switch on the first over-time iteration), efficiency set to a value that
// would change the result if (wrongly) multiplied in, game_clock/anim_dur[1] chosen so exactly one
// switch iteration fires.
sim_fixture &setup(sim_fixture &f, uint16_t online_state) {
    f.reset();
    f.view_cur_player  = PLAYER;
    f.view_cur_index   = BLDG;
    f.cur_building_ptr = &f.b(PLAYER, BLDG);
    building &b        = *f.cur_building_ptr;
    b.sub_id           = SUB_ID;
    b.efficiency       = 4.0; // if wrongly multiplied in, every timing result below would be wrong
    b.online_state     = online_state;
    anim1_set(b, FRAME_IDX);
    f.anim_frames[FRAME_IDX + 1].time = 10.0;
    f.anim_frames[FRAME_IDX + 1].next = 0;
    f.game_clock                      = 100.0;
    b.anim_dur[1]                     = 85.0; // elapsed = 100-85 = 15 > time(10) -> switch fires once, remainder 5
    return f;
}

// ---- case 1: state 0 -- forwards to online_helipad_h_or_misc with the exact args, and the timing
// update is NOT scaled by efficiency (anim_dur[1] ends at GAME_CLOCK - remainder, remainder = 15-10=5,
// spent un-scaled on the next iteration: anim_dur[1] = 100 - 5 = 95). If efficiency (4.0) were wrongly
// multiplied in, this would be 100 - 5/4 = 98.75 or 100 - 5*4 = 80 instead.
void test_state0_forwards_to_online_handler_no_efficiency_scaling() {
    sim_fixture f;
    setup(f, /*online_state=*/0);
    const sim_view v = f.view();
    sim_store      s = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), /*unused_eax=*/0, /*unused_edx=*/0,
                                    /*unused_ebx=*/0x1234, /*unused_ecx=*/0x5678);

    ck_eq((uint32_t)g_log.online_calls, 1u, "state0: online_helipad_h_or_misc called exactly once @0x0047780a");
    ck(g_log.online_player == (int16_t)PLAYER && g_log.online_index == BLDG,
       "state0: forwarded (player, index) == (CUR_PLAYER, CUR_INDEX)");
    ck(g_log.online_ebx == 0x1234u && g_log.online_ecx == 0x5678u,
       "state0: unused_ebx/unused_ecx are forwarded VERBATIM to the online handler");
    ck(g_log.online_clock == 100.0, "state0: GAME_CLOCK (100.0) forwarded as the anim_dur argument");
    ck_eq((uint32_t)g_log.takeoff_calls, 0u, "state0: no takeoff_finalize call");
    ck_eq((uint32_t)g_log.release_calls, 0u, "state0: no target_release_ref call");
    ck_eq((uint32_t)g_log.free_calls, 0u, "state0: no path_free_slot call");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0u, "state0: online_state unchanged (no case-0 write)");
    ck(f.b(PLAYER, BLDG).anim_dur[1] == 95.0,
       "state0 FINDING: anim_dur[1] == 100 - (15-10) == 95.0, NOT scaled by efficiency (4.0) -- "
       "@0x004772db-0x004772ec/0x00477314-0x0047731c have no FMUL/FDIV by efficiency, unlike this "
       "function's four siblings");
}

// ---- case 2: state 1 -- UNCONDITIONAL transition (anim[1]=cfg.anim[5], state=3), no door check at all,
// even with the door BUSY (mutex held + waiters present) -- the one thing that would gate every sibling
// family's own state1.
void test_state1_unconditional_no_door_gate() {
    sim_fixture f;
    setup(f, /*online_state=*/1);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 5, 555);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit   = DOOR_UNIT; // BUSY
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_waiter_count = 3;         // BUSY
    const sim_view v                                                          = f.view();
    sim_store      s                                                          = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)anim1_get(f.b(PLAYER, BLDG)), 555u,
          "state1: anim[1] = cfg.anim[5] UNCONDITIONALLY (busy door does not gate this state, unlike "
          "every sibling family's own state1) @0x00477732-0x00477747");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 3u, "state1: online_state -> 3");
}

// ---- case 3: state 2, door idle (mutex==0) -- anim[1]=cfg.anim[4], online_state UNCHANGED (stays 2).
void test_state2_door_idle_no_state_change() {
    sim_fixture f;
    setup(f, /*online_state=*/2);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 4, 444);
    cfg_anim_set(cb, 6, 666);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = 0; // idle
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)anim1_get(f.b(PLAYER, BLDG)), 444u, "state2 idle: anim[1] = cfg.anim[4]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u,
          "state2 idle: online_state STAYS 2 (no write on this arm) @0x00477748-0x00477775");
}

// ---- case 4: state 2, docked unit exists and IS taking off/landing (PARKED_28) -- anim[1]=cfg.anim[6],
// online_state -> 4.
void test_state2_docked_taking_off_transitions_to_4() {
    sim_fixture f;
    setup(f, /*online_state=*/2);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 4, 444);
    cfg_anim_set(cb, 6, 666);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_PARKED_28;
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)anim1_get(f.b(PLAYER, BLDG)), 666u, "state2 taking-off: anim[1] = cfg.anim[6]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 4u, "state2 taking-off: online_state -> 4");
}

// ---- case 5: state 3, docked unit does NOT exist -- anim[1]=cfg.anim[4], state->2, no takeoff_finalize.
void test_state3_no_docked_unit_no_finalize() {
    sim_fixture f;
    setup(f, /*online_state=*/3);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 4, 400);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = 0; // no docked unit
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)anim1_get(f.b(PLAYER, BLDG)), 400u, "state3: anim[1] = cfg.anim[4]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2u, "state3: online_state -> 2");
    ck_eq((uint32_t)g_log.takeoff_calls, 0u, "state3: no docked unit -> no takeoff_finalize call");
}

// ---- case 6: state 3, docked unit IS PARKED_28 -- finalize + state->TAKEOFF, on top of the anim/state
// writes case 5 already pins.
void test_state3_docked_parked28_finalizes_takeoff() {
    sim_fixture f;
    setup(f, /*online_state=*/3);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 4, 400);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_PARKED_28;
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)g_log.takeoff_calls, 1u, "state3: takeoff_finalize called exactly once @0x004776cd");
    ck(g_log.takeoff_player == (uint32_t)PLAYER && g_log.takeoff_unit == (uint32_t)DOOR_UNIT,
       "state3: takeoff_finalize(player, door_unit)");
    ck_eq((uint32_t)f.u(PLAYER, DOOR_UNIT).state, (uint32_t)UNIT_STATE_TAKEOFF,
          "state3: docked unit's state -> TAKEOFF (0x15) @0x004776e0");
}

// ---- case 7: state 4, docked unit IS PARKED_2B with a live target2_ref and a real path slot -- full
// release path: state->PARKED, target_release_ref(player,unit,mode=3), target2_ref cleared,
// door_mutex_unit cleared, path_free_slot(player,unit).
void test_state4_docked_parked2b_full_release() {
    sim_fixture f;
    setup(f, /*online_state=*/4);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 3, 300);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_PARKED_2B;
    f.u(PLAYER, DOOR_UNIT).target2_ref                                      = 77;
    f.u(PLAYER, DOOR_UNIT).path_slot_id                                     = 5; // != 0xff
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)anim1_get(f.b(PLAYER, BLDG)), 300u, "state4: anim[1] = cfg.anim[3]");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 1u, "state4: online_state -> 1");
    ck_eq((uint32_t)f.u(PLAYER, DOOR_UNIT).state, (uint32_t)UNIT_STATE_PARKED,
          "state4: docked unit's state -> PARKED (0x1f) @0x004777bd");
    ck_eq((uint32_t)g_log.release_calls, 1u, "state4: target_release_ref called exactly once @0x00477771");
    ck(g_log.release_player == (uint32_t)PLAYER && g_log.release_unit == DOOR_UNIT &&
           g_log.release_mode == 3u,
       "state4: target_release_ref(player, door_unit, mode=3)");
    ck_eq((uint32_t)f.u(PLAYER, DOOR_UNIT).target2_ref, 0u, "state4: target2_ref cleared to 0");
    ck_eq((uint32_t)f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit, 0u,
          "state4: door_mutex_unit cleared to 0 @0x004777e8");
    ck_eq((uint32_t)g_log.free_calls, 1u, "state4: path_free_slot called exactly once @0x0047780a");
    ck(g_log.free_player == (uint16_t)PLAYER && g_log.free_unit == DOOR_UNIT,
       "state4: path_free_slot(player, door_unit)");
}

// ---- case 8: state 4, docked unit IS PARKED_2B but target2_ref==0 and path_slot_id==0xff -- the two
// INNER gates each independently suppress their own call, while the outer PARKED-transition and
// door-mutex-clear still fire.
void test_state4_docked_parked2b_no_ref_no_path_suppresses_both_inner_calls() {
    sim_fixture f;
    setup(f, /*online_state=*/4);
    cfg_building &cb = f.cfg_buildings[f.b(PLAYER, BLDG).building_id];
    cfg_anim_set(cb, 3, 300);
    f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit = DOOR_UNIT;
    f.u(PLAYER, DOOR_UNIT).state                                            = UNIT_STATE_PARKED_2B;
    f.u(PLAYER, DOOR_UNIT).target2_ref                                      = 0;    // already clear
    f.u(PLAYER, DOOR_UNIT).path_slot_id                                     = 0xff; // no path held
    const sim_view v                                                        = f.view();
    sim_store      s                                                        = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)g_log.release_calls, 0u,
          "state4: target2_ref already 0 -> target_release_ref NOT called @0x00477757");
    ck_eq((uint32_t)g_log.free_calls, 0u,
          "state4: path_slot_id==0xff -> path_free_slot NOT called @0x004777f1");
    ck_eq((uint32_t)f.u(PLAYER, DOOR_UNIT).state, (uint32_t)UNIT_STATE_PARKED,
          "state4: docked unit's state still -> PARKED even with both inner calls suppressed");
    ck_eq((uint32_t)f.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SUB_ID].door_mutex_unit, 0u,
          "state4: door_mutex_unit still cleared even with both inner calls suppressed");
}

// ---- case 9: the chain-follow branch (Anim[...].next != 0) advances anim[1] by `next` and consumes
// `time` from elapsed WITHOUT touching online_state or firing any callback -- proves the switch is
// reached ONLY when the chain has ended, not on every over-time iteration.
void test_chain_follow_advances_without_switch() {
    sim_fixture f;
    setup(f, /*online_state=*/0);
    // Override the chain-end setup: make the frame's `next` nonzero so the FIRST over-time iteration
    // takes the chain-follow branch instead of the switch, then the remainder is spent normally.
    f.anim_frames[FRAME_IDX + 1].next = 7;
    const sim_view v                  = f.view();
    sim_store      s                  = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck_eq((uint32_t)anim1_get(f.b(PLAYER, BLDG)), (uint32_t)(FRAME_IDX + 7),
          "chain-follow: anim[1] = FRAME_IDX + Anim[FRAME_IDX+1].next (40+7=47) @0x00477835-0x00477846");
    ck_eq((uint32_t)g_log.online_calls, 0u,
          "chain-follow: online handler NOT called -- the switch is reached only when next==0");
    ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 0u, "chain-follow: online_state untouched");
}

// ---- case 10: time == 0.0 on the looked-up frame -- the WHOLE elapsed-time update is skipped (not
// just spent instantly): anim_dur[1] is left UNTOUCHED, matching the bit-mask guard
// (0x004772bb-0x004772c8) collapsed here to a plain `time != 0.0` comparison (see the production
// header's equivalence note).
void test_zero_duration_frame_skips_whole_update() {
    sim_fixture f;
    setup(f, /*online_state=*/0);
    f.anim_frames[FRAME_IDX + 1].time = 0.0;
    const double   anim_dur_before    = f.b(PLAYER, BLDG).anim_dur[1];
    const sim_view v                  = f.view();
    sim_store      s                  = f.store();
    g_log.reset();

    detail::bldg_anim_state_helipad(v, s, mock_calls(), 0, 0, 0, 0);

    ck(f.b(PLAYER, BLDG).anim_dur[1] == anim_dur_before,
       "time==0.0: anim_dur[1] is left byte-identical -- the whole update is skipped, not spent instantly");
    ck_eq((uint32_t)g_log.online_calls, 0u, "time==0.0: no callback fires either");
}

} // namespace

void run_bldg_anim_state_helipad_tests() {
    test_state0_forwards_to_online_handler_no_efficiency_scaling();
    test_state1_unconditional_no_door_gate();
    test_state2_door_idle_no_state_change();
    test_state2_docked_taking_off_transitions_to_4();
    test_state3_no_docked_unit_no_finalize();
    test_state3_docked_parked28_finalizes_takeoff();
    test_state4_docked_parked2b_full_release();
    test_state4_docked_parked2b_no_ref_no_path_suppresses_both_inner_calls();
    test_chain_follow_advances_without_switch();
    test_zero_duration_frame_skips_whole_update();
}

} // namespace mh::sim::test
