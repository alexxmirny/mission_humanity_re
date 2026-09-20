//
// mh_net_udp/mh_net_udp_dllmain.cpp -- mh_net_udp.dll's ENTRY POINT AND MODULE CONTRACT (mp:T1).
//
// The deliberate twin of mh_net/mh_net_dllmain.cpp, and it has to be: mh.dll binds ONE net module
// per run, chosen by `[net] transport`, and both candidates answer the same contract. Read that file
// for the full reasoning; the two rules it states apply here word for word.
//
//   RULE 1: THIS DllMain IS INERT, AND IT IS A GATE RATHER THAN AN INTENTION. mh.dll loads this
//   module with LoadLibrary from inside its own DLL_PROCESS_ATTACH, under the loader lock, which is
//   safe only because (a) this DllMain touches nothing outside this module and (b) this module's
//   static imports are a subset of mh.dll's. (a) is checked by check_module_bind.py --dllmain-inert,
//   which scans EVERY DllMain in src/ and permits only DisableThreadLibraryCalls /
//   GetCurrentThreadId / QueryPerformanceCounter in a satellite body -- so this file is not merely
//   written to the rule, it is measured against it. (b) holds because the import set is the TCP
//   module's: KERNEL32, USER32, WS2_32, ADVAPI32 by GetProcAddress only.
//
//   AND THE HALF NO SCAN CAN SEE: _DllMainCRTStartup runs this module's C++ static constructors
//   BEFORE the body below is called. Every global in this DLL is a zero-initialised POD -- including
//   udp_transport.cpp's Endpoint, whose constructor is a memset and which allocates nothing and
//   touches no OS state until MH_Net_InitEx -- and that is a rule a human keeps.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "mh_net_module.h"

// udp_transport.cpp owns the log path; this TU owns the attach record.
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
        // Wanted here specifically because this module STARTS THREADS later: without it every recv
        // and timer thread would fire a DLL_THREAD_ATTACH notification into a body with nothing to
        // do with it.
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
    // THE SIZE CHECK IS FIRST AND IT IS NOT DECORATION. mh.dll and this module ship as separate
    // files and a player can copy one of them; a struct that grew on one side and not the other
    // would be read past its end here, inside the loader lock, on the first boot after an upgrade.
    if (host == nullptr || host->size != (unsigned)sizeof(MH_NetModuleHost) ||
        host->abi != MH_NET_MODULE_ABI)
        return 0;
    if (g_init_calls == 0) MH_NetInternal_SetRunDir(host->run_dir);
    g_init_calls++;
    return (int)MH_NET_MODULE_ABI;
}

extern "C" void MH_NetModule_Probe(MH_NetModuleProbe *out) {
    if (out == nullptr) return;
    // Deliberately callable before MH_NetModule_Init -- mh.dll probes first, precisely so the
    // numbers describe the LOAD and nothing this module was subsequently told.
    out->size         = (unsigned)sizeof(MH_NetModuleProbe);
    out->abi          = MH_NET_MODULE_ABI;
    out->attach_calls = g_attach_calls;
    out->attach_tid   = g_attach_tid;
    out->init_calls   = g_init_calls;
    out->attach_qpc   = g_attach_qpc;
}
