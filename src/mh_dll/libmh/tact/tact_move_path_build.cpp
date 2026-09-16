//
// tact/tact_move_path_build.cpp -- see tact_move_path_build.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_move_path_build.h"

#include <utility>


namespace mh::tact {

namespace detail {

namespace {

// (col<<8)|row, byte-wrapped on each axis (matches the original's 8-bit AL/AH INC/DEC) -- NOT
// masked by coord_mask here; callers that need the toroidal wrap AND the result themselves (see the
// header banner: goal_packed/start_packed are stored UNMASKED, but every computed NEIGHBOR position
// is masked at its use site).
inline uint32_t pack_tile(uint32_t col, uint32_t row) {
    return ((col & 0xFFu) << 8) | (row & 0xFFu);
}

} // namespace

void move_path_build(const tact_view &v, tact_store &own) {
    // ---- 0: result defaults to start (0x0041c2ad-0x0041c2bc) ----
    own.move_flood_result_col() = *v.move_flood_start_col;
    own.move_flood_result_row() = *v.move_flood_start_row;

    // ---- 1: cost map reset + coord mask (0x0041c2c1-0x0041c2eb) ----
    uint32_t *const cost_map = own.move_path_cost_map();
    for (uint32_t i = 0; i < 0x10000u; ++i) cost_map[i] = 0xFFFFFFFFu;

    const uint32_t grid_w = static_cast<uint32_t>(*v.grid_width) & 0xFFFFu; // word read, per the .asm
    const uint32_t grid_h = static_cast<uint32_t>(*v.grid_height) & 0xFFFFu;
    const uint32_t coord_mask =
        (((grid_w - 1u) & 0xFFu) << 8) | ((grid_h - 1u) & 0xFFu);
    own.move_path_coord_mask() = coord_mask;

    const uint32_t start_col = *v.move_flood_start_col;
    const uint32_t start_row = *v.move_flood_start_row;
    const uint32_t goal_col  = *v.move_flood_goal_col;
    const uint32_t goal_row  = *v.move_flood_goal_row;

    const uint32_t goal_packed  = pack_tile(goal_col, goal_row); // stored UNMASKED, per the .asm
    own.move_path_goal_packed() = goal_packed;

    auto neighbor = [&](uint32_t col, uint32_t row) -> uint32_t {
        return pack_tile(col, row) & coord_mask;
    };

    // ---- 2: goal-surrounded check (0x0041c2f0-0x0041c370) ----
    // Four RAW passable reads (no 0/0x10000 substitution -- that only applies inside the flood
    // relaxation below), order N/S/E/W per the .asm (irrelevant to a sum).
    const uint32_t neighbor_sum = v.passable[neighbor(goal_col, goal_row + 1)] +
                                  v.passable[neighbor(goal_col, goal_row - 1)] +
                                  v.passable[neighbor(goal_col + 1, goal_row)] +
                                  v.passable[neighbor(goal_col - 1, goal_row)];
    if (neighbor_sum == 0) {
        own.move_flood_result_col() = static_cast<uint8_t>(goal_col);
        own.move_flood_result_row() = static_cast<uint8_t>(goal_row);
        own.move_path_slot_id()     = -1;
        return;
    }

    // ---- 3: flood-fill / edge-relaxation loop (0x0041c371-0x0041c487) ----
    const uint32_t start_packed  = pack_tile(start_col, start_row); // stored UNMASKED, per the .asm
    own.move_path_start_packed() = start_packed;
    cost_map[start_packed]       = 0;

    uint16_t *const queue_a = own.move_path_queue_a();
    uint16_t *const queue_b = own.move_path_queue_b();
    queue_b[0]              = static_cast<uint16_t>(start_packed);

    uint16_t *cur_wave             = queue_b;
    uint16_t *next_wave            = queue_a;
    own.move_path_queue_cur_addr() = reinterpret_cast<uint32_t>(cur_wave);

    uint32_t cur_count  = 1; // entries currently queued (the .asm's byte counter / 2)
    uint32_t next_count = 0;

    for (;;) {
        while (cur_count != 0) {
            --cur_count;
            const uint32_t tile       = cur_wave[cur_count];
            const uint32_t base_cost  = cost_map[tile];
            own.move_path_tile_cost() = base_cost;

            // 4 orthogonal neighbors, row-1/row+1/col+1/col-1 order per the .asm. NOTE: unlike
            // every other neighbor computation in this function, these are FULL-EAX arithmetic on
            // the packed tile (`SUB/ADD EAX,0x1` for row, `ADD/SUB EAX,0x100` for col) rather than
            // byte-only INC/DEC AL/AH -- so row-1/row+1 can BORROW/CARRY into the col byte when row
            // underflows/overflows (tile-1 with row==0 also decrements col; tile+1 with row==0xff
            // also increments col). A rig run caught exactly this: treating row as an independent
            // byte-wrapped field (no col interaction) diverged on every call that touched a
            // row-boundary tile. col+1/col-1 (+-0x100) never interact with the row byte either way.
            auto relax = [&](uint32_t raw) {
                const uint32_t ntile     = raw & coord_mask;
                const uint32_t p         = v.passable[ntile];
                const uint32_t step_cost = (p != 0) ? p : 0x10000u; // SOFT penalty, not a hard wall
                const uint32_t new_cost  = base_cost + step_cost;
                if (new_cost < cost_map[ntile]) {
                    cost_map[ntile] = new_cost;
                    // 0x0041c3e9/0x0041c419/0x0041c447/0x0041c477: the push is a 32-BIT store
                    // (`MOV dword ptr [EBP+EBX],EAX`) even though each queue entry is a 16-bit
                    // slot (EBX += 2) -- EAX's zero upper half zeroes the FOLLOWING slot as a
                    // side effect. A later push into that same following slot overwrites it for
                    // real, but a wave's LAST push leaves that trailing zero standing, and the
                    // shadow diff sees it (confirmed: without this, "ours" carries a stale byte
                    // from an earlier call's wave instead of the original's fresh zero).
                    next_wave[next_count]     = static_cast<uint16_t>(ntile);
                    next_wave[next_count + 1] = 0;
                    ++next_count;
                }
            };
            relax(tile - 1u);
            relax(tile + 0x100u);
            relax(tile + 1u);
            relax(tile - 0x100u);
        }

        if (cost_map[goal_packed] < 0x10000u) break; // goal reached with a real (non-penalty) cost
        if (next_count == 0) break;                  // nothing left to expand

        std::swap(cur_wave, next_wave);
        std::swap(cur_count, next_count); // next_count becomes 0 (cur_count was already 0)
        own.move_path_queue_cur_addr() = reinterpret_cast<uint32_t>(cur_wave);
    }

    // ---- 4: free-slot allocation (0x0041c4a8-0x0041c4d6) ----
    int32_t slot_id = -1;
    for (int32_t i = 0; i < 100; ++i) {
        if (own.planes().path_slot_flag_at(0, i) == 0) {
            slot_id = i;
            break;
        }
    }
    own.move_path_slot_id() = slot_id;
    if (slot_id < 0) {
        // result stays at its step-0 default; no buffer touched.
        return;
    }
    own.planes().path_waypoint_at(0, slot_id, 0) = {0, 0};

    // ---- 5: backtrace (0x0041c4d6-0x0041c7e5) ----
    own.move_path_trace_cost_sum() = 0;
    own.move_path_rle_count()      = 0;
    own.move_path_trace_best_dir() = 0; // dead store relative to the loop below; preserved (Law 2)

    uint32_t rle_count = 0;
    uint32_t current   = goal_packed;

    for (;;) {
        own.move_path_trace_cost_sum() += v.passable[current];
        own.move_path_trace_tile() = current;

        const uint8_t cl                = own.planes().path_waypoint_at(0, slot_id, rle_count).heading;
        uint32_t      running_best_cost = cost_map[current];
        own.move_path_trace_best_dir()  = 0xFF;
        own.move_path_trace_base()      = current;

        const uint32_t base_col = current >> 8;
        const uint32_t base_row = current & 0xFFu;

        uint8_t  best_dir  = 0xFF;
        uint32_t best_tile = own.move_path_trace_best_tile(); // carries a STALE value if nothing
                                                              // selects below -- see header banner.

        // Orthogonal candidates, N/S/W/E order per the .asm.
        auto consider = [&](uint32_t tile, uint8_t dir) {
            const uint32_t cost = cost_map[tile];
            if (cost > running_best_cost) return;
            if (cost == running_best_cost && best_dir == cl) return;
            best_dir                        = dir;
            best_tile                       = tile;
            running_best_cost               = cost;
            own.move_path_trace_best_dir()  = dir;
            own.move_path_trace_best_tile() = tile;
        };
        const uint32_t north = neighbor(base_col, base_row - 1);
        const uint32_t south = neighbor(base_col, base_row + 1);
        const uint32_t west  = neighbor(base_col - 1, base_row);
        const uint32_t east  = neighbor(base_col + 1, base_row);
        consider(north, 0xd);
        consider(south, 0x1);
        consider(west, 0x7);
        consider(east, 0x13);

        // Diagonal candidates: DIAG_CAND is written whenever the cost check alone qualifies, BEFORE
        // the corner-passability check -- even on a candidate the corner check goes on to reject.
        // Preserved literally (it is a declared shadow region).
        auto consider_diag = [&](uint32_t tile, uint8_t dir, uint32_t corner_a, uint32_t corner_b) {
            const uint32_t cost = cost_map[tile];
            if (cost > running_best_cost) return;
            if (cost == running_best_cost && best_dir == cl) return;
            own.move_path_trace_diag_cand() = tile;
            if (v.passable[corner_a] == 0) return;
            if (v.passable[corner_b] == 0) return;
            best_dir                        = dir;
            best_tile                       = tile;
            running_best_cost               = cost;
            own.move_path_trace_best_dir()  = dir;
            own.move_path_trace_best_tile() = tile;
        };
        consider_diag(neighbor(base_col - 1, base_row - 1), 0xa, north, west);
        consider_diag(neighbor(base_col + 1, base_row - 1), 0x10, north, east);
        consider_diag(neighbor(base_col - 1, base_row + 1), 0x4, south, west);
        consider_diag(neighbor(base_col + 1, base_row + 1), 0x16, south, east);

        if (v.passable[best_tile] == 0) {
            // The stale-best_tile safety net (header banner) -- stop here, report the CURRENT tile.
            own.move_flood_result_col() = static_cast<uint8_t>(current >> 8);
            own.move_flood_result_row() = static_cast<uint8_t>(current & 0xFFu);
            break; // -> shared terminator write below
        }

        // RLE-append best_dir at rle_count (0x0041c76f-0x0041c7bb).
        {
            auto *entry = &own.planes().path_waypoint_at(0, slot_id, rle_count);
            if (entry->heading != best_dir && entry->heading != 0) {
                ++rle_count;
                own.move_path_rle_count() = rle_count;
                entry                     = &own.planes().path_waypoint_at(0, slot_id, rle_count);
                entry->run_length         = 0;
            }
            entry->heading = best_dir;
            ++entry->run_length;
            if (entry->run_length == 0) {
                // run_length wrapped 0xff -> 0x00: restore the old entry to its max, split into a
                // fresh one. The fresh entry's run_length is NOT zeroed first -- incremented from
                // whatever byte was already there, exactly as the .asm does.
                --entry->run_length;
                ++rle_count;
                own.move_path_rle_count() = rle_count;
                entry                     = &own.planes().path_waypoint_at(0, slot_id, rle_count);
                entry->heading            = best_dir;
                ++entry->run_length;
            }
        }

        if (rle_count > 0x129u || best_tile == start_packed) {
            break; // -> shared terminator write below
        }
        current = best_tile;
    }

    // ---- 6: terminator (0x0041c7d7-0x0041c7e5) ----
    own.planes().path_waypoint_at(0, slot_id, rle_count + 1) = {0, 0};
}

} // namespace detail

void move_path_build() {
    tact_state st = state();
    detail::move_path_build(st.read, st.own);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
