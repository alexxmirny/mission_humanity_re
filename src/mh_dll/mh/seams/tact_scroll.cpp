// seams/tact_scroll.cpp -- TSCR1: the tactical camera scroll paced on the GAME CLOCK, carried by mh.dll for the
// retail body. Mechanism, patch site and contract: include/mh_tactscroll_export.h; the pacing arithmetic is
// libmh/tact/tact_scroll_pace.h (shared with libmh's tact_frame.cpp step 14).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_tactscroll_export.h"
#include "tact/tact_scroll_pace.h"
#include "addr/mh_addrs.gen.h"    // mh::addr::_G_LLM_*
#include "addr/mh_calls.gen.h"    // mh::call::time_GetCurrentTime, llm_tact_view_shift_*
#include "state/region_runtime.h" // mh::state::ptr<> -- the arrow latches are RELOCATABLE regions
#include "hook/patch.h"           // patch_bytes_guarded
#include "hook/promoted.h"        // promoted_owner_of
#include "net_internal.h"         // seam_log; g_ini
#include "en_guard.h"             // EN-only build gate

namespace {

// llm_tact_frame, EN: the four direction arms, first instruction `CMP [cursor_x],0` (7 bytes) to the camera-follow
// check the block falls through to.
constexpr uintptr_t SITE                  = 0x0042a121u;
constexpr uintptr_t SITE_NEXT             = 0x0042a280u; // CMP dword [0x00558c24],1 (cam_follow_selection)
constexpr int       SITE_LEN              = 7;
const uint8_t       SITE_EXPECT[SITE_LEN] = {0x83, 0x3D, 0xA1, 0x04, 0x52, 0x00, 0x00};

mh::tact::scroll_pacer g_pacer;
int                    g_px_s    = 0;
bool                   g_armed   = false;
bool                   g_logged  = false; // the "tactical scroll: N px/s" line, once per mission (see below)
double                 g_log_clk = 0.0;

template <class T>
T &at(uintptr_t a) { return *reinterpret_cast<T *>(a); }

// The block's four arms, one step each, with the original clamp and shift-helper calls unchanged.
void step_left() {
    at<int32_t>(mh::addr::_G_LLM_MAP_CAM_COL) -= 1;
    if (at<int32_t>(mh::addr::_G_LLM_MAP_CAM_COL) < 0) at<int32_t>(mh::addr::_G_LLM_MAP_CAM_COL) = 0;
    else mh::call::llm_tact_view_shift_col_dec();
}
void step_up() {
    at<int32_t>(mh::addr::_G_LLM_MAP_CAM_ROW) -= 1;
    if (at<int32_t>(mh::addr::_G_LLM_MAP_CAM_ROW) < 0) at<int32_t>(mh::addr::_G_LLM_MAP_CAM_ROW) = 0;
    else mh::call::llm_tact_view_shift_row_dec();
}
void step_right() {
    at<int32_t>(mh::addr::_G_LLM_MAP_CAM_COL) += 1;
    const int32_t bound = at<int32_t>(mh::addr::_G_LLM_TACT_MAP_WIDTH_CACHE) - at<int32_t>(mh::addr::G_WIN_W) / 32;
    if (bound < at<int32_t>(mh::addr::_G_LLM_MAP_CAM_COL)) at<int32_t>(mh::addr::_G_LLM_MAP_CAM_COL) = bound;
    else mh::call::llm_tact_view_shift_col_inc();
}
void step_down() {
    at<int32_t>(mh::addr::_G_LLM_MAP_CAM_ROW) += 1;
    const int32_t bound = at<int32_t>(mh::addr::_G_LLM_TACT_MAP_HEIGHT_CACHE) - at<int32_t>(mh::addr::WindowHeight) / 24;
    if (bound < at<int32_t>(mh::addr::_G_LLM_MAP_CAM_ROW)) at<int32_t>(mh::addr::_G_LLM_MAP_CAM_ROW) = bound;
    else mh::call::llm_tact_view_shift_row_inc();
}

// Replaces 0x0042a121..0x0042a27f. Reached only with no mouse button down and the mine-blast step at 0.
void __cdecl tact_scroll_paced() {
    const double now = mh::call::time_GetCurrentTime();
    // One line per mission: the first paced frame after a gap in the calls (the game clock restarts or the
    // block was not reached for a while) is a tactical entry as far as this seam can tell.
    if (!g_logged || now < g_log_clk || now - g_log_clk > 5.0) {
        char m[96];
        wsprintfA(m, "; tactical scroll: %d px/s (game clock)\n", g_px_s);
        seam_log(m);
        g_logged = true;
    }
    g_log_clk = now;

    const bool act_left  = at<int32_t>(mh::addr::_G_LLM_CURSOR_X) == 0 || *mh::state::ptr<int32_t>(mh::state::RID_TACT_CAM_SCROLL_LEFT_HELD) == 1;
    const bool act_up    = at<int32_t>(mh::addr::_G_LLM_CURSOR_Y) == 0 || *mh::state::ptr<int32_t>(mh::state::RID_TACT_CAM_SCROLL_UP_HELD) == 1;
    const bool act_right = at<int32_t>(mh::addr::WindowWidth) - 1 == at<int32_t>(mh::addr::_G_LLM_CURSOR_X) ||
                           *mh::state::ptr<int32_t>(mh::state::RID_TACT_CAM_SCROLL_RIGHT_HELD) == 1;
    const bool act_down = at<int32_t>(mh::addr::WindowHeight) - 1 == at<int32_t>(mh::addr::_G_LLM_CURSOR_Y) ||
                          *mh::state::ptr<int32_t>(mh::state::RID_TACT_CAM_SCROLL_DOWN_HELD) == 1;

    using namespace mh::tact;
    g_pacer.begin_frame(now);
    const int nl = g_pacer.steps(SCROLL_LEFT, act_left, now, g_px_s);
    const int nu = g_pacer.steps(SCROLL_UP, act_up, now, g_px_s);
    const int nr = g_pacer.steps(SCROLL_RIGHT, act_right, now, g_px_s);
    const int nd = g_pacer.steps(SCROLL_DOWN, act_down, now, g_px_s);
    for (int i = 0; i < nl; ++i) step_left();
    for (int i = 0; i < nu; ++i) step_up();
    for (int i = 0; i < nr; ++i) step_right();
    for (int i = 0; i < nd; ++i) step_down();
}

// clang-format off
__declspec(naked) void tact_scroll_thunk() {
    __asm {
        pushad
        pushfd
        cld
        call tact_scroll_paced
        popfd
        popad
        push 0x0042a280                     // SITE_NEXT: the camera-follow check
        ret
    }
}
// clang-format on

} // namespace

extern "C" int MH_TactScroll_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    int px = mh_ini_get_int("tactical", "scroll_px_s", mh::tact::kScrollDefaultPxS, g_ini);
    if (px < 0) px = 0;
    if (px > 100000) px = 100000;
    g_px_s = px;
    if (px == 0) {
        seam_log("; tactical scroll: retail per-frame ([tactical] scroll_px_s=0) -- one tile per rendered frame, fps-dependent\n");
        return 0;
    }
    if (mh::hook::promoted_owner_of(SITE)) {
        seam_log("; tactical scroll: splice DISPLACED (llm_tact_frame is promoted; libmh's frame twin carries it)\n");
        return 0;
    }
    uint8_t repl[SITE_LEN];
    repl[0]     = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)&tact_scroll_thunk - (SITE + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    memset(repl + 5, 0x90, SITE_LEN - 5);
    g_armed = mh::hook::patch_bytes_guarded(SITE, SITE_EXPECT, repl, SITE_LEN);
    char m[200];
    wsprintfA(m, "; tactical scroll: %d px/s (game clock) -- splice %s at %08X\n", px,
              g_armed ? "armed" : "NOT armed (bytes differ from the expected CMP [cursor_x],0)", (unsigned)SITE);
    seam_log(m);
    return g_armed ? 1 : 0;
}
