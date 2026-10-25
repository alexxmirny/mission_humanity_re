//
// mh_optscale_export.h -- mods:RES4: the "Interface size" row of the in-game Options screen.
//
// The row sets [video] ui_scale (Auto, 100, 125, 150, 175, 200 -- the launcher's list) from the game itself:
// ESC > Options, between "Screen size" and the unit-flags checkbox. Presentation only (the layout, the render
// of the strategic HUD, the cursor); no sim or lockstep state is read or written.
//
// HOW THE ROW IS BUILT (all raw EN VAs, ReVA-read 2026-10-20; the Options screen is static data, so no new UI code):
//   * llm_ui_widget_list at 0x00654153 holds `children`, a NULL-terminated pointer array at 0x006534c3: the title,
//     CD audio / its slider, Sounds / its slider, Screen size (0x0064fd53), the unit-flags row (0x0064fd97), Ok
//     (0x0065142b), Cancel (0x0065146f). The widgets lay out RELATIVE to the previous child (x, y offsets), so
//     inserting a child after Screen size moves the rows below it down by the new row's y (30) and nothing above.
//   * The new child is a DLL-owned llm_ui_widget (68 bytes), a copy of Screen size's, whose param_block is a DLL-owned
//     llm_ui_spinner {options_table, count, &index, 0, 0} -- the generic spinner Screen size is
//     (the stock click handler wraps the index modulo the
//     count and the stock draw shows table[index]. Its label ("Interface size:", widget +0x38) is set at every open
//     from the language pack's string table (ui/player_strings.def options.*).
//   * One guarded 4-byte data patch swaps `children` for the DLL-owned array. Two trampolines: llm_ui_options_menu_open
//     (0x004b8383) re-reads the current choice and the strings after the original ran; the Ok action
//     (0x004b86a4) commits the choice BEFORE the original, whose size-mode apply then runs with the new scale.
//     Cancel / ESC need nothing: the value lives in the DLL, outside the 0x58-byte settings block Cancel restores,
//     and the next open re-reads the real state.
//   * WHERE THE ROW SITS. Single player hides the unit-flags row (a flag bit the stock open sets), but the hidden
//     widget still takes its place in the flow, so the stock screen has 30 px of nothing above Ok. In single player
//     the row therefore sits AFTER Flags with y = 0, in that empty slot: the screen, and Ok / Cancel, are exactly the
//     stock ones. In a multiplayer game Flags shows, so the row sits between Screen size and Flags (y = 30) and Ok /
//     Cancel move down one row. Two children arrays, the list's `children` pointer chosen at every open.
//     (The first draft inserted the row for both and moved Ok in single player: a cursor left on Ok then fell off the
//     bottom of a 640x480 frame after a size drop and edge-scrolled the camera 37 tiles -- res_walk's bigmap segment.)
//
// APPLY: live. Ok calls mh::gfx::owned_set_ui_scale_choice (the presenter builds its quad list per present; the
// side-panel row counts follow the new Hp from the strategic pump; the owned cursor hook arms on first use) and
// writes `ui_scale = auto|<pct>` to mh_net.ini, the key the launcher writes. The session frame SIZE of a
// `resolution=auto` run is the boot-time ideal render for the boot scale (the boot buffers are sized once):
// the new scale composes whatever frame exists, and the ideal size follows at the next launch.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Arms the row. EN-only; best-effort (a refused guard leaves the stock Options screen untouched). Logs to
// mh_video.log. Returns 1 when armed. Idempotent.
int MH_OptScale_Install(void);

#ifdef __cplusplus
}
#endif
