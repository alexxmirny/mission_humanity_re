//
// tact/tact_move_path_build.h -- TACT1A/TACT1B: the flood-fill path solver.
//
//   llm_tact_move_path_build @0x0041c2ac (0x53b), `void __mh_watcall_ecx_ebx_volatile
//   llm_tact_move_path_build(void)` -- all state via globals, ZERO parameters (committed prototype
//   re-verified against the disassembly, not just the body, per the tracker's own note that this is
//   REVIEW-flagged / conf:med).
//
// Reads a (start_col,start_row)/(goal_col,goal_row) request the caller has already staged into
// move_flood_start_*/move_flood_goal_* (tact_store's write path; llm_tact_move_step_attempt is the
// migration's only writer so far), and on return leaves EXACTLY ONE of two outcomes in
// move_flood_result_*/move_path_slot_id:
//
//   (a) NO PATH ATTEMPTED -- the goal tile has no passable orthogonal neighbor at all (all four of
//       N/S/E/W sum to zero). result = the GOAL tile itself, slot_id = -1. No path buffer is
//       touched, no free-slot scan happens.
//   (b) A PATH WAS SOLVED (or a best-effort partial one, see below) -- a free slot (0..99) is
//       allocated from _G_LLM_STRAT_PATH_SLOT_FLAGS[owner=0]/*_BUFFERS, an RLE-encoded direction
//       trace is written into it, and result = the tile the backtrace actually stopped at (the true
//       start on a full solve; short of it if the trace hits the 297-entry cap or a stale
//       best-candidate governor -- see (2) below).
//   (b') ALL 100 SLOTS ARE OCCUPIED -- slot_id = -1, result is left at whatever it was set to in
//       step 0 (start_col/start_row, NOT re-derived), no buffer is touched.
//
// ---- 0. RESULT DEFAULTS TO START (0x0041c2ad-0x0041c2bc) ------------------------------------------
// Unconditional first action, covering outcome (b') above: result_col/row = start_col/row.
//
// ---- 1. COST MAP RESET + COORD MASK (0x0041c2c1-0x0041c2eb) ---------------------------------------
// _G_LLM_TACT_MOVE_COST_MAP (65536 uint32_t, indexed by packed tile (col<<8)|row) filled with
// 0xffffffff (`REP STOSD`, ECX=0x10000 -- the whole array, not just the excursion's 128x128
// sub-block: preserved literally, Law 2, even though only a fraction is ever touched by the flood
// below). coord_mask = ((grid_width-1)&0xff)<<8 | ((grid_height-1)&0xff) -- grid_width/height are
// the SHARED (both-mode) `width`/`height` globals (RID_WIDTH/RID_HEIGHT), NOT tact_store's own
// map_width/map_height cache (a different, tactical-only region). This is a byte-AND wraparound
// mask, not a real modulo -- exact only when width/height are powers of two, which
// llm_tact_mission_start's own literal 0x80 satisfies for every tactical excursion; reproduced as
// the literal AND regardless (Law 2).
//
// ---- 2. GOAL-SURROUNDED CHECK (0x0041c2f0-0x0041c370) ---------------------------------------------
// goal_packed = (goal_col<<8)|goal_row, stored to move_path_goal_packed. Then the SUM of
// passable[wrap(goal,N)] + passable[wrap(goal,S)] + passable[wrap(goal,E)] + passable[wrap(goal,W)]
// (four raw, un-substituted passable() reads -- NOT run through the 0/0x10000 substitution step 4
// below) is compared to zero. If ALL FOUR are zero (the goal has no orthogonal neighbor with a
// nonzero passable value at all): this is outcome (a) above -- result = goal, slot_id = -1, RETURN,
// no flood, no backtrace, no slot scan. Otherwise fall into the flood.
//
// ---- 3. FLOOD-FILL / EDGE-RELAXATION LOOP (0x0041c371-0x0041c487) ----------------------------------
// Dijkstra-shaped over the SAME packed-tile domain, in two double-buffered 4096-entry uint16_t wave
// arrays (move_path_queue_a/b, swapped by XCHG each pass -- reproduced here as pointer-swapping
// local variables since both arrays are real, fixed process memory: a genuine base address
// round-trips byte-identically through move_path_queue_cur_addr under shadow). start_packed is
// stored to move_path_start_packed and seeded at cost 0; queue_b[0] = start_packed is the initial
// (size-1) current wave, queue_a is the initial write target.
//
//   For each tile popped off the current wave (LIFO order within a wave -- order doesn't matter for
//   Dijkstra correctness of the relaxed SET, but it does matter here: the push order decides which
//   queue INDEX a tile lands at, and the index is part of the declared shadow region, so an order
//   mismatch surfaces as a byte divergence even though the algorithm is semantically identical),
//   evaluate the 4 orthogonal neighbors in EXACTLY this order -- row-1, col+1, row+1, col-1 --
//   confirmed by a direct memory read at 0x0041c3c0 after an assumed row-1/row+1/col+1/col-1 order
//   rig-diverged. step_cost = passable[neighbor] if nonzero, else 0x10000 (a SOFT penalty, not a
//   hard wall -- a route CAN cross an impassable tile if nothing better exists).
//   UNLIKE every other neighbor computation in this function (goal-surrounded check, backtrace),
//   these 4 are FULL-EAX arithmetic on the packed tile (`SUB/ADD EAX,0x1` for the row pair,
//   `ADD/SUB EAX,0x100` for the col pair), not byte-only INC/DEC AL/AH -- so row-1/row+1 can
//   BORROW/CARRY into the col byte at a row boundary (tile-1 with row==0 also decrements col;
//   tile+1 with row==0xff also increments col too). A rig run caught this directly: treating row
//   as independently byte-wrapped (as the byte-only sections genuinely are) diverged on every call
//   that touched a row-boundary tile.
//   new_cost = cost_map[current] + step_cost; if new_cost < cost_map[neighbor]: relax (write
//   cost_map[neighbor] = new_cost, push neighbor onto the next wave). THE PUSH ITSELF
//   (0x0041c3e9 and its three siblings) IS A 32-BIT STORE (`MOV dword ptr [EBP+EBX],EAX`) even
//   though each queue slot is 16 bits (EBX += 2 per push) -- EAX's always-zero upper half
//   zeroes the FOLLOWING slot as a side effect. A rig run caught this: without reproducing it, a
//   wave's LAST push leaves "ours" holding a stale byte from an earlier call's wave instead of
//   the fresh zero the original leaves there (shadow diff: `_G_LLM_TACT_MOVE_PATH_QUEUE_B +0x2`,
//   original=00 ours=4A, every one of 47 calls). Reproduced as two explicit 16-bit stores.
//
//   After a wave empties: STOP if cost_map[goal] < 0x10000 (goal reached with a real, non-penalty
//   cost -- note this can be true even mid-flood, before every reachable tile has settled) OR the
//   next wave is empty (nothing left to expand, goal unreachable via any real-cost route -- the
//   flood may still have relaxed penalty-cost paths toward it, which the backtrace below can use).
//   Otherwise swap current/next and continue.
//
// ---- 4. FREE-SLOT ALLOCATION (0x0041c4a8-0x0041c4d6) -----------------------------------------------
// Linear scan of _G_LLM_STRAT_PATH_SLOT_FLAGS[owner=0][0..99] (mode_planes::path_slot_flag_at, READ
// only here -- nothing in this function ever SETS a flag) for the first zero byte. None found in
// 100 tries -> outcome (b'): slot_id = -1, JUMP PAST THE BACKTRACE ENTIRELY (result stays at its
// step-0 default, NOT re-derived from the flood). Found at index i -> slot_id = i,
// path_waypoint_at(0, slot_id, 0) zeroed (heading=run_length=0) before the backtrace writes into it.
//
// ---- 5. BACKTRACE (0x0041c4d6-0x0041c7e5), one iteration per LAB_0041c517 visit -------------------
// Walks from GOAL back toward START along the steepest cost-map descent, RLE-encoding the
// (reversed) direction sequence into the allocated slot. Per iteration, for the current tile:
//
//   - trace_cost_sum += passable[current] (raw, unsubstituted -- an accumulated telemetry sum this
//     function computes but the .asm never reads back into anything else in THIS function; kept as
//     real state since it is a declared shadow region, its consumer if any is outside this closure).
//   - best_dir = 0xff (sentinel), base_cost = cost_map[current] (the running "best so far", NOT
//     re-initialized to INT_MAX -- a candidate must be STRICTLY BETTER than the CURRENT tile's own
//     cost, or tie under the CL-preference rule below, to be selected at all).
//   - CL = path_waypoint_at(0, slot_id, rle_count).heading -- the heading already sitting in the
//     slot the NEXT append would land on (i.e. the PREVIOUS iteration's heading, or 0 on the very
//     first iteration since step 4 zeroed it). Used only as a tie-break preference.
//   - Evaluate up to 8 candidates in a FIXED order -- N, S, W, E, then the 4 diagonals (NW, NE, SW,
//     SE), each diagonal gated on BOTH its flanking orthogonal tiles being passable (no cutting a
//     corner between two blocked tiles; uses the SAME two candidate positions the orthogonal tests
//     already computed, not fresh reads). A candidate is selected (best_dir/best_tile updated, cost
//     becomes the new running best) when its cost is STRICTLY LOWER than the running best, OR EQUAL
//     to it AND best_dir != CL (i.e. a tie does NOT overwrite an already-preferred-heading
//     selection, but DOES overwrite anything else) -- transcribed literally; this is preserved
//     exactly as coded even though it does not obviously implement "prefer continuing straight" in
//     every case (Law 2: no behavioural cleanup).
//   - best_tile is a PERSISTENT global (move_path_trace_best_tile) that is NOT reset at the top of
//     an iteration -- only best_dir is. If no candidate is ever selected this iteration, best_tile
//     retains WHATEVER a PRIOR iteration (or, on the very first call after a fresh DLL load, program
//     start) left there. The one guard against using that stale value: after all 8 candidates, IF
//     passable[best_tile] == 0 (the stale/never-set tile reads as impassable), the trace STOPS HERE
//     -- result = the CURRENT tile (not best_tile), slot_id's buffer is left with whatever it holds
//     so far, RETURN. This is the function's own safety net for its own reused-variable hazard, and
//     it is transcribed as literally as everything else: a real game session that happens to hit
//     this on the FIRST-EVER call after DLL load would read genuinely uninitialized memory, exactly
//     as the original does.
//   - Otherwise (best_tile passable): RLE-append best_dir to the path buffer at rle_count (start a
//     NEW entry only if this heading differs from the slot's current entry AND that entry is
//     already non-empty; on a run-length byte overflow from 0xff, split into a fresh entry rather
//     than wrapping to 0). If rle_count exceeds 0x129 (297) OR best_tile == start_packed: stop (the
//     latter is normal termination -- start reached); otherwise loop back to (5) with current :=
//     best_tile.
//
// ---- 6. TERMINATOR (0x0041c7d7-0x0041c7e5) ---------------------------------------------------------
// A trailing zero (heading=0,run_length=0) entry is written one slot PAST rle_count -- the sentinel
// llm_tact_move_path_preview_walk's own heading==0 check reads as "end of path".
//
// ---- CALLEES: NONE. Confirmed by reading the whole 0x53b-byte body -- no CALL instruction anywhere
// (matches the tracker's own "1 function reachable (itself)" closure measurement).
//
// ---- FLOATS: NONE. Every operation is integer (byte/word MOV, AND, ADD/SUB, CMP, INC/DEC, MUL for
// the... no, there is no MUL here at all -- pure integer compare-and-store).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

namespace detail {

// llm_tact_move_path_build @0x0041c2ac. See the header banner for the full derivation.
void move_path_build(const tact_view &v, tact_store &own);

} // namespace detail

void move_path_build();


} // namespace mh::tact
