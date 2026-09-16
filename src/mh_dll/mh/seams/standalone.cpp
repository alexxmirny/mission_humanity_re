//
// standalone.cpp -- make a STOCK, unmodified mh.exe behave like the `build_focus` exe, from inside
// the DLL. See include/mh_standalone_export.h for the mechanism and the disc-check
// RE, and the injection design for why this exists at all.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>

#include "addr/mh_addrs.gen.h"
#include "addr/mh_export.gen.h"
#include "hook/detour.h"
#include "hook/patch.h"
#include "include/mh_standalone_export.h"

// net_seams.cpp; a free function at file scope. Declared rather than pulling in net_internal.h,
// which would drag the whole transport surface into a file that only wants one log line.
void seam_log(const char *s);

namespace {

// The MAX_PATH (0x104) buffer llm_cd_ensure_present loops on. Not in the generated address headers
// -- it is a plain data label, and those cover functions plus the globals the migration binds.
// Spelled here with the doc reference rather than added to a generated header for one use.
constexpr uintptr_t ADDR_CD_DATA_PATH = 0x00603f78u; // G_CD_DATA_PATH, the disc-check RE
constexpr unsigned  CD_DATA_PATH_SIZE = 0x104u;

constexpr uintptr_t ADDR_CD_LOCATE = mh::exp::addr_llm_cd_locate_and_open_audio;

// llm_wnd_on_activate's WA_INACTIVE block. The original `CMP dword[_G_LLM_GAME_RUNNING],0` becomes
// `JMP 0x004a0008` (the function's shared epilogue) + 2 NOP pad, so focus loss stops clearing
// GAME_RUNNING / pausing the clock / showing the cursor. Byte-for-byte the same edit
// src/patcher/run_without_focus_EN.mh.patch.json makes; rel32 0x22 is relative to this site.
constexpr uintptr_t ADDR_WND_ACTIVATE_INACTIVE = 0x004a04ceu;
constexpr uint8_t   FOCUS_EXPECT[]             = {0x83, 0x3d, 0xe8, 0x5f, 0x5d, 0x00, 0x00};
constexpr uint8_t   FOCUS_REPL[]               = {0xe9, 0x22, 0x00, 0x00, 0x00, 0x90, 0x90};

void *g_cd_tramp;

// ---- U21: the WM_DEVICECHANGE NULL-lParam crash -------------------------------------------------
//
// llm_wnd_proc forwards EVERY WM_DEVICECHANGE (0x219) to llm_wnd_on_devicechange, which begins
//
//     mov [ebp-0x20], eax     ; event_code  (wParam)
//     mov [ebp-0x1c], edx     ; broadcast_hdr (lParam)
//     ...
//     cmp dword ptr [eax+4], 2      <-- entry+0x26, the FIRST dereference
//
// with no NULL check and no wParam filter. Windows broadcasts DBT_DEVNODES_CHANGED (wParam 7,
// lParam 0) whenever any device node changes -- a USB device, an audio endpoint, a VM hot-plug --
// and the handler then reads address 4 and the process dies with no WER report on a box where WER
// is off. That is U21's "intermittent" crash: the trigger is an external event with no relation to
// anything the game is doing, which is exactly why it looked random and never reproduced.
//
// Measured 9 times across the rig (both VM peers and this dev box, most recently 2026-08-25) by
// reading the Application log -- see U21.
//
// The guard is a run-before detour that returns 0 for a NULL header. 0 is the right answer: the
// original returns nonzero only to DENY a volume removal it recognises, and a message with no
// header describes no volume. Everything non-NULL reaches the original untouched.
constexpr uintptr_t ADDR_WND_DEVICECHANGE = mh::addr::llm_wnd_on_devicechange;

void *g_dc_tramp;
long  g_dc_swallowed; // how many malformed broadcasts we ate -- read by the probe's log line

// __watcall(event_code EAX, broadcast_hdr EDX). Only those two registers are live at the entry
// (the callee-saved pushes happen after the stack-capacity call), so testing EDX here is safe and
// needs no save/restore: on the pass path every register is still exactly as the caller left it.
__declspec(naked) void devicechange_detour() {
    __asm {
        test edx, edx
        jnz  chain
        lock inc dword ptr [g_dc_swallowed]
        xor  eax, eax // no header -> no opinion; the message is handled
        ret
      chain:
        jmp dword ptr [g_dc_tramp] // stolen 8-byte prologue + jmp back to the original body
    }
}

// The probe. A separate thread rather than a frame hook: PostMessage from any thread queues to the
// window's own thread, which is how Windows delivers the real broadcast, and it needs no per-frame
// plumbing in a file that has none. One shot, then the thread exits.
DWORD WINAPI devicechange_probe(LPVOID ms) {
    Sleep((DWORD)(uintptr_t)ms);
    HWND h = *reinterpret_cast<HWND *>(mh::addr::hWnd_main);
    char b[192];
    // LOG BEFORE POSTING. With the guard off this message is expected to kill the process, so a
    // line written afterwards is a line that may never exist -- and "the probe never fired" and
    // "the probe fired and nothing happened" are the two readings that must not be confused.
    wsprintfA(b, "; compat: probe posting WM_DEVICECHANGE(wParam=7, lParam=0) to hwnd=%p%s\n",
              (void *)h, h ? "" : " -- NO WINDOW, nothing posted");
    seam_log(b);
    if (!h) return 0;
    PostMessageA(h, 0x219 /* WM_DEVICECHANGE */, 7 /* DBT_DEVNODES_CHANGED */, 0);
    Sleep(2000);
    wsprintfA(b, "; compat: 2 s after the probe the process is ALIVE; guard swallowed %ld\n",
              g_dc_swallowed);
    seam_log(b);
    return 0;
}

// Runs after the original scan. Anything that already found a disc -- or an exe that carries the
// cave patch -- leaves the buffer non-empty, and we do nothing at all.
void __cdecl cd_after_locate() {
    MH_Standalone_FillCdPath(reinterpret_cast<char *>(ADDR_CD_DATA_PATH), CD_DATA_PATH_SIZE);
}

// Wrap detour: run the original llm_cd_locate_and_open_audio as a subroutine, then patch up its side
// effect. __watcall with no arguments returning int in EAX, so the only live value across our helper
// is EAX -- saved around the call. EBX/ESI/EDI/EBP are preserved by the C helper's own convention,
// and ECX/EDX are volatile in watcall.
__declspec(naked) void cd_locate_detour() {
    __asm {
        call dword ptr [g_cd_tramp] // stolen prologue + jmp back to locate+8 (the original body)
        push eax
        call cd_after_locate
        pop  eax
        ret
    }
}

} // namespace

extern "C" int MH_Standalone_FillCdPath(char *path_buf, unsigned buf_size) {
    if (!path_buf || buf_size < 4) return 0;
    if (path_buf[0] != '\0') return 0; // a disc was found (or the cave patch already ran)

    char  module_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        // Same last resort the cave uses: a relative path still beats the modal.
        lstrcpynA(path_buf, ".\\", static_cast<int>(buf_size));
        return 1;
    }

    // Truncate after the last backslash, KEEPING it -- the game appends file names directly.
    int cut = -1;
    for (int i = 0; module_path[i] != '\0'; ++i)
        if (module_path[i] == '\\') cut = i;
    if (cut < 0) {
        lstrcpynA(path_buf, ".\\", static_cast<int>(buf_size));
        return 1;
    }
    module_path[cut + 1] = '\0';

    lstrcpynA(path_buf, module_path, static_cast<int>(buf_size));
    return 1;
}

extern "C" int MH_Standalone_Install(void) {
    int armed = 0;

    // U30: the caller-side prologue compare is gone -- the primitive owns it now, and it is the only
    // place that can also say the entry was taken by someone else rather than blame the build.
    if (mh::hook::install_trampoline(ADDR_CD_LOCATE, reinterpret_cast<void *>(cd_locate_detour),
                                     &g_cd_tramp, 8, mh::hook::entry_claim::exclusive,
                                     "the no-CD fallback detour"))
        armed |= MH_STANDALONE_NO_CD;

    // Guarded: an exe that already carries run_without_focus holds the JMP here, the compare fails,
    // and nothing is written. That is the intended no-op, not a failure.
    if (mh::hook::patch_bytes_guarded(ADDR_WND_ACTIVATE_INACTIVE, FOCUS_EXPECT, FOCUS_REPL,
                                      sizeof(FOCUS_EXPECT)))
        armed |= MH_STANDALONE_FOCUS;

    return armed;
}

extern "C" int MH_Standalone_InstallDevChangeGuard(int guard, int probe_ms) {
    int armed = 0;
    if (guard && mh::hook::install_trampoline(ADDR_WND_DEVICECHANGE,
                                              reinterpret_cast<void *>(devicechange_detour), &g_dc_tramp, 8,
                                              mh::hook::entry_claim::exclusive,
                                              "the U21 WM_DEVICECHANGE NULL guard"))
        armed |= MH_STANDALONE_DEVCHANGE;
    if (probe_ms > 0) {
        HANDLE t = CreateThread(nullptr, 0, devicechange_probe,
                                (LPVOID)(uintptr_t)(unsigned)probe_ms, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return armed;
}

extern "C" long MH_Standalone_DevChangeSwallowed(void) { return g_dc_swallowed; }
