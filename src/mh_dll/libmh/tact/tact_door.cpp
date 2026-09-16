//
// tact/tact_door.cpp -- see tact_door.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_door_tick_004322c1.asm, llm_tact_door_anim_start_00432266.asm,
// llm_tact_door_update_tile_state_004324c0.asm, llm_tact_door_path_clear_00432a05.asm,
// llm_tact_door_apply_to_map_00439cf5.asm, llm_tact_door_parse_definition_004398eb.asm), not from
// Ghidra's .c.
//
#include "tact/tact_door.h"

#include <cstddef>
#include <cstring>

#include "addr/mh_calls.gen.h"       // frontier callees (Law 4): time_GetCurrentTime, llm_fatal_cleanup, utils_abort
#include "tact/tact_mission_parse.h" // mh::tact::mission_parse_int_token (already-translated sibling TU)
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const door_calls &live_door_calls() {
    static const door_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
        mh::tact::mission_parse_int_token,
    };
    return c;
}

namespace {

// ---- helpers shared by the tile-adjacency writes/reads (header comment: the -6/-2 bias trick) ----

// The `.class_owner` field (offset +6) of the record ONE FLAT INDEX BEFORE (col,row) -- i.e. -2
// bytes from tile_object_at(col,row)'s own address (-8 for one whole record, +6 back to the field).
// @0x0043257f/0x00432659 (door_update_tile_state, right leaf), mirrored @0x004327f6/0x004328db
// (left leaf) and @0x00439dc6/0x00439ee7 (door_apply_to_map, right/left). See tact_door.h's header
// comment for why this is raw pointer arithmetic and not tile_object_at(col, row-1).
uint8_t &door_prev_tile_class_owner_at(mh::state::mode_planes &planes, int32_t col, int32_t row) {
    uint8_t *rec = reinterpret_cast<uint8_t *>(&planes.tile_object_at(col, row));
    return *(rec - 2);
}

// The read counterpart used by door_path_clear: `.building` (offset +2) of the SAME preceding
// record, i.e. -6 bytes from tile_at(view,col,row)'s own address -- the exact bias Ghidra's own
// plates at 0x00432a86/0x00432b30 already document ("tile_objects_base - 6 ... the PRECEDING
// element's .building field").
uint16_t door_prev_tile_building_at(const tact_view &view, int32_t col, int32_t row) {
    const uint8_t *rec = reinterpret_cast<const uint8_t *>(&tile_at(view, col, row));
    return *reinterpret_cast<const uint16_t *>(rec - 6);
}

// The 3-way (col,row) walk step keyed by `.direct_mode`, IDENTICAL across all three original
// functions that walk a door's footprint: door_update_tile_state
// (@0x0043273c-0x00432774/@0x00439e3b-0x00439e73, right/left), door_apply_to_map
// (@0x00439e3b-0x00439e73/@0x00439f5c-0x00439f94) and door_path_clear
// (@0x00432aa9-0x00432ae1/@0x00432b50-0x00432b88). 0=DIRECT_LeftRight steps (col+1,row+1),
// 1=RightLeft steps (col+1,row-1), 2=Normal steps (col+1,row) -- per mh_llm_tact_door.direct_mode's
// own comment.
void door_leaf_step(uint8_t direct_mode, int32_t &col, int32_t &row) {
    if (direct_mode == 0) {
        ++col;
        ++row;
    } else if (direct_mode == 1) {
        ++col;
        --row;
    } else if (direct_mode == 2) {
        ++col;
    }
}

// The screen-space redraw-cache stamp shared by every k-sub-loop pass in both writers: if
// (screen_col, screen_row) lies within the current view, mark TILE_VIS_MAP so the renderer redraws
// it. @0x004325c6-0x00432602 et al. (six structurally-identical occurrences across
// door_update_tile_state's two branches x two leaves, and door_apply_to_map's two leaves).
void door_mark_tile_vis(tact_store &own, int32_t screen_col, int32_t screen_row) {
    if (screen_col < 0 || screen_row < 0) return;
    if (screen_col >= own.view_tiles_w() || screen_row >= own.view_tiles_h()) return;
    own.tile_vis_map_at(screen_row * own.view_tiles_w() + screen_col) = 2;
}

// The CLEAR path's k-sub-loop (door_update_tile_state only): for k in [0,k_bound), stop once
// (row-k)<0; otherwise stamp TILE_VIS_MAP (if on screen) and unconditionally zero the height-sprite
// cache slot for this k, then step the screen-row cursor down by one. @0x004325a5-0x0043262c (right
// leaf, k_bound=6), mirrored @0x00432827-0x004328ae (left leaf, k_bound=8) -- reimpl-verify
// (2026-08-27) caught this bound as a hardcoded 6 for both leaves; the two loops are NOT the same
// bound, confirmed independently at 0x004325a5 (CMP [EBP-0x34],0x6) vs 0x00432827
// (CMP [EBP-0x34],0x8).
void door_clear_leaf_tile(tact_store &own, mh::state::mode_planes &planes, int32_t col, int32_t row,
                          int32_t k_bound) {
    planes.tile_object_at(col, row).class_owner     = 0;
    door_prev_tile_class_owner_at(planes, col, row) = 0;

    const int32_t screen_col = col - own.map_cam_col();
    int32_t       vis_row    = row - own.map_cam_row();
    uint16_t     *height_row = own.map_tile_height_sprites() + (col << 10) + (row << 3);

    for (int32_t k = 0; k < k_bound; ++k) {
        if (row - k < 0) break;
        door_mark_tile_vis(own, screen_col, vis_row);
        height_row[k] = 0;
        --vis_row;
    }
}

// The MARK path's k-sub-loop, shared by door_update_tile_state's nonzero-row_value branch
// (@0x00432631-0x0043272a right leaf, @0x004328b3-0x004329ac left leaf) and BOTH of
// door_apply_to_map's (always-mark) leaves (@0x00439d7b-0x00439e2c,
// @0x00439e9c-0x00439f4d). Stamps `.class_owner` = door_idx at (col,row) and its flat-index-minus-
// one twin, then chains a sprite-bank frame lookup: while the just-read chain byte equals the loop
// counter `k`, write the current frame value into the height-sprite cache (+ redraw stamp), step
// frame_val down by 20 (0xff-terminator countdown per tact_view's own comment), and on a
// non-negative result decode the NEXT chain byte from `*gfx_bank_pixels +
// sprite_pix_offsets[frame_val + bank_sprite_base_at(20)] + 1` (the +1 skips the sprite header's
// first byte, @0x00432717/0x00439e19).
void door_mark_leaf_tile(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
                         int32_t door_idx, int32_t col, int32_t row, int32_t row_value) {
    planes.tile_object_at(col, row).class_owner     = static_cast<uint8_t>(door_idx);
    door_prev_tile_class_owner_at(planes, col, row) = static_cast<uint8_t>(door_idx);

    const int32_t screen_col = col - own.map_cam_col();
    int32_t       vis_row    = row - own.map_cam_row();
    uint16_t     *height_row = own.map_tile_height_sprites() + (col << 10) + (row << 3);

    int32_t frame_val = row_value; // var_3c/var_24 in the .asm
    int32_t chain_val = 0;         // var_24: the sprite-chain "expected k" gate
    int32_t k         = 0;         // var_28

    for (;;) {
        if (chain_val != k) break; // @0x00432685-0x0043268b
        door_mark_tile_vis(own, screen_col, vis_row);
        --vis_row;
        height_row[k] = static_cast<uint16_t>(frame_val);

        frame_val -= 20;          // @0x004326f5 -- step -0x14
        if (frame_val < 0) break; // @0x004326fd

        const int32_t  idx = frame_val + own.bank_sprite_base_at(20);
        const uint8_t *ptr = *view.gfx_bank_pixels + view.sprite_pix_offsets[idx];
        ++ptr; // @0x00432717: INC EDX, skip the sprite header's first byte
        chain_val = *ptr;
        ++k;
    }
}

// door_apply_to_map's leaf-mark step -- reimpl-verify (2026-08-27) caught this as WRONGLY sharing
// door_mark_leaf_tile above: the original llm_tact_door_apply_to_map (@0x00439dcc-0x00439e2c right
// leaf, @0x00439eed-0x00439f4d left leaf) does the SAME class_owner writes + height-sprite chain walk
// but NEVER computes screen_col/vis_row (no read of _G_LLM_MAP_CAM_COL/ROW anywhere in either loop)
// and NEVER touches _G_LLM_TILE_VIS_MAP (door_mark_tile_vis's write target, 0x713d20) -- confirmed by
// grepping the .asm export for both address literals: zero hits. door_update_tile_state's
// structurally-identical loop (0x00432631-0x0043272a) DOES both (0x0043266d-0x00432682 computes
// screen_col/vis_row, 0x004325fb stamps TILE_VIS_MAP) -- the two callers are genuinely different
// bodies, not the same helper called from two sites.
void door_apply_mark_leaf_tile(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
                               int32_t door_idx, int32_t col, int32_t row, int32_t row_value) {
    planes.tile_object_at(col, row).class_owner     = static_cast<uint8_t>(door_idx);
    door_prev_tile_class_owner_at(planes, col, row) = static_cast<uint8_t>(door_idx);

    uint16_t *height_row = own.map_tile_height_sprites() + (col << 10) + (row << 3);

    int32_t frame_val = row_value; // var_2c in the .asm
    int32_t chain_val = 0;         // var_1c: the sprite-chain "expected k" gate
    int32_t k         = 0;         // var_20

    for (;;) {
        if (chain_val != k) break; // @0x00439dcc-0x00439dd2 / @0x00439eed-0x00439ef3
        height_row[k] = static_cast<uint16_t>(frame_val);

        frame_val -= 20;          // @0x00439df7/0x00439f18 -- step -0x14
        if (frame_val < 0) break; // @0x00439dff/0x00439f20

        const int32_t  idx = frame_val + own.bank_sprite_base_at(20);
        const uint8_t *ptr = *view.gfx_bank_pixels + view.sprite_pix_offsets[idx];
        ++ptr; // @0x00439e19/0x00439f3a: INC EDX, skip the sprite header's first byte
        chain_val = *ptr;
        ++k;
    }
}

// door_update_tile_state's per-leaf driver: walks `col_count` columns from the CURRENT (col,row)
// cursor (shared across leaves via the by-reference parameters), reading
// `rows[(frame_index-1)*col_count + c]` per column and branching clear-vs-mark on whether it is
// zero, then stepping (col,row) by `direct_mode`.
void door_update_leaf(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
                      int32_t door_idx, uint8_t frame_index, uint16_t col_count,
                      const uint16_t *rows, uint8_t direct_mode, int32_t &col, int32_t &row,
                      int32_t clear_k_bound) {
    const int32_t row_base =
        (static_cast<int32_t>(frame_index) - 1) * static_cast<int32_t>(col_count);
    for (int32_t c = 0; c < static_cast<int32_t>(col_count); ++c) {
        const uint16_t row_value = rows[row_base + c];
        if (row_value == 0) {
            door_clear_leaf_tile(own, planes, col, row, clear_k_bound);
        } else {
            door_mark_leaf_tile(own, planes, view, door_idx, col, row, row_value);
        }
        door_leaf_step(direct_mode, col, row);
    }
}

// door_apply_to_map's per-leaf driver: same walk, but ALWAYS marks (using `rows[c]` directly, not
// frame_index-selected -- door_apply_to_map never reads `.frame_index` at all).
void door_apply_leaf(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
                     int32_t door_idx, uint16_t col_count, const uint16_t *rows, uint8_t direct_mode,
                     int32_t &col, int32_t &row) {
    for (int32_t c = 0; c < static_cast<int32_t>(col_count); ++c) {
        door_apply_mark_leaf_tile(own, planes, view, door_idx, col, row, rows[c]);
        door_leaf_step(direct_mode, col, row);
    }
}

// door_parse_definition's single write-and-advance step. `slot` starts at 1: slot==1 writes the
// LEADING col_count token (@ `.right_col_count`/`.left_col_count`); slot>=2 writes
// `rows[slot-2]`. Reproduced as ONE displacement-from-col_count pointer walk because the original
// computes both with a single formula (`right_frame_count_offset + slot*2`, matching
// frame_count/col_count/rows[] sitting CONTIGUOUSLY in the struct: see the static_assert below) --
// not because col_count and rows[] are conceptually the same array.
static_assert(offsetof(door_record, right_col_count) == offsetof(door_record, right_frame_count) + 2 &&
                  offsetof(door_record, right_rows) == offsetof(door_record, right_col_count) + 2 &&
                  offsetof(door_record, left_col_count) == offsetof(door_record, left_frame_count) + 2 &&
                  offsetof(door_record, left_rows) == offsetof(door_record, left_col_count) + 2,
              "llm_tact_door_parse_definition's single displacement-formula write depends on "
              "col_count/rows[] being adjacent in mh_llm_tact_door");

void door_parse_write_slot(door_record &door, bool is_left, uint32_t &slot, int32_t value) {
    uint16_t *dest = is_left ? &door.left_col_count : &door.right_col_count;
    dest[slot - 1] = static_cast<uint16_t>(value);
    ++slot;
}

} // namespace

namespace detail {

void door_tick(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
               const door_calls &c) {
    // @0x004322d9: loop bound is the literal 0x20 -- see tact_door.h's header comment on the
    // 16-vs-31 mismatch. Slot 0 is never touched (loop starts at 1), preserved literally.
    for (int32_t i = 1; i < 0x20; ++i) {
        door_record &door = own.door_table_at(i);
        // @0x004322f7-0x0043230d: skip entirely when id==0 or state==0.
        if (door.id == 0 || door.state == 0) continue;

        if (door.state == 1) {
            // ---- opening ----------------------------------------------------------------------
            if (door.frame_index == door.right_frame_count) {
                // @0x00432341-0x0043235f: fully open.
                door.state          = 2;
                door.opened_at_time = c.time_now();
                detail::door_update_tile_state(own, planes, view, i);
            } else {
                // @0x00432361-0x004323b1: advance one frame once `last_step_time + speed` elapses.
                const double deadline = door.last_step_time + door.speed;
                if (c.time_now() > deadline) {
                    ++door.frame_index;
                    detail::door_update_tile_state(own, planes, view, i);
                    door.last_step_time = door.speed + door.last_step_time;
                }
            }
            // @0x004323b1/0x004324b1: state==1 handling never falls into the state 2/3 checks this
            // tick -- both its exits jump straight back to the loop increment.
            continue;
        }

        if (door.state == 2) {
            // ---- open / holding ------------------------------------------------------------------
            const double deadline = door.opened_at_time + *view.door_open_hold_time_sec;
            if (c.time_now() > deadline) {
                ++door.state; // -> 3, same tick: falls straight into the state==3 body below.
                door.last_step_time = c.time_now();
            }
        }

        if (door.state == 3) {
            // ---- closing ---------------------------------------------------------------------
            const int32_t clear = detail::door_path_clear(own, view, i);
            if (clear == 1) {
                if (door.frame_index == 1) {
                    // @0x0043242b-0x00432449: fully closed.
                    detail::door_update_tile_state(own, planes, view, i);
                    door.frame_index = 0;
                    door.state       = 0;
                } else {
                    // @0x0043244b-0x0043249b: retreat one frame once `last_step_time + speed` elapses.
                    const double deadline = door.last_step_time + door.speed;
                    if (c.time_now() > deadline) {
                        --door.frame_index;
                        detail::door_update_tile_state(own, planes, view, i);
                        door.last_step_time = door.speed + door.last_step_time;
                    }
                }
            } else {
                // @0x0043249d-0x004324ac: blocked (a unit is still on the footprint) -- just push
                // the deadline out to now, no other state change.
                door.last_step_time = c.time_now();
            }
        }
    }
}

void door_anim_start(tact_store &own, const door_calls &c, int32_t door_idx) {
    door_record &door = own.door_table_at(door_idx);
    // @0x00432285-0x00432299: only fires from fully-idle (frame_index==0 AND state==0).
    if (door.frame_index != 0) return;
    if (door.state != 0) return;
    door.state          = 1;
    door.last_step_time = c.time_now();
}

void door_update_tile_state(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
                            int32_t door_idx) {
    door_record &door = own.door_table_at(door_idx);
    int32_t      col  = door.tile_x;
    int32_t      row  = door.tile_y;

    // RIGHT then LEFT, sharing the SAME (col,row) walk cursor -- neither loop resets it. The clear
    // k-sub-loop bound genuinely differs per leaf: 6 for right (@0x004325a5), 8 for left
    // (@0x00432827) -- see door_clear_leaf_tile's header comment.
    door_update_leaf(own, planes, view, door_idx, door.frame_index, door.right_col_count,
                     door.right_rows, door.direct_mode, col, row, /*clear_k_bound=*/6);
    door_update_leaf(own, planes, view, door_idx, door.frame_index, door.left_col_count,
                     door.left_rows, door.direct_mode, col, row, /*clear_k_bound=*/8);
}

int32_t door_path_clear(tact_store &own, const tact_view &view, int32_t door_idx) {
    door_record &door = own.door_table_at(door_idx);
    int32_t      col  = door.tile_x;
    int32_t      row  = door.tile_y;

    for (int32_t c = 0; c < static_cast<int32_t>(door.right_col_count); ++c) {
        if (tile_at(view, col, row).building != 0) return 0;
        if (door_prev_tile_building_at(view, col, row) != 0) return 0;
        door_leaf_step(door.direct_mode, col, row);
    }
    for (int32_t c = 0; c < static_cast<int32_t>(door.left_col_count); ++c) {
        if (tile_at(view, col, row).building != 0) return 0;
        if (door_prev_tile_building_at(view, col, row) != 0) return 0;
        door_leaf_step(door.direct_mode, col, row);
    }
    return 1;
}

void door_apply_to_map(tact_store &own, mh::state::mode_planes &planes, const tact_view &view) {
    // Same literal 0x20 bound / slot-0-skipped shape as door_tick.
    for (int32_t door_idx = 1; door_idx < 0x20; ++door_idx) {
        door_record &door = own.door_table_at(door_idx);
        // @0x00439d2b-0x00439d35: self-consistency check -- a slot whose own `.id` does not match
        // its table index is skipped.
        if (door.id != door_idx) continue;

        int32_t col = door.tile_x;
        int32_t row = door.tile_y;

        door_apply_leaf(own, planes, view, door_idx, door.right_col_count, door.right_rows,
                        door.direct_mode, col, row);
        door_apply_leaf(own, planes, view, door_idx, door.left_col_count, door.left_rows,
                        door.direct_mode, col, row);
    }
}

void door_parse_definition(tact_store &own, const door_calls &c, char *door_def_line,
                           int32_t door_index) {
    door_record &door = own.door_table_at(door_index);

    uint32_t pos = 1; // @0x00439908
    // @0x0043990f-0x00439936: skip spaces starting at line[1] to find the first non-space byte (or
    // the end of the line). strlen recomputed every pass, NOT hoisted -- same "quadratic, the
    // batch's shape" idiom as tact_mission_parse.cpp's own scanners.
    while (pos < static_cast<uint32_t>(std::strlen(door_def_line)) && door_def_line[pos] == ' ') {
        ++pos;
    }

    // @0x00439936-0x00439951: fatal if that landed at/after the end of the line.
    if (pos >= static_cast<uint32_t>(std::strlen(door_def_line))) {
        c.llm_fatal_cleanup();
        c.utils_abort(0);
        return; // unreachable
    }

    bool is_left;
    // @0x00439954-0x004399e6: literal "RIGHT" (5 chars) or "LEFT" (4 chars) at the cursor, else
    // fatal. Neither match is bounds-checked beyond `pos` itself (matches mission_parse_keyword_int's
    // own char-by-char compare, which relies on the NUL terminator to stop a would-be wild read).
    if (door_def_line[pos] == 'R' && door_def_line[pos + 1] == 'I' && door_def_line[pos + 2] == 'G' &&
        door_def_line[pos + 3] == 'H' && door_def_line[pos + 4] == 'T') {
        is_left = false;
        pos += 5;
    } else if (door_def_line[pos] == 'L' && door_def_line[pos + 1] == 'E' &&
               door_def_line[pos + 2] == 'F' && door_def_line[pos + 3] == 'T') {
        is_left = true;
        pos += 4;
    } else {
        c.llm_fatal_cleanup();
        c.utils_abort(0);
        return; // unreachable
    }

    uint32_t slot        = 1; // @0x004399f2: 1 = writes col_count; >=2 writes rows[slot-2]
    int32_t  range_start = 0; // @0x004399f9: pending dash-range start, 0 = none pending
    int32_t  delim       = 1; // @0x00439a00: mission_parse_int_token's last delimiter; nonzero to
                              // enter the loop

    while (delim != 0) {
        int32_t value = 0;
        delim         = c.mission_parse_int_token(door_def_line, &pos, &value);

        if (range_start > 0) {
            // @0x00439a2c-0x00439a9e: fill the inclusive range [range_start, value] -- reached
            // REGARDLESS of what `delim` came back as (even 0, the last-token case): the range fill
            // for THIS iteration runs before the outer loop condition is re-checked.
            for (int32_t i = range_start; i <= value; ++i) {
                door_parse_write_slot(door, is_left, slot, i);
            }
            range_start = 0;
        } else if (delim == '-') {
            // @0x00439aa7-0x00439ab3: stash the range start, write nothing yet.
            range_start = value;
        } else {
            // @0x00439ab5-0x00439ae6: a single value (delimiter was ',' or the loop is ending).
            door_parse_write_slot(door, is_left, slot, value);
        }
    }

    // @0x00439af8 onward: frame_count = (rows written) / col_count; fatal if that division has a
    // nonzero remainder. `entries` mirrors `slot-2` from the .asm (>=0 once at least one row token
    // was written; a negative value here means zero rows were written and requires zero remainder
    // trivially, matching the original's IDIV-of-a-negative-dividend behaviour bit for bit since
    // C++'s `%`/`/` truncate toward zero the same way IDIV does).
    const uint16_t col_count = is_left ? door.left_col_count : door.right_col_count;
    const int32_t  entries   = static_cast<int32_t>(slot) - 2;
    if (entries % static_cast<int32_t>(col_count) > 0) {
        c.llm_fatal_cleanup();
        c.utils_abort(0);
        return; // unreachable
    }
    const uint16_t frame_count =
        static_cast<uint16_t>(entries / static_cast<int32_t>(col_count));
    if (is_left) {
        door.left_frame_count = frame_count;
    } else {
        door.right_frame_count = frame_count;
    }

    // @0x00439b54-0x00439b69 / 0x00439bbf-0x00439bd4: a bounded counting loop with NO body -- reads
    // nothing, writes nothing, calls nothing. Preserved as a literal no-op for 1:1 correspondence
    // with the assembly (it is provably behaviour-free: no global, no call, no observable local
    // escapes it).
    for (int32_t i = 0; i < entries; ++i) {
        // intentionally empty -- @0x00439b61/0x00439bcc
    }
}

} // namespace detail

void door_tick() {
    tact_state st = state();
    detail::door_tick(st.own, st.own.planes(), st.read, live_door_calls());
}

void door_anim_start(int32_t door_idx) {
    tact_state st = state();
    detail::door_anim_start(st.own, live_door_calls(), door_idx);
}

void door_update_tile_state(int32_t door_idx) {
    tact_state st = state();
    detail::door_update_tile_state(st.own, st.own.planes(), st.read, door_idx);
}

int32_t door_path_clear(int32_t door_idx) {
    tact_state st = state();
    return detail::door_path_clear(st.own, st.read, door_idx);
}

void door_apply_to_map() {
    tact_state st = state();
    detail::door_apply_to_map(st.own, st.own.planes(), st.read);
}

void door_parse_definition(char *door_def_line, int32_t door_index) {
    tact_state st = state();
    detail::door_parse_definition(st.own, live_door_calls(), door_def_line, door_index);
}

} // namespace mh::tact
