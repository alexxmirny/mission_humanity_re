//
// sim/sim_bldg_liftoff_anim.cpp -- see sim_bldg_liftoff_anim.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_start_liftoff_anim_shuttle_00475ce3.asm,
// _mother_00475ef9.asm), cross-checked against the Ghidra .c drafts (tmp/decomp_sim/*.c) -- both agree
// with the assembly on shape.
//
#include "sim/sim_bldg_liftoff_anim.h"

#include <cstring>

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// SAME pattern sim_map_create_building.cpp's own store_u32_le/load_u32_le establish for the identical
// pair of flattened fields (mh_map_object_building::anim[48] / mh_cfg_final_struct_Building::anim[48],
// both really cfg_t_frame_index[12]) -- duplicated per-TU rather than shared, same precedent every other
// sim/ TU with this helper follows (avoids an ODR collision with a differently-scoped copy elsewhere).
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

// Reconstructs the double GAME_CLOCK value the caller (llm_strat_bldg_state_deploy_start,
// sim_bldg_state_deploy.cpp) split into two raw dwords before this call -- the INVERSE of that file's own
// split_game_clock() helper. memcpy, not a reinterpret_cast, matching this project's other bit-pattern
// helpers (see the header's derivation on why param_5/param_6 are GAME_CLOCK's low/high dwords, not an
// independent pair of scalars).
inline double combine_dword_pair(uint32_t lo, uint32_t hi) {
    uint64_t bits = (uint64_t)lo | ((uint64_t)hi << 32);
    double   result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// This file's own literal operands -- see the header's DECLARED NEED note: both are real, already-named
// `cfg.anim` (cfg_t_frame_index[12]) sub-indices, just not documented per-slot. Per-TU anonymous
// namespace, same precedent every other sim/bldg_state_*.cpp file's own local constants follow.
inline constexpr int32_t SHUTTLE_LIFTOFF_ANIM_CFG_SLOT = 10; // Building[bid].anim[10], byte offset 0x28
                                                             // into the flattened array (cfg-relative
                                                             // 0x177 -- see header derivation)
inline constexpr int32_t MOTHER_LIFTOFF_ANIM_CFG_SLOT = 5;   // Building[bid].anim[5], byte offset 0x14
                                                             // into the flattened array (cfg-relative
                                                             // 0x163 -- see header derivation)

// online_state's value on both paths (0x00475e3d / 0x00475f8d) -- no backing Ghidra enum, same posture
// sim_bldg_state_deploy.cpp's own DEPLOY_ANIM_WAIT_ONLINE_STATE_ARRIVED/LAND_ACTIVATE_ONLINE_STATE_
// ALREADY_LANDED constants document for online_state's per-family overload.
inline constexpr int16_t LIFTOFF_ONLINE_STATE = 0xb;

} // namespace

namespace detail {

void bldg_start_liftoff_anim_shuttle(const sim_view &v, sim_store &own, uint32_t param_1, int32_t param_2,
                                     uint32_t param_3, uint32_t param_4, uint32_t param_5,
                                     uint32_t param_6) {
    // param_3(EBX)/param_4(ECX) are DEAD -- see the header derivation. Kept for prototype fidelity.
    (void)param_3;
    (void)param_4;

    const uint16_t player = static_cast<uint16_t>(param_1); // asm: MOVZX word ptr, i.e. param_1 & 0xffff
    building      &b      = own.building_at(player, param_2);

    // ---- anim[0..2] zeroed, anim[3] re-seeded from the cfg record's own liftoff frame
    // (0x00475d13-0x00475d90) -- all four RAW 4-byte stores into the flattened uint8_t[48] field, same
    // pattern sim_map_create_building.cpp's own anim[0] re-seed uses.
    store_u32_le(&b.anim[0 * 4], 0);
    store_u32_le(&b.anim[1 * 4], 0);
    store_u32_le(&b.anim[2 * 4], 0);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[SHUTTLE_LIFTOFF_ANIM_CFG_SLOT * 4]));

    // ---- anim_dur[0..3] <- GAME_CLOCK, reconstructed from (param_5=lo, param_6=hi) (0x00475da9-0x00475e24)
    // -- see the header's CORRECTION note: this is anim_dur, NOT pip_frame/pip_timer. All four entries get
    // the SAME value (the asm re-reads the identical two stack slots four times, never four distinct
    // timestamps).
    const double anim_dur_stamp = combine_dword_pair(param_5, param_6);
    for (int32_t i = 0; i < 4; ++i) b.anim_dur[i] = anim_dur_stamp;

    // ---- unconditional tail (0x00475e2a-0x00475e3d)
    b.online_state = LIFTOFF_ONLINE_STATE;
}

void bldg_start_liftoff_anim_mother(const sim_view &v, sim_store &own, uint32_t param_1, int32_t param_2,
                                    uint32_t param_3, uint32_t param_4, uint32_t param_5,
                                    uint32_t param_6) {
    // param_3(EBX)/param_4(ECX) are DEAD -- see the header derivation. Kept for prototype fidelity.
    (void)param_3;
    (void)param_4;

    const uint16_t player = static_cast<uint16_t>(param_1); // asm: MOVZX word ptr, i.e. param_1 & 0xffff
    building      &b      = own.building_at(player, param_2);

    // ---- anim[0] <- the cfg record's own (DIFFERENT sub-index) liftoff frame (0x00475f29-0x00475f4f)
    const cfg_building &cfg = v.cfg_buildings[b.building_id];
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[MOTHER_LIFTOFF_ANIM_CFG_SLOT * 4]));

    // ---- anim_dur[0] <- GAME_CLOCK, reconstructed from (param_5=lo, param_6=hi) (0x00475f68-0x00475f74)
    // -- ONE write, not four (see header: the mother variant has no loop over anim_dur[1..3]).
    b.anim_dur[0] = combine_dword_pair(param_5, param_6);

    // ---- unconditional tail (0x00475f7a-0x00475f8d)
    b.online_state = LIFTOFF_ONLINE_STATE;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_start_liftoff_anim_shuttle(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                                     uint32_t param_5, uint32_t param_6) {
    sim_state st = state();
    detail::bldg_start_liftoff_anim_shuttle(st.read, st.own, param_1, param_2, param_3, param_4, param_5,
                                            param_6);
}

void bldg_start_liftoff_anim_mother(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                                    uint32_t param_5, uint32_t param_6) {
    sim_state st = state();
    detail::bldg_start_liftoff_anim_mother(st.read, st.own, param_1, param_2, param_3, param_4, param_5,
                                           param_6);
}


} // namespace mh::sim
