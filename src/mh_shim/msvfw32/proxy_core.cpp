//
// msvfw32 shim -- the resolver half.
//
// WHAT THIS DLL IS: a stand-in for the system msvfw32.dll, living next to mh.exe. mh.exe statically
// imports MSVFW32.dll and msvfw32 is not a KnownDLL, so the application directory wins the search and
// the loader maps US. We statically import mh.dll's force-load anchor, which makes the loader chain
//
//     mh.exe -> msvfw32.dll (this) -> mh.dll
//
// and run mh.dll's DllMain BEFORE mh.exe's entry point -- the same timing the .mhimp added-import
// surgery buys, without touching a single byte of the game exe.
//
// THE TWO RULES this file exists to obey:
//   1. NOTHING happens in DllMain. mh.dll is a static import, so the loader has already initialised it
//      by the time we get here; we have nothing to do and must not do it. In particular no LoadLibrary
//      (the Windows-compat notes -- calling it under the loader lock is a documented deadlock).
//   2. The real msvfw32 is loaded by ABSOLUTE PATH, on first forwarded call. A bare name would find us
//      again -- we are already in the loader's module list under that name -- and recurse.
//
#include <windows.h>

#include "proxy_core.h"

// The definition (the header only declares it -- inside a linkage specification, a declaration
// without an initializer is implicitly `extern`).
extern "C" void *g_mh_proxy_real[MH_PROXY_COUNT] = {};

// THE FORCE-LOAD ANCHOR. Importing anything from mh.dll is what makes the loader map it; the symbol
// itself returns NULL and nothing calls it (src/mh_dll/mh/mh.c:47-62). Taking its address in a
// non-const global is enough to make the linker emit the import descriptor, and unlike a call in
// DllMain it cannot be optimised away.
extern "C" __declspec(dllimport) void *MH_HostedPoolBase(void);
extern "C" void                       *g_mh_shim_anchor = (void *)&MH_HostedPoolBase;

static HMODULE g_real_module;

// A resolve failure means the process is about to jump to address 0. Say why, loudly, instead.
static void __declspec(noreturn) fatal(const char *what, const char *detail) {
    char msg[512];
    wsprintfA(msg,
              "mh shim (" MH_PROXY_DLL_NAME "): %s\n\n%s\n\n"
              "The game cannot continue. Remove " MH_PROXY_DLL_NAME " from the game folder to run "
              "unmodified.",
              what, detail);
    MessageBoxA(NULL, msg, "mh shim", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static HMODULE load_real(void) {
    char path[MAX_PATH + 32];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        fatal("GetSystemDirectoryA failed", "Could not locate the Windows system directory.");
    }
    // GetSystemDirectoryA yields SysWOW64 for this 32-bit process on 64-bit Windows, which is where the
    // 32-bit system DLL lives -- the same directory tools/gen_proxy_stubs.py read the export table from.
    lstrcatA(path, "\\" MH_PROXY_DLL_NAME);

    HMODULE h = LoadLibraryA(path);
    if (!h) {
        fatal("could not load the real " MH_PROXY_DLL_NAME, path);
    }
    if (h == GetModuleHandleA(MH_PROXY_DLL_NAME)) {
        // Would mean the absolute path resolved back to us; every forwarded call would recurse.
        fatal("the real " MH_PROXY_DLL_NAME " resolved to the shim itself", path);
    }
    return h;
}

extern "C" void *__cdecl mh_proxy_resolve(int index) {
    // Racing callers both do the whole table and write identical pointers, so no lock is needed --
    // LoadLibrary is refcounted and GetProcAddress is pure. The only cost of a race is a redundant
    // pass, and the alternative (a lock, or a DllMain-time resolve) buys nothing.
    if (!g_real_module) {
        HMODULE h = load_real();
        for (int i = 0; i < MH_PROXY_COUNT; ++i) {
            g_mh_proxy_real[i] = (void *)GetProcAddress(h, g_mh_proxy_names[i]);
        }
        g_real_module = h;
    }

    void *target = g_mh_proxy_real[index];
    if (!target) {
        char        detail[128];
        const char *name = g_mh_proxy_names[index];
        // A low pointer is an ordinal, not a string (the by-ordinal GetProcAddress form).
        if ((ULONG_PTR)name > 0xFFFF) {
            wsprintfA(detail, "Export not found: %s", name);
        } else {
            wsprintfA(detail, "Export not found: ordinal %u", (unsigned)(ULONG_PTR)name);
        }
        fatal("the real " MH_PROXY_DLL_NAME " is missing an export we re-export", detail);
    }
    return target;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        // Rule 1 above: no work here. mh.dll has already armed itself by now.
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
