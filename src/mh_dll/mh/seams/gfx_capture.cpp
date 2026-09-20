//
// gfx_capture.cpp -- UI capture harness Phase 1: dump the rendered frame.
//
// Hooks llm_gfx_present_flip (0x42644a) -- the shared "push composed frame to screen" primitive, called
// once per presented frame from every screen's frame fn (menu / lobby / in-game). At its ENTRY the frame
// is fully composed INCLUDING the menu cursor (llm_frame_present draws the cursor into the framebuffer
// before calling this) and the back buffer is still locked, so _G_LLM_FRAMEBUFFER is a valid RGB565 image.
// On a trigger we copy it out to a 24-bit BMP; tools/bmp_to_png.py converts for viewing.
//
// Trigger (prototype): F12 (rising edge) grabs one frame; [capture] frames=N in mh_net.ini auto-grabs the
// first N presented frames (headless). Read-only w.r.t. game state; determinism-neutral.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

#include "include/mh_capture_export.h"
#include "include/mh_run_context.h" // MH_RunDir
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / GetAsyncKeyState

namespace {

constexpr uintptr_t ADDR_FB    = mh::addr::_G_LLM_FRAMEBUFFER; // ptr to locked RGB565 bits
constexpr uintptr_t ADDR_PITCH = mh::addr::_G_LLM_FB_PITCH;    // bytes/row
constexpr uintptr_t ADDR_W     = mh::addr::WindowWidth;
constexpr uintptr_t ADDR_H     = mh::addr::WindowHeight;

int           g_seq        = 0;
int           g_burst      = 0; // [capture] frames=N countdown (auto-grab the first N frames)
int           g_every      = 0; // [capture] every=K -- grab 1 frame per K presented (0 = off)
long          g_framecount = 0;
bool          g_prev_f12   = false; // F12 rising-edge latch
constexpr int CAP_MAX      = 60;    // hard cap on auto-captured files (runaway guard)

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
    HANDLE h = CreateFileA(g_clog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, line, lstrlenA(line), &w, nullptr);
    CloseHandle(h);
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
    if (g_burst > 0) {
        fire = true;
        --g_burst;
    }
    if (g_every > 0 && (g_framecount % g_every) == 0 && g_seq < CAP_MAX) fire = true;
    ++g_framecount;
    if (fire) capture_frame(nullptr);
}

// On-demand named capture (UI-script interpreter): grab the current frame to capture_<name>.bmp. MUST be
// called at present-flip ENTRY (the on_present path) -- same validity window as the periodic capture.
extern "C" void MH_Capture_Shot(const char *name) { capture_frame(name); }

extern "C" int MH_Capture_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);
    g_burst = GetPrivateProfileIntA("capture", "frames", 0, ini); // N>0 -> auto-grab first N frames
    g_every = GetPrivateProfileIntA("capture", "every", 0, ini);  // K>0 -> grab 1 frame per K presented
    cap_log("; capture enabled (F12=grab, [capture] frames=%d every=%d) -- via the lockstep present hook",
            g_burst, g_every);
    return 1;
}
