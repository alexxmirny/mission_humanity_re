//
// sim/sim_bldg_anim_state_port.cpp -- see sim_bldg_anim_state_port.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_anim_state_port_004786f5.asm,
// tmp/decomp_sim/llm_strat_bldg_anim_state_port_h_00478bfe.asm), cross-checked against Ghidra's own
// already-typed decompile (both functions are already fully RE'd with named fields -- see the header).
//
#include "sim/sim_bldg_anim_state_port.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// SAME pattern sim_bldg_anim_tick.cpp / sim_bldg_anim_state_online.cpp establish for the identical
// pair of flattened fields -- duplicated per-TU per this project's own established precedent.
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
// sim_bldg_anim_state_online.h already establish) -- values confirmed against Ghidra's OWN
// `llm_strat_unit_state` enum (dumped via run-script).
inline constexpr uint16_t UNIT_STATE_TAKEOFF_TAXI = 0x27;
inline constexpr uint16_t UNIT_STATE_LANDING      = 0x16;
inline constexpr uint16_t UNIT_STATE_DOCK_TAXI_IN = 0x2a;

// Advance one animation slot by the shared "credit clock" mechanism (see sim_bldg_anim_tick.h),
// gated ONLY by the zero-duration check (no `anim[slot] > 0` pre-guard -- callers that need one apply
// it themselves before calling), restarting to `cb.anim[restart_idx]` at chain end. `restart_idx` is
// an explicit parameter because port's slot 1 restarts to a DIFFERENT index (6) than the generic
// `slot+1` convention (port_h's slot 1 uses restart_idx=2, which IS slot+1 -- see header).
void advance_anim_slot_restart(const sim_view &v, building &b, const cfg_building &cb, int32_t slot,
                               int32_t restart_idx) {
    const double time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
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
        } else {
            anim_slot_set(b, slot, cfg_anim_slot_get(cb, restart_idx));
        }
        elapsed -= time;
    }
}

} // namespace

namespace detail {

// llm_strat_bldg_anim_state_port_h @0x00478bfe. Slot 1, restart to cfg.anim[2] (slot+1), no pre-guard.
void bldg_anim_state_port_h(const sim_view &v, sim_store &own) {
    building           &b  = own.cur_building();
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    advance_anim_slot_restart(v, b, cb, /*slot=*/1, /*restart_idx=*/2);
}

// llm_strat_bldg_anim_state_port @0x004786f5. Two independent slots -- see header.
void bldg_anim_state_port(const sim_view &v, sim_store &own) {
    building           &b  = own.cur_building();
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // slot 1 (idle) -- gated additionally by `anim[1] > 0` (unlike port_h). Restart to cfg.anim[6],
    // NOT the generic slot+1 convention -- see header.
    if (anim_slot_get(b, 1) > 0) advance_anim_slot_restart(v, b, cb, /*slot=*/1, /*restart_idx=*/6);

    // slot 3 (pad door) -- no `anim[3] > 0` pre-guard; chain end runs the door online-state switch
    // instead of a plain cfg.anim[] restart.
    const double time3 = v.anim_frames[anim_slot_get(b, 3) + 1].time;
    if (time3 == 0.0) return;

    double elapsed = (*v.game_clock - b.anim_dur[3]) * b.efficiency;
    b.anim_dur[3]  = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time3) {
            b.anim_dur[3] -= elapsed / b.efficiency;
            elapsed = 0.0;
            continue;
        }
        const int32_t     cur_idx = anim_slot_get(b, 3);
        const anim_frame &frame   = v.anim_frames[cur_idx + 1];
        if (frame.next != 0) {
            anim_slot_set(b, 3, cur_idx + frame.next);
            elapsed -= time3;
            continue;
        }
        switch (b.online_state) {
            case 1: {
                const int32_t door_unit = own.storage_at(*v.cur_player, b.sub_id).door_mutex_unit;
                const bool    idle_or_other =
                    door_unit == 0 || (own.unit_at(*v.cur_player, door_unit).state != UNIT_STATE_TAKEOFF_TAXI &&
                                       own.unit_at(*v.cur_player, door_unit).state != UNIT_STATE_LANDING);
                if (idle_or_other) {
                    anim_slot_set(b, 3, cfg_anim_slot_get(cb, 2));
                } else {
                    anim_slot_set(b, 3, cfg_anim_slot_get(cb, 4));
                    b.online_state = 3;
                }
                break;
            }
            case 2: {
                const int32_t door_unit = own.storage_at(*v.cur_player, b.sub_id).door_mutex_unit;
                // Reproduced literally from the original's boolean, not algebraically simplified --
                // see header.
                const bool not_taxi_in =
                    door_unit == 0 || own.unit_at(*v.cur_player, door_unit).state != UNIT_STATE_DOCK_TAXI_IN;
                if (not_taxi_in && door_unit != 0) {
                    anim_slot_set(b, 3, cfg_anim_slot_get(cb, 3));
                } else {
                    anim_slot_set(b, 3, cfg_anim_slot_get(cb, 5));
                    b.online_state = 4;
                }
                break;
            }
            case 3:
                anim_slot_set(b, 3, cfg_anim_slot_get(cb, 3));
                b.online_state = 2;
                break;
            case 4:
                anim_slot_set(b, 3, cfg_anim_slot_get(cb, 2));
                b.online_state = 1;
                break;
            default:
                break; // states 0, 5+: no case in the original -- nothing happens at chain end.
        }
        elapsed -= time3;
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void bldg_anim_state_port() {
    sim_state st = state();
    detail::bldg_anim_state_port(st.read, st.own);
}
void bldg_anim_state_port_h() {
    sim_state st = state();
    detail::bldg_anim_state_port_h(st.read, st.own);
}


} // namespace mh::sim
