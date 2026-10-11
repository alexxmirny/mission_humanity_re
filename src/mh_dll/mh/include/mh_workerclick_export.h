//
// mh_workerclick_export.h -- mods:RES8: THE WORKER +/- BUTTONS' FIRST CLICK IS LOST.
//
// THE PATH. The building panel's two worker-adjust buttons are two 20-byte records,
// `_G_LLM_UI_BLDG_WORKER_ADJ_BTNS` (0x0050bddb, `llm_ui_bldg_worker_adjust_btn[2]`: direction,
// icon_x_offset, icon_id, pending_worker_amount +0x0c, pending_row +0x10; record 0 is "+", record 1
// is "-"). `llm_ui_bldg_worker_adjust_btn_tick` (0x00413364, once per list row per frame) sets
// pending_row = row + 1 on the PRESS edge and leaves pending_worker_amount as it was; a completed
// click (released inside) writes amount = 1 and clears pending_row in the same tick; a hold past
// 200 ms writes amount = (hold_ms - 200) / 50 (the auto-repeat). `llm_ui_bldg_worker_adjust_apply`
// (0x004134b1) runs BEFORE the row ticks each frame and issues the assign/unassign order from
// pending_row with pending_worker_amount. So the order a press issues carries the amount the
// PREVIOUS click left behind: the cells are statically 0, and the first press of each button in a
// process orders 0 workers. A hold that ends outside the button leaves its last repeat count (0 or a
// multi-worker step) for the next press the same way.
//
// THE FIX. (1) Both pending_worker_amount cells (0x0050bde7, 0x0050bdfb) are set to 1 at install,
// guarded on their stock 0. (2) The press-edge branch's click-sound CALL (`CALL llm_snd_play` at
// 0x0041341b, E8 13 1E 01 00, right after the pending_row store) is spliced to a thunk that writes
// amount = 1 into the record (the frame's [EBP-0x18]) and tail-jumps to llm_snd_play, so every
// frame of a press orders one worker (none is lost; how many frames a press orders is part (3)).
// The auto-repeat branch is untouched: while the button is held past 200 ms it overwrites the
// amount every frame exactly as stock does. UI-only: the order is the same
// order kind (0x7e / 0x7f) through the normal path with a count of 1, so a lockstep match replicates
// it like any other order.
//
// ONE WORKER PER PRESS. Parts (1)+(2) fix the amount but not the count of orders: apply issues the
// order EVERY frame pending_row is set, so a press held for k frames orders k workers (a normal
// 80-150 ms click adds ~5-9 at 60 fps). (3) Both order CALLs in the apply (assign at 0x00413509,
// E8 A8 AA 05 00; unassign at 0x00413520, E8 2F AB 05 00) are spliced to thunks that zero the
// record's pending_worker_amount (EBX already carries the count) and tail-jump to the order. The
// rest of the press then orders 0 -- stock already issues 0-count orders while a fresh press is held
// -- and past 200 ms the hold branch rewrites the amount every frame, so the auto-repeat is stock.
// A completed click writes amount = 1 and clears pending_row in the same tick, so a release issues
// no order.
//
// `[ui] worker_click_fix` (C_DEV): 0 = stock (the diagnosis arm), 1 = parts (1)+(2) (the default),
// 2 = all three. Mode 1 stays the default for v0.2.0 (user, 2026-10-12): a held press adding a worker
// per frame is today the only fast way to change a count besides the Home/End keys, so how a press
// should behave is an open design question (mods:RES11), not a bug fix.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Apply the parts the mode selects (best-effort: a wrong byte string or a refused VirtualProtect
// leaves that part stock and logs it). Returns the number of parts armed (0..4: the primed cells
// count as one, the press splice one, each drain site one). Never fails the process.
int MH_WorkerClick_Install(void);

#ifdef __cplusplus
}
#endif
