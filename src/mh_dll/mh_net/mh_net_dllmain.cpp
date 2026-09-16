//
// mh_net/mh_net_dllmain.cpp -- mh_net.dll's ENTRY POINT AND MODULE CONTRACT (fork F4B).
//
// Two things live here and nothing else: the DllMain that is required to do NOTHING, and the two
// module-level exports mh.dll calls immediately after binding. The transport itself is
// net_transport.cpp; the contract is mh_common/include/mh_net_module.h; the loader ruling that makes
// all of it legal is docs/dll-split.md.
//
// ---- RULE 1: THIS DllMain IS INERT, AND THAT IS A GATE RATHER THAN AN INTENTION ------------------
//
// mh.dll loads this module with LoadLibrary from inside its OWN DLL_PROCESS_ATTACH, under the loader
// lock. That is safe because of exactly two properties (docs/dll-split.md, "the subset rule"):
//
//   (a) this DllMain touches nothing outside this module, and
//   (b) this module's static imports are a SUBSET of mh.dll's own.
//
// (a) is checked by `tools/check_module_bind.py --dllmain-inert`, a lint_repo row: every DllMain in
// src/ is found, exactly one is exempt by name (mh.dll's -- the sole orchestrator), and every other
// body may call nothing but DisableThreadLibraryCalls / GetCurrentThreadId /
// QueryPerformanceCounter. (b) is checked by `--subset`, which reads both PEs' import tables.
//
// The trap the rule exists for is R2, and it was MEASURED at F4A rather than argued: a statically
// imported satellite's DLL_PROCESS_ATTACH runs 181 us BEFORE its importer's. If this file armed
// anything, that arm would happen at a point mh.dll does not choose and cannot order -- G104 at
// module scope, the failure class where every gate stayed green and every real boot died at
// 0xC0000409. So the rule has to hold whether or not the final build imports statically, and the
// only way to keep it true is to have nothing here that could go wrong.
//
// WHAT THE THREE CALLS BELOW ARE FOR. DisableThreadLibraryCalls is loader bookkeeping (the msvfw32
// shim does it for the same reason), and it is wanted here specifically because this module STARTS
// THREADS later: without it every accept/recv/watchdog thread would fire a DLL_THREAD_ATTACH
// notification into a body that has nothing to do with it. The other two record what this module's
// attach observed -- a timestamp and a thread id -- for MH_NetModule_Probe to report. Recording a
// counter for a later readout is self-observation, not work: nothing outside this module can see it
// until mh.dll asks.
//
// AND THE HALF NO SCAN CAN SEE: _DllMainCRTStartup runs this module's C++ static constructors BEFORE
// the body below is called, so a satellite with dynamic initialisers has already run arbitrary code
// under the loader lock. Every global in this DLL is a zero-initialised POD, and that is a rule a
// human keeps.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "include/mh_net_module.h"

// net_transport.cpp's one internal entry: it owns the log path, this TU owns the attach record.
extern "C" void MH_NetInternal_SetRunDir(const char *dir);

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

extern "C" int MH_NetModule_Init(const MH_NetModuleHost *host) {
    // THE SIZE CHECK IS FIRST AND IT IS NOT DECORATION. mh.dll and mh_net.dll ship as separate
    // files and a player can copy one of them; a struct that grew on one side and not the other
    // would be read past its end here, inside the loader lock, on the first boot after an upgrade.
    // Refusing costs the run its transport and says so; not refusing costs it the process.
    if (host == nullptr || host->size != (unsigned)sizeof(MH_NetModuleHost) ||
        host->abi != MH_NET_MODULE_ABI)
        return 0;
    if (g_init_calls == 0) MH_NetInternal_SetRunDir(host->run_dir);
    g_init_calls++;
    return (int)MH_NET_MODULE_ABI;
}

extern "C" void MH_NetModule_Probe(MH_NetModuleProbe *out) {
    if (out == nullptr) return;
    // `size` and `abi` are filled BY THE MODULE and checked by mh.dll, so a shape change across a
    // mismatched pair is caught before any other field is read. Deliberately callable before
    // MH_NetModule_Init -- mh.dll probes first, precisely so the numbers describe the LOAD and
    // nothing this module was subsequently told.
    out->size         = (unsigned)sizeof(MH_NetModuleProbe);
    out->abi          = MH_NET_MODULE_ABI;
    out->attach_calls = g_attach_calls;
    out->attach_tid   = g_attach_tid;
    out->init_calls   = g_init_calls;
    out->attach_qpc   = g_attach_qpc;
}
