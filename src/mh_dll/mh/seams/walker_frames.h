//
// seams/walker_frames.h -- the pure fill rule of the walker facing-frame table (render only).
//
// THE BUG. A walker unit (cfg TYPE R_aWALKER 11 / R_hWALKER 12: Centaur = N_alien_Walker1, N_alien_Walker2,
// N_human_Walker1, N_human_Walker2; all four sprite banks are 64 frames = 24 rest + 8 dirs x 5 stride) is drawn as
// `Unit.sprite + _G_LLM_STRAT_WALKER_FRAME_TABLE[facing * 32 + move_microstep]` (dwords; 0x00ae1df8, read by
// llm_strat_render_tile_object @0x00452681 and llm_strat_render_bldg_docked_unit @0x00451b8a, written only by
// llm_strat_walker_frame_blend_table_init @0x00460a23, once, from llm_strat_mode_init in boot stage 4). Facings are the
// sim's 1..24 (0 = "no heading"). The retail init fills only f = 1, 4, 7, ... 22 (`for f = 1; f < 25; f += 3`) via
// llm_strat_walker_frame_blend_index:
//     ms 0..4 and 31      -> f - 1                       (the facing's own rest image)
//     ms 5..30            -> 0x18 + 5 * ((f-1)/3) + min((ms-5)/5, 4)   (the 8 stride banks)
// The other 16 facings stay 0 (BSS), i.e. the f = 1 north rest image: a walker turning in place flashes north on two
// of every three notches. Nothing but the two renderers reads the table, so the sim is not involved.
//
// THE FILL. fill_unfilled() runs after the retail init and, for each unfilled facing f in 1..24:
//     rest microsteps (0..4, 31)  -> f - 1                  (its own rest frame; every bank has all 24)
//     stride microsteps           -> the donor facing's entry at the same microstep, the donor being the NEAREST filled
//                                    facing on the 24-cycle. Each unfilled facing is exactly 1 notch from one filled
//                                    one (f = 3k+2 -> 3k+1, f = 3k+3 -> 3k+4, and 24 wraps to 1), so there are no ties.
// Row 0 and everything past row 24 are left alone. The filled rows are never written.
//
#pragma once
#include <stdint.h>

namespace mh::seams::walker_frames {

inline constexpr int FACING_FIRST = 1; // the sim's facings are 1..24
inline constexpr int FACING_LAST  = 24;
inline constexpr int MICROSTEPS   = 32; // dwords per facing row (0x80 bytes)

// A facing the retail init fills (`for f = 1; f < 0x19; f += 3`).
constexpr bool retail_fills(int f) { return f >= 1 && f <= 22 && (f - 1) % 3 == 0; }

// ms values the retail blend_index maps to the facing's rest frame.
constexpr bool is_rest_ms(int ms) { return ms < 5 || ms == 31; }

// The nearest retail-filled facing for an unfilled one (see the header).
constexpr int donor_facing(int f) {
    const int r = (f - 1) % 3; // 1 or 2 for an unfilled facing
    if (r == 1) return f - 1;  // 3k+2 -> 3k+1
    const int d = f + 1;       // 3k+3 -> 3k+4, and 24 -> 25 wraps to 1
    return d > FACING_LAST ? FACING_FIRST : d;
}

// `table` = the 25+ rows of 32 dwords the game reads (row stride MICROSTEPS), already filled by the retail init.
// Returns the number of cells written (16 facings x 32 = 512 on a retail-filled table).
inline int fill_unfilled(uint32_t *table) {
    int n = 0;
    for (int f = FACING_FIRST; f <= FACING_LAST; ++f) {
        if (retail_fills(f)) continue;
        const uint32_t *donor = table + donor_facing(f) * MICROSTEPS;
        uint32_t       *row   = table + f * MICROSTEPS;
        for (int ms = 0; ms < MICROSTEPS; ++ms) {
            row[ms] = is_rest_ms(ms) ? static_cast<uint32_t>(f - 1) : donor[ms];
            ++n;
        }
    }
    return n;
}

// The retail table after the retail init has run, for the guard (and the selftest's reference): the spot values the install
// checks before it writes anything -- a wrong table means the init did not run or the exe is not the expected build.
inline bool looks_like_retail_init(const uint32_t *table) {
    return table[1 * MICROSTEPS + 0] == 0 && table[1 * MICROSTEPS + 5] == 0x18 && table[1 * MICROSTEPS + 25] == 0x1c &&
           table[4 * MICROSTEPS + 0] == 3 && table[4 * MICROSTEPS + 10] == 5 + 0x19 && table[22 * MICROSTEPS + 31] == 21 &&
           table[22 * MICROSTEPS + 15] == 35 + 0x1a && table[2 * MICROSTEPS + 0] == 0 && table[2 * MICROSTEPS + 12] == 0;
}

} // namespace mh::seams::walker_frames
