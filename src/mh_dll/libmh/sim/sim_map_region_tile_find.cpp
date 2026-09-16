//
// sim/sim_map_region_tile_find.cpp -- see sim_map_region_tile_find.h. Translated from the
// DISASSEMBLY, not from Ghidra's C drafts:
//   tmp/decomp_sim/llm_map_region_find_nearest_valid_tile_0041ed6d.asm
//   tmp/decomp_sim/llm_strat_pathfind_find_closer_visible_tile_0041fff5.asm
//   tmp/decomp_sim/llm_map_region_walk_to_valid_tile_004201e4.asm
//
// DECLARED NEED (see mh_structs.gen.h): `mh_llm_map_region_cell::terrain_flags`'s field comment only
// documents its LOW byte ("low byte from g::passable; bits 2/3/4 = obstacle within ~5/~3/~1 tiles").
// All three functions below instead read the UPPER 24 bits (`terrain_flags >> 8`, confirmed: the x86
// `XOR AL,AL` before the `SHR EAX,8` is redundant -- the low byte is shifted out either way, so the
// result is exactly `terrain_flags >> 8`) and treat 0 / 0xffffff as sentinels, the same two sentinel
// values `mh_llm_map_region::region`'s OWN comment documents ("0xffffffff = flood-fill pending, 0 =
// no region") one field over. This strongly suggests the upper 24 bits are a packed/truncated copy of
// the region generation-id for a fast check without dereferencing `region`, but that is this
// translator's inference, not a confirmed fact -- the struct's field comment should be extended by
// whoever verifies it, not left silently relied upon by three call sites.
//
#include "sim/sim_map_region_tile_find.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_strat_tile_dist_wrapped -- frontier callee, already committed
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const find_closer_visible_tile_calls &live_find_closer_visible_tile_calls() {
    static const find_closer_visible_tile_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
    };
    return c;
}

namespace detail {

int32_t find_nearest_valid_tile(const sim_view &v, sim_store &own, uint8_t *col, uint8_t *row) {
    // 0x0041ed8a/0x0041ed91: direction index and ring size, both start at 1. `dir` indexes
    // v.map_dir_step_deltas[dir*2+0]/[dir*2+1] (flat, UNSIGNED bytes -- see sim_state.h's own comment
    // on this table; a 0xff delta means +255 wrapped through the mask below, never -1).
    int32_t dir  = 1;
    int32_t ring = 1;

    // 0x0041ed98/0x0041ed9b: outer do-while, condition checked BEFORE each ring -- exits (return 0)
    // the instant `ring` reaches the map width.
    while (ring < *v.map_width) {
        // ---- leg 1: `ring` steps in direction `dir` (0x0041edae-0x0041ee57) ----
        for (int32_t k = 0; k < ring; ++k) {
            // 0x0041edc3-0x0041edd3: step happens BEFORE the check every iteration (so this
            // function's own starting cell, passed in by the caller, is never itself tested).
            *col = static_cast<uint8_t>(*col + v.map_dir_step_deltas[dir * 2 + 0]);
            *col &= static_cast<uint8_t>(*v.path_wrap_mask);
            // 0x0041edde-0x0041edf7: row step + mask, AFTER col's step+mask (interleaved per-axis,
            // not batched -- see walk_to_valid_tile below for the DIFFERENT order that function uses).
            *row = static_cast<uint8_t>(*row + v.map_dir_step_deltas[dir * 2 + 1]);
            *row &= static_cast<uint8_t>(*v.path_wrap_mask);

            // 0x0041edf9-0x0041ee1a: reject if the region cell's upper-24-bit field is zero.
            if ((own.region_cell_at(*col, *row).terrain_flags >> 8) != 0) {
                // 0x0041ee1e-0x0041ee3d: re-read the SAME field (transcribed as a duplicate load, not
                // cached in a local -- matches the original's two separate loads) and also reject the
                // 0xffffff sentinel.
                if ((own.region_cell_at(*col, *row).terrain_flags >> 8) != 0xffffffu) {
                    return 1; // 0x0041ee46: found -- *col/*row already hold the valid cell
                }
            }
        }

        // ---- rotate `dir` by one quarter turn: +6, wrap at 24 -- 0x0041ee57-0x0041ee65 ----
        dir += 6;
        if (dir > 0x18) dir -= 0x18;

        // ---- leg 2: `ring` steps in the (now rotated) direction `dir` (0x0041ee6c-0x0041ef12),
        // identical body to leg 1 ----
        for (int32_t k = 0; k < ring; ++k) {
            *col = static_cast<uint8_t>(*col + v.map_dir_step_deltas[dir * 2 + 0]);
            *col &= static_cast<uint8_t>(*v.path_wrap_mask);
            *row = static_cast<uint8_t>(*row + v.map_dir_step_deltas[dir * 2 + 1]);
            *row &= static_cast<uint8_t>(*v.path_wrap_mask);

            if ((own.region_cell_at(*col, *row).terrain_flags >> 8) != 0) {
                if ((own.region_cell_at(*col, *row).terrain_flags >> 8) != 0xffffffu) {
                    return 1; // 0x0041ef04
                }
            }
        }

        // ---- rotate `dir` again, then grow the ring -- 0x0041ef12-0x0041ef26 ----
        dir += 6;
        if (dir > 0x18) dir -= 0x18;
        ++ring;
    }
    return 0; // 0x0041ef2b: ring reached the map width without finding a valid cell
}

int32_t find_closer_visible_tile(const sim_view &v, sim_store &own,
                                 const find_closer_visible_tile_calls &c, uint8_t src_col,
                                 uint8_t src_row, uint32_t *out_col, uint32_t *out_row,
                                 int32_t dst_col, int32_t dst_row) {
    uint8_t col = src_col;
    uint8_t row = src_row;

    // 0x00420031-0x0042003e: best_dist is the ORIGINAL (src_col,src_row) position's distance to
    // (dst_col,dst_row), computed ONCE before the ring search starts -- a candidate must be STRICTLY
    // closer than this, not merely close.
    const int32_t best_dist = c.tile_dist_wrapped(col, row, dst_col, dst_row);

    // 0x00420041-0x0042004e: the fog-of-war visibility bit for the current group-order owner. Reads
    // only the LOW BYTE of the (4-byte) _G_LLM_STRAT_GROUP_ORDER_OWNER global as the shift count --
    // unambiguous since a player index is always small (0..7).
    const uint32_t vis_mask = 1u << static_cast<uint8_t>(*v.group_order_owner);

    // 0x00420016/0x0042001d/0x00420024/0x00420051: ring size and the (ddx,ddy) direction pair --
    // start at ring=1, (ddx,ddy)=(0,-1).
    int32_t ring = 1;
    int32_t ddx  = 0;
    int32_t ddy  = -1;

    // 0x00420058/0x0042005b: outer do-while, condition checked BEFORE each ring.
    while (ring < *v.map_width) {
        // ---- leg 1: `ring` steps (0x0042006e-0x004200f6) ----
        for (int32_t k = 0; k < ring; ++k) {
            // 0x00420083-0x00420097: only a tile with a nonzero v.passable[] entry is a candidate at
            // all (checked BEFORE stepping, on the CURRENT (col,row)).
            if (v.passable[(col << 8) | row] != 0) {
                // 0x00420099-0x004200b2: ...and only if the owner has discovered it.
                if ((own.fog_discovered_at(col, row) & vis_mask) != 0) {
                    // 0x004200b6-0x004200cc: strictly closer than the ORIGINAL position -> found.
                    if (c.tile_dist_wrapped(col, row, dst_col, dst_row) < best_dist) {
                        *out_col = col; // 0x004201bf-0x004201c6
                        *out_row = row; // 0x004201c8-0x004201cf
                        return 0;       // 0x004201d1
                    }
                }
            }
            // 0x004200d2-0x004200f1: step, byte arithmetic (8-bit wraparound), then wrap-masked.
            col = static_cast<uint8_t>(col + ddx) & static_cast<uint8_t>(*v.path_wrap_mask);
            row = static_cast<uint8_t>(row + ddy) & static_cast<uint8_t>(*v.path_wrap_mask);
        }

        // ---- rotate 90 degrees: (ddx,ddy) -> (-ddy,ddx) -- 0x004200f6-0x00420108 ----
        {
            const int32_t tmp = ddx;
            ddx               = -ddy;
            ddy               = tmp;
        }

        // ---- leg 2: `ring` steps (0x00420112-0x00420196), identical body to leg 1 ----
        for (int32_t k = 0; k < ring; ++k) {
            if (v.passable[(col << 8) | row] != 0) {
                if ((own.fog_discovered_at(col, row) & vis_mask) != 0) {
                    if (c.tile_dist_wrapped(col, row, dst_col, dst_row) < best_dist) {
                        *out_col = col;
                        *out_row = row;
                        return 0;
                    }
                }
            }
            col = static_cast<uint8_t>(col + ddx) & static_cast<uint8_t>(*v.path_wrap_mask);
            row = static_cast<uint8_t>(row + ddy) & static_cast<uint8_t>(*v.path_wrap_mask);
        }

        // ---- rotate again, then grow the ring -- 0x00420196-0x004201b1 ----
        {
            const int32_t tmp = ddx;
            ddx               = -ddy;
            ddy               = tmp;
        }
        ++ring;
    }
    return 1; // 0x004201b6: exhausted the spiral without a strictly-closer visible tile
}

int32_t walk_to_valid_tile(const sim_view &v, sim_store &own, uint32_t src_col, uint32_t src_row,
                           uint32_t dst_col, uint32_t dst_row, uint32_t *out_col, uint32_t *out_row) {
    int32_t       col = static_cast<int32_t>(src_col);
    int32_t       row = static_cast<int32_t>(src_row);
    const int32_t dc  = static_cast<int32_t>(dst_col);
    const int32_t dr  = static_cast<int32_t>(dst_row);

    // 0x00420205-0x00420217: the loop-continuation guard is `col != dst_col && row != dst_row` -- NOT
    // the AND-style "loop while not exactly at the destination" a naive reading would produce.
    // Confirmed from the raw bytes: `CMP col,dst_col; JZ exit` then, only on the fallthrough (col !=
    // dst_col), `CMP row,dst_row; JNZ continue` -- so the loop EXITS as soon as EITHER axis alone
    // matches the destination (col==dst_col OR row==dst_row), not only on an exact hit. The separate
    // exact-match check below (proper AND) is what then decides the return value.
    while (col != dc && row != dr) {
        // 0x00420217-0x00420266: obstruction check at the CURRENT (col,row), BEFORE stepping.
        const uint32_t region_id = own.region_cell_at(col, row).terrain_flags >> 8;
        if (region_id != 0xffffffu) {
            // 0x00420237-0x00420252: re-read the SAME field (duplicate load, not cached -- matches
            // the original's two separate loads, same posture as find_nearest_valid_tile above).
            const uint32_t region_id2 = own.region_cell_at(col, row).terrain_flags >> 8;
            if (region_id2 != 0) {
                // NOTE 0x0042025c: the original's index is a full-width ADD, not the OR the other two
                // functions in this TU use -- col/row here are int32_t and UNMASKED on the loop's first
                // iteration (masking happens at the tail of this same loop body, below), so ADD and OR
                // are NOT interchangeable the way they are for find_nearest_valid_tile/
                // find_closer_visible_tile's byte-range col/row. Caught by reimpl-verify.js
                // (2026-08-20): a naive `|` here diverges from the original whenever row >= 256 on the
                // first pass. Must stay `+`.
                if (v.passable[(col << 8) + row] != 0) break; // 0x0042025f-0x00420266: blocked, stop
            }
        }

        // ---- toroidal-shortest step delta for col (0x0042026f-0x004202c2) ----
        int32_t dx = col - dc;
        // 0x00420278-0x00420288: this 3-instruction sequence (SAR by 31, SUB, SAR by 1) IS exactly
        // C's truncating signed division by 2 -- and *v.map_width is always positive, so `/ 2` below
        // reproduces it bit-for-bit, not merely approximately.
        int32_t half_width = *v.map_width / 2;
        if (dx > half_width) dx -= half_width;     // 0x0042028a-0x004202a1: subtracts HALF width here --
        half_width = *v.map_width / 2;             // 0x0042028f-0x0042029f: recomputed, same value
        if (dx <= -half_width) dx += *v.map_width; // 0x004202b8-0x004202c2: ...but adds the FULL
                                                   // width here. Confirmed asymmetric against the
                                                   // raw bytes, not a translation slip -- reproduced
                                                   // as-is, not "fixed" to be symmetric.

        // ---- same toroidal-shortest computation for row/height (0x004202c5-0x0042031b) ----
        int32_t dy          = row - dr;
        int32_t half_height = *v.map_height / 2;
        if (dy > half_height) dy -= half_height;
        half_height = *v.map_height / 2;
        if (dy <= -half_height) dy += *v.map_height;

        // ---- apply one step toward the destination (0x0042031b-0x00420348) ----
        if (dx < 0) ++col;
        if (dx > 0) --col;
        if (dy < 0) ++row;
        if (dy > 0) --row;

        // ---- mask BOTH axes AFTER both mutations (0x0042034b-0x00420358) -- batched at the end of
        // the step, UNLIKE find_nearest_valid_tile's interleaved per-axis "mutate, mask, mutate, mask"
        // above. Each function's own order is reproduced from its own bytes, not assumed uniform. ----
        col &= static_cast<int32_t>(*v.path_wrap_mask);
        row &= static_cast<int32_t>(*v.path_wrap_mask);
    }

    // 0x00420360-0x00420389: the SEPARATE exact-match check (proper AND) that decides the return.
    if (col == dc && row == dr) {
        return 1; // 0x00420389: landed exactly on the destination; out params NOT written
    }
    *out_col = static_cast<uint32_t>(col); // 0x00420370-0x00420376
    *out_row = static_cast<uint32_t>(row); // 0x0042037b-0x0042037e
    return 0;                              // 0x00420380: stopped partway (one-axis match or blocked)
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t find_nearest_valid_tile(uint8_t *col, uint8_t *row) {
    sim_state st = state();
    return detail::find_nearest_valid_tile(st.read, st.own, col, row);
}

int32_t find_closer_visible_tile(uint8_t src_col, uint8_t src_row, uint32_t *out_col, uint32_t *out_row,
                                 int32_t dst_col, int32_t dst_row) {
    sim_state st = state();
    return detail::find_closer_visible_tile(st.read, st.own, live_find_closer_visible_tile_calls(),
                                            src_col, src_row, out_col, out_row, dst_col, dst_row);
}

int32_t walk_to_valid_tile(uint32_t src_col, uint32_t src_row, uint32_t dst_col, uint32_t dst_row,
                           uint32_t *out_col, uint32_t *out_row) {
    sim_state st = state();
    return detail::walk_to_valid_tile(st.read, st.own, src_col, src_row, dst_col, dst_row, out_col,
                                      out_row);
}


} // namespace mh::sim
