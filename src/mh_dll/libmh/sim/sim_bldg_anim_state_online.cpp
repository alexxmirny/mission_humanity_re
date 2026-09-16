//
// sim/sim_bldg_anim_state_online.cpp -- see sim_bldg_anim_state_online.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_anim_state_{barracks_garage_a,vehicles_h,soldiers_h,
// helipad,airfield_a,shuttle_a,shuttle_h,online_toggle}_*.asm), cross-checked against Ghidra's own
// already-typed decompile (all eight are already fully RE'd with named fields -- see the header).
//
#include "sim/sim_bldg_anim_state_online.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
// LIB-VA-PROMOTE (2026-09-08): the four `llm_strat_bldg_online_<kind>` handlers this file's state-0
// arms reach are VERIFIED sim rows with C++ bodies, and were still reached through the VA. They now
// call the detail:: overload directly, with the SAME (v, own) this body already holds -- NOT the
// public wrapper, which re-fetches state() and would take the offline oracle's fixture out from
// under them.
#include "sim/sim_bldg_register_online.h"

namespace mh::sim {

namespace {

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

// No backing Ghidra C++ enum for this domain (same posture sim_dock_slot_is_busy.h/
// sim_unit_state_enter.h already establish) -- values confirmed against Ghidra's OWN
// `llm_strat_unit_state` enum (dumped via run-script).
inline constexpr uint16_t UNIT_STATE_TAKEOFF   = 0x15;
inline constexpr uint16_t UNIT_STATE_PARKED    = 0x1f;
inline constexpr uint16_t UNIT_STATE_PARKED_28 = 0x28;
inline constexpr uint16_t UNIT_STATE_PARKED_2B = 0x2b;

// door_mutex_unit==0 && door_waiter_count==0 -- the shared "door is idle" gate all five state-1/state-2
// branches test (unit_storage[player][sub_id]).
bool door_is_idle(const sim_view &v, sim_store &own, uint16_t player, uint8_t sub_id) {
    (void)v;
    unit_storage &st = own.storage_at(player, sub_id);
    return st.door_mutex_unit == 0 && st.door_waiter_count == 0;
}

// llm_strat_bldg_anim_state_airfield_a's online-state switch, pulled out of its while-loop into its own
// function (MSVC ASan build hit an internal compiler error -- C1001 -- on the combined while+switch
// inline; extraction is a pure refactor with no behavioural change, verified by the unaffected offline
// oracle/rig results before and after).
void airfield_a_switch(const sim_view &v, sim_store &own, const cfg_building &cb, building &b,
                       uint32_t unused_ebx, uint32_t unused_ecx) {
    switch (b.online_state) {
        case 0:
            MH_LIBMH_BIND(llm_strat_bldg_online_airfield_a)(static_cast<int16_t>(*v.cur_player),
                                                            static_cast<int32_t>(*v.cur_index), unused_ebx,
                                                            unused_ecx, *v.game_clock);
            break;
        case 1:
            if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                anim_slot_set(b, 3, cfg_anim_slot_get(cb, 3));
            } else {
                anim_slot_set(b, 3, cfg_anim_slot_get(cb, 4));
                b.online_state = 3;
            }
            break;
        case 2:
            if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                anim_slot_set(b, 3, cfg_anim_slot_get(cb, 5));
                b.online_state = 4;
            } else {
                anim_slot_set(b, 3, cfg_anim_slot_get(cb, 6));
            }
            break;
        case 3:
            anim_slot_set(b, 3, cfg_anim_slot_get(cb, 6));
            b.online_state = 2;
            break;
        case 4:
            anim_slot_set(b, 3, cfg_anim_slot_get(cb, 3));
            b.online_state = 1;
            break;
    }
}

} // namespace

const bldg_anim_state_helipad_calls &live_bldg_anim_state_helipad_calls() {
    static const bldg_anim_state_helipad_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_online_helipad_h_or_misc),
        MH_LIBMH_BIND(llm_strat_unit_takeoff_finalize),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
    };
    return c;
}

namespace detail {

// llm_strat_bldg_anim_state_barracks_garage_a @0x00476627. slot = 0 (fixed).
void bldg_anim_state_barracks_garage_a(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                       uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    constexpr int32_t   slot = 0;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    double elapsed   = (*v.game_clock - b.anim_dur[slot]) * b.efficiency;
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed / b.efficiency;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        switch (b.online_state) {
            case 0:
                bldg_online_barracks_garage_a(v, own, static_cast<int16_t>(*v.cur_player),
                                              static_cast<int32_t>(*v.cur_index), unused_ebx, unused_ecx,
                                              *v.game_clock);
                break;
            case 1:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 2));
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                    b.online_state = 3;
                }
                break;
            case 2:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                    b.online_state = 4;
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                }
                break;
            case 3:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                b.online_state = 2;
                break;
            case 4:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 2));
                b.online_state = 1;
                break;
        }
        elapsed -= time;
    }
}

// llm_strat_bldg_anim_state_vehicles_h @0x004769a9. slot = (online_state!=0)?1:0, PLUS a second,
// independent tick of anim[4] while online (see header).
void bldg_anim_state_vehicles_h(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    const int32_t       slot = (b.online_state != 0) ? 1 : 0;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time != 0.0) {
        double elapsed   = (*v.game_clock - b.anim_dur[slot]) * b.efficiency;
        b.anim_dur[slot] = *v.game_clock;
        while (elapsed > 0.0) {
            if (elapsed <= time) {
                b.anim_dur[slot] -= elapsed / b.efficiency;
                elapsed = 0.0;
                continue;
            }
            const int32_t     cur_idx = anim_slot_get(b, slot);
            const anim_frame &frame   = v.anim_frames[cur_idx + 1];
            if (frame.next != 0) {
                anim_slot_set(b, slot, cur_idx + frame.next);
                elapsed -= time;
                continue;
            }
            switch (b.online_state) {
                case 0:
                    bldg_online_vehicles_h(v, own, static_cast<int32_t>(*v.cur_player),
                                           static_cast<int32_t>(*v.cur_index), unused_ebx, unused_ecx,
                                           *v.game_clock);
                    break;
                case 1:
                    if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                        anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                    } else {
                        anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                        b.online_state = 3;
                    }
                    break;
                case 2:
                    if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                        anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                        b.online_state = 4;
                    } else {
                        anim_slot_set(b, slot, cfg_anim_slot_get(cb, 2));
                    }
                    break;
                case 3:
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 2));
                    b.online_state = 2;
                    break;
                case 4:
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                    b.online_state = 1;
                    break;
            }
            elapsed -= time;
        }
    }

    // ---- the second, independent slot-4 tick, gated ONLY on online_state != 0 (no anim[4] guard) ----
    if (b.online_state != 0) {
        constexpr int32_t idle_slot = 4;
        const double      idle_time = v.anim_frames[anim_slot_get(b, idle_slot) + 1].time;
        if (idle_time != 0.0) {
            double elapsed        = (*v.game_clock - b.anim_dur[idle_slot]) * b.efficiency;
            b.anim_dur[idle_slot] = *v.game_clock;
            while (elapsed > 0.0) {
                if (elapsed <= idle_time) {
                    b.anim_dur[idle_slot] -= elapsed / b.efficiency;
                    elapsed = 0.0;
                } else {
                    const int32_t     cur_idx = anim_slot_get(b, idle_slot);
                    const anim_frame &frame   = v.anim_frames[cur_idx + 1];
                    if (frame.next == 0) {
                        anim_slot_set(b, idle_slot, cfg_anim_slot_get(cb, 8));
                    } else {
                        anim_slot_set(b, idle_slot, cur_idx + frame.next);
                    }
                    elapsed -= idle_time;
                }
            }
        }
    }
}

// llm_strat_bldg_anim_state_soldiers_h @0x00476e9d. Same shape as vehicles_h's primary block, NO second
// tick block.
// WORKAROUND, and it is about the COMPILER, not about this function. MSVC 19.44's LTCG backend
// hits an internal compiler error (C1001, compiler file p2/main.cpp:258) here when the ASan build
// of mh_nettest grows past some internal threshold. Nothing about this code is wrong or unusual --
// it was named by three different unrelated edits in one session, none of which touched it or
// anything it includes, and it built fine before and after each of them. Bisecting to a "cause"
// therefore produced three confident wrong answers.
//
// The pragma is scoped to this one function and costs a test-only binary nothing measurable; the
// PLAIN build is unaffected, so the optimised shape of this code is still compiled and still runs
// in every gate.
#pragma optimize("", off)
void bldg_anim_state_soldiers_h(const sim_view &v, sim_store &own, uint32_t unused_eax, uint32_t unused_edx,
                                uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    const int32_t       slot = (b.online_state != 0) ? 1 : 0;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    double elapsed   = (*v.game_clock - b.anim_dur[slot]) * b.efficiency;
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed / b.efficiency;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        switch (b.online_state) {
            case 0:
                bldg_online_soldiers_h(v, own, static_cast<int16_t>(*v.cur_player),
                                       static_cast<int32_t>(*v.cur_index), unused_ebx, unused_ecx,
                                       *v.game_clock);
                break;
            case 1:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                    b.online_state = 3;
                }
                break;
            case 2:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 6));
                    b.online_state = 4;
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                }
                break;
            case 3:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                b.online_state = 2;
                break;
            case 4:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                b.online_state = 1;
                break;
        }
        elapsed -= time;
    }
}

#pragma optimize("", on)


// llm_strat_bldg_anim_state_helipad @0x004773f4. slot = 1 (fixed). NO efficiency scaling (see header --
// a real, asm-confirmed difference from its four siblings). Also completes the docked unit's takeoff/
// landing off the door mutex.
void bldg_anim_state_helipad(const sim_view &v, sim_store &own, const bldg_anim_state_helipad_calls &c,
                             uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                             uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    constexpr int32_t   slot = 1;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    double elapsed   = *v.game_clock - b.anim_dur[slot]; // NO efficiency scaling -- see header
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed; // NO efficiency scaling
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        switch (b.online_state) {
            case 0:
                c.online_helipad_h_or_misc(static_cast<int16_t>(*v.cur_player),
                                           static_cast<int32_t>(*v.cur_index), unused_ebx, unused_ecx,
                                           *v.game_clock);
                break;
            case 1:
                // UNCONDITIONAL -- no door-idle gate, unlike every sibling family's state1.
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                b.online_state = 3;
                break;
            case 2: {
                const int32_t door_unit = own.storage_at(*v.cur_player, b.sub_id).door_mutex_unit;
                const bool    docked_taking_or_landing =
                    door_unit != 0 && (own.unit_at(*v.cur_player, door_unit).state == UNIT_STATE_PARKED_28 ||
                                       own.unit_at(*v.cur_player, door_unit).state == UNIT_STATE_PARKED_2B);
                if (!docked_taking_or_landing) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 6));
                    b.online_state = 4;
                }
                break;
            }
            case 3: {
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                b.online_state          = 2;
                const int32_t door_unit = own.storage_at(*v.cur_player, b.sub_id).door_mutex_unit;
                if (door_unit != 0 && own.unit_at(*v.cur_player, door_unit).state == UNIT_STATE_PARKED_28) {
                    c.unit_takeoff_finalize(*v.cur_player, door_unit);
                    own.unit_at(*v.cur_player, door_unit).state = UNIT_STATE_TAKEOFF;
                }
                break;
            }
            case 4: {
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                b.online_state          = 1;
                const int32_t door_unit = own.storage_at(*v.cur_player, b.sub_id).door_mutex_unit;
                if (door_unit != 0 && own.unit_at(*v.cur_player, door_unit).state == UNIT_STATE_PARKED_2B) {
                    unit &docked = own.unit_at(*v.cur_player, door_unit);
                    docked.state = UNIT_STATE_PARKED;
                    if (docked.target2_ref != 0) {
                        c.target_release_ref(*v.cur_player, door_unit, /*mode=*/3);
                        docked.target2_ref = 0;
                        // extraout_EBX here is a confirmed dead store -- see header. Omitted.
                    }
                    own.storage_at(*v.cur_player, b.sub_id).door_mutex_unit = 0;
                    if (docked.path_slot_id != 0xff) {
                        c.path_free_slot(*v.cur_player, door_unit);
                    }
                }
                break;
            }
        }
        elapsed -= time;
    }
}

// llm_strat_bldg_anim_state_airfield_a @0x00477881. slot = (anim[1]<0)?3:0 selects which frame is
// TICKED, but the switch branches all write literal anim[3] (see header) -- reproduced literally.
void bldg_anim_state_airfield_a(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    const int32_t       slot = (anim_slot_get(b, 1) < 0) ? 3 : 0;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    double elapsed   = (*v.game_clock - b.anim_dur[slot]) * b.efficiency;
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed / b.efficiency;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            // The ONE branch that uses `slot`, not the literal 3 -- see header.
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        airfield_a_switch(v, own, cb, b, unused_ebx, unused_ecx);
        elapsed -= time;
    }
}

// llm_strat_bldg_anim_state_shuttle_a @0x00477d9e. slot = 3 (fixed). Calls
// llm_strat_bldg_online_shuttle_a(int16_t player, ...) in state 0.
void bldg_anim_state_shuttle_a(const sim_view &v, sim_store &own, uint32_t unused_eax, uint32_t unused_edx,
                               uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    constexpr int32_t   slot = 3;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    // NO efficiency scaling -- bare FSUB at 0x00477e0c, bare FSUB at 0x0047813c, and NO reference to
    // building+0x29 (efficiency) anywhere in the body. Contrast airfield_a above, which really does
    // scale (FMUL [EAX+0x29] @0x0047790d, FDIV [EDX+0x29] @0x00477bc7). See the header's table.
    double elapsed   = *v.game_clock - b.anim_dur[slot];
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        switch (b.online_state) {
            case 0:
                MH_LIBMH_BIND(llm_strat_bldg_online_shuttle_a)(static_cast<int16_t>(*v.cur_player),
                                                               static_cast<int32_t>(*v.cur_index), unused_ebx,
                                                               unused_ecx, *v.game_clock);
                break;
            case 1:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                    b.online_state = 3;
                }
                break;
            case 2:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                    b.online_state = 4;
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 6));
                }
                break;
            case 3:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 6));
                b.online_state = 2;
                break;
            case 4:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                b.online_state = 1;
                break;
            case 10:
                b.online_state = 0;
                break;
            case 11:
                b.online_state = 0xc;
                break;
                // states 5-9: no matching case in the original -- nothing happens at chain end.
        }
        elapsed -= time;
    }
}

// llm_strat_bldg_anim_state_shuttle_h @0x0047815f. slot = 2 (fixed). IDENTICAL cfg.anim[]/state shape
// to shuttle_a (see header) -- only the slot and the online_<kind> callee differ.
void bldg_anim_state_shuttle_h(const sim_view &v, sim_store &own, uint32_t unused_eax, uint32_t unused_edx,
                               uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    constexpr int32_t   slot = 2;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    // NO efficiency scaling -- bare FSUB at 0x004781cd, bare FSUB at 0x004784fd, and NO reference to
    // building+0x29 (efficiency) anywhere in the body. Same as shuttle_a; see the header's table.
    double elapsed   = *v.game_clock - b.anim_dur[slot];
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        switch (b.online_state) {
            case 0:
                MH_LIBMH_BIND(llm_strat_bldg_online_shuttle_h)(static_cast<int16_t>(*v.cur_player),
                                                               static_cast<int32_t>(*v.cur_index), unused_ebx,
                                                               unused_ecx, *v.game_clock);
                break;
            case 1:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 4));
                    b.online_state = 3;
                }
                break;
            case 2:
                if (door_is_idle(v, own, *v.cur_player, b.sub_id)) {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 5));
                    b.online_state = 4;
                } else {
                    anim_slot_set(b, slot, cfg_anim_slot_get(cb, 6));
                }
                break;
            case 3:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 6));
                b.online_state = 2;
                break;
            case 4:
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 3));
                b.online_state = 1;
                break;
            case 10:
                b.online_state = 0;
                break;
            case 11:
                b.online_state = 0xc;
                break;
                // states 5-9: no matching case in the original -- nothing happens at chain end.
        }
        elapsed -= time;
    }
}

// llm_strat_bldg_anim_state_online_toggle @0x00478520. slot = 0 (fixed). Shared across SIX building
// types. Calls llm_strat_bldg_online_default(int32_t player, ...) in state 0 -- NOT a `_<kind>`
// sibling, and its player param is int32_t (not int16_t like shuttle_a/_h's callee) -- see header.
void bldg_anim_state_online_toggle(const sim_view &v, sim_store &own, uint32_t unused_eax,
                                   uint32_t unused_edx, uint32_t unused_ebx, uint32_t unused_ecx) {
    (void)unused_eax;
    (void)unused_edx;
    building           &b    = own.cur_building();
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    constexpr int32_t   slot = 0;
    const double        time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    if (time == 0.0) return;

    // NO efficiency scaling. The original computes elapsed with a bare FSUB (no FMUL) and applies the
    // correction with a bare FSUB (no FDIV) -- verified in the listing 2026-09-05. A `* efficiency` /
    // `/ efficiency` round-trip here is NOT a no-op: it leaks 1/efficiency into the frame-chain term,
    // which is exactly how SIM-SAVE-DIV surfaced (predicted 1/0.9904 - 1 = 0.0096930533 on a damaged
    // building; measured 0.009693053311). Match bldg_anim_state_helipad, the sibling translated right.
    double elapsed   = *v.game_clock - b.anim_dur[slot];
    b.anim_dur[slot] = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, slot);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, slot, cur_idx + frame.next);
            elapsed -= time;
            continue;
        }
        const uint16_t state = b.online_state;
        if (state < 10) {
            if (state == 0) {
                bldg_online_default(v, own, static_cast<int32_t>(*v.cur_player),
                                    static_cast<int32_t>(*v.cur_index), unused_ebx, unused_ecx,
                                    *v.game_clock);
            } else {
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, 1));
            }
        } else if (state < 11) {
            b.online_state = 0;
        } else if (state == 11) {
            b.online_state = 0xc;
        } else {
            anim_slot_set(b, slot, cfg_anim_slot_get(cb, 1));
        }
        elapsed -= time;
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void bldg_anim_state_barracks_garage_a(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                       uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_barracks_garage_a(st.read, st.own, unused_eax, unused_edx, unused_ebx,
                                              unused_ecx);
}
void bldg_anim_state_vehicles_h(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_vehicles_h(st.read, st.own, unused_eax, unused_edx, unused_ebx, unused_ecx);
}
void bldg_anim_state_soldiers_h(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_soldiers_h(st.read, st.own, unused_eax, unused_edx, unused_ebx, unused_ecx);
}
void bldg_anim_state_helipad(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                             uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_helipad(st.read, st.own, live_bldg_anim_state_helipad_calls(), unused_eax,
                                    unused_edx, unused_ebx, unused_ecx);
}
void bldg_anim_state_airfield_a(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_airfield_a(st.read, st.own, unused_eax, unused_edx, unused_ebx, unused_ecx);
}
void bldg_anim_state_shuttle_a(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                               uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_shuttle_a(st.read, st.own, unused_eax, unused_edx, unused_ebx, unused_ecx);
}
void bldg_anim_state_shuttle_h(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                               uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_shuttle_h(st.read, st.own, unused_eax, unused_edx, unused_ebx, unused_ecx);
}
void bldg_anim_state_online_toggle(uint32_t unused_eax, uint32_t unused_edx, uint32_t unused_ebx,
                                   uint32_t unused_ecx) {
    sim_state st = state();
    detail::bldg_anim_state_online_toggle(st.read, st.own, unused_eax, unused_edx, unused_ebx, unused_ecx);
}


} // namespace mh::sim
