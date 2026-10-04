//
// dinput_own.h -- PT-INPUT1: mh.dll's own DirectInput 5, on Raw Input ([input] backend=own).
//
// With `own` the game's DINPUT.DLL loader is replaced and the game is handed an IDirectInputA and two
// IDirectInputDeviceA (mouse, keyboard) implemented in dinput_own.cpp, fed from Raw Input on the game
// window. The default `system` leaves the loader alone; the tracing below still works under it.
//
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

namespace mh::input {

// Read [input] backend / mouse_trace / key_trace from `ini` and, for backend=own, take over the game's
// DINPUT.DLL loader. Called once from MH_Video_ApplyProcessAttrs (DllMain, before the exe runs).
// Idempotent. Returns true iff the owned DirectInput is armed.
bool install_owned_dinput(const char *ini);
bool owned_dinput_active();

// Once per present, either backend (ui_drive.cpp MH_UIDrive_OnPresent): the dinput.dll module check,
// the system-DirectInput element trace ([input] mouse_trace), the key-ring trace ([input] key_trace).
void on_present();

// ---- the UI harness: feed the OWNED devices exactly as a Raw Input packet would --------------------
// Each goes through the same packet handler WM_INPUT does. The absolute form takes a GAME pixel and
// maps it through the inverse chain (game -> client -> screen -> normalized virtual desktop), so the
// forward chain the real pointer uses is the one under test. All return false when backend != own or
// the device does not exist yet.
bool harness_mouse_abs_game(int gx, int gy);
bool harness_mouse_rel(int dx, int dy);
bool harness_mouse_button(int button, bool down); // 0 left, 1 right, 2 middle
bool harness_key(uint16_t make, bool e0, bool down);
// The foreground the owned devices see: -1 = the real one (default), 0 = lost, 1 = focused. A headless
// lane's window is never foreground, so a script sets 1 to let the game acquire; 0 exercises the
// focus-loss path (synthetic key-ups).
void harness_focus(int state);

// The filter point on the OWNED mouse: `fn` sees every batch of mouse elements a GetDeviceData is about
// to return (the game's poll and anyone else's), and may remove records in place, updating *count. The
// ImGui overlay installs it instead of the vtable-slot hook it puts on a system device's GetDeviceData
// (overlay_imgui.cpp "the DirectInput mouse filter"). Records are DIDEVICEOBJECTDATA as DI5 lays it out
// (16 bytes: ofs, data, timestamp, sequence). One filter; null removes it.
using mouse_filter_fn = void (*)(void *records, DWORD *count);
void set_mouse_filter(mouse_filter_fn fn);

enum class query_what { active,
                        mouse_elements,
                        key_elements,
                        raw_packets,
                        unsupported,
                        mouse_acquired,
                        key_acquired };
int query(query_what what);

} // namespace mh::input
