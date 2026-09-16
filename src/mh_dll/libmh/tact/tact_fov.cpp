//
// tact/tact_fov.cpp -- see tact_fov.h. Translated from the DISASSEMBLY, not from Ghidra's C.
//
// Zero outward (frontier) callees: the only CALL targets in this closure are
// llm_tact_fov_raycast_stencil (from vision_cone_setup) and llm_tact_fov_update_nearest_target
// (from fov_raycast_stencil), both same-TU siblings translated here (Law 3b) -- so there is no
// `_calls` struct and no `live_..._calls()` in this file. `assert_stack_capacity`
// (vision_cone_setup's opening CALL) is the inert CRT prologue helper rule 6 says to omit.
//
#include "tact/tact_fov.h"


namespace mh::tact {
namespace detail {

namespace {

// The 64x64 visibility stencil's shape (RID_TACT_FOV_STENCIL, 4096 B) and its fixed center index --
// see tact_state.h's own fov_stencil_at() comment. @0x00437020-0x00437027 re-derives this same
// constant via a bit-twiddle every single ray; it is invariant (no register dependency), so it is
// written here as a named constant instead of re-deriving it in a loop.
inline constexpr int32_t TACT_FOV_STENCIL_SLOTS  = 4096;
inline constexpr int32_t TACT_FOV_STENCIL_CENTER = 0x7df; // 31*64 + 31

// TACT1D (2026-08-28): the three ray-delta tables now come through tact_view (RID_TACT_FOV_DELTA_
// TABLE_{72,120,360}), NOT a raw address cast.
//
// WHY THAT MATTERED, because the earlier note here called it a style inconsistency and it was not.
// These are the only data tables in the tact domain reached by `reinterpret_cast<const int8_t *>(
// 0x00559428)`. In the GAME that address is right; in `net_selftest.exe` -- a standalone process --
// it lands inside the TEST BINARY'S OWN .text, so the raycast read two bytes of net_selftest's
// compiled machine code as a ray delta, differently after every relink. The consequence was not a
// crash but a blind spot: no offline case could control ray_dx/ray_dy, so nothing could reach the
// DDA stepping at all -- which is exactly how the AH/AL map-edge wrap bug below survived in a row
// recorded `verified`/T1. Bound through the view, a fixture seeds them like any other table.
// @0x00436f5c-0x00436f8e: the cascading vision_dist thresholds.
inline constexpr int32_t TACT_FOV_TABLE_THRESHOLD_72_120  = 0xc;
inline constexpr int32_t TACT_FOV_TABLE_THRESHOLD_120_360 = 0x14;

struct fov_ray_plan {
    int32_t       angle_step;
    int32_t       ray_count;
    const int8_t *table; // packed {dx,dy} pairs, one per ray-table angle index
};

// @0x00436f57-0x00436f93. The table pointers come from the view, so the offline suite can seed them.
fov_ray_plan select_fov_ray_plan(const tact_view &v, int32_t vision_dist) {
    if (vision_dist <= TACT_FOV_TABLE_THRESHOLD_72_120) {
        return {5, 0x48, v.fov_delta_table_72};
    }
    if (vision_dist <= TACT_FOV_TABLE_THRESHOLD_120_360) {
        return {3, 0x78, v.fov_delta_table_120};
    }
    return {1, 0x168, v.fov_delta_table_360};
}

} // namespace

// ---- llm_tact_fov_update_nearest_target @0x00437138 --------------------------------------------

void fov_update_nearest_target(tact_store &own, const tact_view &v,
                               const mh::state::mode_planes &planes, int32_t col, int32_t row) {
    // cell_index, same (col<<8)|row convention tile_object_at() uses; AX (16 bits) is what the
    // original stores into fov_nearest_hibit_cell/fov_nearest_low_cell.
    const int16_t cell_index = static_cast<int16_t>((col << 8) | row);
    // Re-reads the SAME byte the caller (fov_raycast_stencil) already checked nonzero before
    // calling -- exactly as the original does (the caller's CMP is a call-skip optimization, not a
    // value hand-off; this function derives its own read from [EBP+EAX*8+4]).
    const uint8_t occ = planes.tile_object_at(col, row).unit[0];

    // DEAD GLOBAL, preserved literally -- see the header banner. NOT a real ray distance.
    const int32_t candidate_dist = *v.fov_candidate_dist;

    // @0x00437138-0x0043715b: "hibit" branch -- occ >= 0x80 (top bit set: a unit stands here, per
    // mode_planes.h's tile_occupancy comment).
    if (occ >= 0x80) {
        if (candidate_dist < own.fov_nearest_hibit_dist()) {
            own.fov_nearest_hibit_dist() = candidate_dist;
            own.fov_nearest_hibit_cell() = cell_index;
        }
    }

    // @0x0043715b-0x0043717e: "low" branch -- occ <= 0x20. Mutually exclusive with the hibit branch
    // (0x20 < 0x80: a byte can satisfy at most one of the two conditions).
    if (occ <= 0x20) {
        if (candidate_dist < own.fov_nearest_low_dist()) {
            own.fov_nearest_low_dist() = candidate_dist;
            own.fov_nearest_low_cell() = cell_index;
        }
    }
}

// ---- llm_tact_fov_raycast_stencil @0x00436f30 ---------------------------------------------------

void fov_raycast_stencil(const tact_view &v, tact_store &own, const mh::state::mode_planes &planes) {
    // @0x00436f31-0x00436f4d: reset the nearest-target trackers for this whole cast.
    own.fov_nearest_low_cell()   = 0;
    own.fov_nearest_hibit_cell() = 0;
    own.fov_nearest_hibit_dist() = 0xff;
    own.fov_nearest_low_dist()   = 0xff;

    // @0x00436f57-0x00436f93: pick the ray table by vision distance.
    const fov_ray_plan plan = select_fov_ray_plan(v, own.fov_dist());

    // @0x00436fab-0x00436fb2: zero the 64x64 stencil.
    for (int32_t i = 0; i < TACT_FOV_STENCIL_SLOTS; ++i) {
        own.fov_stencil_at(i) = 0;
    }
    // NOTE: @0x00436fba `MOV ESI,[map_tile_height_sprites]` is a provably dead load -- see the
    // header banner. Not translated.

    // @0x00436fc6-0x00436fdf: starting angle = angle_base - angle_width/2, wrapped into [0,360).
    // fov_angle_base/fov_angle_width are tact_store-ONLY (no view path) -- see the header banner.
    const int32_t angle_width0 = own.fov_angle_width();
    int32_t       angle_start  = own.fov_angle_base() - (angle_width0 / 2);
    if (angle_start < 0) {
        angle_start += 0x168; // 360
    }

    // @0x00436fdf-0x00436fe9: starting ray-table index. Unsigned divide (DIV, not IDIV) in the
    // original; angle_start is >= 0 here so this matches ordinary int division unambiguously, but
    // cast explicitly to state the original's unsignedness.
    int32_t angle_index = static_cast<int32_t>(static_cast<uint32_t>(angle_start) /
                                               static_cast<uint32_t>(plan.angle_step));

    // @0x00436fee-0x00437000: OVERWRITES fov_angle_width() with the total ray count for this cast
    // (angle_width0 / angle_step + 1) -- see the header banner on this store-slot reuse.
    own.fov_angle_width() =
        static_cast<int32_t>(static_cast<uint32_t>(angle_width0) / static_cast<uint32_t>(plan.angle_step)) + 1;

    // @0x00437005: mark the stencil's own center cell visible, unconditionally, once per cast.
    own.fov_stencil_at(TACT_FOV_STENCIL_CENTER) = 1;

    int32_t ray_index = 0; // @0x0043700c-0x0043700e

    // @0x00437013-0x00437130: the outer per-ray loop. fov_angle_width() now holds the ray count
    // (re-read fresh every iteration, exactly as the original re-reads the memory cell).
    while (ray_index < own.fov_angle_width()) {
        // @0x00437015-0x0043701a: this ray's starting position -- re-read fresh every ray, exactly
        // as the original (AL=fov_row, AH=fov_col; AX==(col<<8)|row).
        //
        // uint8_t, NOT int32_t, AND THAT IS LOAD-BEARING (fixed 2026-08-28, TACT1D reimpl-verify).
        // The original walks this ray in AH (col) and AL (row) -- two 8-bit halves of ONE EAX that
        // `XOR EAX,EAX` @0x00437013 zeroes and that nothing in the loop ever widens. Every step is a
        // genuine 8-bit op (`INC AH` FE C4 @0x0043707b, `DEC AH` FE CC @0x0043709a, `INC AL` FE C0
        // @0x004370c9, `DEC AL` FE C8 @0x004370e6), so each half WRAPS MODULO 256 with no carry into
        // the other half and none into EAX's upper bits. The tile lookups are all `[EBP + EAX*8 +
        // ..]`, so the index is (col<<8)|row and can NEVER leave the 65536-entry grid: a ray leaving
        // the west edge re-enters at column 255. Held as int32_t with plain --col/++col, the walk
        // instead runs to -1 and `tile_object_at` (mode_planes.h, `tile_objects_[(x<<8)|y]`, no mask)
        // indexes far outside the plane -- an out-of-bounds READ on any unit within vision_dist of a
        // map edge. Widening is correct ONLY for this fresh per-ray reload, whose sources hold
        // genuine 0..255 values; it is wrong for the stepped position.
        uint8_t col = static_cast<uint8_t>(own.fov_col());
        uint8_t row = static_cast<uint8_t>(own.fov_row());

        int32_t stencil_idx = TACT_FOV_STENCIL_CENTER; // @0x0043701f-0x00437027

        uint8_t bl_accum = 0; // BL: the X (col) fixed-point fractional accumulator
        uint8_t bh_accum = 0; // BH: the Y (row) fixed-point fractional accumulator

        // @0x0043702e-0x0043703d: this ray's per-degree delta pair. Only the low two bytes of the
        // original's 4-byte dword read are ever used (see header banner) -- read as a packed
        // {int8 dx; int8 dy;} pair directly instead.
        const int8_t ray_dx = plan.table[angle_index * 2];
        const int8_t ray_dy = plan.table[angle_index * 2 + 1];

        // @0x00437042-0x00437044: RAY_DELTA_NEG's low two bytes, via a 16-bit combined negate (see
        // header banner for why this reproduces the original's 32-bit NEG exactly on the only bytes
        // that are ever read back).
        const uint16_t delta16 = static_cast<uint16_t>(static_cast<uint8_t>(ray_dx)) |
                                 (static_cast<uint16_t>(static_cast<uint8_t>(ray_dy)) << 8);
        const uint16_t delta16_neg = static_cast<uint16_t>(-delta16);
        const uint8_t  ray_dx_neg  = static_cast<uint8_t>(delta16_neg & 0xff);
        const uint8_t  ray_dy_neg  = static_cast<uint8_t>((delta16_neg >> 8) & 0xff);

        // @0x00437049-0x00437051: STEP_BUDGET = 2*vision_dist + 1, re-read fresh every ray.
        const int32_t step_budget = own.fov_dist() * 2 + 1;

        // @0x0043705e-0x004370fb: the inner per-step DDA loop.
        for (int32_t step = 0; step < step_budget; ++step) {
            // @0x0043705e-0x00437065: wall check at the CURRENT tile -- a set 0x20 flags[1] bit ends
            // this ray immediately (no more stepping, no more marking).
            if (planes.tile_object_at(col, row).flags[1] & 0x20) {
                break;
            }

            // ---- X sub-step (@0x0043706a-0x004370af) ---------------------------------------------
            if (ray_dx & 0x80) {
                // negative direction: accumulate the positive magnitude; overflow (carry) steps col--.
                const uint16_t sum = static_cast<uint16_t>(bl_accum) + ray_dx_neg;
                bl_accum           = static_cast<uint8_t>(sum);
                if (sum > 0xff) {
                    --col;
                    own.fov_stencil_at(stencil_idx) = 1;
                    stencil_idx -= 0x40;
                    if (planes.tile_object_at(col, row).unit[0] != 0) {
                        fov_update_nearest_target(own, v, planes, col, row);
                    }
                }
            } else {
                const uint16_t sum = static_cast<uint16_t>(bl_accum) + static_cast<uint8_t>(ray_dx);
                bl_accum           = static_cast<uint8_t>(sum);
                if (sum > 0xff) {
                    ++col;
                    own.fov_stencil_at(stencil_idx) = 1;
                    stencil_idx += 0x40;
                    if (planes.tile_object_at(col, row).unit[0] != 0) {
                        fov_update_nearest_target(own, v, planes, col, row);
                    }
                }
            }

            // @0x004370af-0x004370b8: wall re-check at the (possibly X-stepped) tile.
            if (planes.tile_object_at(col, row).flags[1] & 0x20) {
                break;
            }

            // ---- Y sub-step (@0x004370b8-0x004370fb) ---------------------------------------------
            if (ray_dy & 0x80) {
                const uint16_t sum = static_cast<uint16_t>(bh_accum) + ray_dy_neg;
                bh_accum           = static_cast<uint8_t>(sum);
                if (sum > 0xff) {
                    --row;
                    own.fov_stencil_at(stencil_idx) = 1;
                    stencil_idx -= 1;
                    if (planes.tile_object_at(col, row).unit[0] != 0) {
                        fov_update_nearest_target(own, v, planes, col, row);
                    }
                }
            } else {
                const uint16_t sum = static_cast<uint16_t>(bh_accum) + static_cast<uint8_t>(ray_dy);
                bh_accum           = static_cast<uint8_t>(sum);
                if (sum > 0xff) {
                    ++row;
                    own.fov_stencil_at(stencil_idx) = 1;
                    stencil_idx += 1;
                    if (planes.tile_object_at(col, row).unit[0] != 0) {
                        fov_update_nearest_target(own, v, planes, col, row);
                    }
                }
            }
        }

        // @0x00437102-0x0043711f: advance + wrap the angle index.
        ++angle_index;
        if (angle_index >= plan.ray_count) {
            angle_index = 0;
        }

        // @0x0043711f-0x00437130: advance the ray index (loop condition re-tested at the top).
        ++ray_index;
    }
}

// ---- llm_tact_vision_cone_setup @0x0042e1c9 -----------------------------------------------------

void vision_cone_setup(const tact_view &v, tact_store &own, const mh::state::mode_planes &planes,
                       int32_t col, int32_t row, int32_t angle_base, int32_t angle_width,
                       int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                       void *dead_outptr2, void *dead_outptr3) {
    (void)dead_outptr0;
    (void)dead_outptr1;
    (void)dead_outptr2;
    (void)dead_outptr3; // genuinely unused in the original body -- see header banner

    // @0x0042e1ed-0x0042e20d: stash all five scratch inputs for the raycaster to read back.
    own.fov_col()         = col;
    own.fov_row()         = row;
    own.fov_angle_base()  = angle_base;
    own.fov_angle_width() = angle_width;
    own.fov_dist()        = vision_dist;

    // @0x0042e212: llm_tact_fov_raycast_stencil, a same-TU sibling (Law 3b).
    fov_raycast_stencil(v, own, planes);
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

void vision_cone_setup(int32_t col, int32_t row, int32_t angle_base, int32_t angle_width,
                       int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                       void *dead_outptr2, void *dead_outptr3) {
    tact_state st = state();
    detail::vision_cone_setup(st.read, st.own, st.own.planes(), col, row, angle_base, angle_width,
                              vision_dist, dead_outptr0, dead_outptr1, dead_outptr2, dead_outptr3);
}

void fov_raycast_stencil() {
    tact_state st = state();
    detail::fov_raycast_stencil(st.read, st.own, st.own.planes());
}

// ---- the rebind ABI shim -------------------------------------------------------------------------
//
// `angle_width` unsigned where the wrapper takes it signed.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {

void vision_cone_setup(int32_t col, int32_t row, int32_t angle_base, uint32_t angle_width,
                       int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                       void *dead_outptr2, void *dead_outptr3) {
    mh::tact::vision_cone_setup(col, row, angle_base, static_cast<int32_t>(angle_width),
                                vision_dist, dead_outptr0, dead_outptr1, dead_outptr2,
                                dead_outptr3);
}

} // namespace rebind_arm

} // namespace mh::tact
