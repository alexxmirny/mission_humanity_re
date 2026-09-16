//
// sim/sim_bldg_anim_tick.cpp -- see sim_bldg_anim_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_anim_tick_00476447.asm,
// tmp/decomp_sim/llm_strat_bldg_anim_state_helipad_a_00477231.asm,
// tmp/decomp_sim/llm_strat_bldg_anim_state_airfield_h_00477bed.asm), cross-checked against Ghidra's own
// already-typed decompile (all three functions are already fully RE'd with named fields -- see the
// header).
//
#include "sim/sim_bldg_anim_tick.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// SAME pattern sim_bldg_anim_trigger.cpp / sim_bldg_liftoff_anim.cpp / sim_map_create_building.cpp
// establish for the identical pair of flattened fields (mh_map_object_building::anim[48] /
// mh_cfg_final_struct_Building::anim[48], both really cfg_t_frame_index[12]) -- duplicated per-TU
// per this project's own established precedent.
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

// ---- the shared per-slot advance, once a slot has passed its function's own guard -----------------
// `time` is the value read ONCE at loop entry (see header: dVar1/dVar2 are the SAME memory read,
// modelled as one local) and stays fixed across the whole while-loop even as the chain-walk changes
// `b.anim[slot]`.
void advance_anim_slot(const sim_view &v, building &b, const cfg_building &cb, int32_t slot) {
    const double time    = v.anim_frames[anim_slot_get(b, slot) + 1].time;
    double       elapsed = (*v.game_clock - b.anim_dur[slot]) * b.efficiency;
    b.anim_dur[slot]     = *v.game_clock;
    while (elapsed > 0.0) {
        if (elapsed <= time) {
            b.anim_dur[slot] -= elapsed / b.efficiency;
            elapsed = 0.0;
        } else {
            const int32_t     cur_idx = anim_slot_get(b, slot);
            const anim_frame &frame   = v.anim_frames[cur_idx + 1];
            if (frame.next == 0) {
                anim_slot_set(b, slot, cfg_anim_slot_get(cb, slot + 1));
            } else {
                anim_slot_set(b, slot, cur_idx + frame.next);
            }
            elapsed -= time;
        }
    }
}

} // namespace

namespace detail {

// llm_strat_bldg_anim_tick @0x00476447. Guard: `anim[slot] != 0` (0x00476479).
void bldg_anim_tick(const sim_view &v, sim_store &own) {
    building           &b  = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    for (int32_t slot = 0; slot < cb.sprite_quantity; ++slot) {
        if (anim_slot_get(b, slot) != 0) advance_anim_slot(v, b, cb, slot);
    }
}

// llm_strat_bldg_anim_state_helipad_a @0x00477231. Guard: `0 < anim[slot]` (0x00477286, JLE-skip),
// PLUS a zero-duration skip on the looked-up frame's `time` (0x004772bb-0x004772c8, see header) --
// both differences from bldg_anim_tick preserved literally.
void bldg_anim_state_helipad_a(const sim_view &v, sim_store &own) {
    building           &b  = own.cur_building();
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    for (int32_t slot = 0; slot < cb.sprite_quantity; ++slot) {
        if (anim_slot_get(b, slot) > 0) {
            const double time = v.anim_frames[anim_slot_get(b, slot) + 1].time;
            if (time != 0.0) advance_anim_slot(v, b, cb, slot);
        }
    }
}

// llm_strat_bldg_anim_state_airfield_h @0x00477bed. Guard: `0 < anim[slot]` (like helipad_a). NO
// zero-duration skip (like anim_tick) -- the third guard/skip combination, see header.
void bldg_anim_state_airfield_h(const sim_view &v, sim_store &own) {
    building           &b  = own.cur_building();
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    for (int32_t slot = 0; slot < cb.sprite_quantity; ++slot) {
        if (anim_slot_get(b, slot) > 0) advance_anim_slot(v, b, cb, slot);
    }
}

// llm_strat_bldg_anim_state_turret @0x00476605. Genuine no-op -- see the header derivation.
void bldg_anim_state_turret() {}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void bldg_anim_tick() {
    sim_state st = state();
    detail::bldg_anim_tick(st.read, st.own);
}
void bldg_anim_state_helipad_a() {
    sim_state st = state();
    detail::bldg_anim_state_helipad_a(st.read, st.own);
}
void bldg_anim_state_airfield_h() {
    sim_state st = state();
    detail::bldg_anim_state_airfield_h(st.read, st.own);
}
void bldg_anim_state_turret() { detail::bldg_anim_state_turret(); }


} // namespace mh::sim
