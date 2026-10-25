//
// ddraw_own.h -- PT-GFX1: mh.dll's own DirectDraw v1. With [video] backend=own (or gdi / null / d3d11) the game is handed COM
// objects implemented here instead of DDRAW.DLL's; the default `system` leaves every byte alone.
//
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "gfx/ui_layout.h"
#include "gfx/overlay_list.h"

namespace mh::gfx {

// RES4 (stage 2): [video] ui_scale in percent (100 = off). owned_set_ui_scale is live (set_config + re-attach of the
// geometry); the in-game options menu calls it in a later stage. owned_present_layout builds the quad list the
// presenter draws NOW for a frame of frame_w x frame_h: the screen is the game window's client area (or the dev
// `[video] ui_screen=WxH` when set, for a headless lane that presents nothing). False when the owned device is not
// armed or there is no window / size. The UI harness `pcapture` composes it on the CPU.
// [video] ui_scale resolved from the ini value: a percentage, or `auto` = the Windows display scale of the game's (primary) monitor
// (GetDpiForMonitor; the dev key [video] display_scale=N overrides it so a rig can fake the DPI), rounded to the nearest of
// 100/125/150/175/200. The value is logged once; the screen-size clamp is the layout's. Never below 100.
int  owned_resolve_ui_scale(const char *ini);
int  owned_ui_scale();
void owned_set_ui_scale(int pct);
// The in-game options menu's Interface size row: index 0..OWNED_UI_SCALE_CHOICES-1 = Auto, 100, 125, 150, 175, 200 (the launcher's
// order). owned_ui_scale_choice = the row's current value (Auto when the ini said auto); owned_set_ui_scale_choice applies one live
// (owned_set_ui_scale; Auto is resolved like the boot read). Persisting it to the ini is the caller's.
constexpr int OWNED_UI_SCALE_CHOICES = 6;
int           owned_ui_scale_choice();
void          owned_set_ui_scale_choice(int ix, const char *ini);
bool          owned_present_layout(int frame_w, int frame_h, uilayout::layout *out);
// RES23-S0 `bcapture <name>`: the next present writes capture_<name>__b.bmp from the presenter's real output (gdi: the client area
// via BitBlt; d3d11: the back buffer after the overlay pass); the null presenter logs a SKIPPED line instead.
void owned_request_bcapture(const char *name);
// Stage 3: the frame rows of the side panel the screen shows; 0 = ui_scale not in force.
bool owned_ui_layout(uilayout::layout *out);
// RES23-S5: THE overlay list (gfx/overlay_list.h: the scaled notice, the layered dialogs bottom to top, the cursor) over the presented image of a
// frame_w x frame_h frame (premultiplied, clipped to the screen; static buffers, valid until the next present). Only sources stamped for the frame
// the framebuffer holds now are in it (a stale one is dropped). Empty = the frame carries the game's own cursor / notices (100%, an unscaled mode).
// sprite_bbox (optional, 4 ints) gets the cursor sprite's opaque box in scratch pixels (hotspot at uilayout::CURSOR_HOT).
bool owned_overlay_list(int frame_w, int frame_h, overlay_list *out, int *sprite_bbox = nullptr);
int  owned_ui_panel_rows();
// RES23-S2: the committed HUD geometry. Committed at ONE point -- the strategic pre-input of a mode 2 / 5 frame (owned_hud_commit(true): takes the
// asked Interface size, the pending value) -- and recomputed for a new frame size when the picker applies a mode (false: same percentage). The P6 row
// thunks, the filler and the RES7 resize read it; nothing in game draw code builds a layout. epoch counts commits that changed anything (0 = none
// yet, the asked value is then read live); each presented strategic frame is stamped with the epoch it was drawn under.
struct hud_geometry {
    unsigned epoch   = 0;
    int      pct     = 100; // the percentage in force for the strategic HUD
    int      hp      = 0;   // whole side-panel rows the screen shows (0 = ui_scale not in force)
    int      frame_w = 0, frame_h = 0;
    // RES23-S3: the rest of what the panel lists and the filler read, computed at the same commit.
    int vis_h    = 0;   // min(composition height, Hp): the frame rows the screen shows (the composition's when ui_scale is not in force)
    int h_built  = 0;   // the window height the RES7 tables hold rows for (0 = not grown)
    int list_cap = 768; // the row-count cap of the scrollable lists: max(768, h_built)
    // RES23-S4: THE COMPOSITION -- the part of the frame the picture really occupies (one rect for what were three mechanisms: the RES18 presenter crop, the
    // RES21 overlay's base and the window's native client size; the ideal frame's ceil32 margin is the same concept, expressed by build()'s min(room, composition)).
    // comp_w x comp_h of the comp_fw x comp_fh frame the picker applied (comp_w == 0: the frame is the composition). Committed when the picker applies a
    // mode (owned_set_crop) and carried by the epoch; the layout, the window size, the clip, input and capture all read this one rect.
    int comp_fw = 0, comp_fh = 0, comp_w = 0, comp_h = 0;
    // THE FILL RULE lives in seams/video_mode_guards.cpp panel_fill_plan (every whole empty slot down to the last whole slot of the frame is teal; only the partial slot stays pink).
};
bool owned_hud_commit(bool take_pending);
// RES26: a mode was applied (Options Ok): like owned_hud_commit(false), but in a dialog mode (3, 4) over a strategic frame the HUD stamp moves to the new frame and
// takes the asked Interface size first, so the requested size shows before the game resumes. Same return as owned_hud_commit.
bool owned_hud_commit_applied(); // true = the percentage or Hp changed: the caller owes every panel mode + the strip a redraw
// RES23-S3: the height the RES7 tables were grown for (boot, a rebuild). Recommits when a geometry exists.
void                owned_hud_set_built(int h_built);
int                 owned_hud_list_cap();
const hud_geometry &owned_hud_geometry();
// RES16: the info screen's AVI clip in a scaled layout (see ddraw_own.cpp g_info_pass). owned_info_layer_pass: 0 = not in a dialog-layer pass, 1 = an early
// pass (the tick does nothing), 2 = the last (the tick decodes and calls owned_info_clip_stash instead of blitting into the frame). owned_set_info_post
// registers the step run once the layer is stored (the cursor and the flip); registering it is what turns the mode-4 layer on.
int  owned_info_layer_pass();
void owned_info_clip_stash(void *surface, int x, int y);
void owned_set_info_post(void (*cb)());

// Read [video] backend from `ini` and, unless it is `system`, take over the game's DDRAW.DLL loader.
// Called once from MH_Video_ApplyProcessAttrs (DllMain, before the exe entry point -- the game's first
// DirectDraw use is llm_game_init_subsystems, long after). Idempotent. Returns true iff armed.
bool install_owned_ddraw(const char *ini);

// True once install_owned_ddraw armed: the game's DirectDraw is ours. video.cpp reads it so the
// dgVoodoo-shaped checks (mode_is_available) stop refusing modes nothing will actually switch to.
bool owned_ddraw_active();

// The mouse mapping of a scaled window (ddraw_own.cpp "the mouse"). owned_image_rect: where the frame
// is drawn inside the game window's client area. owned_client_to_game / owned_game_to_client: one point
// through that rect (client pixels <-> game pixels; a client point in the bars clamps to the image edge,
// a game point maps to its pixel's centre). All three return false -- and leave the point alone -- when
// nothing is mapped: backend system/null, [video] no_window, or no mode yet. Used by the UI harness.
bool owned_image_rect(RECT *out);
bool owned_client_to_game(POINT *p);
bool owned_game_to_client(POINT *p);
// RES6: one game point <-> SCREEN pixels through the current mode's image rect (the geometry is settled first).
// False, point untouched, when nothing is mapped (as above). owned_refresh_clip re-applies the OS-pointer clip for
// the current mode now instead of at the next present, so a SetCursorPos right after a mode change is not clamped
// to the previous mode's image.
bool owned_game_to_screen(POINT *p);
bool owned_screen_to_game(POINT *p);
void owned_refresh_clip();
// Windowed: the player resized (or maximised) the window, so its size no longer follows the mode.
bool owned_user_sized();
// PT-GFX8: would Windows list `h` on the taskbar / in Alt-Tab (visible, not cloaked, unowned without
// WS_EX_TOOLWINDOW or WS_EX_APPWINDOW)? `facts` (optional) receives the bits the verdict rests on.
bool window_switchable(HWND h, char *facts, int n);
// The game's current display mode (the frame size every presenter scales), false before SetDisplayMode.
bool owned_mode_size(int *w, int *h);
// The largest frame (w x h, 16 bpp) the session can switch to: every game-frame surface created afterwards is
// allocated at least that big (pitch stays w*2, the presenter reads only w x h). A stale menu frame drawn after
// a switch to a smaller mode writes past a bevel's unclipped rows; this keeps those writes inside owned memory.
// Call before the game's first CreateSurface; the largest request wins. Not applicable to backend=system.
void owned_reserve_frame(int w, int h);
// Selftest hook: the bytes a w x h surface is actually allocated with (0 on failure), through the real create().
SIZE_T owned_probe_frame_alloc(int w, int h, bool primary);

// The presenter's LIVE options (present_backend::set_config): applied to the bound backend at once and
// kept for any later re-attach. filter: 0 point, 1 linear, 2 sharp, 3 area (present_backend.h
// scale_filter). integer_scale: [video] scale=integer. Each change is logged to mh_video.log.
// Used by the ImGui overlay's panel; no-ops before install_owned_ddraw armed.
constexpr int OWNED_FILTER_COUNT = 4;
const char   *owned_filter_name(int filter); // the [video] filter spelling; "?" out of range
int           owned_filter();
void          owned_set_filter(int filter);
bool          owned_integer_scale();
void          owned_set_integer_scale(bool on);
// RES18: the session size is lw x lh but the game renders the tile-rounded fw x fh; the strategic frame is presented cropped to lw x lh
// (uilayout::build's crop). lw == fw && lh == fh (or 0) clears it. Called by the size picker whenever a mode is applied.
void owned_set_crop(int fw, int fh, int lw, int lh);
bool owned_vsync();
void owned_set_vsync(bool on);

} // namespace mh::gfx
