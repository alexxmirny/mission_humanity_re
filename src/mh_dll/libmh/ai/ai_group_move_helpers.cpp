//
// ai/ai_group_move_helpers.cpp -- see ai_group_move_helpers.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_pick_owned_tile_or_home_004e7138.asm and
// tmp/decomp/llm_strat_ai_unit_is_order_pending_004d41ff.asm), not from Ghidra's .c for the first
// (whose only annotation is "WARNING: Inlined function: llm_watcom_epilogue_004e419e" -- a Watcom
// shared-epilogue tail JMP, not a real call, same class as notify_map_changed's tail below) and not
// from Ghidra's .c for the second (whose CONCAT/shift rendering of the bit-0x80 test was re-derived
// against addr/mh_structs.gen.h's offsetof asserts rather than trusted -- see the header comment).
//
#include "ai/ai_group_move_helpers.h"


namespace mh::ai {
namespace detail {

void pick_owned_tile_or_home(const ai_view &v, const ai_calls &gc, int32_t player, uint32_t *out_x,
                             uint32_t *out_y) {
    const uint32_t width  = (uint32_t)*v.map_width;
    const uint32_t height = (uint32_t)*v.map_height;
    const uint8_t *grid   = v.players[player].ai_tile_flags_grid;

    uint32_t owned = 0;
    for (uint32_t x = 0; x < width; ++x)
        for (uint32_t y = 0; y < height; ++y)
            if (grid[x * 256u + y] == 1) ++owned;

    int32_t rank = gc.rand_below_ai(owned);

    for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t y = 0; y < height; ++y) {
            if (grid[x * 256u + y] != 1) continue;
            if (rank == 0) {
                *out_x = x;
                *out_y = y;
                return;
            }
            --rank;
        }
    }

    // No owned tile (or the countdown ran past the last one, which cannot happen for a correct
    // `owned` count but is reproduced as a fallthrough exactly like the original's own loop shape):
    // fall back to the player's home tile.
    *out_x = (uint32_t)v.players[player].ai_home_tile_x;
    *out_y = (uint32_t)v.players[player].ai_home_tile_y;
}

uint32_t unit_is_order_pending(const ai_view &v, uint32_t player, uint32_t unit_id) {
    const unit &u = unit_of(v, player, (int32_t)unit_id);
    return (uint32_t)((uint32_t)(u.order_status_flags & 0x80u) << 8);
}

} // namespace detail

void pick_owned_tile_or_home(int32_t player, uint32_t *out_x, uint32_t *out_y) {
    const ai_state st = state();
    detail::pick_owned_tile_or_home(st.read, live_calls(), player, out_x,
                                    out_y);
}

uint32_t unit_is_order_pending(uint32_t player, uint32_t unit_id) {
    const ai_state st = state();
    return detail::unit_is_order_pending(st.read, player, unit_id);
}

// ---- the differential-oracle arms ---------------------------------------------------------------
//
// pick_owned_tile_or_home's only callee is rand_below_ai, which draws _G_LLM_STRAT_RNG_STATE channel
// 2 and writes nothing else -- REAL, and every site reaching this function must declare that region
// (same rule as random_point_near, ai_group_task_workers.cpp). Writes only through its two
// out-pointers otherwise. unit_is_order_pending is PURE (the write-closure derivation: 1 reachable
// function, no writes, no callees).

} // namespace mh::ai
