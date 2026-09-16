//
// mh/ui internals -- the three injected facts, shared by every TU in the module.
//
// Nothing here names a net symbol on purpose: lobby_ui.h's three setters are the only way a value
// gets in, so the module's whole dependency on the rest of the DLL is three function pointers set
// once at arm time. See lobby_ui.h for why each one exists.
//
#pragma once

#include "ui/lobby_ui.h"

namespace mh {
namespace ui {
namespace detail {

// The injected slots. Defined in ui_runtime.cpp.
extern void (*g_log)(const char *);
extern const bool *g_diag;
extern int (*g_client_session)(void);

// Append one already-formatted line to the module's log. Safe before set_logger (drops the line) --
// every install in this module runs after the arm wiring, but a future caller may not.
inline void ui_log(const char *s) {
    if (g_log) g_log(s);
}

// `[net] lockstep_log` -- the verbose gate for this module's per-event diagnostic lines. False until
// the flag is injected, which is the correct answer for anything that runs before the arm.
inline bool ui_verbose() {
    return g_diag && *g_diag;
}

// Is this peer a CLIENT in a live session? False when nothing answered -- "we do not know" must
// read as "leave retail alone", never as "take over".
inline bool ui_client_session() {
    return g_client_session && g_client_session() != 0;
}

// ---- the menu slide's two end states ------------------------------------------------------------
//
// `llm_lobby_show_intro_wait(dir)` tweens the SHARED menu frame widget between exactly two states,
// and `dir` picks which one it runs to. Both are pure functions of two sprite widths the retail body
// reads, so the end state can be computed without running the tween -- which is the whole of what
// the take-over does. Formulas transcribed from the retail body (0x004bc3c1); the values they
// produce were confirmed live on four routes (the lobby-slide notes "The mechanism", measurement M2):
// IN -> frame_x -124, right_x 363; OUT -> frame_x -683, right_x 1481.
//
// ONE COPY, because there were two: the U3/U7 snap in the lobby-dispatch seam re-derived the IN
// state independently and could drift from the take-over's. It calls this now, keeping only its own
// degenerate-case fallback (see lobby_frame_snap_on_screen).
constexpr int SLIDE_IN  = 0; // the EAX argument: slide on-screen
constexpr int SLIDE_OUT = 1; //                   slide off-screen left

bool slide_end_state(int dir, int *out_frame_x, int *out_right_x);

} // namespace detail
} // namespace ui
} // namespace mh
