//
// ddraw_own.h -- PT-GFX1: mh.dll's own DirectDraw v1. With [video] backend=own (or gdi / null / d3d11) the game is handed COM
// objects implemented here instead of DDRAW.DLL's; the default `system` leaves every byte alone.
//
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mh::gfx {

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
// Windowed: the player resized (or maximised) the window, so its size no longer follows the mode.
bool owned_user_sized();
// The game's current display mode (the frame size every presenter scales), false before SetDisplayMode.
bool owned_mode_size(int *w, int *h);

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
bool          owned_vsync();
void          owned_set_vsync(bool on);

} // namespace mh::gfx
