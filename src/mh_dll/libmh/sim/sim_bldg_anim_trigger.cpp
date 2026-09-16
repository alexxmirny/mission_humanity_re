//
// sim/sim_bldg_anim_trigger.cpp -- see sim_bldg_anim_trigger.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_start_special_anim_00475b75.asm,
// llm_strat_bldg_anim_state_trigger_00475e51.asm).
//
#include "sim/sim_bldg_anim_trigger.h"

#include <cstring>

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// SAME pattern sim_bldg_liftoff_anim.cpp / sim_map_create_building.cpp establish for the identical
// pair of flattened fields (mh_map_object_building::anim[48] / mh_cfg_final_struct_Building::anim[48],
// both really cfg_t_frame_index[12]) -- duplicated per-TU rather than shared, same precedent every
// other sim/ TU with this helper follows (avoids an ODR collision with a differently-scoped copy
// elsewhere).
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

// Reconstructs a double from a (lo, hi) raw dword pair, bit-for-bit -- the SAME helper
// sim_bldg_liftoff_anim.cpp uses for its own identically-shaped param_5/param_6 pair. Unlike that
// file, this TU cannot independently confirm the pair's origin (no caller assembly in view for
// llm_strat_bldg_anim_state_trigger) -- see UNCERTAINTIES. The reconstruction is correct regardless
// of what the two dwords represent, since it is a plain bit copy with no arithmetic.
inline double combine_dword_pair(uint32_t lo, uint32_t hi) {
    uint64_t bits = (uint64_t)lo | ((uint64_t)hi << 32);
    double   result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// This file's own literal operands -- both are real, already-named `cfg.anim`
// (cfg_t_frame_index[12]) sub-indices, just not documented per-slot (same posture
// sim_bldg_liftoff_anim.cpp's own SHUTTLE_/MOTHER_LIFTOFF_ANIM_CFG_SLOT constants document).
inline constexpr int32_t SPECIAL_ANIM_CFG_SLOT = 2; // Building[bid].anim[2], cfg-relative offset
                                                    // 0x157 (0x157-0x14f=8 bytes into `anim`)
inline constexpr int32_t TRIGGER_ANIM_CFG_SLOT = 4; // Building[bid].anim[4], cfg-relative offset
                                                    // 0x15f (0x15f-0x14f=0x10 bytes into `anim`)

// online_state's value on both paths (0x00475ccf / 0x00475ee5) -- no backing Ghidra enum, same
// posture sim_bldg_liftoff_anim.cpp's own LIFTOFF_ONLINE_STATE documents for online_state's
// per-family overload (this family's own value, 0xa, differs from the liftoff family's 0xb).
inline constexpr int16_t ANIM_TRIGGER_ONLINE_STATE = 0xa;

} // namespace

namespace detail {

void bldg_start_special_anim(const sim_view &v, sim_store &own, uint16_t player, int32_t b_index,
                             uint32_t unused_ebx, uint32_t unused_ecx, double timestamp) {
    // unused_ebx(EBX)/unused_ecx(ECX) are DEAD -- see the header derivation. Kept for prototype
    // fidelity.
    (void)unused_ebx;
    (void)unused_ecx;

    building &b = own.building_at(player, b_index);

    // ---- anim[0..2] zeroed, anim[3] re-seeded from the cfg record's own slot 2 frame
    // (0x00475b92-0x00475c22) -- all four RAW 4-byte stores into the flattened uint8_t[48] field.
    store_u32_le(&b.anim[0 * 4], 0);
    store_u32_le(&b.anim[1 * 4], 0);
    store_u32_le(&b.anim[2 * 4], 0);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[SPECIAL_ANIM_CFG_SLOT * 4]));

    // ---- anim_dur[0..3] <- timestamp, stamped four times (0x00475c28-0x00475cbc) -- ALREADY a
    // committed `double` parameter, no dword-pair reconstruction needed (unlike
    // bldg_anim_state_trigger below).
    for (int32_t i = 0; i < 4; ++i) b.anim_dur[i] = timestamp;

    // ---- unconditional tail (0x00475cbc-0x00475ccf)
    b.online_state = ANIM_TRIGGER_ONLINE_STATE;
}

void bldg_anim_state_trigger(const sim_view &v, sim_store &own, uint32_t param_1, int32_t param_2,
                             uint32_t param_3, uint32_t param_4, uint32_t param_5,
                             uint32_t param_6) {
    // param_3(EBX)/param_4(ECX) are DEAD -- see the header derivation. Kept for prototype fidelity.
    (void)param_3;
    (void)param_4;

    const uint16_t player = static_cast<uint16_t>(param_1); // asm: MOVZX word ptr, i.e. param_1 & 0xffff
    building      &b      = own.building_at(player, param_2);

    // ---- anim[0] <- the cfg record's own slot 4 frame (0x00475e6e-0x00475ea7) -- only anim[0] is
    // touched, no zeroing of anim[1..3] (same single-slot shape as the liftoff-anim mother sibling).
    const cfg_building &cfg = v.cfg_buildings[b.building_id];
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[TRIGGER_ANIM_CFG_SLOT * 4]));

    // ---- anim_dur[0] <- reconstructed from (param_5=lo, param_6=hi) (0x00475ead-0x00475ecc) -- ONE
    // write, not four.
    b.anim_dur[0] = combine_dword_pair(param_5, param_6);

    // ---- unconditional tail (0x00475ed2-0x00475ee5)
    b.online_state = ANIM_TRIGGER_ONLINE_STATE;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_start_special_anim(uint16_t player, int32_t b_index, uint32_t unused_ebx, uint32_t unused_ecx,
                             double timestamp) {
    sim_state st = state();
    detail::bldg_start_special_anim(st.read, st.own, player, b_index, unused_ebx, unused_ecx, timestamp);
}

void bldg_anim_state_trigger(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                             uint32_t param_5, uint32_t param_6) {
    sim_state st = state();
    detail::bldg_anim_state_trigger(st.read, st.own, param_1, param_2, param_3, param_4, param_5,
                                    param_6);
}


} // namespace mh::sim
