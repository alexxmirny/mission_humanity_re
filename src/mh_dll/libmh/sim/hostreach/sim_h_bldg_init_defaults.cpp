//
// sim/hostreach/sim_h_bldg_init_defaults.cpp -- see sim_h_bldg_init_defaults.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_init_defaults_0045a068.asm); the Ghidra .c is a draft
// (trusted only for the FIRST dispatch's jump-table case GROUPING, per the header banner).
//
// ---- DECLARED NEED (conductor): `cfg_final_struct_Building::_pad_0x796` (128 bytes, offset 0x796,
// between dock_lift_offset_y and door_exit_route) is NOT anonymous padding in practice -- this
// function is its SOLE writer and the write is fully decoded: four back-to-back int32_t[8] arrays,
// each one entry per "heading" 0..7 (a mh::sim::MICROSTEPS_PER_HEADING-indexed table, NOT unit
// facing's 24-heading resolution):
//   +0x00 (0x796): approach_x[8]   +0x20 (0x7b6): approach_y[8]
//   +0x40 (0x7d6): exit_x[8]       +0x60 (0x7f6): exit_y[8]
// populated ONLY for the building families that dock/launch shuttles (A/H_MOTHER, A/H_SHUTTLE) --
// see fill_heading_pixel_table() below for the exact derivation, cited against 0x0045a360-0x0045a512
// (the first, canonical instance of the loop body). The "approach"/"exit" labels are inferred from
// proximity to door_approach_route/door_exit_route and the shuttle-docking membership pattern -- the
// ARITHMETIC and VALUES are 100% asm-derived, only that english label is a naming guess. Recommend
// splitting `_pad_0x796` into four named `int32_t[8]` fields once this evidence is applied to Ghidra.
// Until then this file writes through `_pad_0x796` directly (not a hardcoded offset) so the write
// stays byte-for-byte immune to any unrelated field being inserted earlier in the struct.
//
#include "sim/hostreach/sim_h_bldg_init_defaults.h"

#include "sim/sim_bldg_sprite_anchor.h" // llm_strat_bldg_sprite_anchor_offset -- already-translated sibling, called DIRECTLY (rule 3c)

#include "sim/hostreach/sim_h_bldg_frame_center_offset.h" // the sibling this batch translated

namespace mh::sim {

const bldg_init_defaults_calls &live_bldg_init_defaults_calls() {
    static const bldg_init_defaults_calls c = {
        // WAVE 2 DISSOLVED THIS EDGE (2026-09-10). llm_gfx_bldg_frame_center_offset was the unit's
        // one outward call; the conductor adjudicated it on evidence (pure, 210 B, its ONLY caller
        // in the whole image is this function, and its twin llm_strat_bldg_sprite_anchor_offset was
        // already a verified sim row) and it joined the batch. So this is now a DIRECT sibling
        // `detail::` call -- translator-brief rule 3c, and the sibling takes no `_calls` of its own,
        // so there is nothing to thread. The struct member survives so the offline oracle can still
        // substitute a recorder here; the LIVE binding is our own body.
        // bound to the sibling's PUBLIC WRAPPER, not its detail:: -- the detail:: takes the view as
        // its first parameter and this member's signature is the ORIGINAL's three. That is the same
        // shape sim_h_map_build_regions.cpp and sim_map_apply_area.cpp already use for their own
        // translated siblings (&mh::sim::merge_small_regions, &mh::sim::region_pick_smaller).
        &mh::sim::gfx_bldg_frame_center_offset,
    };
    return c;
}

namespace {

// x87 idiom `EDX=SAR(val,31); (val-EDX)>>1` -- signed divide-by-2 rounding TOWARD ZERO (not floor).
// Same idiom, same helper shape as sim_bldg_sprite_anchor.cpp's divide_round_toward_zero (a fresh
// same-TU copy per this codebase's established per-file-helper convention).
inline int32_t divide_round_toward_zero(int32_t val) {
    const int32_t sign = val >> 31;
    return (val - sign) >> 1;
}

// cfg_t_frame_index / dword field byte-store helpers -- same load_u32_le/store_u32_le idiom
// sim_map_create_building.cpp and sim_bldg_sprite_anchor.cpp already establish for this codebase.
inline uint32_t load_u32_le(const uint8_t *src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}
inline void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

// cfg_enum_E_BUILDING members this function's two dispatches key on. Values from
// sim/sim_order_enqueue.h's BUILDING_TYPE_* (Ghidra enum dump 2026-08-08/2026-08-12) -- see the
// header banner for the cross-confirmation and the A_LAB/A_PORT/A_SHUTTLE trap this avoided.
constexpr uint8_t A_PRODUCTION = 0x01, A_MINE = 0x02, A_PLANT = 0x03, A_COLONY = 0x04, A_TURRET = 0x05,
                  A_MOTHER = 0x06, A_BARRACKS = 0x07, A_GARAGE = 0x08, A_AIRFIELD = 0x09,
                  A_HELIPAD = 0x0a, A_LAB = 0x0b, A_PORT = 0x0c, A_SHUTTLE = 0x0d, A_MAIN_BASE = 0x0e,
                  A_RELAY = 0x0f, A_SILOS = 0x10, A_CIVIL = 0x12, H_PRODUCTION = 0x15, H_MINE = 0x16,
                  H_PLANT = 0x17, H_COLONY = 0x18, H_TURRET = 0x19, H_MOTHER = 0x1a,
                  H_BARRACKS = 0x1b, H_GARAGE = 0x1c, H_AIRFIELD = 0x1d, H_HELIPAD = 0x1e,
                  H_LAB = 0x1f, H_PORT = 0x20, H_SHUTTLE = 0x21, H_MAIN_BASE = 0x22, H_RELAY = 0x23,
                  H_SILOS = 0x24, H_CIVIL = 0x26;

// Anim[ Building.anim[anim_slot] + 1 ].sprite_id -- the frame lookup every anchor computation in this
// function performs. `anim_slot` is 0 almost everywhere; the A/H_HELIPAD dock-lift block uses slot 4
// (0x0045ac55: `[EAX + 0xd9eddf]` = base+0x15f = anim[4], NOT anim[0] -- re-derived and confirmed
// against mh_structs.gen.h's `anim` offset 0x14f, since 0x15f-0x14f = 0x10 = 4 * sizeof(cfg_t_frame_index)).
inline int32_t sprite_id_for_anim_slot(const sim_view &v, const cfg_building &b, uint32_t anim_slot) {
    const uint32_t anim_val = load_u32_le(&b.anim[anim_slot * 4]);
    return v.anim_frames[anim_val + 1].sprite_id;
}

// The shared per-heading pixel-offset table filler (0x0045a320's / 0x0045a693+0x0045a74b's /
// 0x0045adea+0x0045ae92's loop body -- byte-identical across all four call sites, confirmed via
// linear instruction comparison). Called only for the shuttle-docking families (A/H_MOTHER,
// A/H_SHUTTLE): one llm_strat_bldg_sprite_anchor_offset call, one dword store into
// door_approach_route[10..13] (the tail 4 of the 14-byte array), then the 8-heading table itself.
void fill_heading_pixel_table(const sim_view &v, cfg_building &b, uint16_t building_id,
                              int32_t door_approach_tail_value) {
    int32_t out_x = 0, out_y = 0;
    detail::sprite_anchor_offset(v, building_id, &out_x, &out_y);

    // door_approach_route[10..13] as one dword -- 0x0045a343 (A/H_MOTHER: value 6) /
    // 0x0045a72e,0x0045ae85 (A/H_SHUTTLE: value 0). The literal, not anything type-derived.
    store_u32_le(&b.door_approach_route[10], static_cast<uint32_t>(door_approach_tail_value));

    const int32_t frame0      = sprite_id_for_anim_slot(v, b, 0);
    const int32_t origin_x    = v.sprite_meta[static_cast<uint32_t>(frame0)].origin_x;
    const int32_t origin_y    = v.sprite_meta[static_cast<uint32_t>(frame0)].origin_y;
    const int32_t width_term  = divide_round_toward_zero(static_cast<int32_t>(b.width) - 1) * 32;
    const int32_t height_term = divide_round_toward_zero(static_cast<int32_t>(b.height) - 1) * 32;

    uint8_t *table = b._pad_0x796; // see the file banner's DECLARED NEED
    for (int32_t heading = 0; heading < 8; ++heading) {
        const move_microstep &m0  = v.move_microsteps[heading * MICROSTEPS_PER_HEADING + 0];
        const move_microstep &m31 = v.move_microsteps[heading * MICROSTEPS_PER_HEADING + 31];

        const int32_t approach_x = (origin_x + out_x) - (width_term + static_cast<int32_t>(m0.x_off));
        const int32_t approach_y = (origin_y + out_y) - (height_term + static_cast<int32_t>(m0.y_off));
        const int32_t exit_x     = (origin_x + out_x) - (width_term + static_cast<int32_t>(m31.x_off));
        const int32_t exit_y     = (origin_y + out_y) - (height_term + static_cast<int32_t>(m31.y_off));

        store_u32_le(table + 0x00 + heading * 4, static_cast<uint32_t>(approach_x));
        store_u32_le(table + 0x20 + heading * 4, static_cast<uint32_t>(approach_y));
        store_u32_le(table + 0x40 + heading * 4, static_cast<uint32_t>(exit_x));
        store_u32_le(table + 0x60 + heading * 4, static_cast<uint32_t>(exit_y));
    }
}

// The original calls llm_strat_bldg_sprite_anchor_offset here (LAB_0045ad06 / LAB_0045b05e /
// LAB_0045b135) and then overwrites both its out-params with LITERAL constants without ever reading
// them. Preserved literally -- same Law-2 precedent as llm_strat_prod_shuttle_slot_spawn_arrival's
// "the call itself is observable, do not restructure control flow to skip it" (SIM-HOSTREACH batch H
// context), even though sprite_anchor_offset is independently known to be pure / zero-region-write
// (sim_bldg_sprite_anchor.h), so omitting it would in fact be unobservable either way.
void call_sprite_anchor_offset_discard(const sim_view &v, uint16_t building_id) {
    int32_t dead_x = 0, dead_y = 0;
    detail::sprite_anchor_offset(v, building_id, &dead_x, &dead_y);
    (void)dead_x;
    (void)dead_y;
}

// The A/H_HELIPAD dock_lift_offset_x/y single-value computation (LAB_0045ab8e, H_HELIPAD only --
// A_HELIPAD instead uses a plain literal {8,8}, see the switch below). Uses anim SLOT 4 (not 0) and a
// FIXED heading (dir=1, the original's byte[EBP-0x18]=1, not looped) with microstep INDEX 31 (the
// last microstep of that heading) on BOTH axes -- 0x0045ac89/0x0045ac9e (x) and
// 0x0045ace4/0x0045acfb (y). This is exactly the pair of addresses SIM-HOSTREACH batch H's context
// cited as `v.move_microsteps[dir * MICROSTEPS_PER_HEADING + 31].x_off/.y_off`; confirmed here to
// belong to this specific block (not the pad-table loop, which uses BOTH index 0 and 31).
void compute_dock_lift_via_microstep(const sim_view &v, cfg_building &b, uint16_t building_id) {
    int32_t out_x = 0, out_y = 0;
    detail::sprite_anchor_offset(v, building_id, &out_x, &out_y);

    const int32_t frame4   = sprite_id_for_anim_slot(v, b, 4); // Building.anim[4], NOT anim[0]
    const int32_t origin_x = v.sprite_meta[static_cast<uint32_t>(frame4)].origin_x;
    const int32_t origin_y = v.sprite_meta[static_cast<uint32_t>(frame4)].origin_y;

    constexpr int32_t     DIR = 1; // byte[EBP-0x18] = 1, fixed for this one arm (not looped)
    const move_microstep &m31 = v.move_microsteps[DIR * MICROSTEPS_PER_HEADING + 31];

    // 0x0045ac79 `MOV EBX,dword ptr [EAX + 0xd9f402]` / `SHL EBX,0x5` and 0x0045acd4
    // `MOV EDX,dword ptr [EAX + 0xd9f40a]` / `SHL EDX,0x5`. THE PAD OFFSETS, NOT THE FOOTPRINT:
    // 0xd9f402 - 0xd9ec80 = 0x782 = shuttle_pad_offset_x and 0xd9f40a - 0xd9ec80 = 0x78a =
    // shuttle_pad_offset_y (mh_structs.gen.h's static_asserts). This block never reads `width` or
    // `height` at all -- it does not touch 0xd9ec8a / 0xd9ec89.
    //
    // CORRECTED 2026-09-10 by the batch's adversarial review, and it is the only BUG the four
    // reviewers found. The first draft reused fill_heading_pixel_table's
    // `divide_round_toward_zero(width - 1) * 32` term here, where that helper legitimately DOES read
    // the footprint -- a value taken from the wrong field because it resembled a sibling pattern,
    // which is the G92 shape one field over. It happened to agree whenever `width`/`height` land in
    // {3,4} (the only footprints for which `(w-1)/2*32 == 32`), and the H_HELIPAD arm sets both pad
    // offsets to the literal 1 twenty lines above (0x0045ac1e / 0x0045ac32), so the original's term
    // is the CONSTANT 32 while ours tracked a .cfg-parsed field. Every other helipad footprint would
    // have placed the docked air unit's sprite wrong by (true_term - 32) pixels.
    const int32_t pad_x_term = static_cast<int32_t>(b.shuttle_pad_offset_x) * 32;
    const int32_t pad_y_term = static_cast<int32_t>(b.shuttle_pad_offset_y) * 32;

    b.dock_lift_offset_x = (origin_x + out_x) - (pad_x_term + static_cast<int32_t>(m31.x_off));
    b.dock_lift_offset_y = (origin_y + out_y) - (pad_y_term + static_cast<int32_t>(m31.y_off));
}

} // namespace

namespace detail {

void bldg_init_defaults(const sim_view &v, sim_store &own, const bldg_init_defaults_calls &c,
                        uint32_t building_id) {
    // The WHOLE body operates on this single masked value -- see the header banner.
    const uint16_t bid = static_cast<uint16_t>(building_id);
    cfg_building  &b   = own.cfg_building_at(static_cast<int32_t>(bid));

    // 0x0045a121-0x0045a13f: unconditional, every building_id.
    b.state_transition_ids[0] = 100;
    b.state_transition_ids[2] = 0x82;

    const uint8_t type = b.type;

    // ---- FIRST DISPATCH: the recovered jump table (selector = type-1, range checked 0..0x25) ------
    // state_transition_ids[1] (+ builder_count for A/H_MOTHER, + state_transition_ids[3] for A/H_LAB).
    switch (type) {
        case A_PRODUCTION:
        case A_MAIN_BASE:
        case A_RELAY:
        case A_SILOS:
        case A_CIVIL:
        case H_PRODUCTION:
        case H_MAIN_BASE:
        case H_RELAY:
        case H_SILOS:
        case H_CIVIL:
            b.state_transition_ids[1] = 1; // caseD_1 @0x0045a241
            break;
        case A_MINE:
        case H_MINE:
            b.state_transition_ids[1] = 0x72; // caseD_2 @0x0045a176
            break;
        case A_PLANT:
        case H_PLANT:
            b.state_transition_ids[1] = 0x86; // caseD_3 @0x0045a1bc
            break;
        case A_COLONY:
        case H_COLONY:
            b.state_transition_ids[1] = 0x87; // caseD_4 @0x0045a1eb
            break;
        case A_TURRET:
        case H_TURRET:
            b.state_transition_ids[1] = 0x7a; // caseD_5 @0x0045a201
            break;
        case A_MOTHER:
        case H_MOTHER:
            b.state_transition_ids[1] = 0x8a; // caseD_6 @0x0045a18f
            b.builder_count           = 0;
            break;
        case A_BARRACKS:
        case A_GARAGE:
        case A_AIRFIELD:
        case A_HELIPAD:
        case A_PORT:
        case A_SHUTTLE:
        case H_BARRACKS:
        case H_GARAGE:
        case H_AIRFIELD:
        case H_HELIPAD:
        case H_PORT:
        case H_SHUTTLE:
            b.state_transition_ids[1] = 0x77; // caseD_7 @0x0045a1d5
            break;
        case A_LAB:
        case H_LAB:
            b.state_transition_ids[1] = 1; // caseD_b @0x0045a217
            b.state_transition_ids[3] = 0x89;
            break;
        default:
            break; // includes A_BIURO/H_BYURO (0x11/0x25 -- no case in the original switch either) and 0
    }

    // ---- SECOND DISPATCH: the range-compare tree on the SAME `type` byte, entered unconditionally
    // after the first (caseD_11 @0x0045a255). door_approach_route/door_exit_route, park/shuttle_pad/
    // dock_lift offsets, and (shuttle-docking families only) the 8-heading pixel-offset table.
    switch (type) {
        case A_MOTHER:
        case H_MOTHER:
            fill_heading_pixel_table(v, b, bid, 6); // LAB_0045a320, ONE shared body for both
            break;

        case A_BARRACKS:
            b.park_offset_x        = 3;
            b.park_offset_y        = 2;
            b.shuttle_pad_offset_x = 5;
            b.shuttle_pad_offset_y = 4;
            b.dock_lift_offset_x   = 0;
            b.dock_lift_offset_y   = 0;
            break; // LAB_0045a616
        case H_BARRACKS:
            b.park_offset_x        = 1;
            b.park_offset_y        = 1;
            b.shuttle_pad_offset_x = -1;
            b.shuttle_pad_offset_y = 3;
            b.dock_lift_offset_x   = 0;
            b.dock_lift_offset_y   = 0;
            break; // LAB_0045a51c

        case A_GARAGE:
            b.park_offset_x        = 3;
            b.park_offset_y        = 2;
            b.shuttle_pad_offset_x = 4;
            b.shuttle_pad_offset_y = 3;
            b.dock_lift_offset_x   = 0;
            b.dock_lift_offset_y   = 0;
            break; // LAB_0045a907
        case H_GARAGE:
            b.park_offset_x        = 1;
            b.park_offset_y        = 2;
            b.shuttle_pad_offset_x = 0;
            b.shuttle_pad_offset_y = 3;
            b.dock_lift_offset_x   = 0;
            b.dock_lift_offset_y   = 0;
            break; // LAB_0045a599

        case A_AIRFIELD:
            b.park_offset_x          = 4;
            b.park_offset_y          = 0;
            b.shuttle_pad_offset_x   = 1;
            b.shuttle_pad_offset_y   = 3;
            b.dock_lift_offset_x     = 0;
            b.dock_lift_offset_y     = 0;
            b.door_exit_route[0]     = 6;
            b.door_exit_route[1]     = 6;
            b.door_exit_route[2]     = 6;
            b.door_exit_route[3]     = 0xff;
            b.door_approach_route[0] = 4;
            b.door_approach_route[1] = 4;
            b.door_approach_route[2] = 4;
            b.door_approach_route[3] = 0xff;
            break; // LAB_0045a984
        case H_AIRFIELD:
            b.park_offset_x          = 2;
            b.park_offset_y          = 3;
            b.shuttle_pad_offset_x   = 5;
            b.shuttle_pad_offset_y   = 0;
            b.dock_lift_offset_x     = -35; // 0xffffffdd
            b.dock_lift_offset_y     = 15;
            b.door_exit_route[0]     = 4;
            b.door_exit_route[1]     = 4;
            b.door_exit_route[2]     = 4;
            b.door_exit_route[3]     = 0xff;
            b.door_approach_route[0] = 6;
            b.door_approach_route[1] = 6;
            b.door_approach_route[2] = 6;
            b.door_approach_route[3] = 0xff;
            break; // LAB_0045aa89

        case A_HELIPAD:
            b.state_transition_ids[1] = 0x77; // redundant with the first dispatch (same value); transcribed literally
            b.door_exit_route[0]      = 5;
            b.door_exit_route[1]      = 0xff;
            b.door_approach_route[0]  = 7;
            b.door_approach_route[1]  = 0xff;
            b.park_offset_x           = 1;
            b.park_offset_y           = 0;
            b.shuttle_pad_offset_x    = 2;
            b.shuttle_pad_offset_y    = 1;
            call_sprite_anchor_offset_discard(v, bid); // LAB_0045ad06's call -- result unused
            b.dock_lift_offset_x = 8;
            b.dock_lift_offset_y = 8;
            break; // LAB_0045ad06
        case H_HELIPAD:
            b.state_transition_ids[1] = 0x77; // redundant; transcribed literally
            b.door_exit_route[0]      = 1;    // == the fixed heading used below (byte[EBP-0x18] = 1)
            b.door_exit_route[1]      = 0xff;
            b.door_approach_route[0]  = 3;
            b.door_approach_route[1]  = 0xff;
            b.park_offset_x           = 1;
            b.park_offset_y           = 1;
            b.shuttle_pad_offset_x    = 1;
            b.shuttle_pad_offset_y    = 1;
            compute_dock_lift_via_microstep(v, b, bid); // LAB_0045ab8e -- writes dock_lift_offset_x/y
            break;

        case A_PORT:
            b.state_transition_ids[1] = 0x77; // redundant; transcribed literally
            b.door_exit_route[0]      = 0xff;
            b.door_approach_route[0]  = 2;
            b.door_approach_route[1]  = 0xff;
            b.park_offset_x           = 3;
            b.park_offset_y           = 2;
            b.shuttle_pad_offset_x    = 3;
            b.shuttle_pad_offset_y    = 2;
            call_sprite_anchor_offset_discard(v, bid); // LAB_0045b05e's call -- result unused
            b.dock_lift_offset_x = 0x17;
            b.dock_lift_offset_y = 0x12;
            break;
        case H_PORT:
            b.state_transition_ids[1] = 0x77; // redundant; transcribed literally
            b.door_exit_route[0]      = 0;
            b.door_exit_route[1]      = 0;
            b.door_exit_route[2]      = 0xff;
            b.door_approach_route[0]  = 2;
            b.door_approach_route[1]  = 2;
            b.door_approach_route[2]  = 2;
            b.door_approach_route[3]  = 0xff;
            b.park_offset_x           = 2;
            b.park_offset_y           = 2;
            b.shuttle_pad_offset_x    = 2;
            b.shuttle_pad_offset_y    = 0;
            call_sprite_anchor_offset_discard(v, bid); // LAB_0045b135's call -- result unused
            b.dock_lift_offset_x = 0x10;
            b.dock_lift_offset_y = 0;
            break;

        case A_SHUTTLE:
            b.park_offset_x        = 2;
            b.park_offset_y        = 2;
            b.shuttle_pad_offset_x = 3;
            b.shuttle_pad_offset_y = 3;
            b.dock_lift_offset_x   = 0;
            b.dock_lift_offset_y   = 0;
            fill_heading_pixel_table(v, b, bid, 0); // LAB_0045a693 + LAB_0045a74b
            break;
        case H_SHUTTLE:
            b.park_offset_x        = 2;
            b.park_offset_y        = 2;
            b.shuttle_pad_offset_x = 0;
            b.shuttle_pad_offset_y = 0;
            b.dock_lift_offset_x   = 0;
            b.dock_lift_offset_y   = 0;
            fill_heading_pixel_table(v, b, bid, 0); // LAB_0045adea + LAB_0045ae92
            break;

        default:
            // A_LAB/H_LAB (confirmed no-op from the asm's own range-compare tree, NOT inferred from a
            // name -- see the header banner's A_LAB/A_PORT/A_SHUTTLE trap note) and every other type
            // (A_PRODUCTION/A_MINE/A_PLANT/A_COLONY/A_TURRET/A_RELAY/A_SILOS/A_CIVIL/A_MAIN_BASE and H_
            // siblings, plus A_BIURO/H_BYURO): nothing written by this dispatch.
            break;
    }

    // ---- COMMON TAIL, PART 1: runs for EVERY building_id, always (0x0045b24d). A fresh
    // sprite_anchor_offset call, then the mount/submount/pip anchor table (undef_block_tail) and
    // pip_slot_count -- a strict progressive gate: mount1 must pass before mount2 is even tested, etc.
    {
        int32_t out_x = 0, out_y = 0;
        detail::sprite_anchor_offset(v, bid, &out_x, &out_y);

        const int32_t            frame0 = sprite_id_for_anim_slot(v, b, 0);
        const sprite_meta_entry &sm     = v.sprite_meta[static_cast<uint32_t>(frame0)];

        if (sm.mount1_x > 0 && sm.mount1_y > 0) {
            store_u32_le(&b.undef_block_tail[0], static_cast<uint32_t>(sm.mount1_x + out_x));
            store_u32_le(&b.undef_block_tail[4], static_cast<uint32_t>(sm.mount1_y + out_y));
            b.pip_slot_count = 1;

            if (sm.mount2_x > 0 && sm.mount2_y > 0) {
                store_u32_le(&b.undef_block_tail[8], static_cast<uint32_t>(sm.mount2_x + out_x));
                store_u32_le(&b.undef_block_tail[12], static_cast<uint32_t>(sm.mount2_y + out_y));
                b.pip_slot_count = 2;

                if (sm.submount_x > 0 && sm.submount_y > 0) {
                    store_u32_le(&b.undef_block_tail[16], static_cast<uint32_t>(sm.submount_x + out_x));
                    store_u32_le(&b.undef_block_tail[20], static_cast<uint32_t>(sm.submount_y + out_y));
                    b.pip_slot_count = 3;

                    if (sm.pip_anchor4_x > 0 && sm.pip_anchor4_y > 0) {
                        store_u32_le(&b.undef_block_tail[24],
                                     static_cast<uint32_t>(sm.pip_anchor4_x + out_x));
                        store_u32_le(&b.undef_block_tail[28],
                                     static_cast<uint32_t>(sm.pip_anchor4_y + out_y));
                        b.pip_slot_count = 4;
                    }
                }
            }
        } else {
            // 0x0045b5a8: no valid mount1 anchor -- fallback to width/height*16, WITHOUT out_x/out_y
            // (unlike the primary path above; transcribed exactly, this asymmetry is real).
            b.pip_slot_count = 1;
            store_u32_le(&b.undef_block_tail[0], static_cast<uint32_t>(b.width) << 4);
            store_u32_le(&b.undef_block_tail[4], static_cast<uint32_t>(b.height) << 4);
        }
        // [EBP-0x28] in the .asm (a local that records which slot was last filled, 0..3) is written
        // at each step above but never read anywhere before the function returns -- a stack local
        // with no external write and no later read has no observable effect, so it is not modeled.
    }

    // ---- COMMON TAIL, PART 2: the TRUE final common code (0x0045b608) -- sprite_offset_x/y, then
    // selection_marker_offset_x/y derived from llm_gfx_bldg_frame_center_offset's out_dx/out_dy.
    {
        int32_t out_x = 0, out_y = 0;
        detail::sprite_anchor_offset(v, bid, &out_x, &out_y);
        b.sprite_offset_x = out_x;
        b.sprite_offset_y = out_y;

        int32_t out_dx = 0, out_dy = 0;
        c.gfx_bldg_frame_center_offset(bid, &out_dx, &out_dy);
        b.selection_marker_offset_x = b.sprite_offset_x - out_dx;
        b.selection_marker_offset_y = b.sprite_offset_y - out_dy;
    }
}

} // namespace detail

// ---- the public wrapper ----------------------------------------------------------------------------

void bldg_init_defaults(uint32_t building_id) {
    sim_state st = state();
    detail::bldg_init_defaults(st.read, st.own, live_bldg_init_defaults_calls(), building_id);
}

} // namespace mh::sim
