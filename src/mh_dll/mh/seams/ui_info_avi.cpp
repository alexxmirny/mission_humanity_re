// seams/ui_info_avi.cpp -- mp:X2g: an entity info screen whose AVI will not open no longer divides by
// zero. Mechanism, evidence and knobs: include/mh_infoavi_export.h. The arithmetic the guarded tick
// runs is in ui_info_avi.h (selftested by `net_selftest.exe infoavitest`).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "include/mh_infoavi_export.h"
#include "addr/mh_addrs.gen.h" // mh::addr::_G_LLM_GAME_MODE
#include "addr/mh_calls.gen.h" // mh::call::llm_ui_avi_open / _decode_frame / gfx lock+unlock / cursor / flip
#include "config/ini_read.h"   // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "hook/detour.h"       // install_jmp + WATCOM_PROLOGUE
#include "hook/patch.h"        // patch_bytes_guarded
#include "hook/promoted.h"     // promoted_owner_of
#include "net_internal.h"      // seam_log; g_ini
#include "en_guard.h"          // EN-only build gate
#include "ui_info_avi.h"       // the pure tick arithmetic + the test-knob matcher

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

// ---- the addresses (EN, read from the disassembly 2026-09-26) -----------------------------------
//
// Raw VAs with provenance rather than addr-manifest entries: these are UI-only globals and code sites,
// and a manifest data entry renumbers the harness hash registry (the fixture-fingerprint trap).
constexpr uintptr_t ADDR_MEDIA_TICK = 0x004caa46u; // llm_ui_info_media_frame_tick (the replaced body)

// llm_ui_entity_info_screen_open (0x004cad31):
constexpr uintptr_t ADDR_AVI_OPEN_CALL1  = 0x004cb1edu; // CALL llm_ui_avi_open  (res\<clip>)
constexpr uintptr_t ADDR_AVI_FAIL_BRANCH = 0x004cb1fau; // CALL llm_snd_cd_stop  (the failure branch)
constexpr uintptr_t ADDR_AVI_OPEN_CALL2  = 0x004cb229u; // CALL llm_ui_avi_open  (%sres\<clip>, CD loop)
constexpr uintptr_t ADDR_OPEN_SUCCESS    = 0x004cb274u; // XOR EBX,EBX -- retail's "clip is open" path
constexpr uintptr_t ADDR_OPEN_EPILOGUE   = 0x004cb3e9u; // LEA ESP,[EBP-0x10]; POP EDI..EBP; RET

const uint8_t CALL1_EXPECT[5]  = {0xE8, 0x5C, 0x1E, 0x00, 0x00}; // -> 0x004cd04e
const uint8_t FAIL_EXPECT[5]   = {0xE8, 0xAE, 0xC5, 0xFF, 0xFF}; // -> 0x004c77ad
const uint8_t CALL2_EXPECT[5]  = {0xE8, 0x20, 0x1E, 0x00, 0x00}; // -> 0x004cd04e
constexpr int FRAME_PATH_OFF   = -0x12c;                         // the open's char[260] path buffer, [EBP-0x12c]
constexpr int FRAME_CLIP_OFF   = -0x18;                          // the open's INFO_FLC name pointer (local_1c), [EBP-0x18]
constexpr int FRAME_PATH_BYTES = 260;

// _G_LLM_UI_INFO_MEDIA_CTX (llm_ui_info_media_ctx, 0x0065f82f) and its neighbours:
constexpr uintptr_t ADDR_MEDIA_CTX       = 0x0065f82fu;
constexpr uintptr_t ADDR_STREAM_LENGTH   = 0x0065f993u; // video_stream.stream_length_time (ctx+0x164)
constexpr uintptr_t ADDR_STREAM_WRAPMARK = 0x0065f99bu; // video_stream +0xb0, set -1 on loop wrap
constexpr uintptr_t ADDR_LOCKED_PTR      = 0x0065fa27u; // ctx.locked_surface_ptr
constexpr uintptr_t ADDR_PLAYBACK_POS    = 0x0065fa43u; // ctx.playback_pos_ms
constexpr uintptr_t ADDR_LAST_TICK       = 0x0065fa47u; // ctx.last_tick_ms
constexpr uintptr_t ADDR_SURFACE         = 0x0065fa4fu; // ctx.ddraw_surface (IDirectDrawSurface*)
constexpr uintptr_t ADDR_SURFACE_DESC    = 0x0065f69fu; // _G_LLM_UI_INFO_DDRAW_SURFACE_DESC
constexpr uintptr_t ADDR_SURFACE_LPSURF  = 0x0065f6c3u; // .lpSurface (desc+0x24)
constexpr uintptr_t ADDR_BACK_SURFACE    = 0x007100a0u; // _G_LLM_GFX_DISPLAY_MODE_DESC.surf_back
constexpr uintptr_t ADDR_BLT_X           = 0x0065431eu; // the media widget's draw x
constexpr uintptr_t ADDR_BLT_Y           = 0x00654322u; // ...and y
constexpr uintptr_t ADDR_DLG_FLAGS_BYTE  = 0x0065434eu; // _G_LLM_DLG_STATE_FLAGS, low byte
constexpr uintptr_t ADDR_FRAME_DELTA     = 0x0065447eu; // _G_LLM_UI_INFO_SCREEN_FRAME_DELTA_MS
constexpr uintptr_t ADDR_TICKS_MS        = 0x00e654f8u; // _G_LLM_TIME_TICKS_MS (llm_time_get_ticks_ms)
constexpr uintptr_t ADDR_CD_DATA_PATH    = 0x00603f78u; // G_CD_DATA_PATH (the exe dir on a no-CD install)

// IDirectDrawSurface vtable slots the tick uses (the offsets retail calls through).
using fn_lock            = long(__stdcall *)(void *self, RECT *rc, void *desc, unsigned long flags, void *ev);
using fn_unlock          = long(__stdcall *)(void *self, void *rc);
using fn_bltfast         = long(__stdcall *)(void *self, unsigned long x, unsigned long y, void *src, RECT *rc,
                                     unsigned long trans);
constexpr int VT_BLTFAST = 0x1c / 4, VT_LOCK = 0x64 / 4, VT_UNLOCK = 0x80 / 4;

template <class T>
T           &at(uintptr_t a) { return *reinterpret_cast<T *>(a); }
void *const *vtbl(void *com) { return *reinterpret_cast<void *const *const *>(com); }

bool g_tick_guarded     = false;
bool g_fail_spliced     = false;
bool g_fallback_on      = true; // [net] info_avi_fallback (default 1)
char g_test_absent[128] = {0};  // [net] info_avi_test_absent

long g_tick_skipped = 0; // frames that drew no video because there was none to draw
long g_no_video     = 0; // info screens opened without video

// ---- 1. the guarded media tick -------------------------------------------------------------------

void media_tick() {
    void         *surf = at<void *>(ADDR_SURFACE);
    const int32_t len  = at<int32_t>(ADDR_STREAM_LENGTH);
    const bool    vid  = mh::info_avi::video_on(surf != nullptr, len);
    if (vid) {
        ((fn_lock)vtbl(surf)[VT_LOCK])(surf, nullptr, (void *)ADDR_SURFACE_DESC, 1, nullptr);
        at<uint32_t>(ADDR_LOCKED_PTR) = at<uint32_t>(ADDR_SURFACE_LPSURF);
        mh::call::llm_ui_avi_decode_frame((void *)ADDR_MEDIA_CTX, at<int32_t>(ADDR_PLAYBACK_POS));
        ((fn_unlock)vtbl(surf)[VT_UNLOCK])(surf, nullptr);
    } else {
        ++g_tick_skipped;
    }
    mh::call::llm_gfx_surface_unlock(2);
    if (vid) {
        void *back = at<void *>(ADDR_BACK_SURFACE);
        ((fn_bltfast)vtbl(back)[VT_BLTFAST])(back, at<uint32_t>(ADDR_BLT_X), at<uint32_t>(ADDR_BLT_Y), surf,
                                             nullptr, 0x10 /* DDBLTFAST_WAIT */);
    }
    mh::call::llm_gfx_surface_lock(2);
    mh::call::llm_time_get_ticks_ms();
    const mh::info_avi::tick_step s = mh::info_avi::plan_tick(
        len, at<int32_t>(ADDR_PLAYBACK_POS), at<uint32_t>(ADDR_TICKS_MS), at<uint32_t>(ADDR_LAST_TICK));
    at<int32_t>(ADDR_PLAYBACK_POS) = s.pos;
    at<int32_t>(ADDR_FRAME_DELTA)  = s.delta; // the MP keepalive in llm_ui_paged_list_frame reads it
    at<uint32_t>(ADDR_LAST_TICK)   = at<uint32_t>(ADDR_TICKS_MS);
    if (s.wrapped) at<int32_t>(ADDR_STREAM_WRAPMARK) = -1;
    mh::call::llm_gfx_draw_cursor_menu();
    mh::call::llm_gfx_present_flip();
}

// A widget draw callback (called, never jumped to). Watcom callers expect every register but EAX
// preserved; the __cdecl body keeps EBX/ESI/EDI/EBP, so the thunk saves ECX/EDX.
// clang-format off
__declspec(naked) void media_tick_thunk() {
    __asm {
        push ecx
        push edx
        call media_tick
        pop  edx
        pop  ecx
        ret
    }
}
// clang-format on

// ---- the test knob: the info screen's clip opens, redirected to a file nobody ships ---------------

int32_t open_clip(void *ctx, char *path) {
    if (mh::info_avi::name_listed(g_test_absent, path)) {
        char line[MAX_PATH + 160];
        wsprintfA(line, "; [ui] uitest info_avi_test_absent: %.200s opened as res\\NOT.AVI (mp:X2g)\n",
                  path);
        seam_log(line);
        char absent[] = "res\\NOT.AVI"; // retail INIT.CFG's own never-shipped clip (Chatka/Baza/Rakieta)
        return mh::call::llm_ui_avi_open(ctx, absent);
    }
    return mh::call::llm_ui_avi_open(ctx, path);
}

int32_t __cdecl open_clip_cdecl(void *ctx, char *path) { return open_clip(ctx, path); }

// Stands in for `CALL llm_ui_avi_open` at the two call sites (only while the test knob is set):
// __watcall(EAX = ctx, EDX = path) -> EAX, everything else preserved.
// clang-format off
__declspec(naked) void open_clip_call_thunk() {
    __asm {
        push ecx
        push edx
        push edx                    // path
        push eax                    // ctx
        call open_clip_cdecl
        add  esp, 8
        pop  edx
        pop  ecx
        ret
    }
}
// clang-format on

// ---- 2. the failure branch -----------------------------------------------------------------------

// Retail's success tail (0x004cb39c..0x004cb3df) minus the video half: the screen is the ordinary
// mode-4 info screen, it simply has no clip.
void enter_without_video() {
    at<uint8_t>(mh::addr::_G_LLM_GAME_MODE) = 4;
    mh::call::llm_time_get_ticks_ms();
    at<uint32_t>(ADDR_LAST_TICK)     = at<uint32_t>(ADDR_TICKS_MS);
    at<int32_t>(ADDR_PLAYBACK_POS)   = 0;
    uint8_t f                        = at<uint8_t>(ADDR_DLG_FLAGS_BYTE);
    f                                = (uint8_t)((f | 0x03u) & ~0x10u); // OR 2; bit0 := 1; AND ~0x10
    at<uint8_t>(ADDR_DLG_FLAGS_BYTE) = f;
    at<int32_t>(ADDR_FRAME_DELTA)    = 1000;
}

// `path` is the open's own buffer (it holds "res\<clip>", the open that just failed); `clip` is the
// INFO_FLC name. Returns 1 when the CD-path retry opened the clip (resume retail's success path), 0
// when the screen was entered without video (leave through the epilogue).
int32_t __cdecl on_open_failed(char *path, const char *clip) {
    char first[FRAME_PATH_BYTES];
    lstrcpynA(first, path, sizeof(first));
    // Retail's one useful step, kept: stop CD audio (the drive may be the source) and try the same
    // clip under G_CD_DATA_PATH -- ONCE, and without the insert-CD dialog.
    mh::call::llm_snd_cd_stop();
    const char *cd = (const char *)ADDR_CD_DATA_PATH;
    if (cd[0] && clip) {
        char   alt[FRAME_PATH_BYTES + 16];
        size_t n = 0;
        for (const char *p = cd; *p && n < sizeof(alt) - 1; ++p) alt[n++] = *p;
        for (const char *p = "res\\"; *p && n < sizeof(alt) - 1; ++p) alt[n++] = *p;
        for (const char *p = clip; *p && n < sizeof(alt) - 1; ++p) alt[n++] = *p;
        alt[n] = 0;
        lstrcpynA(path, alt, FRAME_PATH_BYTES);
        if (open_clip((void *)ADDR_MEDIA_CTX, path) == 0) {
            char line[2 * MAX_PATH + 96];
            wsprintfA(line, "; [ui] info screen clip %.200s opened from %.260s (mp:X2g)\n", first, path);
            seam_log(line);
            return 1;
        }
    }
    ++g_no_video;
    char line[2 * MAX_PATH + 256];
    wsprintfA(line,
              "; [ui] info screen clip missing: %.200s%s%.260s did not open -- shown WITHOUT video; "
              "retail's insert-CD prompt and error box are NOT raised (mp:X2g)\n",
              first, cd[0] ? " and " : "", cd[0] ? path : "");
    seam_log(line);
    enter_without_video();
    return 0;
}

const uintptr_t g_success_target  = ADDR_OPEN_SUCCESS;
const uintptr_t g_epilogue_target = ADDR_OPEN_EPILOGUE;

// JMPed to from 0x004cb1fa inside llm_ui_entity_info_screen_open's frame (EBP is ITS frame). Nothing
// is pending on the stack there (the sprintf's args were popped at 0x004cb1df).
// clang-format off
__declspec(naked) void open_failed_thunk() {
    __asm {
        push ecx
        push edx
        push dword ptr [ebp - 0x18]   // FRAME_CLIP_OFF
        lea  eax, [ebp - 0x12c]       // FRAME_PATH_OFF
        push eax
        call on_open_failed
        add  esp, 8
        pop  edx
        pop  ecx
        test eax, eax
        jz   no_video
        jmp  dword ptr [g_success_target]
    no_video:
        jmp  dword ptr [g_epilogue_target]
    }
}
// clang-format on

bool splice(uintptr_t site, const uint8_t *expect, uint8_t opcode, const void *dest, const char *what) {
    if (mh::hook::promoted_owner_of(site)) {
        char m[200];
        wsprintfA(m, "; [ui] X2g %s DISPLACED: the owner of %08X is promoted this run\n", what, (unsigned)site);
        seam_log(m);
        return false;
    }
    uint8_t repl[5];
    repl[0]     = opcode;
    int32_t rel = (int32_t)((uintptr_t)dest - (site + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    return mh::hook::patch_bytes_guarded(site, expect, repl, 5);
}

} // namespace

extern "C" int MH_InfoAvi_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    int st = (g_tick_guarded ? 1 : 0) | (g_fail_spliced ? 2 : 0);
    if (st) return st;
    g_fallback_on = GetPrivateProfileIntA("net", "info_avi_fallback", 1, g_ini) != 0;
    mh::config::read_ini_string("net", "info_avi_test_absent", "", g_test_absent, sizeof(g_test_absent),
                                g_ini);

    // `info_avi_fallback=0` is RETAIL, whole: neither write, so the repro arm reproduces the field
    // sequence (prompt, box, the IDIV) rather than half of it.
    if (g_fallback_on) {
        // 1. The tick guard. Changes nothing while a clip is open (the arithmetic and the calls are
        //    retail's), so it is safe on every screen, not only the failed ones.
        g_tick_guarded = mh::hook::install_jmp(ADDR_MEDIA_TICK, (const void *)media_tick_thunk,
                                               mh::hook::entry_claim::exclusive, "the X2g info media tick guard");
        // 2. The failure branch.
        g_fail_spliced = splice(ADDR_AVI_FAIL_BRANCH, FAIL_EXPECT, 0xE9, (const void *)open_failed_thunk,
                                "failure branch");
    }
    // 3. The test knob's two call sites.
    bool knob = false;
    if (g_test_absent[0]) {
        const bool a = splice(ADDR_AVI_OPEN_CALL1, CALL1_EXPECT, 0xE8, (const void *)open_clip_call_thunk,
                              "test-knob open (res)");
        const bool b = splice(ADDR_AVI_OPEN_CALL2, CALL2_EXPECT, 0xE8, (const void *)open_clip_call_thunk,
                              "test-knob open (CD path)");
        knob         = a && b;
    }
    char m[400];
    wsprintfA(m,
              "; [ui] info screen AVI guard: tick %s, open failure %s%s%s%s (mp:X2g)\n",
              !g_fallback_on   ? "RETAIL"
              : g_tick_guarded ? "guarded"
                               : "NOT guarded (install refused -- see the [interlock] line)",
              !g_fallback_on   ? "RETAIL ([net] info_avi_fallback=0: insert-CD prompt + error box, then the IDIV)"
              : g_fail_spliced ? "-> no-video screen"
                               : "NOT spliced (bytes differ at 004CB1FA; retail prompt + box kept)",
              g_test_absent[0] ? "; TEST info_avi_test_absent=" : "", g_test_absent[0] ? g_test_absent : "",
              g_test_absent[0] ? (knob ? " armed" : " NOT armed") : "");
    seam_log(m);
    return (g_tick_guarded ? 1 : 0) | (g_fail_spliced ? 2 : 0) | (knob ? 4 : 0);
}
