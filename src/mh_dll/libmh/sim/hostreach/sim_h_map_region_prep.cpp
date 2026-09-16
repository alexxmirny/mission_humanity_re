//
// sim/hostreach/sim_h_map_region_prep.cpp -- see sim_h_map_region_prep.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_map_compute_obstacle_proximity_flags_0042207b.asm,
// tmp/decomp_sim/llm_map_init_region_route_step_deltas_00423299.asm,
// tmp/decomp_sim/llm_map_compute_region_merge_threshold_0042308b.asm), NOT the Ghidra .c drafts --
// llm_map_init_region_route_step_deltas' own .c renders its byte stores as
// `_G_LLM_MAP_REGION_ROUTE_STEP_DELTAS._1_3_ = SUB43(..., 1) & 0xffff00`, a decompiler artifact of a
// plain run of `MOV byte ptr [addr], imm8` (per the batch context's own warning); the other two
// drafts were cross-checked instruction-by-instruction and agree with the readings in the header
// banner.
//
#include "sim/hostreach/sim_h_map_region_prep.h"

namespace mh::sim {

namespace detail {

// ---- llm_map_compute_obstacle_proximity_flags @0x0042207b -------------------------------------------
void compute_obstacle_proximity_flags(const sim_view &v, sim_store &own) {
    // 0x0042209a-0x004220f3: seed GRID[x][y].terrain_flags from the passable plane. x is the
    // WIDTH-bounded index (inner loop / GRID address term x<<11 / passable term x<<8), y the
    // HEIGHT-bounded index (outer loop / GRID address term y<<3 / passable term +y) -- see the header
    // banner's address-arithmetic derivation.
    for (int32_t y = 0; y < *v.map_height; ++y) {
        for (int32_t x = 0; x < *v.map_width; ++x) {
            own.region_cell_at(x, y).terrain_flags = static_cast<uint32_t>(v.passable[(x << 8) | y]);
        }
    }

    // 0x004220fa-0x004222c5: for every OBSTACLE cell (terrain_flags==0) bordering at least one OTHER
    // obstacle cell, stamp the 13x13 proximity stencil. An isolated single-tile obstacle (all four
    // torus-wrapped orthogonal neighbours passable) is skipped entirely.
    for (int32_t y = 0; y < *v.map_height; ++y) {
        for (int32_t x = 0; x < *v.map_width; ++x) {
            if (own.region_cell_at(x, y).terrain_flags != 0) continue; // 0x00422146 JNZ: passable, skip

            // 0x0042214c-0x004221ca: short-circuit OR over west/east/north/south, torus-wrapped via
            // width_m/height_m -- in that exact order, matching the asm's JZ/JNZ chain (each neighbour
            // is only read if the previous one did NOT already prove "has an obstacle neighbour").
            const uint32_t west =
                own.region_cell_at(static_cast<int32_t>((static_cast<uint32_t>(x) - 1) & *v.width_m), y)
                    .terrain_flags;
            bool has_obstacle_neighbor = (west == 0);
            if (!has_obstacle_neighbor) {
                const uint32_t east =
                    own.region_cell_at(static_cast<int32_t>((static_cast<uint32_t>(x) + 1) & *v.width_m),
                                       y)
                        .terrain_flags;
                has_obstacle_neighbor = (east == 0);
            }
            if (!has_obstacle_neighbor) {
                const uint32_t north =
                    own.region_cell_at(x,
                                       static_cast<int32_t>((static_cast<uint32_t>(y) - 1) & *v.height_m))
                        .terrain_flags;
                has_obstacle_neighbor = (north == 0);
            }
            if (!has_obstacle_neighbor) {
                const uint32_t south =
                    own.region_cell_at(x,
                                       static_cast<int32_t>((static_cast<uint32_t>(y) + 1) & *v.height_m))
                        .terrain_flags;
                has_obstacle_neighbor = (south == 0);
            }
            if (!has_obstacle_neighbor) continue; // 0x004221ca JNZ: all four neighbours passable, skip

            // 0x004221d3-0x004221f0: the stencil's top-left corner, in BYTE-TRUNCATED arithmetic (both
            // the `-6` and the mask are 8-bit ops in the original -- see the header banner).
            const uint8_t bx =
                static_cast<uint8_t>(static_cast<uint8_t>(x) - 6) & static_cast<uint8_t>(*v.width_m);
            const uint8_t by =
                static_cast<uint8_t>(static_cast<uint8_t>(y) - 6) & static_cast<uint8_t>(*v.height_m);

            // 0x004221fa-0x004222bb: the 13x13 (0xa9=169) stencil sweep. i%13 is the column offset
            // (paired with bx/width_m, the FULL 32-bit mask -- NOT byte-truncated like the corner
            // above), i/13 the row offset (paired with by/height_m). Both IDIVs divide the literal 13.
            for (int32_t i = 0; i < 0xa9; ++i) {
                const int32_t sx = static_cast<int32_t>(
                    (static_cast<uint32_t>(bx) + static_cast<uint32_t>(i % 13)) & *v.width_m);
                const int32_t sy = static_cast<int32_t>(
                    (static_cast<uint32_t>(by) + static_cast<uint32_t>(i / 13)) & *v.height_m);

                llm_map_region_cell &target = own.region_cell_at(sx, sy);
                if (target.terrain_flags == 0) continue; // 0x00422259 JZ: not passable, skip

                // 0x004222a3 `MOVZX EAX, byte ptr [ECX*4 + 0x51deac]` -- v.proximity_stencil
                // (_G_LLM_MAP_PROXIMITY_STENCIL, 169 entries at a 4-byte stride, only .bit_index
                // read). The `& 0x1f` reproduces x86 SHL's implicit shift-count mask.
                const uint32_t bit = v.proximity_stencil[i].bit_index & 0x1fu; // SHL masks mod 32
                target.terrain_flags |= (1u << bit);
            }
        }
    }
}

// ---- llm_map_init_region_route_step_deltas @0x00423299 ----------------------------------------------
void init_region_route_step_deltas(sim_store &own) {
    uint8_t *deltas = own.region_route_step_deltas_mut(); // byte[32], 8 dirs * 4-byte stride

    // 0x004232b1-0x0042331a: 16 byte stores, one {+0,+1} pair per direction. +0 = row/y delta,
    // +1 = col/x delta -- see the header banner's SETTLED note (this function's own Ghidra plate has
    // the axes swapped).
    deltas[0 * 4 + 0] = 1;
    deltas[0 * 4 + 1] = 0; // 0x004232b8 / 0x004232b1: dir0 (row+1, col+0)
    deltas[1 * 4 + 0] = 1;
    deltas[1 * 4 + 1] = 0xff; // 0x004232c6 / 0x004232bf: dir1 (row+1, col-1)
    deltas[2 * 4 + 0] = 0;
    deltas[2 * 4 + 1] = 0xff; // 0x004232d4 / 0x004232cd: dir2 (row+0, col-1)
    deltas[3 * 4 + 0] = 0xff;
    deltas[3 * 4 + 1] = 0xff; // 0x004232e2 / 0x004232db: dir3 (row-1, col-1)
    deltas[4 * 4 + 0] = 0xff;
    deltas[4 * 4 + 1] = 0; // 0x004232f0 / 0x004232e9: dir4 (row-1, col+0)
    deltas[5 * 4 + 0] = 0xff;
    deltas[5 * 4 + 1] = 1; // 0x004232fe / 0x004232f7: dir5 (row-1, col+1)
    deltas[6 * 4 + 0] = 0;
    deltas[6 * 4 + 1] = 1; // 0x0042330c / 0x00423305: dir6 (row+0, col+1)
    deltas[7 * 4 + 0] = 1;
    deltas[7 * 4 + 1] = 1; // 0x0042331a / 0x00423313: dir7 (row+1, col+1)

    // 0x00423321-0x00423326: dword copy of entry 0 (deltas[0..3] -- [2]/[3] are whatever the
    // BSS-zero-initialised global already held, this function never writes them) into the 4-byte WRAP
    // sentinel immediately past the array (0x00708af4+32==0x00708b14 exactly) -- so a walker can read
    // [i]/[i+1] for any i in 0..7 without a bounds check.
    uint8_t *wrap = own.region_route_step_delta_wrap();
    wrap[0]       = deltas[0];
    wrap[1]       = deltas[1];
    wrap[2]       = deltas[2];
    wrap[3]       = deltas[3];
}

// ---- llm_map_compute_region_merge_threshold @0x0042308b ---------------------------------------------
void compute_region_merge_threshold(const sim_view &v, sim_store &own) {
    // 0x004230b1-0x004230de: walk the ENTIRE active region list (*v.region_list_head, via ->next),
    // summing cell_count and counting nodes.
    uint32_t sum   = 0; // [EBP-0x1c]
    uint32_t count = 0; // [EBP-0x20]
    for (llm_map_region *r = *v.region_list_head; r != nullptr; r = r->next) {
        sum += r->cell_count;
        ++count;
    }
    // 0x004230e0-0x004230e8: UNSIGNED DIV (not IDIV). PRESERVE-BUG: an empty list (count==0) is a
    // hardware divide-by-zero fault in the original -- see the header banner's reachability note.
    // Transcribed literally, no guard added (translator-brief rule 10).
    own.region_merge_threshold_mut() = sum / count;
}

} // namespace detail


void compute_obstacle_proximity_flags() {
    sim_state st = state();
    detail::compute_obstacle_proximity_flags(st.read, st.own);
}

void init_region_route_step_deltas() {
    sim_state st = state();
    detail::init_region_route_step_deltas(st.own);
}

void compute_region_merge_threshold() {
    sim_state st = state();
    detail::compute_region_merge_threshold(st.read, st.own);
}

} // namespace mh::sim
