//
// ai/ai_map_influence.cpp -- see ai_map_influence.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_recompute_map_influence_004d7a2e.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE IN THAT LISTING WAS RE-DERIVED against addr/mh_structs.gen.h and
// addr/mh_regions.gen.h rather than transcribed. The body computes `player * 0x288fc` three times
// over (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD = 166140 = 0x288fc, the committed
// player_data stride) and reaches exactly two things through it:
//   0xe6def4 = player_data + 0x34 = ai_map_changed_pending  (MOV dword ..,0x0 @0x004d7a51)
//   0xe6defc = player_data + 0x3c = ai_tile_flags_grid      (ADD EAX,0xe6ded0 / +0x2c @0x004d7a6b)
// with player_data's base 0xe6dec0 fixed independently as ai_enabled's absolute 0xe6ded8 minus its
// committed offset 0x18. The two pushed globals are 0x00825084 = `width` and 0x00825064 = `height`,
// which are RID_WIDTH and RID_HEIGHT in mh_regions.gen.h -- so they come in through v.map_width /
// v.map_height and no literal VA appears below (Law 1).
//
// THE 28-CALL SEQUENCE, derived site by site rather than counted off the old plate (which said 24 --
// it added up the three loops and missed the three unlooped calls; corrected in Ghidra,
// finding 2026-08-02-0810-5):
//
//   0x004d7a74  fill (threshold 3, fill 0)                                          1
//   0x004d7a8d  flood (source 4, fill 3)                                            1
//   0x004d7acb  flood (3, 3)   MOV EDX,1 @0x004d7a95 / INC / CMP EDX,2  @0x004d7ad4  1
//   0x004d7aea  flood (3, 2)                                                        1
//   0x004d7b28  flood (2, 2)   MOV EDX,1 @0x004d7af2 / INC / CMP EDX,0xf @0x004d7b31 14
//   0x004d7b47  flood (2, 1)                                                        1
//   0x004d7b83  flood (1, 1)   MOV EDX,1 @0x004d7b4f / INC / CMP EDX,0xa @0x004d7b8c  9
//                                                                    = 1 fill + 27 flood
//
// It is a distance/influence field: whatever seeded the grid at level 4 spreads outward as 3, then 2,
// then 1, with the number of wavefront steps setting each band's reach (1, 14, 9). The one-iteration
// loop at 0x004d7a9a is the original's shape, kept rather than folded into a straight call so the
// count above stays auditable against the listing.
//
// WHY THE CALLEES ARE NOT REIMPLEMENTED HERE. llm_strat_ai_grid_fill_below_threshold @0x004b49da and
// llm_strat_ai_grid_flood_step @0x004b4a10 are members of the AI manifest at antichain LAYER 2, i.e.
// a later slice; until then they stay original and are called through mh_calls (Law 4). Neither had
// a committed prototype previously -- mh_calls.gen.h refused both -- which is what blocked
// this slice; both were committed here (findings 2026-08-02-0810-1 and -2) with the width/height
// order derived three ways, because their own plates and the two already-committed sibling grid
// helpers all carry the axes SWAPPED.
//
// THE WRITE SET, and it is bigger than the matrix row. The matrix attributes one region,
// player_data, for the single dword this body stores itself. The 28 calls write two more things
// that no address-literal sweep can attribute to this function: the 64 KB threat grid (through the
// `grid` POINTER -- still inside player_data, so already covered) and _G_LLM_STRAT_AI_GRID_WRAP_MASK
// (llm_strat_ai_grid_flood_step stores the two extent bytes into it on entry, @0x004b4a23 and
// @0x004b4a2c). The second is declared BY HAND in dll_shadow_manifest.json for the same reason the
// layer-2 grid helpers declare theirs -- an empty matrix row is not a proof that
// nothing is written.
//
#include "ai/ai_map_influence.h"


namespace mh::ai {
namespace detail {

namespace {
// The six (source_level, fill_value) shapes, in the order the original emits them, and the iteration
// count of each. Kept as data so the sequence reads the way the listing does; the loop below is
// still the original's structure, not a table-driven rewrite of it.
constexpr int32_t FILL_THRESHOLD = 3;
constexpr int32_t FILL_VALUE     = 0;
constexpr int32_t FLOOD_MARK_BIT = 0x20; // documented here only because it explains the 3->3 shapes
} // namespace

influence_report recompute_map_influence(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         int32_t player) {
    static_cast<void>(FLOOD_MARK_BIT); // documentation constant; the mark lives in the callee

    influence_report rep{};
    player_data     &wp = own.players[player];

    rep.was_pending = wp.ai_map_changed_pending != 0;

    // 0x004d7a51. The trigger is consumed FIRST, before any grid work and unconditionally -- this
    // body never tests it, the CALLER does (llm_strat_ai_players_tick @0x004db94a). So calling it
    // with the flag already clear is legal and does the identical 28-call pass; `was_pending` is
    // recorded rather than acted on.
    wp.ai_map_changed_pending = 0;

    uint8_t *grid = wp.ai_tile_flags_grid;

    // THE EXTENTS ARE RE-READ FOR EVERY CALL. The original pushes `[0x00825064]` and `[0x00825084]`
    // afresh at each of the 28 sites (e.g. 0x004d7a5f/65, then again at 0x004d7a9e/aa4, 0x004d7afb/
    // b01, 0x004d7b58/b5e) instead of holding them in a register across the pass. Nothing in the
    // pass writes them, so hoisting would be equivalent today -- these stay dereferenced per call
    // because that is what the listing does, and because a hoist would silently change behaviour if
    // a future callee ever touched the globals.
    auto fill = [&](int32_t threshold, int32_t fill_value) {
        gc.grid_fill_below_threshold(grid, *v.map_width, *v.map_height, threshold, fill_value);
        ++rep.fill_calls;
    };
    auto flood = [&](int32_t source_level, int32_t fill_value) {
        gc.grid_flood_step(grid, *v.map_width, *v.map_height, source_level, fill_value);
        ++rep.flood_calls;
    };

    fill(FILL_THRESHOLD, FILL_VALUE); // 0x004d7a74 -- everything at or below 3 goes back to 0
    flood(4, 3);                      // 0x004d7a8d

    // 0x004d7a95-0x004d7ad7. `MOV EDX,1` then `INC EDX / CMP EDX,2 / JL`: exactly one iteration.
    for (int32_t i = 1; i < 2; ++i) flood(3, 3); // 0x004d7acb

    flood(3, 2); // 0x004d7aea

    // 0x004d7af2-0x004d7b34. Fourteen iterations (i = 1..14).
    for (int32_t i = 1; i < 0xf; ++i) flood(2, 2); // 0x004d7b28

    flood(2, 1); // 0x004d7b47

    // 0x004d7b4f-0x004d7b8f. Nine iterations (i = 1..9).
    for (int32_t i = 1; i < 0xa; ++i) flood(1, 1); // 0x004d7b83

    rep.width  = *v.map_width;
    rep.height = *v.map_height;
    return rep;
}

} // namespace detail

void recompute_map_influence(int32_t player) {
    const ai_state st = state();
    (void)detail::recompute_map_influence(st.read, st.own, live_calls(), player);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Both callees run FOR REAL. Their entire write-set is declarable and declared: the grid they write
// through the pointer argument lives inside player_data, and llm_strat_ai_grid_flood_step's only
// other store is _G_LLM_STRAT_AI_GRID_WRAP_MASK, which this site declares via extra_regions. Stubbing
// either would not make the run safer -- it would GUARANTEE a divergence, because the original arm
// would have transformed 64 KB of grid and ours would not.
//
// WHAT THE VACUITY MODE IS HERE, and it is unlike the other batch-B sites. There is no branch in the
// body, so a call cannot "do nothing" through a gate -- every call emits all 28. What a call CAN be
// is a pass over a grid with nothing to propagate: the fill zeroes everything <= 3, so if no cell
// holds a value >= 4 on entry there are no wavefront sources and all 27 flood steps write nothing at
// all. That is a fully clean, fully vacuous call. `seeds` below is the one number that separates the
// two, so read it before the divergence count.

} // namespace mh::ai
