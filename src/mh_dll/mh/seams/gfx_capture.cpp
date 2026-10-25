//
// gfx_capture.cpp -- UI capture harness Phase 1: dump the rendered frame.
//
// Hooks llm_gfx_present_flip (0x42644a) -- the shared "push composed frame to screen" primitive, called
// once per presented frame from every screen's frame fn (menu / lobby / in-game). At its ENTRY the frame
// is fully composed INCLUDING the menu cursor (llm_frame_present draws the cursor into the framebuffer
// before calling this) and the back buffer is still locked, so _G_LLM_FRAMEBUFFER is a valid RGB565 image.
// On a trigger we copy it out to a 24-bit BMP; tools/bmp_to_png.py converts for viewing.
//
// Trigger: F12 (rising edge) grabs one frame; the UI-script `capture` op grabs a named one
// (MH_Capture_Shot). Read-only w.r.t. game state; determinism-neutral.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

#include "gfx/ddraw_own.h"       // RES4: owned_present_layout (the presented image)
#include "gfx/ui_layout.h"       // RES4: the CPU compositor
#include "gfx/present_backend.h" // RES23-S1: build_present_view
#include "include/mh_capture_export.h"
#include "include/mh_run_context.h" // MH_RunDir
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / GetAsyncKeyState

namespace {

constexpr uintptr_t ADDR_FB    = mh::addr::_G_LLM_FRAMEBUFFER; // ptr to locked RGB565 bits
constexpr uintptr_t ADDR_PITCH = mh::addr::_G_LLM_FB_PITCH;    // bytes/row
constexpr uintptr_t ADDR_W     = mh::addr::WindowWidth;
constexpr uintptr_t ADDR_H     = mh::addr::WindowHeight;

int  g_seq      = 0;
bool g_prev_f12 = false; // F12 rising-edge latch

char g_clog[MAX_PATH];
// SES1: PROCESS-scoped. capture_*.bmp and this log are the RIG's channel -- tools/ui_test.py
// discovers ONE run directory at launch and pulls the captures from it, so a capture that moved
// into a session directory mid-scenario would simply not be found.
unsigned long g_clog_gen = 0;

void cap_log(const char *fmt, ...) {
    mh_proc_path(g_clog, MAX_PATH, "%smh_capture.log", &g_clog_gen);
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_clog, line, lstrlenA(line));
}

// Dump the current RGB565 framebuffer to a bottom-up 24-bit BMP (self-describing, zero-dep). If `name`
// is non-null the file is capture_<name>.bmp (used by the UI-script interpreter for per-screen shots);
// otherwise it's the numbered capture_NNN.bmp of the periodic/F12 path.
void capture_frame(const char *name) {
    const uint16_t *fb    = *(const uint16_t **)ADDR_FB;
    int             pitch = *(const int *)ADDR_PITCH;
    int             w     = *(const int *)ADDR_W;
    int             h     = *(const int *)ADDR_H;
    if (!fb || w <= 0 || h <= 0 || w > 4096 || h > 4096 || pitch < w * 2) {
        cap_log("; capture SKIPPED (fb=%p w=%d h=%d pitch=%d -- unexpected)", (void *)fb, w, h, pitch);
        return;
    }
    const int      rowstride = (w * 3 + 3) & ~3; // BMP rows padded to 4 bytes
    const int      datasize  = rowstride * h;
    const int      filesize  = 54 + datasize;
    unsigned char *out       = (unsigned char *)malloc((size_t)filesize);
    if (!out) return;
    memset(out, 0, 54);
    // BITMAPFILEHEADER (14)
    out[0]                  = 'B';
    out[1]                  = 'M';
    *(uint32_t *)(out + 2)  = (uint32_t)filesize;
    *(uint32_t *)(out + 10) = 54; // pixel-data offset
    // BITMAPINFOHEADER (40)
    *(uint32_t *)(out + 14) = 40;
    *(int32_t *)(out + 18)  = w;
    *(int32_t *)(out + 22)  = h; // positive -> bottom-up
    *(uint16_t *)(out + 26) = 1;
    *(uint16_t *)(out + 28) = 24;
    *(uint32_t *)(out + 34) = (uint32_t)datasize;
    // pixels: BMP is bottom-up, so output row o comes from source row (h-1-o)
    for (int o = 0; o < h; ++o) {
        const uint16_t *src = (const uint16_t *)((const unsigned char *)fb + (size_t)(h - 1 - o) * pitch);
        unsigned char  *dst = out + 54 + (size_t)o * rowstride;
        for (int x = 0; x < w; ++x) {
            uint16_t px = src[x];
            int      r5 = (px >> 11) & 0x1f, g6 = (px >> 5) & 0x3f, b5 = px & 0x1f;
            dst[x * 3 + 0] = (unsigned char)((b5 << 3) | (b5 >> 2)); // B
            dst[x * 3 + 1] = (unsigned char)((g6 << 2) | (g6 >> 4)); // G
            dst[x * 3 + 2] = (unsigned char)((r5 << 3) | (r5 >> 2)); // R
        }
    }
    char path[MAX_PATH];
    // SES1: MH_ProcessDir, not MH_RunDir -- captures are pulled by the runner from the ONE directory
    // it discovered at launch (tools/ui_test.py pull_local_captures), so they must not follow a session.
    if (name)
        wsprintfA(path, "%scapture_%s.bmp", MH_ProcessDir(), name);
    else
        wsprintfA(path, "%scapture_%03d.bmp", MH_ProcessDir(), g_seq);
    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(hf, out, (DWORD)filesize, &wr, nullptr);
        CloseHandle(hf);
        if (name)
            cap_log("; capture_%s.bmp written (%dx%d, pitch=%d)", name, w, h, pitch);
        else
            cap_log("; capture_%03d.bmp written (%dx%d, pitch=%d)", g_seq, w, h, pitch);
        ++g_seq;
    }
    free(out);
}

// RES4 `pcapture <name>`: the image the PRESENTER shows, not the game framebuffer. The game frame is composed on
// the CPU into the quad list the presenter draws (gfx/ui_layout.h: map 1:1, side panel and strip scaled, bars
// black), so a headless lane -- the null backend presents nothing -- can verify quad placement. Point sampling by
// the pixel-centre rule whatever [video] filter says. At ui_scale=100 it is the fitted frame (or, with no owned
// device, the frame itself). Screen = the window's client area, or the dev key [video] ui_screen=WxH.
void capture_presented(const char *name) {
    const uint16_t *fb    = *(const uint16_t **)ADDR_FB;
    int             pitch = *(const int *)ADDR_PITCH;
    int             w     = *(const int *)ADDR_W;
    int             h     = *(const int *)ADDR_H;
    if (!fb || w <= 0 || h <= 0 || w > 4096 || h > 4096 || pitch < w * 2) {
        cap_log("; pcapture SKIPPED (fb=%p w=%d h=%d pitch=%d -- unexpected)", (void *)fb, w, h, pitch);
        return;
    }
    mh::gfx::uilayout::layout lay;
    if (!mh::gfx::owned_present_layout(w, h, &lay)) {
        mh::gfx::present_view v; // no owned device: the frame itself, one fitted quad
        mh::gfx::build_present_view(v, w, h, w, h, 100, false, nullptr, 0, 0);
        lay = v.layout;
    }
    const int sw = lay.screen_w, sh = lay.screen_h;
    if (sw <= 0 || sh <= 0 || sw > 8192 || sh > 8192) {
        cap_log("; pcapture SKIPPED (screen %dx%d)", sw, sh);
        return;
    }
    const int      rowstride = (sw * 3 + 3) & ~3;
    unsigned char *top       = (unsigned char *)malloc((size_t)rowstride * sh);
    if (!top) return;
    mh::gfx::uilayout::compose_bgr24(lay, fb, pitch, top, rowstride);
    // RES4 stage 6: the dialog quads, bottom to top (frame rect -> screen rect), read back by tools/check_res4_dialog.py
    for (int i = 0; i < lay.ndialogs; ++i)
        cap_log("; pcapture dialog %d %d %d %d screen %d %d %d %d", lay.dialog_src[i].x0, lay.dialog_src[i].y0, lay.dialog_src[i].x1, lay.dialog_src[i].y1,
                lay.dialog_dst[i].x0, lay.dialog_dst[i].y0, lay.dialog_dst[i].x1, lay.dialog_dst[i].y1);
    if (!lay.ndialogs) cap_log("; pcapture dialog none");
    // RES23-S5: the overlay list -- the scaled notice, the translucent dialog layers (stage 6b), then the owned scaled cursor (stage 5), over the quads
    mh::gfx::overlay_list ovl;
    int                   bbox[4] = {0, 0, -1, -1};
    mh::gfx::owned_overlay_list(w, h, &ovl, bbox);
    int ndl = 0;
    for (int i = 0; i < ovl.n; ++i) {
        mh::gfx::uilayout::blend_cursor_bgr24(ovl.item[i].ov, top, rowstride, sw, sh);
        if (ovl.item[i].slot != mh::gfx::overlay_slot::cursor && ovl.item[i].slot != mh::gfx::overlay_slot::notice) ++ndl;
    }
    if (const mh::gfx::overlay_item *n = ovl.find(mh::gfx::overlay_slot::notice))
        cap_log("; pcapture notice rect %d %d %d %d", n->ov.x, n->ov.y, n->ov.x + n->ov.w, n->ov.y + n->ov.h);
    else
        cap_log("; pcapture notice none");
    cap_log("; pcapture dialog layers %d", ndl);
    if (const mh::gfx::overlay_item *c = ovl.find(mh::gfx::overlay_slot::cursor))
        cap_log("; pcapture cursor rect %d %d %d %d sprite %d %d %d %d", c->ov.x, c->ov.y, c->ov.x + c->ov.w, c->ov.y + c->ov.h, bbox[0], bbox[1], bbox[2], bbox[3]);
    else
        cap_log("; pcapture cursor rect none");
    const int      datasize = rowstride * sh, filesize = 54 + datasize;
    unsigned char *out = (unsigned char *)malloc((size_t)filesize);
    if (!out) {
        free(top);
        return;
    }
    memset(out, 0, 54);
    out[0]                  = 'B';
    out[1]                  = 'M';
    *(uint32_t *)(out + 2)  = (uint32_t)filesize;
    *(uint32_t *)(out + 10) = 54;
    *(uint32_t *)(out + 14) = 40;
    *(int32_t *)(out + 18)  = sw;
    *(int32_t *)(out + 22)  = sh;
    *(uint16_t *)(out + 26) = 1;
    *(uint16_t *)(out + 28) = 24;
    *(uint32_t *)(out + 34) = (uint32_t)datasize;
    for (int o = 0; o < sh; ++o) memcpy(out + 54 + (size_t)o * rowstride, top + (size_t)(sh - 1 - o) * rowstride, (size_t)rowstride);
    char path[MAX_PATH];
    wsprintfA(path, "%scapture_%s.bmp", MH_ProcessDir(), name);
    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(hf, out, (DWORD)filesize, &wr, nullptr);
        CloseHandle(hf);
        cap_log("; capture_%s.bmp written (presented image %dx%d from a %dx%d frame, ui_scale %d%%, %d quad%s)", name, sw, sh, w, h,
                lay.pct, lay.nquads, lay.nquads == 1 ? "" : "s");
    }
    free(out);
    free(top);
}

} // namespace

// Per-present callback -- called from the lockstep present hook (net_lockstep on_present), which runs at
// present-flip ENTRY (frame fully composed INCLUDING the cursor, back buffer still locked). We piggyback
// there instead of installing our own hook, because net_lockstep already owns the present_flip hook (a
// second hook would fight the same 8-byte prologue). Checks the trigger, grabs a frame if fired.
extern "C" void MH_Capture_OnPresent(void) {
    bool fire = false;
    bool f12  = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12 && !g_prev_f12) fire = true; // rising edge -> one grab
    g_prev_f12 = f12;
    if (fire) capture_frame(nullptr);
}

// On-demand named capture (UI-script interpreter): grab the current frame to capture_<name>.bmp. MUST be
// called at present-flip ENTRY (the on_present path) -- same validity window as the periodic capture.
extern "C" void MH_Capture_Shot(const char *name) { capture_frame(name); }

// RES4: the PRESENTED image (quad list composed on the CPU) -- see capture_presented. Same validity window.
extern "C" void MH_Capture_ShotPresented(const char *name) { capture_presented(name); }

// RES23-S0 `bcapture`: request (the write happens at the next present, in ddraw_own.cpp present_surface).
extern "C" void MH_Capture_ShotBackbuffer(const char *name) { mh::gfx::owned_request_bcapture(name); }

extern "C" void MH_Capture_WriteBgra32(const char *name, const unsigned int *px, int w, int h) {
    if (!px || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        cap_log("; bcapture %s SKIPPED (the presenter shows nothing here -- null backend or a failed read)", name);
        return;
    }
    const int      rowstride = (w * 3 + 3) & ~3;
    const int      datasize = rowstride * h, filesize = 54 + datasize;
    unsigned char *out = (unsigned char *)malloc((size_t)filesize);
    if (!out) return;
    memset(out, 0, 54);
    out[0]                  = 'B';
    out[1]                  = 'M';
    *(uint32_t *)(out + 2)  = (uint32_t)filesize;
    *(uint32_t *)(out + 10) = 54;
    *(uint32_t *)(out + 14) = 40;
    *(int32_t *)(out + 18)  = w;
    *(int32_t *)(out + 22)  = h; // bottom-up
    *(uint16_t *)(out + 26) = 1;
    *(uint16_t *)(out + 28) = 24;
    *(uint32_t *)(out + 34) = (uint32_t)datasize;
    for (int o = 0; o < h; ++o) {
        const unsigned int *src = px + (size_t)(h - 1 - o) * w;
        unsigned char      *dst = out + 54 + (size_t)o * rowstride;
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = (unsigned char)(src[x] & 0xff);         // B
            dst[x * 3 + 1] = (unsigned char)((src[x] >> 8) & 0xff);  // G
            dst[x * 3 + 2] = (unsigned char)((src[x] >> 16) & 0xff); // R
        }
    }
    char path[MAX_PATH];
    wsprintfA(path, "%scapture_%s__b.bmp", MH_ProcessDir(), name);
    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(hf, out, (DWORD)filesize, &wr, nullptr);
        CloseHandle(hf);
        cap_log("; capture_%s__b.bmp written (presenter output %dx%d)", name, w, h);
    }
    free(out);
}

extern "C" int MH_Capture_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    cap_log("; capture enabled (F12=grab) -- via the lockstep present hook");
    return 1;
}
