//
// mh_tactscroll_export.h -- TSCR1: THE TACTICAL CAMERA SCROLL PACED ON THE GAME CLOCK, CARRIED BY mh.dll FOR THE
// RETAIL BODY.
//
// THE PROBLEM. llm_tact_frame (EN 0x00429b1a) scrolls the camera in 0x0042a121..0x0042a27f: with no mouse button
// down and _G_LLM_TACT_MINE_BLAST_SHAKE_STEP (0x00558ba0, really the mine-blast step) == 0, four arms test the arrow latches
// _G_LLM_TACT_CAM_SCROLL_{UP,DOWN,RIGHT,LEFT}_HELD (0x00558c68/6c/74/70) or the cursor on the screen edge, move
// _G_LLM_MAP_CAM_COL/ROW (0x00e58138/0x00e58134) by ONE tile with the map clamp, and call
// llm_tact_view_shift_{col,row}_{inc,dec} (0x0043c0e1/0x0043c155/0x0043c1dd/0x0043c266). There is no timer: one
// tile per RENDERED frame, so the speed is fps x tile (60 fps: 1920 px/s across, 1440 down; an uncapped gdi
// backend is unbounded) and a 32x24 tile makes vertical 25% slower.
//
// THE FIX ([tactical] scroll_px_s, default 480; 0 = retail). One guarded edit, 7 bytes at 0x0042a121:
//   83 3D A1 04 52 00 00   CMP dword ptr [_G_LLM_CURSOR_X],0
// ->
//   E9 <rel32> 90 90       JMP tact_scroll_thunk (pushad/pushfd; tact_scroll_paced; popfd/popad; JMP 0x0042a280)
// 0x0042a280 is the camera-follow check the block falls through to. The block's only side effects besides the
// camera are four dead stores to [EBP-0x3c] (the hover loop at 0x0042a3b4 assigns it before 0x0042a3f6 reads it),
// so skipping them is safe. tact_scroll_paced reads the same latches and cursor tests, asks the pacer
// (libmh/tact/tact_scroll_pace.h) how many whole tiles each direction owes on the GAME clock (time_GetCurrentTime,
// so pin_wallclock replays stay deterministic and a focus loss pauses it) and runs the original clamp + shift
// helper that many times. A direction becoming active steps at once (a tap = one tile), then every tile_px/px_s
// seconds; at most 2 steps per direction per frame, re-based when late. Diagonals: each direction runs at its own
// rate. The camera feeds the sim (facing commands to selected idle units in the camera window), hence the game
// clock; the tactical journal rows pin scroll_px_s=0.
//
// px_s = 0 installs nothing. Tactical mode is never promoted (fork F2E), so no carrier is needed beyond
// libmh's frame twin (tact_frame.cpp step 14, mh::tact::set_scroll_px_s), which runs the same pacer.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Reads [tactical] scroll_px_s and arms the retail splice. Returns 1 when armed. Idempotent; logs one line.
int MH_TactScroll_Install(void);

#ifdef __cplusplus
}
#endif
