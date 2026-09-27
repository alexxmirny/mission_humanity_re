//
// mh_infoavi_export.h -- mp:X2g: AN INFO SCREEN WHOSE CLIP WILL NOT OPEN NO LONGER CRASHES THE GAME.
//
// THE CRASH (rc4 field report 01M3FHH6G7TH43JZNHVFSEX89Q, client, match ab1aba66): right-click a
// research in the Design Office -> "insert the CD" -> "can't find ...avi" -> 0xc0000094 at
// 0x004cab52. The client's install had no Res\H_INV.AVI (every human research's clip).
//
// THE RETAIL PATH (EN VAs, read from the disassembly 2026-09-26):
//   llm_ui_entity_info_screen_open (0x004cad31) installs the info widget list and MENU_STATE=1
//   FIRST (0x004cad73), builds the text pages, then llm_ui_avi_open(ctx, "res\<INFO_FLC>")
//   (0x004cb1ed). llm_ui_avi_open ZERO-FILLS ctx[0..0x214) before AVIFileOpenA -- which clears the
//   video stream record, stream_length_time (0x0065f993) with it. On failure (0x004cb1fa): stop CD
//   audio, retry "%sres\%s" on G_CD_DATA_PATH, and while that fails call
//   llm_game_verify_cd_inserted -- the blocking "insert CD" dialog -- then
//   llm_ui_show_formatted_error_dialog (the "can't find" box) and RETURN, leaving the widget list
//   installed, the game mode unchanged, and no video. The next frame (dispatched from inside the
//   box's own modal loop in the field stack) draws that list, whose media widget is
//   llm_ui_info_media_frame_tick (0x004caa46): `playback_pos %= stream_length_time`, `IDIV` by 0.
//   The same happens for every entity whose INFO_FLC names a file nobody ships -- retail INIT.CFG
//   gives NOT.AVI to Rakieta x2, Baza x2 and Chatka1-8. A retail bug, reachable in single player.
//   In a match the CD dialog also blocks the frame pump for as long as it is up.
//
// THE FIX (two guarded writes, default ON, SP and MP alike):
//   1. llm_ui_info_media_frame_tick is REPLACED (install_jmp, Watcom-prologue guard) by the same
//      body with the video half guarded: Lock/decode/Unlock/BltFast run only when there IS a surface
//      and a clip (stream_length_time > 0), and the loop wrap never divides by <= 0. Everything else
//      is retail's line for line -- the frame delta store the MP keepalive reads, the cursor draw
//      and the present flip, which is how this screen reaches the display at all.
//   2. The failure branch at 0x004cb1fa (`CALL llm_snd_cd_stop`, E8 AE C5 FF FF) is spliced to a
//      JMP into our handler, which does retail's one useful step -- stop CD audio and try the same
//      clip on G_CD_DATA_PATH (on a no-CD install that path is the exe's own directory, so this also
//      rescues a wrong working directory) -- and then, instead of the CD dialog and the error box,
//      logs one line naming both paths and opens the screen WITHOUT VIDEO: retail's own success
//      tail minus the video half (GAME_MODE=4, the clock stamps, the dialog-state bits, the 1000 ms
//      delta seed) and out through the function's epilogue. If the CD path opens, execution
//      resumes at retail's success continuation (0x004cb274) exactly as retail's loop would.
//   So the player gets the text pages with the clip panel showing the map behind it (measured on the
//   rig, 2026-09-26), and Esc closes it through llm_ui_entity_info_screen_close as usual (its
//   codec/file teardown is safe on the zeroed context; the surface release is NULL-checked).
//
// WHY SP TOO. The only thing retail's SP path adds over this is the CD prompt and the box, and the
// crash follows both whatever the player does: the prompt cannot be satisfied (no MH disc exists
// for the builds we ship; with the no-CD fallback G_CD_DATA_PATH is the install itself, already
// tried) and the box's own modal loop is where the tick divided by zero. There is nothing retail
// UX to preserve.
//
// SIM STATE. None of it is hashed: the info screen writes UI globals (widget list, menu state,
// 0x0065xxxx), _G_LLM_GAME_MODE (0x005202d0) and the media context -- none in
// tools/data/hash_manifest.json. The no-video screen is the SAME mode-4 screen a working clip gives
// (llm_ui_paged_list_frame drives it, and its lockstep keepalive), so a match sees exactly what it
// sees when the file is present.
//
// KNOBS ([net], mh_net.ini):
//   info_avi_fallback=0        RETAIL, whole: neither write is made (no tick guard either), so the
//                              failure path runs CD prompt -> box -> the IDIV. The repro arm.
//   info_avi_test_absent=<l>   TEST AFFORDANCE: the info screen's opens of the clips in <l> (`*`, or
//                              a comma list of file names) are redirected to res\NOT.AVI -- the
//                              name retail's own Chatka/Baza/Rakieta entries carry, which exists
//                              nowhere -- so the rig reaches the failure without touching the shared
//                              Res directory. Nothing on disk is moved. Empty (default) = off.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Apply the tick replacement and the failure splice (best-effort: a byte mismatch or a refused
// install leaves retail untouched and says so in mh_net.log). Returns a bit set: 1 = tick guarded,
// 2 = failure branch spliced, 4 = test knob armed. Never fails the process.
int MH_InfoAvi_Install(void);

#ifdef __cplusplus
}
#endif
