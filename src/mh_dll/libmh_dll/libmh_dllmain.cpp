//
// libmh_dll/libmh_dllmain.cpp -- libmh.dll's ENTRY POINT AND MODULE CONTRACT (fork F4D).
//
// Two things live here and nothing else: the DllMain that is required to do NOTHING, and the two
// module-level exports mh.dll calls immediately after binding. The spine itself is the 627 TUs of
// the roster; the contract is libmh/include/libmh_module.h plus the GENERATED libmh.def; the loader
// ruling that makes all of it legal is docs/dll-split.md.
//
// ---- RULE 1: THIS DllMain IS INERT, AND THAT IS A GATE RATHER THAN AN INTENTION ------------------
//
// mh.dll loads this module with LoadLibrary from inside its OWN DLL_PROCESS_ATTACH, under the
// loader lock. That is safe because of exactly two properties (docs/dll-split.md, "the subset
// rule"):
//
//   (a) this DllMain touches nothing outside this module, and
//   (b) this module's static imports are a SUBSET of mh.dll's own.
//
// (a) is checked by `tools/check_module_bind.py --dllmain-inert`, a lint_repo row. (b) is checked by
// `--subset`, which reads both PEs' import tables; measured for this module at F4D: KERNEL32 and
// USER32 and nothing else, against mh.dll's KERNEL32/USER32/WINMM/WS2_32. It holds with room to
// spare and it holds BECAUSE the Release build pins the STATIC CRT -- a dynamic one would put
// VCRUNTIME140 and MSVCP140 in this table, neither of which mh.dll imports.
//
// ---- THE HALF NO SCAN CAN SEE, AND WHY IT IS A REAL QUESTION FOR *THIS* MODULE -------------------
//
// _DllMainCRTStartup runs a module's C++ static constructors BEFORE the body below is called. For
// mh_net.dll the rule "every global is a zero-initialised POD" is a rule a human keeps over two
// source files. This module is 627 TUs, so the rule is kept by the ROSTER's own discipline instead:
// libmh's state lives in function-local statics and zero-initialised arrays, which is what makes it
// serialisable at all (RI-STATE), and tools/scan_libmh_vas.py's byte ratchet already walks the
// built artifact for anything that materialises an address. A dynamic initialiser that ran real
// code here would be a finding for those gates, not only for this comment.
//
// ---- TWO STATIC CRTs, i.e. TWO HEAPS ------------------------------------------------------------
//
// mh.dll and libmh.dll each link their own /MT runtime, so each owns a heap, a locale, an errno and
// a stdio table. Anything allocated on one side must be freed on the same side. The contract was
// reviewed for that at F4D and nothing crosses owning memory: the ~100 spine rows pass PODs,
// function pointers and BORROWED const char * into static storage (mh::save::last_save_path,
// mh::libmh_in::last_trap), and the file I/O the spine does is already the host's -- libmh asks
// mh.dll to open through the host-api `vfs_open` row and hands the handle back to the same side
// that made it (SIMABI-VFS). That property is the reason this split is possible at all; it was
// designed in long before this item, and this note is here so a future row that breaks it is
// recognised as breaking something.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../libmh/include/libmh_module.h"

namespace {
// ZERO-INITIALISED PODs, every one -- see the static-constructor note above.
unsigned  g_attach_calls = 0;
unsigned  g_attach_tid   = 0;
unsigned  g_init_calls   = 0;
long long g_attach_qpc   = 0;
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_attach_calls++;
        g_attach_tid = GetCurrentThreadId();
        LARGE_INTEGER t;
        if (QueryPerformanceCounter(&t)) g_attach_qpc = (long long)t.QuadPart;
    }
    return TRUE;
}

// ---- the two module-level exports ----------------------------------------------------------------

extern "C" int libmh_module_init(const libmh_module_host *host) {
    // THE SIZE CHECK IS FIRST AND IT IS NOT DECORATION. mh.dll and libmh.dll ship as separate files
    // and a player can copy one of them; a struct that grew on one side and not the other would be
    // read past its end here, inside the loader lock, on the first boot after an upgrade. Refusing
    // costs the run its spine and says so; not refusing costs it the process.
    if (host == nullptr || host->size != (unsigned)sizeof(libmh_module_host) ||
        host->abi != LIBMH_MODULE_ABI)
        return 0;
    // NOTHING IS DONE WITH run_dir YET, and it is accepted rather than dropped for the reason Q1
    // gives one level over: run_context.cpp stays mh.dll-side, so if any libmh diagnostic ever
    // wants a file it must be GIVEN the directory rather than compose one. Accepting it now means
    // that day is a body change here, not an ABI change across two shipped files.
    (void)host->run_dir;
    g_init_calls++;
    return (int)LIBMH_MODULE_ABI;
}

extern "C" void libmh_module_probe_read(libmh_module_probe *out) {
    if (out == nullptr) return;
    // `size` and `abi` are filled BY THE MODULE and checked by mh.dll, so a shape change across a
    // mismatched pair is caught before any other field is read. Deliberately callable before
    // libmh_module_init -- mh.dll probes first, precisely so the numbers describe the LOAD and
    // nothing this module was subsequently told.
    out->size         = (unsigned)sizeof(libmh_module_probe);
    out->abi          = LIBMH_MODULE_ABI;
    out->attach_calls = g_attach_calls;
    out->attach_tid   = g_attach_tid;
    out->init_calls   = g_init_calls;
    out->attach_qpc   = g_attach_qpc;
}
