//
// tact/tact_unit_move_tick.h -- TACT1B: the per-unit per-frame move state machine.
//
//   llm_tact_unit_move_tick @0x0042fb9b (0x7c8)
//
// Called once per unit per tick with (unit_idx, cmd_slot, dt). `cmd_slot` is the SAME value the
// caller also uses to index cmd_queue directly at several points in this body -- it is NOT always
// cmd_index (the unit's own live head), so both are named distinctly below exactly as the
// disassembly keeps them distinct.
//
// TWO QUIRKS PRESERVED LITERALLY (Law 2 -- do not "fix" either):
//   * The KNEEL early-return (step 1) and the "waiting on a fast retry" return (step 5's
//     move_retry_wait>0 arm) are the only paths that do NOT fall into the common dt-trailer
//     (step 10) by themselves -- the kneel path advances move_state_timer by the character's fixed
//     kneel_time instead and returns directly.
//   * The move_retry_wait>0 arm (@0x0042fcb5-0x0042fccb) explicitly adds `dt` to move_state_timer
//     and then falls into the common trailer, which adds `dt` AGAIN -- move_state_timer advances by
//     2*dt on that specific path and dt everywhere else that reaches the trailer. Confirmed by
//     reading both FADD sites; not a transcription artifact.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_move_tick @0x0042fb9b.
//
//  1. @0x0042fbc6-0x0042fc19: KNEEL early-return. If anim_state is 2 or 3, call
//     llm_tact_unit_stand_tick (frontier), add the unit's character_type.kneel_time to
//     move_state_timer, and return -- skipping every step below, including the dt trailer.
//  2. @0x0042fc19-0x0042fc3b: has-a-live-retry gate. If move_retry_wait>0 OR move_retry_attempts>0,
//     go to step 3 (peek the next queue slot). Otherwise go to step 5 (retry state exhausted).
//  3. @0x0042fc3b-0x0042fc96: peek cmd_queue[(cmd_index+1) wrapped at 0x80] (NOT cmd_slot). If that
//     entry's op != 0 AND progress==0, dispatch llm_tact_unit_cmd_advance(unit_idx, cmd_slot) --
//     using the ORIGINAL cmd_slot parameter, not the peeked index -- and return (skip the trailer).
//     op==0 skips the progress check entirely and falls through to step 4 with no dispatch; the two
//     JZ's at 0x0042fc72/0x0042fc82 are NOT parallel conditions -- the second is nested inside the
//     "op != 0" arm of the first, so op==0 never even reads progress.
//  4/5. @0x0042fc96-0x0042fcd0: if move_retry_wait>0, decrement it, add dt to move_state_timer
//     (the quirk above), and go straight to the trailer. Otherwise fall to step 6.
//  6. @0x0042fcd0-0x0042fdd8: if move_retry_attempts<=0, go to step 7 (stuck handling). Else reset
//     move_retry_wait=0x10, decrement move_retry_attempts, and if move_path_slot>0 consume the
//     current path step's target tile (step 6a); otherwise fall to the trailer.
//  6a. @0x0042fd18-0x0042fdd3: the NEXT tile from path_waypoint_at(0,move_path_slot,move_path_step)
//     .heading via llm_tact_facing_to_delta (frontier). If that tile is NOT passable AND progress==0,
//     arm a short move_retry_wait=0x10 pause; otherwise (passable, or mid-progress) zero
//     move_retry_wait/attempts/stuck_countdown outright so the situation is re-evaluated fresh. The
//     tested byte is at 0xb64bb0 -- the `passable` region base, NOT tile_objects.class_owner (an
//     earlier reading of this address mislabeled it; verified against the region registry and
//     against step 12's OWN passable_at/building read at the same relative address pattern).
//     Either way, fall to the trailer.
//  7. @0x0042fdd8-0x0042fe3c: stuck-countdown. If move_stuck_countdown<=0, go to step 8 (path
//     slot / acquire, directly). Else decrement it; if it did NOT just reach 0, go to step 9
//     (check the path cache). If it DID just reach 0, ABORT the queued command: snapshot
//     cmd_queue[cmd_index].op into move_aborted_op, dispatch
//     llm_tact_unit_cmd_advance(unit_idx, cmd_slot), and go to the trailer.
//  8. @0x0042ff54-0x0042ff72: if move_path_slot != 0, go straight to step 10 (consume the existing
//     slot) -- no cache check at all on this arm. If move_path_slot == 0, THIS site is ALSO gated
//     by MOVE_PATH_CACHE_VALID (@0x0042ff69-0x0042ffb6): if the cache is already valid, do nothing
//     at all this call -- not even a park -- straight to the trailer. Only an invalid cache
//     proceeds to acquire (step 9b). Missing this second cache gate was an earlier draft's bug,
//     caught by a rig divergence on move_retry_wait.
//  9. @0x0042fe3c-0x0042fe89: if MOVE_PATH_CACHE_VALID is already set, do nothing further this call
//     (straight to the trailer) -- some other unit already refreshed the flood result this frame.
//  8/9b. @0x0042fe45-0x0042ff54 (also re-entered from step 8 directly, @0x0042ff72-0x0042ffe5):
//     acquire a path via llm_tact_move_step_attempt(MOVE_CUR_COL, MOVE_CUR_ROW,
//     cmd_queue[cmd_slot].arg0/.arg1) -- factored into `try_acquire_path` below. Sets
//     MOVE_PATH_CACHE_VALID=1. If the flood result equals the current tile, PATH_SLOT_ID is forced
//     to -1 (no real move). PATH_SLOT_ID==-1 arms move_retry_wait=0x10/attempts=0xc and returns
//     "no slot"; otherwise it assigns move_path_slot, marks _G_LLM_STRAT_PATH_SLOT_FLAGS[0][slot]=1,
//     zeroes move_path_step, and stamps cmd_queue[cmd_slot].arg2/.arg3 with the flood result
//     (scratch, per llm_tact_unit_cmd_entry's own field doc) -- then falls into step 10 (consume).
//     THE TWO CALL SITES ARE **NOT** IDENTICAL, exactly one field apart: the step-9 site
//     (0x0042fe45, PATH_SLOT_ID==-1 arm at 0x0042fec1-0x0042fee1) sets ONLY move_retry_wait/
//     move_retry_attempts on park; the step-8 direct-entry site (0x0042ff72, arm at
//     0x0042ffee-0x0043001e) ALSO sets move_stuck_countdown=3. An earlier draft conflated the two
//     as byte-identical and a rig divergence on move_stuck_countdown caught it -- `try_acquire_path`
//     below takes an explicit `also_arm_stuck_countdown` flag rather than assuming they match.
// 10. @0x00430091-0x00430163: consume the CURRENT path_waypoint_at(0,move_path_slot,move_path_step).
//     If .run_length != 0, go to step 11 (still executing this leg). If it is 0 and
//     cmd_queue[cmd_slot].arg0==.arg2 && .arg1==.arg3 (the ARRIVAL test), dispatch
//     llm_tact_unit_cmd_advance and go to the trailer. If it is 0 but NOT arrived, release
//     _G_LLM_STRAT_PATH_SLOT_FLAGS[0][move_path_slot], zero move_path_slot, and go to the trailer
//     (forces a fresh path-acquire next call).
// 11. @0x00430163-0x004302e0: if progress!=0 (mid-step), skip straight to step 13 (advance) with no
//     door/passable recheck. Else (progress==0, a fresh step) check the NEXT tile
//     (MOVE_CUR_COL+dx, MOVE_CUR_ROW+dy) from llm_tact_facing_to_delta(heading): if its class_owner
//     byte is nonzero, call llm_tact_door_anim_start(class_owner) (the whole byte, not just a
//     nibble -- preserved literally), OVERWRITE move_state_timer with time_GetCurrentTime() (not an
//     add -- the trailer then adds dt to that absolute value), and go to the trailer.
// 12. @0x0043021e-0x00430293: (class_owner==0) if the tile is not passable, OR its `building` field
//     is nonzero, park with move_retry_wait=0x10/attempts=0xc/stuck_countdown=3 and go to the
//     trailer. Otherwise go to step 13.
// 13. @0x00430293-0x004302de: if facing_dir==heading, call llm_tact_unit_move_advance(unit_idx, dx,
//     dy) (the EDX/EBX args carry MOVE_CUR_COL/ROW, unused by the callee per its own committed
//     prototype). Else, only when status bit 0x8 (FIRE) is NOT set, call
//     llm_tact_unit_rotate_step(unit_idx, heading). Either way, fall to the trailer.
// 14 (= step 11's progress!=0 arm). @0x004302e0-0x00430342: re-derive heading/dx/dy for the SAME
//     (move_path_slot, move_path_step) and call llm_tact_unit_move_advance unconditionally (no
//     door/passable/facing checks) -- falls straight into the trailer.
// TRAILER @0x00430342: move_state_timer += dt.
void unit_move_tick(const tact_view &v, tact_store &own, int32_t unit_idx, int32_t cmd_slot,
                    double dt);

} // namespace detail

void unit_move_tick(int32_t unit_idx, int32_t cmd_slot, double dt);


} // namespace mh::tact
