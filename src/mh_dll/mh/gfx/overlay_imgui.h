//
// overlay_imgui.h -- PT-GFX4: a Dear ImGui overlay drawn by the d3d11 presenter.
//
// Armed only when [video] backend=d3d11 AND [video] imgui=1; otherwise nothing here runs, no ImGui
// context exists, and the game window is never subclassed. Even when armed, the ImGui context is
// created on the first OPEN (the toggle key, [video] imgui_key, default F11, seen as the game window's
// own WM_KEYDOWN or the UI harness's `hotkey` chord) -- a closed overlay costs one comparison per
// window message and one table read per present.
//
// Read-only with respect to the game: the overlay reads globals through the debug overlay's provider
// registry (mh_overlay_export.h) and never calls sim code or writes sim state. The only game memory it
// ever WRITES is the input layer, and only to keep a click or key it consumed from reaching the game
// (overlay_imgui.cpp, "INPUT").
//
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace mh::gfx::imgui_overlay {

// Read [video] imgui / imgui_key from `ini`. Called by install_owned_ddraw only when the chosen
// backend is d3d11. Idempotent. Returns true iff armed.
bool arm(const char *ini);

// The d3d11 backend's hook point: after the game-frame draw, before Present, with the back buffer's
// render target bound. Polls the toggle key, and while open builds + renders one ImGui frame at
// output resolution. A no-op unless armed.
void on_frame(ID3D11Device *device, ID3D11DeviceContext *context, HWND hwnd);

// The backend is about to release its device (detach, or device lost). Drops every ImGui DX11 object
// and the references ImGui holds on the device/context; the next on_frame re-creates them.
void on_device_release();

// The backend is releasing its WINDOW (detach(): a rebind to another HWND, a re-attach, shutdown).
// Unlinks the subclass from that window and shuts down ImGui's Win32 backend, which caches the HWND;
// the next on_frame binds whatever window it is handed. The context and open/closed state survive.
// on_frame also notices a changed HWND by itself, so a presenter that never detaches is still safe.
void on_detach();

// Read-only state for the UI harness's `imgui` predicate (ui_drive.cpp) and the logs.
enum class query_what { armed,
                        open,
                        swallowed_mouse,
                        swallowed_keys,
                        drained_mouse,
                        rebinds,
                        subclassed,
                        want_mouse, // io.WantCaptureMouse of the last frame built (0 while closed)
                        mouse_x,    // ImGui's mouse position last frame, client px (the game cursor)
                        mouse_y,
                        di_filtered }; // DI button/wheel records withheld from the game
int query(query_what what);

} // namespace mh::gfx::imgui_overlay
