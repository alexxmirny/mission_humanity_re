//
// sim/hostreach/sim_h_bldg_frame_center_offset.cpp -- see sim_h_bldg_frame_center_offset.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_gfx_bldg_frame_center_offset_00450b40.asm). The Ghidra .c
// draft beside it was consulted only to CROSS-CHECK the field-selection shape (which header goes with
// which out-pointer) -- its guess that offsets 0/2 are "width,height" is NOT trusted; see the header
// banner's FINDING.
//
#include "sim/hostreach/sim_h_bldg_frame_center_offset.h"

namespace mh::sim {

namespace {

// x87 idiom `EDX=SAR(val,31); (val-EDX)>>1` -- signed divide-by-2 rounding TOWARD ZERO (not floor).
// Identical to sim_bldg_sprite_anchor.cpp's divide_round_toward_zero (each hostreach/sim TU keeps its
// own copy of this tiny helper per the existing convention -- see that file and
// sim_map_create_building.cpp's load_u32_le/store_u32_le for the same per-file-duplication idiom).
inline int32_t divide_round_toward_zero(int32_t val) {
    const int32_t sign = val >> 31;
    return (val - sign) >> 1;
}

// gfx_sprite_hdr (the sprite-format notes) is a runtime-loaded-blob header with no fixed Ghidra-applicable
// address -- read via named byte offsets on a raw pointer, same idiom sim_bldg_sprite_anchor.cpp and
// sim_map_create_building.cpp already establish for this codebase.
inline uint16_t load_u16_le(const uint8_t *src) {
    return static_cast<uint16_t>(src[0] | (src[1] << 8));
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

// FINDING (see the header banner): these are NOT the twin's x_left/y_top@4/6 or width/height@8/0xA.
// Both are read at these two raw offsets and nothing else in this body touches +4/+6/+8/+0xA.
constexpr uint32_t SPRITE_HDR_FIELD0_OFFSET = 0; // 0x00450bb3 / 0x00450bc6: MOVZX word ptr [hdr]
constexpr uint32_t SPRITE_HDR_FIELD2_OFFSET = 2; // 0x00450bbc / 0x00450bcf: MOVZX word ptr [hdr+2]

} // namespace

namespace detail {

void gfx_bldg_frame_center_offset(const sim_view &v, uint16_t building_id, int32_t *out_dx,
                                  int32_t *out_dy) {
    const cfg_building &cfg = v.cfg_buildings[building_id];

    // 0x00450b5f-0x00450b89: header1 = *gfx_bank_pixels + sprite_pix_offsets[
    //   Anim[ Building[building_id].anim[0] + 1 ].sprite_id ] -- the anim-chain (frame-0 default
    // sprite) lookup, same chain sim_bldg_sprite_anchor.cpp's sprite_anchor_offset reads. `.anim[0]`
    // is a full dword cfg_t_frame_index read here, via load_u32_le like every other `.anim[]`
    // consumer in this closure.
    const uint32_t anim0   = load_u32_le(&cfg.anim[0]);
    const int32_t  frame1  = v.anim_frames[anim0 + 1].sprite_id;
    const uint8_t *header1 = *v.gfx_bank_pixels + v.sprite_pix_offsets[static_cast<uint32_t>(frame1)];

    // 0x00450b8c-0x00450bad: header2 = *gfx_bank_pixels + sprite_pix_offsets[ Building[building_id]
    //   .frame ] -- `frame` (cfg_final_struct_Building::frame, a plain int32_t field) used DIRECTLY as
    // the sprite index, with NO Anim hop -- unlike header1.
    const int32_t  frame2  = cfg.frame;
    const uint8_t *header2 = *v.gfx_bank_pixels + v.sprite_pix_offsets[static_cast<uint32_t>(frame2)];

    // 0x00450bb0-0x00450bd3: read both headers' fields at +0 and +2 (see the header banner's FINDING --
    // NOT the twin's x_left/y_top@4/6 or width/height@8/0xA).
    const uint32_t h1_f0 = load_u16_le(header1 + SPRITE_HDR_FIELD0_OFFSET);
    const uint32_t h1_f2 = load_u16_le(header1 + SPRITE_HDR_FIELD2_OFFSET);
    const uint32_t h2_f0 = load_u16_le(header2 + SPRITE_HDR_FIELD0_OFFSET);
    const uint32_t h2_f2 = load_u16_le(header2 + SPRITE_HDR_FIELD2_OFFSET);

    // 0x00450bd6-0x00450bee: *out_dx = round_toward_zero(h2_f0 - h1_f0) / 2.
    *out_dx = divide_round_toward_zero(static_cast<int32_t>(h2_f0) - static_cast<int32_t>(h1_f0));

    // 0x00450bf0-0x00450c08: *out_dy = round_toward_zero(h2_f2 - h1_f2) / 2.
    *out_dy = divide_round_toward_zero(static_cast<int32_t>(h2_f2) - static_cast<int32_t>(h1_f2));
}

} // namespace detail

// ---- the public wrapper (the promotion seam, batch rule 2) ------------------------------------------

void gfx_bldg_frame_center_offset(uint16_t building_id, int32_t *out_dx, int32_t *out_dy) {
    const sim_view v = state().read;
    detail::gfx_bldg_frame_center_offset(v, building_id, out_dx, out_dy);
}

} // namespace mh::sim
