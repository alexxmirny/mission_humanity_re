//
// sim/libtrans/sim_lt_bldg_cell_grid.cpp -- see sim_lt_bldg_cell_grid.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_strat_bldg_recompute_cell_grid_004dc25f.asm), NOT from the
// Ghidra .c draft beside it -- the draft mis-bases four of the eight halo probes onto the adjacent
// spiral array (context_C.md G2; the header banner carries the full note).
//
#include "sim/libtrans/sim_lt_bldg_cell_grid.h"

#include <cstddef> // offsetof

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void bldg_recompute_cell_grid(const sim_view &v, sim_store &own) {
    // static_assert'd layout facts the byte arithmetic below rests on (mh_structs.gen.h):
    // stride 0x842 (IMUL @0x004dc2af), type @+8 (base literal 0xd9ec88 = Building[1]+8),
    // area @+0xb with row stride 10 (0xd9ec8b + dy*0xa + dx @0x004dc2d6..0x004dc2db).
    static_assert(sizeof(cfg_building) == 0x842, "cfg Building stride");
    static_assert(offsetof(cfg_building, type) == 8, "cfg Building .type offset");
    static_assert(offsetof(cfg_building, area) == 0xb, "cfg Building .area offset");

    // 0x004dc26e / 0x004dc534-0x004dc53a: b = 1; loop while b <= total, UNSIGNED INCLUSIVE (JBE).
    // Slot 0 of both outputs is never touched.
    for (uint32_t b = 1; b <= (uint32_t)v.cfg_building_sec->total; ++b) {
        const int32_t base = (int32_t)(b << 6); // b*64, the flat base of grid[b] (SHL ESI,0x6)

        // Phase 1 (0x004dc278..0x004dc29e): clear grid[b][0..7][0..7].
        for (int32_t dy = 0; dy < 8; ++dy)
            for (int32_t dx = 0; dx < 8; ++dx)
                own.bldg_cell_grid_byte(base + dy * 8 + dx) = 0;

        // Phase 2 (0x004dc2a0..0x004dc2c6): row_shift[b] = 0, then -1 for a type-0x1c building.
        const cfg_building &cb                      = v.cfg_buildings[b];
        own.bldg_cell_grid_row_shift_at((int32_t)b) = 0;
        if (cb.type == 0x1c) own.bldg_cell_grid_row_shift_at((int32_t)b) = -1;

        // Phase 3 (0x004dc2c8..0x004dc30e): stamp the 5x5 cfg footprint at (+1-row_shift, +1).
        // The store base literal is 0xfb34b1 = grid+1 (the +1 column), and the row term is
        // (dy + 1 - row_shift[b]) << 3 -- ESI holds dy+1 (LEA ESI,[EDX+0x1] @0x004dc301) when the
        // subtraction at 0x004dc2e7 runs. row_shift is re-READ from the array each stamp, matching
        // the memory operand at 0x004dc2e7 (harmless -- nothing writes it inside the loop).
        for (int32_t dy = 0; dy < 5; ++dy)
            for (int32_t dx = 0; dx < 5; ++dx)
                if (cb.area[dy][dx] != 0) {
                    const int32_t row                                   = dy + 1 - own.bldg_cell_grid_row_shift_at((int32_t)b);
                    own.bldg_cell_grid_byte((row << 3) + base + dx + 1) = 1;
                }

        // Phase 4 (0x004dc310..0x004dc43e): halo -- an empty cell adjacent to a 1 becomes 2.
        // The eight probes are FLAT byte deltas off the running index, each behind its own edge
        // guards (asm order -9, -1, +7, -8, +8, -7, +1, +9); the guards make flat == 2D exactly.
        for (int32_t dy = 0; dy < 8; ++dy)
            for (int32_t dx = 0; dx < 8; ++dx) {
                const int32_t cell = base + dy * 8 + dx;
                if (own.bldg_cell_grid_byte(cell) != 0) continue; // 0x004dc32c JNZ
                const bool hit =
                    (dy > 0 && dx > 0 && own.bldg_cell_grid_byte(cell - 9) == 1) || // 0x004dc341
                    (dx > 0 && own.bldg_cell_grid_byte(cell - 1) == 1) ||           // 0x004dc360
                    (dy < 7 && dx > 0 && own.bldg_cell_grid_byte(cell + 7) == 1) || // 0x004dc384
                    (dy > 0 && own.bldg_cell_grid_byte(cell - 8) == 1) ||           // 0x004dc3a3
                    (dy < 7 && own.bldg_cell_grid_byte(cell + 8) == 1) ||           // 0x004dc3c3
                    (dy > 0 && dx < 7 && own.bldg_cell_grid_byte(cell - 7) == 1) || // 0x004dc3e3
                    (dx < 7 && own.bldg_cell_grid_byte(cell + 1) == 1) ||           // 0x004dc3ff
                    (dy < 7 && dx < 7 && own.bldg_cell_grid_byte(cell + 9) == 1);   // 0x004dc420
                if (hit) own.bldg_cell_grid_byte(cell) = 2;                         // 0x004dc429
            }

        // Phase 5/6 (0x004dc444..0x004dc4c5): types 7, 0x1b and 8 stamp the SE quadrant
        // (rows 4..7 x cols 4..7) = 1. Two separate compare chains in the original -- 7/0x1b share
        // one loop, 8 gets its own byte-identical copy; folded here since the stamps are identical.
        if (cb.type == 7 || cb.type == 0x1b || cb.type == 8)
            for (int32_t dy = 4; dy < 8; ++dy)
                for (int32_t dx = 4; dx < 8; ++dx)
                    own.bldg_cell_grid_byte((dy << 3) + base + dx) = 1;

        // Phase 7 (0x004dc4c7..0x004dc4ff): type 0x1c stamps the NE quadrant (rows 0..3 x
        // cols 4..7) = 1.
        if (cb.type == 0x1c)
            for (int32_t dy = 0; dy < 4; ++dy)
                for (int32_t dx = 4; dx < 8; ++dx)
                    own.bldg_cell_grid_byte((dy << 3) + base + dx) = 1;

        // Phase 8 (0x004dc501..0x004dc531): normalize -- every nonzero cell becomes 1 (the halo's
        // transient 2s collapse; 0/1 is all a reader could ever see).
        for (int32_t dy = 0; dy < 8; ++dy)
            for (int32_t dx = 0; dx < 8; ++dx) {
                const int32_t cell = base + dy * 8 + dx;
                if (own.bldg_cell_grid_byte(cell) != 0) own.bldg_cell_grid_byte(cell) = 1;
            }
    }
}

} // namespace detail

// ---- the public wrapper ---------------------------------------------------------------------------

void bldg_recompute_cell_grid() {
    sim_state st = state();
    detail::bldg_recompute_cell_grid(st.read, st.own);
}


} // namespace mh::sim
