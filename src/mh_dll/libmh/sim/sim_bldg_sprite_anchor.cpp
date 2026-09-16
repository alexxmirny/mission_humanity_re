//
// sim/sim_bldg_sprite_anchor.cpp -- see sim_bldg_sprite_anchor.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_sprite_anchor_offset_00450a60.asm,
// _get_sprite_anchor_coord_00490eb1.asm). No Ghidra .c draft consulted.
//
#include "sim/sim_bldg_sprite_anchor.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// x87 idiom `EDX=SAR(val,31); (val-EDX)>>1` -- signed divide-by-2 rounding TOWARD ZERO (not floor).
inline int32_t divide_round_toward_zero(int32_t val) {
    const int32_t sign = val >> 31;
    return (val - sign) >> 1;
}

// gfx_sprite_hdr (the sprite-format notes) is a runtime-loaded-blob header with no fixed Ghidra-applicable
// address (see the header banner's FIELDS note) -- read via named byte offsets on a raw pointer, same
// idiom sim_map_create_building.cpp's load_u32_le/store_u32_le already establish for this codebase.
inline uint16_t load_u16_le(const uint8_t *src) {
    return static_cast<uint16_t>(src[0] | (src[1] << 8));
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

constexpr uint32_t SPRITE_HDR_X_LEFT_OFFSET = 4;   // 0x00450aaf: MOVZX word[hdr+4] -- unsigned here
constexpr uint32_t SPRITE_HDR_Y_TOP_OFFSET  = 6;   // 0x00450ab9: MOVZX word[hdr+6]
constexpr uint32_t SPRITE_HDR_WIDTH_OFFSET  = 8;   // 0x00450ac3: MOVZX word[hdr+8]
constexpr uint32_t SPRITE_HDR_HEIGHT_OFFSET = 0xa; // 0x00450acd: MOVZX word[hdr+0xa]

} // namespace

namespace detail {

void sprite_anchor_offset(const sim_view &v, uint16_t building_id, int32_t *out_x, int32_t *out_y) {
    // 0x00450a89-0x00450a92: frame = Anim[ Building[building_id].anim[0] + 1 ].sprite_id. Building's
    // own default anim-chain entry for this TYPE (a per-type default, not any specific instance) --
    // `.anim[0]` is a full dword cfg_t_frame_index read here (unlike get_sprite_anchor_coord's byte
    // reads of the SAME field elsewhere), so read via load_u32_le like every other `.anim[]` consumer
    // in this closure.
    const cfg_building &cfg   = v.cfg_buildings[building_id];
    const uint32_t      anim0 = load_u32_le(&cfg.anim[0]);
    const int32_t       frame = v.anim_frames[anim0 + 1].sprite_id;

    // 0x00450a9b-0x00450aa7: header = *gfx_bank_pixels + sprite_pix_offsets[frame].
    const uint8_t *header = *v.gfx_bank_pixels + v.sprite_pix_offsets[static_cast<uint32_t>(frame)];

    // 0x00450aaf-0x00450ad1: the four gfx_sprite_hdr fields, all read UNSIGNED here (see header banner).
    const uint32_t x_left = load_u16_le(header + SPRITE_HDR_X_LEFT_OFFSET);
    const uint32_t y_top  = load_u16_le(header + SPRITE_HDR_Y_TOP_OFFSET);
    const uint32_t width  = load_u16_le(header + SPRITE_HDR_WIDTH_OFFSET);
    const uint32_t height = load_u16_le(header + SPRITE_HDR_HEIGHT_OFFSET);

    // 0x00450ad4-0x00450b06: *out_x = ((Building.width*32 - width) / 2, round-to-zero) - x_left.
    const int32_t half_x = divide_round_toward_zero(static_cast<int32_t>(cfg.width) * 32 -
                                                    static_cast<int32_t>(width));
    *out_x               = half_x - static_cast<int32_t>(x_left);

    // 0x00450b06-0x00450b38: *out_y = ((Building.height*32 - height) / 2, round-to-zero) - y_top.
    const int32_t half_y = divide_round_toward_zero(static_cast<int32_t>(cfg.height) * 32 -
                                                    static_cast<int32_t>(height));
    *out_y               = half_y - static_cast<int32_t>(y_top);
}

uint32_t bldg_get_sprite_anchor_coord(const sim_view &v, uint32_t player, int32_t b_index,
                                      int32_t anchor_kind, uint8_t axis) {
    const building &b = building_of(v, player, b_index);

    // 0x00490ee5-0x00490ef4: a FIRST `Anim[ b.anim[0] + 1 ].sprite_id` computation whose result is
    // PROVABLY DEAD -- the only later read of that local (the sprite_meta-index IMUL) is strictly
    // after the SECOND, unconditional overwrite below, on every control-flow path (no branch reachable
    // between the two stores skips the overwrite). A pure read of static cfg/Anim data with no side
    // effect, so it changes nothing observable and is not reproduced here -- same treatment as
    // sim_bldg_free_record.cpp's own "trailing dead-code block".

    // 0x00490f36-0x00490f6c: frame2 = Anim[ b.anim[cfg.sprite_quantity - 1] + 1 ].sprite_id.
    // `cfg.sprite_quantity` is a per-building-TYPE byte; `anim[sprite_quantity - 1]` reads ONE ELEMENT
    // BEFORE anim[0] (into the tail of the preceding anim_dur[12], still inside `building`) whenever
    // sprite_quantity == 0 for a type -- a PRESERVE-BUG shape (Law 2). Implemented via raw byte-pointer
    // arithmetic on the record (never `.anim[-1]` array indexing), so the same in-struct underflow read
    // happens instead of undefined behaviour.
    const cfg_building &cfg       = v.cfg_buildings[b.building_id];
    const int32_t       anim_slot = static_cast<int32_t>(cfg.sprite_quantity) - 1;
    // `b.anim` is a plain array (`&b.anim[0]`); byte-pointer arithmetic from it (rather than
    // `.anim[anim_slot]` element indexing) so a negative `anim_slot` reads into the immediately-
    // preceding `anim_dur` field within the SAME `building` object instead of hitting array-indexing
    // undefined behaviour -- see the header banner's PRESERVE-BUG note.
    const uint32_t anim2  = load_u32_le(&b.anim[0] + anim_slot * 4);
    const int32_t  frame2 = v.anim_frames[anim2 + 1].sprite_id;

    // out_x/out_y from sprite_anchor_offset, keyed off THIS instance's building_id.
    int32_t out_x = 0, out_y = 0;
    sprite_anchor_offset(v, b.building_id, &out_x, &out_y);

    const sprite_meta_entry &sm = v.sprite_meta[static_cast<uint32_t>(frame2)];

    // 0x00490f6f-0x00491076: by (anchor_kind, axis). bw_mask/bh_mask are geom's PIXEL-space wrap masks
    // (offsets 0/4 -- NOT width_mask/height_mask at 8/32, the TILE-space pair).
    uint32_t result;
    if (anchor_kind == 1) {
        if (axis == 1) {
            result = (static_cast<uint32_t>(b.x) * 32 + static_cast<uint32_t>(out_x) +
                      static_cast<uint32_t>(sm.mount1_x)) &
                     v.geom->bw_mask;
        } else {
            result = (static_cast<uint32_t>(b.y) * 32 + static_cast<uint32_t>(out_y) +
                      static_cast<uint32_t>(sm.mount1_y)) &
                     v.geom->bh_mask;
        }
    } else {
        if (axis == 1) {
            result = (static_cast<uint32_t>(b.x) * 32 + static_cast<uint32_t>(out_x) +
                      static_cast<uint32_t>(sm.mount2_x)) &
                     v.geom->bw_mask;
        } else {
            result = (static_cast<uint32_t>(b.y) * 32 + static_cast<uint32_t>(out_y) +
                      static_cast<uint32_t>(sm.mount2_y)) &
                     v.geom->bh_mask;
        }
    }
    return result;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_sprite_anchor_offset(uint16_t building_id, int32_t *out_x, int32_t *out_y) {
    const sim_view v = state().read;
    detail::sprite_anchor_offset(v, building_id, out_x, out_y);
}

uint32_t bldg_get_sprite_anchor_coord(uint32_t player, int32_t b_index, int32_t anchor_kind,
                                      uint8_t axis) {
    const sim_view v = state().read;
    return detail::bldg_get_sprite_anchor_coord(v, player, b_index, anchor_kind, axis);
}


} // namespace mh::sim
