//
// mh_walkerframes_export.h -- a walker turning in place flashes the north image on 2 of every 3 notches.
//
// THE PATH. Walker units (cfg TYPE R_aWALKER / R_hWALKER: the Centaur family) are drawn as
// `Unit.sprite + _G_LLM_STRAT_WALKER_FRAME_TABLE[facing*32 + move_microstep]` (0x00ae1df8, dwords). The retail
// init (llm_strat_walker_frame_blend_table_init @0x00460a23, called once from llm_strat_mode_init in boot stage 4)
// fills only facings 1, 4, 7, ... 22; the other 16 stay 0, the facing-1 (north) rest image. The sim facing is right;
// only the picture is wrong. Nothing but the two renderers reads the table.
//
// THE FIX. The CALL to the init inside llm_strat_mode_init (0x0045f067, E8 B7 19 00 00) is spliced to a thunk that
// runs the retail init and then fills the 16 missing facings (seams/walker_frames.h: rest microsteps -> the facing's
// own rest frame, stride microsteps -> the nearest filled facing's stride frames). Render only: the table is in no
// hashed or synced region, so lockstep, the gameplay digest and the synced-settings cap are untouched, and it needs
// no host agreement -- every peer may run it on or off.
//
// `[ui] walker_turn_frames_fix` (C_DEV, default 1): 0 = retail (the reproduction arm).
// Carrier: mh.dll only (the release ships without libmh; rendering is not migrated to libmh).
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 when the splice is armed, 0 otherwise (key off, wrong bytes, promoted owner). Never fails the process.
int MH_WalkerFrames_Install(void);

#ifdef __cplusplus
}
#endif
