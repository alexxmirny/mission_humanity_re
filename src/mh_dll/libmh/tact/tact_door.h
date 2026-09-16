//
// tact/tact_door.h -- TACT1D: the tactical mission's DOOR subsystem. Six original functions sharing
// ONE 16-slot table, door_record (tact_state.h: door_table_at(index), mh_llm_tact_door / stride
// 0x68 / RID_TACT_DOOR_TABLE):
//
//   llm_tact_door_tick                @0x004322c1 (0x1ff) -- the per-frame animation driver
//   llm_tact_door_anim_start          @0x00432266 (0x5b)  -- external "start opening" trigger
//   llm_tact_door_update_tile_state   @0x004324c0 (0x545) -- stamps tile_objects/TILE_VIS_MAP/the
//                                                             height-sprite cache for the CURRENT
//                                                             animation frame
//   llm_tact_door_path_clear          @0x00432a05 (0x19c) -- "is a unit still standing on the
//                                                             footprint" gate for finishing a close
//   llm_tact_door_apply_to_map        @0x00439cf5 (0x2b3) -- the mission-LOAD-time equivalent of
//                                                             update_tile_state (always marks, using
//                                                             row 0 of each leaf's row data)
//   llm_tact_door_parse_definition    @0x004398eb (0x2f4) -- the mission-file DOOR-line parser
//
// (*) llm_tact_door_tick and llm_tact_door_apply_to_map both loop door slots 1..0x1f INCLUSIVE
// (`for (i=1; i<0x20; ++i)`, 31 slots touched) -- DOUBLE _G_LLM_TACT_DOOR_TABLE's Ghidra-declared
// length of 16 (`llm_tact_door[16]`, docs/symbols.md / RID_TACT_DOOR_TABLE's registered 1664-byte
// extent = 16*0x68). Preserved LITERALLY here (a "sensible" bounds check is frequently
// the WRONG reimplementation for this codebase, and slot 0 being skipped -- loop starts at 1 -- is
// exactly this kind of preserved off-by-one) rather than clamped to 16. See the translation report's
// declared_needs/uncertainties for what this means for the region's real size and for arming safety.
//
// THE -6/-2 BYTE-BIAS WRITES (door_update_tile_state / door_apply_to_map / door_path_clear). All
// three read or write the tile ADJACENT to (col,row) in the flat (col<<8|row) layout via a raw
// literal-displacement trick identical in shape to the one door_path_clear's own Ghidra plates
// already document (0x00432a86/0x00432b30: "tile_objects_base - 6 ... lands ... two bytes into the
// PRECEDING element, which is map_tile_object_data.building"). The two WRITE sites
// (door_update_tile_state/door_apply_to_map) use the SAME preceding-element trick but on
// `.class_owner` (offset +6, bias -2 = -8+6): every write to `.class_owner` at (col,row) is paired
// with an IDENTICAL write to `.class_owner` of the flat-index-minus-one record. This is NOT the
// `.unit[0]`/+4 pair a prior read of the task brief predicted -- the actual bytes (identical in both
// functions, confirmed independently) support only `.class_owner` at two adjacent flat indices; see
// uncertainties[]. Reproduced via raw pointer arithmetic (door_prev_tile_class_owner_at /
// door_prev_tile_building_at below), not `tile_object_at(col, row-1)`, because tact's indexing is
// `(col<<8)|row` (a bitwise OR relied on by mode_planes::tile_object_at), which would diverge from
// the original's true flat-index SUBTRACTION at row==0 (wraps to (col-1,255) either way, but OR with
// a negative row does not compute that).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The outward callees door_tick/door_anim_start/door_parse_definition make, behind function
// pointers so `net_selftest tacttest` can substitute recorders (same shape as mission_parse_calls --
// one struct shared by every original function in this TU, each using only the members it needs).
// door_update_tile_state, door_path_clear and door_apply_to_map make NO outward calls beyond the
// inert `assert_stack_capacity` prologue (brief Sect. 2.6), so they take no `door_calls` parameter.
struct door_calls {
    double (*time_now)(); // time_GetCurrentTime @0x00427616

    void (*llm_fatal_cleanup)();         // @0x0042613b
    void (*utils_abort)(int32_t status); // @0x004da944
    // mh::tact::mission_parse_int_token @0x00439bdf -- an already-translated SIBLING in a DIFFERENT
    // TU (tact_mission_parse.cpp), bound through this struct for the same reason unit_teleport_calls
    // binds facing_to_delta: it is not a frontier callee, but going through the struct keeps this
    // function offline-testable without dragging in tact_mission_parse's own live bindings.
    int32_t (*mission_parse_int_token)(char *line, uint32_t *io_pos, int32_t *out_value);
};

const door_calls &live_door_calls();

namespace detail {

// llm_tact_door_tick @0x004322c1. Walks door slots 1..0x1f (see the header comment on the 16-vs-31
// mismatch); a slot with `.id==0` or `.state==0` is skipped entirely. Drives the 4-state machine
// (0 closed -> 1 opening -> 2 open/holding -> 3 closing -> 0) against `time_now()`, calling
// door_update_tile_state after every frame-index change and door_path_clear to gate whether a
// closing door may finish. The state==2 -> 3 transition (hold-time expired) falls straight into the
// state==3 body IN THE SAME TICK (@0x004323fc is reached unconditionally after the state==2 check,
// whether or not that check fired) -- transcribed as a fallthrough, not two separate ticks.
void door_tick(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
               const door_calls &c);

// llm_tact_door_anim_start @0x00432266. External "close is idle -> start opening" trigger: only
// fires when both `.frame_index==0` and `.state==0`.
void door_anim_start(tact_store &own, const door_calls &c, int32_t door_idx);

// llm_tact_door_update_tile_state @0x004324c0. For each leaf (RIGHT then LEFT, walking (col,row)
// from (tile_x,tile_y) and stepping it by `.direct_mode` after every column -- the SAME cursor is
// shared across both leaves, not reset in between), reads `rows[(frame_index-1)*col_count + c]`:
// zero clears this door's occupancy stamp at the walked tile (+ redraws a short run of tiles above
// it on screen and zeroes their cached height-sprite slot); nonzero marks the stamp with the door's
// own 1-based slot index and, for the same short run, chains a sprite-bank frame lookup into the
// height-sprite cache instead of writing zero.
void door_update_tile_state(tact_store &own, mh::state::mode_planes &planes, const tact_view &view,
                            int32_t door_idx);

// llm_tact_door_path_clear @0x00432a05. ZERO writes -- reads `.building` (offset +2) at the walked
// tile AND at the flat-index-minus-one tile (the SAME -6-byte-bias trick as the two writers, applied
// to a READ), for both leaves in sequence (the (col,row) walk cursor carries over from right to
// left, exactly as in door_update_tile_state/door_apply_to_map). Returns 0 (blocked: something is
// standing on the footprint) the instant either check anywhere in either leaf sees a nonzero
// `.building`; returns 1 only if the whole walk (both leaves) sees no occupant.
int32_t door_path_clear(tact_store &own, const tact_view &view, int32_t door_idx);

// llm_tact_door_apply_to_map @0x00439cf5. The mission-LOAD-time counterpart of
// door_update_tile_state: loops ALL door slots 1..0x1f, skips any slot whose own `.id` does not
// equal the slot index (the self-consistency check @0x00439d2b-0x00439d35), then for each leaf
// ALWAYS marks (no clear branch -- unlike door_update_tile_state, row_value==0 does not skip the
// mark) using `rows[c]` (row 0 of each leaf's data, not frame_index-selected).
void door_apply_to_map(tact_store &own, mh::state::mode_planes &planes, const tact_view &view);

// llm_tact_door_parse_definition @0x004398eb. Mission-file DOOR-line parser (door_def_line,
// door_index): after skipping spaces from position 1, requires the literal keyword "RIGHT" (5
// chars) or "LEFT" (4 chars) at the cursor (fatal_cleanup+abort otherwise), then repeatedly calls
// mission_parse_int_token to fill `right_col_count`/`right_rows[]` or `left_col_count`/`left_rows[]`
// (the FIRST token written goes to col_count, every token after that to rows[] -- a single write
// keyed only by a running slot counter, since col_count sits immediately before rows[] in the
// struct; see door_parse_write_slot's own comment). A '-' delimiter stashes the just-parsed value as
// a pending range start; the NEXT token's value closes an inclusive fill [start, value]. Finishes by
// computing frame_count = (rows written) / col_count, fatal-aborting on a nonzero remainder.
void door_parse_definition(tact_store &own, const door_calls &c, char *door_def_line,
                           int32_t door_index);

} // namespace detail

void    door_tick();
void    door_anim_start(int32_t door_idx);
void    door_update_tile_state(int32_t door_idx);
int32_t door_path_clear(int32_t door_idx);
void    door_apply_to_map();
void    door_parse_definition(char *door_def_line, int32_t door_index);

// Declared here per the module convention; DEFINED in tact_door.cpp, DECLARED in tact_state.h and
// CALLED from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
