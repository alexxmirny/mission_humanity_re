//
// sim_bldg_sprite_anchor_selftest.cpp -- `simtest` oracle for llm_strat_bldg_sprite_anchor_offset
// @0x00450a60 and llm_strat_bldg_get_sprite_anchor_coord @0x00490eb1 (sim/sim_bldg_sprite_anchor.h/
// .cpp, RI-SIM / SIM1-G4).
//
// sprite_anchor_offset is NOT SHADOWABLE (zero region writes, void return -- nothing to compare) and
// has no shadow-arm evidence at all; this file is its ONLY evidence. bldg_get_sprite_anchor_coord IS
// shadowable (compare_return), but this file also covers its branch structure (all four
// (anchor_kind, axis) combinations) and the sprite_quantity==0 PRESERVE-BUG shape directly, cheaper
// and more targeted than waiting for a live state to exercise every arm.
//
// EXPECTED BEHAVIOUR, from the disassembly (tmp/decomp_sim/llm_strat_bldg_sprite_anchor_offset_
// 00450a60.asm, _get_sprite_anchor_coord_00490eb1.asm) and the header banner's own derivation:
//
//   sprite_anchor_offset (per-building-TYPE, pure, writes only through *out_x/*out_y):
//     0x00450a76-0x00450a92: frame = Anim[ cfg_buildings[building_id].anim[0] + 1 ].sprite_id -- the
//       TYPE's own default anim-chain entry, read as a full dword (cfg_t_frame_index), NOT the byte
//       reads get_sprite_anchor_coord performs on the INSTANCE's anim[] elsewhere.
//     0x00450a9b-0x00450ad1: header = *gfx_bank_pixels + sprite_pix_offsets[frame]; the four
//       gfx_sprite_hdr fields (x_left@4/y_top@6/width@8/height@0xa) all read UNSIGNED (MOVZX word).
//     0x00450ad4-0x00450b06: *out_x = round_toward_zero((cfg.width*32 - hdr.width) / 2) - hdr.x_left.
//     0x00450b06-0x00450b38: *out_y = round_toward_zero((cfg.height*32 - hdr.height) / 2) - hdr.y_top.
//       The round is the x87 SAR/SUB idiom -- ROUND TOWARD ZERO, not floor (T2 below pins the
//       difference with a negative odd numerator).
//
//   bldg_get_sprite_anchor_coord (per-building-INSTANCE, pure query, returns uint32_t):
//     0x00490f36-0x00490f6c: frame2 = Anim[ b.anim[ cfg_buildings[b.building_id].sprite_quantity - 1
//       ] + 1 ].sprite_id. `anim[sprite_quantity - 1]` is byte-pointer arithmetic on the record (never
//       `.anim[-1]` element indexing), so sprite_quantity==0 makes the index -1 and the read lands ONE
//       ELEMENT BEFORE anim[0] -- into the tail of the immediately-preceding anim_dur[12] array,
//       STILL INSIDE the same `building` record (no padding between the two fields per
//       mh_structs.gen.h) -- a genuine PRESERVE-BUG shape (Law 2), pinned as T-BUG below.
//     0x00490f6f-0x00491076: by (anchor_kind, axis):
//       kind==1, axis==1:  (b.x*32 + out_x + sprite_meta[frame2].mount1_x) & geom->bw_mask
//       kind==1, axis!=1:  (b.y*32 + out_y + sprite_meta[frame2].mount1_y) & geom->bh_mask
//       kind!=1, axis==1:  (b.x*32 + out_x + sprite_meta[frame2].mount2_x) & geom->bw_mask
//       kind!=1, axis!=1:  (b.y*32 + out_y + sprite_meta[frame2].mount2_y) & geom->bh_mask
//       out_x/out_y come from sprite_anchor_offset(v, b.building_id, ...), an ordinary intra-TU call.
//       bw_mask/bh_mask are geom's PIXEL-space wrap masks at offsets 0/4 -- NOT width_mask/height_mask
//       at 8/0x20 (the TILE-space pair); sim_fixture::reset() only seeds width_mask/height_mask, so
//       every case here sets geom.bw_mask/bh_mask itself (left 0 otherwise).
//     The FIRST (dead) `Anim[b.anim[0]+1].sprite_id` computation the header banner documents is
//     provably unreachable-as-observable (unconditionally overwritten before its only read) and is
//     not exercised by a dedicated case here for the same reason the translation doesn't reproduce it.
//
// To keep the arithmetic in every bldg_get_sprite_anchor_coord case tractable, every case there routes
// sprite_anchor_offset's own contribution to (0, 0) via seed_zero_sprite_offset() below (cfg.width =
// cfg.height = 0, and an all-zero gfx_sprite_hdr blob) -- that isolates the mount-field / mask / axis
// logic under test from the (separately, directly) verified sprite_anchor_offset arithmetic above.
//
#include "sim/sim_bldg_sprite_anchor.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void store_u16_le(uint8_t *dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xff);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void store_u32_le(uint8_t *dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xff);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

// Wires cfg_buildings[bid].anim[0] = anim0_raw and Anim[anim0_raw + 1].sprite_id = frame_id -- the
// per-building-TYPE default anim-chain entry sprite_anchor_offset reads. Does NOT touch
// sprite_pix_offsets[frame_id] or the pixel-bank storage -- callers wire those themselves (a case may
// share one frame_id across several purposes, or none at all).
void seed_cfg_anim0(sim_fixture &fx, uint16_t bid, uint32_t anim0_raw, uint32_t frame_id) {
    cfg_building &cfg = fx.cfg_buildings[bid];
    store_u32_le(&cfg.anim[0], anim0_raw);
    fx.anim_frames[anim0_raw + 1].sprite_id = static_cast<int32_t>(frame_id);
}

// Writes a real 12-byte gfx_sprite_hdr blob (x_left@4/y_top@6/width@8/height@0xa, all uint16 LE) at
// hdr_offset within fx.gfx_bank_pixels_storage, growing the storage first (so any pointer already
// taken into it from an EARLIER call in the same case is invalidated -- callers that need several
// headers alive at once call this before reading any of them, matching every case below).
void seed_sprite_header(sim_fixture &fx, uint32_t hdr_offset, uint16_t x_left, uint16_t y_top,
                        uint16_t width, uint16_t height) {
    if (fx.gfx_bank_pixels_storage.size() < hdr_offset + 12) fx.gfx_bank_pixels_storage.resize(hdr_offset + 12);
    uint8_t *h = fx.gfx_bank_pixels_storage.data() + hdr_offset;
    store_u16_le(h + 4, x_left);
    store_u16_le(h + 6, y_top);
    store_u16_le(h + 8, width);
    store_u16_le(h + 0xa, height);
    fx.gfx_bank_pixels_base = fx.gfx_bank_pixels_storage.data();
}

// bldg_get_sprite_anchor_coord's helper: makes sprite_anchor_offset(v, bid, ...)'s own contribution
// (0, 0) so a case can isolate the mount-field/mask/axis arithmetic under test. cfg.width=cfg.height=0
// and an all-zero header at offset 0 -> half_x=half_y=0, x_left=y_top=0 -> out_x=out_y=0.
void seed_zero_sprite_offset(sim_fixture &fx, uint16_t bid) {
    cfg_building &cfg = fx.cfg_buildings[bid];
    cfg.width         = 0;
    cfg.height        = 0;
    seed_cfg_anim0(fx, bid, /*anim0_raw=*/1, /*frame_id=*/1);
    fx.sprite_pix_offsets[1] = 0;
    seed_sprite_header(fx, /*hdr_offset=*/0, /*x_left=*/0, /*y_top=*/0, /*width=*/0, /*height=*/0);
}

} // namespace

void run_bldg_sprite_anchor_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- sprite_anchor_offset: basic bounding-box-centering formula, DISTINCT non-symmetric
    // cfg.width/height and all four header fields so a mixed-up field/formula fails.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t BID = 5;
        cfg_building      &cfg = fx.cfg_buildings[BID];
        cfg.width              = 10;
        cfg.height             = 20;
        seed_cfg_anim0(fx, BID, /*anim0_raw=*/100, /*frame_id=*/7);
        fx.sprite_pix_offsets[7] = 16;
        seed_sprite_header(fx, /*hdr_offset=*/16, /*x_left=*/3, /*y_top=*/9, /*width=*/44, /*height=*/66);

        int32_t out_x = -999, out_y = -999;
        detail::sprite_anchor_offset(fx.view(), BID, &out_x, &out_y);

        // half_x = round_to_zero((10*32-44)/2) = round_to_zero(276/2) = 138; out_x = 138-3 = 135.
        ck_eq((uint32_t)out_x, 135u,
              "sprite_anchor_offset: out_x = round_to_zero((cfg.width*32-hdr.width)/2)-hdr.x_left, "
              "0x00450ad4-0x00450b06");
        // half_y = round_to_zero((20*32-66)/2) = round_to_zero(574/2) = 287; out_y = 287-9 = 278.
        ck_eq((uint32_t)out_y, 278u,
              "sprite_anchor_offset: out_y = round_to_zero((cfg.height*32-hdr.height)/2)-hdr.y_top, "
              "0x00450b06-0x00450b38");
    }

    // =================================================================================================
    // T2 -- sprite_anchor_offset: NEGATIVE, ODD numerator on both axes, pinning ROUND-TOWARD-ZERO
    // against floor (which would disagree by exactly 1 on each).
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t BID = 6;
        cfg_building      &cfg = fx.cfg_buildings[BID];
        cfg.width              = 1; // 1*32 = 32
        cfg.height             = 1; // 1*32 = 32
        seed_cfg_anim0(fx, BID, /*anim0_raw=*/40, /*frame_id=*/9);
        fx.sprite_pix_offsets[9] = 32;
        // hdr.width=35 -> numerator 32-35=-3 (round-to-zero -1; floor would give -2).
        // hdr.height=39 -> numerator 32-39=-7 (round-to-zero -3; floor would give -4).
        seed_sprite_header(fx, /*hdr_offset=*/32, /*x_left=*/100, /*y_top=*/55, /*width=*/35, /*height=*/39);

        int32_t out_x = -999, out_y = -999;
        detail::sprite_anchor_offset(fx.view(), BID, &out_x, &out_y);

        // round_to_zero(-3/2) = -1 (a floor divide would give -2): out_x = -1-100 = -101.
        ck_eq((uint32_t)out_x, (uint32_t)-101,
              "sprite_anchor_offset: negative odd numerator rounds TOWARD ZERO, not floor "
              "(-3/2 -> -1, not -2), 0x00450af2-0x00450af7 SAR/SUB idiom");
        // round_to_zero(-7/2) = -3 (a floor divide would give -4): out_y = -3-55 = -58.
        ck_eq((uint32_t)out_y, (uint32_t)-58,
              "sprite_anchor_offset: same round-toward-zero pin on the Y arm (-7/2 -> -3, not -4), "
              "0x00450b24-0x00450b29");
    }

    // =================================================================================================
    // T3 -- sprite_anchor_offset: confirms Anim[cfg.anim[0]+1].sprite_id is the frame used, NOT
    // Anim[cfg.anim[0]]. Two DIFFERENT header blobs behind the two frame ids -- if the +1 were dropped
    // the result would match the "WRONG" numbers noted in the comments, not the asserted ones.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t BID       = 8;
        cfg_building      &cfg       = fx.cfg_buildings[BID];
        cfg.width                    = 4;                           // 4*32 = 128
        cfg.height                   = 6;                           // 6*32 = 192
        fx.anim_frames[50].sprite_id = 11;                          // WRONG: this is Anim[anim0] itself, not anim0+1
        seed_cfg_anim0(fx, BID, /*anim0_raw=*/50, /*frame_id=*/22); // RIGHT: Anim[anim0+1] = frame 22
        fx.sprite_pix_offsets[11] = 8;
        fx.sprite_pix_offsets[22] = 64;
        seed_sprite_header(fx, /*hdr_offset=*/8, /*x_left=*/1, /*y_top=*/2, /*width=*/3, /*height=*/4);  // WRONG hdr
        seed_sprite_header(fx, /*hdr_offset=*/64, /*x_left=*/9, /*y_top=*/8, /*width=*/7, /*height=*/5); // RIGHT hdr

        int32_t out_x = -999, out_y = -999;
        detail::sprite_anchor_offset(fx.view(), BID, &out_x, &out_y);

        // RIGHT (frame 22, from anim0+1): half_x=round_to_zero((128-7)/2)=60; out_x=60-9=51.
        // WRONG (frame 11, from anim0 alone) would give half_x=round_to_zero((128-3)/2)=62; out_x=61.
        ck_eq((uint32_t)out_x, 51u,
              "sprite_anchor_offset: frame = Anim[cfg.anim[0]+1].sprite_id, NOT Anim[cfg.anim[0]] -- "
              "would read 61 if the +1 were dropped, 0x00450a89-0x00450a92");
        // RIGHT: half_y=round_to_zero((192-5)/2)=93; out_y=93-8=85.
        // WRONG (anim0 alone) would give half_y=round_to_zero((192-4)/2)=94; out_y=94-2=92.
        ck_eq((uint32_t)out_y, 85u,
              "sprite_anchor_offset: same +1 pin on the Y arm -- would read 92 if the +1 were "
              "dropped, 0x00450a89-0x00450a92");
    }

    // =================================================================================================
    // T4 -- bldg_get_sprite_anchor_coord: all four (anchor_kind, axis) combinations off ONE shared
    // instance/type setup, with DISTINCT mount1_x/mount1_y/mount2_x/mount2_y and DISTINCT b.x/b.y so a
    // mixed-up mount field, axis selection, or mask fails. sprite_anchor_offset's own contribution is
    // routed to (0,0) via seed_zero_sprite_offset so only THIS function's arithmetic is under test.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER  = 2;
        constexpr int32_t  B_INDEX = 5;
        constexpr uint16_t BID     = 15;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = BID;
        b.x           = 7;  // 7*32 = 224
        b.y           = 11; // 11*32 = 352

        cfg_building &cfg   = fx.cfg_buildings[BID];
        cfg.sprite_quantity = 3; // anim_slot = 3-1 = 2: the NORMAL (non-underflow) path
        seed_zero_sprite_offset(fx, BID);

        // frame2 = Anim[ b.anim[2] + 1 ].sprite_id -- write anim2_raw at byte offset 2*4=8 into b.anim.
        store_u32_le(&b.anim[0] + 2 * 4, /*anim2_raw=*/300);
        fx.anim_frames[301].sprite_id = 55;

        sprite_meta_entry &sm = fx.sprite_meta[55];
        sm.mount1_x           = 111;
        sm.mount1_y           = 222;
        sm.mount2_x           = 333;
        sm.mount2_y           = 444;

        fx.geom.bw_mask = 0xfff; // 4095, DISTINCT from bh_mask and from reset()'s width_mask/height_mask
        fx.geom.bh_mask = 0x1ff; // 511

        const sim_view v = fx.view();

        // kind==1, axis==1 -> mount1_x, bw_mask, b.x*32, out_x(=0): (224+0+111)&0xfff = 335.
        ck_eq(detail::bldg_get_sprite_anchor_coord(v, PLAYER, B_INDEX, /*anchor_kind=*/1, /*axis=*/1),
              335u,
              "bldg_get_sprite_anchor_coord: kind==1,axis==1 -> (x*32+out_x+mount1_x)&bw_mask = 335, "
              "0x00490f7f-0x00490fb9");

        // kind==1, axis!=1 -> mount1_y, bh_mask, b.y*32, out_y(=0): (352+0+222)&0x1ff = 574&511 = 62.
        ck_eq(detail::bldg_get_sprite_anchor_coord(v, PLAYER, B_INDEX, /*anchor_kind=*/1, /*axis=*/2),
              62u,
              "bldg_get_sprite_anchor_coord: kind==1,axis!=1 -> (y*32+out_y+mount1_y)&bh_mask = 62, "
              "0x00490fbb-0x00490ff2");

        // kind!=1, axis==1 -> mount2_x, bw_mask, b.x*32, out_x(=0): (224+0+333)&0xfff = 557.
        ck_eq(detail::bldg_get_sprite_anchor_coord(v, PLAYER, B_INDEX, /*anchor_kind=*/2, /*axis=*/1),
              557u,
              "bldg_get_sprite_anchor_coord: kind!=1,axis==1 -> (x*32+out_x+mount2_x)&bw_mask = 557, "
              "0x00491000-0x0049103a");

        // kind!=1, axis!=1 -> mount2_y, bh_mask, b.y*32, out_y(=0): (352+0+444)&0x1ff = 796&511 = 284.
        ck_eq(detail::bldg_get_sprite_anchor_coord(v, PLAYER, B_INDEX, /*anchor_kind=*/2, /*axis=*/2),
              284u,
              "bldg_get_sprite_anchor_coord: kind!=1,axis!=1 -> (y*32+out_y+mount2_y)&bh_mask = 284, "
              "0x0049103c-0x00491073");
    }

    // =================================================================================================
    // T-BUG -- PRESERVE-BUG: cfg.sprite_quantity==0 makes anim_slot = -1, so the frame2 lookup reads
    // `&b.anim[0] + (-1)*4` -- the EXACT SAME pointer arithmetic the production code performs (never
    // `.anim[-1]` element indexing), landing 4 bytes before b.anim[0], i.e. the LAST 4 bytes of the
    // immediately-preceding anim_dur[12] array (no padding between the two fields in
    // mh_map_object_building, confirmed against mh_structs.gen.h). Seeding THAT exact address with a
    // chosen frame id makes the buggy read fully deterministic and observable rather than "whatever
    // garbage happens to be there" -- and the assertion below is the BUGGY result, per Law 2: preserve,
    // never "fix". This case IS fully deterministic (no gap left undemonstrated).
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER  = 4;
        constexpr int32_t  B_INDEX = 2;
        constexpr uint16_t BID     = 20;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = BID;
        b.x           = 13; // 13*32 = 416
        b.y           = 17;

        cfg_building &cfg   = fx.cfg_buildings[BID];
        cfg.sprite_quantity = 0; // anim_slot = 0-1 = -1: the UNDERFLOW path
        seed_zero_sprite_offset(fx, BID);

        // Write the frame id directly at the buggy read address: &b.anim[0] + (-1)*4.
        store_u32_le(&b.anim[0] + (-1) * 4, /*anim2_raw=*/777);
        fx.anim_frames[778].sprite_id = 88;

        sprite_meta_entry &sm = fx.sprite_meta[88];
        sm.mount1_x           = 501;
        sm.mount1_y           = 502;
        sm.mount2_x           = 503;
        sm.mount2_y           = 504;

        fx.geom.bw_mask = 0xfff;
        fx.geom.bh_mask = 0x1ff;

        const uint32_t result = detail::bldg_get_sprite_anchor_coord(
            fx.view(), PLAYER, B_INDEX, /*anchor_kind=*/1, /*axis=*/1);

        // BUGGY result, asserted as-is: (x*32+out_x+mount1_x)&bw_mask = (416+0+501)&0xfff = 917.
        ck_eq(result, 917u,
              "bldg_get_sprite_anchor_coord: PRESERVE-BUG -- sprite_quantity==0 -> anim_slot=-1 reads "
              "INTO anim_dur[11]'s last 4 bytes instead of anim[0] (genuine underflow, matching the "
              "original's own byte-pointer arithmetic); the decoded frame is used anyway, "
              "0x00490f36-0x00490f6c");
    }
}

} // namespace mh::sim::test
