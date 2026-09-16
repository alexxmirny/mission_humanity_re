//
// tact/tact_unit_destroy.h -- TACT1C: remove a unit/building from the tactical roster: drop its
// vision, clear its `.building` stamp from every plausible footprint tile, decrement the active
// count, mark its own tile passable-default, and refresh the three roster/HUD presentations.
//
//   llm_tact_unit_destroy @0x00431d8d (0x2bb)
//   void __watcall llm_tact_unit_destroy(uint unit_idx)
//
// SHAPE, transcribed literally from @0x00431da8-0x00432047:
//   1. @0x00431da8-0x00431dab: unit_vision_remove(unit_idx).
//   2. @0x00431db0-0x00431dbe: reads units[unit_idx].type into a local -- NEVER READ AGAIN anywhere
//      in the function (verified: no later instruction references that stack slot). A genuinely
//      dead load, same posture as tact_unit_death_tick.h's dead CMP; not reproduced.
//   3. @0x00431de3-0x00431dea: status = 0.
//   4. @0x00431df1-0x00432006: clear this unit's id from the `tile_objects[].building` field
//      (the SAME field llm_tact_unit_despawn's review caught a sibling function reaching via the
//      wrong offset -- +2, not `.unit[0..1]` at +4) at up to 9 candidate tiles around (col,row),
//      TERMINATING EARLY the moment the FIRST one -- (col,row) itself -- matches:
//        (col,   row  )                                              -- @0x00431df1, EARLY EXIT on match
//        (col,   row  )  -- REDUNDANT re-check of the tile block A just found NOT matching (memory
//                            unchanged in between, so this can never itself match) -- preserved
//                            literally, not folded away                            -- @0x00431e27
//        (col+1, row  )                                                            -- @0x00431e58
//        (col,   row+1)  -- FIXED 2026-08-26 (reimpl-verify wf_c3236abe-45f, all 3 lens groups
//                            independently caught this): the EAX operand at @0x00431e8b carries NO
//                            increment -- only the true +1-col candidates (@0x00431e58, @0x00431ebf)
//                            do `INC EAX` before the shift. The block's own immediate (0xd1ec8a =
//                            0xd1ec82+8) is a ROW-stride step (stride 8, vs. col's stride 0x800), so
//                            this is (col, row+1). A prior translation collapsed it into the next
//                            candidate below, silently dropping this tile from the search entirely.
//                                                                                    -- @0x00431e8b
//        (col+1, row+1)                                                            -- @0x00431ebc
//        (col-1, row-1)  IF col>0 AND row>0                                        -- @0x00431eef/efd
//        (col-1, row  )  IF col>0                                                  -- @0x00431f30
//        (col-1, row+1)  IF col>0 (same guard as the row above)                    -- @0x00431f69
//        (col,   row-1)  IF row>0                                                  -- @0x00431f9c
//        (col+1, row-1)  IF row>0 (same guard as the row above)                    -- @0x00431fd3
//      Every check past the first is UNCONDITIONAL on the earlier ones' outcome (no further early
//      exits) -- more than one tile can be cleared.
//   5. @0x00432006-0x00432023: units[unit_idx].type = 0 (marks the roster slot empty); decrement
//      _G_LLM_TACT_UNIT_ACTIVE_COUNT; passable_at(col,row) = PASSABLE_DEFAULT (2).
//   6. @0x0043202a-0x00432039: selection_panel_refresh(); active_unit_count_hud_draw();
//      ui_draw_player_row_list(-1).
//
// PROOF: OFFLINE. the measured write closure of llm_tact_unit_destroy -> 29 functions reachable,
// 55 regions written -- the presentation cascade (llm_tact_ui_draw_player_row_list's own FOV/UI
// scratch, font-glyph globals reached through it) is structurally expensive to rig-arm for no
// verification gain, the SAME posture as llm_tact_unit_despawn's manifest entry (which reaches this
// same cascade through a different door). REVIEW REQUIRED (writes shared `passable` and
// `tile_objects`). Proof is an offline oracle over the 9-tile clear search (both guard combinations,
// the early-exit vs fall-through distinction, and the deliberately-redundant second check), the
// final teardown fields, and the four outward calls -- all mocked via the calls struct.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

struct unit_destroy_calls {
    void (*unit_vision_remove)(int32_t unit_idx);          // llm_tact_unit_vision_remove @0x0042e53f
    void (*selection_panel_refresh)();                     // llm_tact_selection_panel_refresh @0x00434af7
    void (*active_unit_count_hud_draw)();                  // llm_tact_active_unit_count_hud_draw @0x00434dc2
    void (*ui_draw_player_row_list)(int32_t selected_row); // llm_tact_ui_draw_player_row_list @0x004354d3
};

const unit_destroy_calls &live_unit_destroy_calls();

namespace detail {

// llm_tact_unit_destroy @0x00431d8d.
void unit_destroy(tact_store &own, const unit_destroy_calls &c, uint32_t unit_idx);

} // namespace detail

void unit_destroy(uint32_t unit_idx);


} // namespace mh::tact
