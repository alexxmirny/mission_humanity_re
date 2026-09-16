//
// mh_harness/mh_harness_dllmain.cpp -- mh_harness.dll's ENTRY POINT AND MODULE CONTRACT (fork F4E).
//
// Three things live here and nothing else: the DllMain that is required to do NOTHING, the two
// module-level exports mh.dll calls immediately after binding, and the LATE BIND of this module's own
// two contract tables. The instrument itself is harness.cpp beside this file; the contract mh.dll reaches
// it through is mh/include/mh_harness_module.h plus the GENERATED mh_harness.def; the loader ruling
// that makes all of it legal is docs/dll-split.md.
//
// ---- RULE 1: THIS DllMain IS INERT, AND THAT IS A GATE RATHER THAN AN INTENTION -------------------
//
// mh.dll loads this module with LoadLibrary from inside its OWN DLL_PROCESS_ATTACH, under the loader
// lock. That is safe because of exactly two properties (docs/dll-split.md, "the subset rule"):
//
//   (a) this DllMain touches nothing outside this module, and
//   (b) this module's static imports are a SUBSET of mh.dll's own.
//
// (a) is checked by `tools/check_module_bind.py --dllmain-inert`, a lint_repo row. (b) is checked by
// `--subset`, which reads both PEs' import tables; SUBSET_PAIRS gained this module's row at F4E. It
// holds BECAUSE the Release build pins the STATIC CRT -- a dynamic one would put VCRUNTIME140 and
// MSVCP140 in this table, neither of which mh.dll imports.
//
// The half no scan can see: _DllMainCRTStartup runs this module's C++ static constructors BEFORE the
// body below is called. harness.cpp's state is ~200 zero-initialised PODs and function-local statics,
// which is why the rule holds here over one large TU rather than over two small ones.
//
// ---- RULE 2: THE TWO BINDS HAPPEN IN Init, NOT HERE -----------------------------------------------
//
// This module reaches OUT in two directions -- into mh.dll (the hook API, the UI-drive readouts, the
// host-api tables, the run paths) and into libmh.dll (the spine rows the instrument reads and pokes).
// Both are resolved with GetModuleHandle + GetProcAddress, never LoadLibrary: those modules are
// already in the process by construction (mh.dll loaded us; libmh.dll was bound one call earlier),
// and a LOOKUP of an already-loaded module is the primitive video.cpp:969 says to use. It is also
// what docs/dll-split.md pre-ruled for this edge -- "F4D's libmh.dll -> mh.dll edge and F4E's
// mh_harness.dll -> libmh.dll edge inherit that verbatim".
//
// They happen in MH_HarnessModule_Init because that is where mh.dll has finished deciding: the host
// hands us `spine_bound`, so the module's verdict and the loader's verdict are the same fact rather
// than two independent lookups that could disagree.
//
// ---- RULE 3 (Q4): NO SPINE, NO INSTRUMENT, AND SAY SO --------------------------------------------
//
// The spine rows are ~30 libmh symbols the harness reads state through. In configuration (1) there is
// no libmh.dll, so they cannot be resolved -- and unlike mh.dll's own contract rows, there is no
// meaningful zero answer: a determinism harness whose region reads all return 0 would produce a
// well-formed log of nothing. Ruling Q4 says HARD LOUD REFUSAL, and that is what spine_refuse() is.
// The refusal is loud on three channels and the boot CONTINUES, for the reason stated at length in
// mh/seams/harness_bind.cpp: the instrument installs nothing, so its absence cannot change which
// bodies run, and every consumer of mh_harness.log reds on a log that is not there.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "include/mh_harness_module.h"
#include "harness_contract.gen.h"

// The instrument's own arm gate reads this (harness.cpp, beside this file). Declared here rather
// than in a header because it is the ONE thing this module tells that file, and harness.cpp
// deliberately includes none of this module's plumbing -- it is compiled by net_selftest.exe too,
// where there is no boundary and no module.
extern "C" void MH_Harness_SetModuleRefused(int refused);

namespace {
// ZERO-INITIALISED PODs, every one -- see the static-constructor note above.
unsigned  g_attach_calls = 0;
unsigned  g_attach_tid   = 0;
unsigned  g_init_calls   = 0;
long long g_attach_qpc   = 0;
int       g_host_rows    = 0;
int       g_spine_rows   = 0;
int       g_refused      = 0;

// THE LOUD REFUSAL. The same three channels mh::config::detail::refuse uses -- a file beside the exe
// (what an automated lane reads back after the process is gone), OutputDebugString (what a debugger
// sees), and stderr (what a console host shows) -- and deliberately NOT its TerminateProcess.
int g_configured = 0;

void spine_refuse(const char *why, int got, int want) {
    // RECORDED ALWAYS, SHOUTED ONLY WHEN SOMEBODY ASKED. `configured` is `[harness] enable=1`, read
    // by mh.dll and handed to us; without it this run wanted no instrument, so the flag below is
    // enough and three channels of refusal would be noise in a shipped configuration.
    g_refused = 1;
    MH_Harness_SetModuleRefused(1);
    if (!g_configured) return;
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    {
        char *slash = nullptr;
        for (char *p = dir; *p != '\0'; ++p)
            if (*p == '\\' || *p == '/') slash = p;
        if (slash != nullptr) slash[1] = '\0';
    }
    char line[1400];
    wsprintfA(line,
              "mh_harness.dll REFUSES TO ARM: %s\r\n"
              "Resolved %d of %d libmh.dll spine symbols. The determinism harness reads the sim's "
              "state regions, the RNG trace and the promotion predicates THROUGH the spine; with no "
              "spine those reads have no honest answer, and a harness that answered zero would "
              "write a well-formed log of nothing -- which is the one failure this instrument must "
              "never produce.\r\n"
              "THIS RUN IS NOT INSTRUMENTED. The game is unaffected and the boot continues: the "
              "harness only observes, so its absence cannot change which bodies run.\r\n"
              "Either deploy libmh.dll beside mh.dll (configuration (2)), or remove `enable=1` from "
              "the `[harness]` section of mh_net.ini so the configuration and the run agree.\r\n",
              why, got, want);
    OutputDebugStringA(line);
    char path[MAX_PATH];
    wsprintfA(path, "%smh_harness_refused.log", dir);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
        CloseHandle(h);
    }
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e != nullptr && e != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(e, line, lstrlenA(line), &wrote, nullptr);
    }
}
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

// ---- the two module-level exports ------------------------------------------------------------------

extern "C" int MH_HarnessModule_Init(const MH_HarnessModuleHost *host) {
    // THE SIZE CHECK IS FIRST AND IT IS NOT DECORATION -- mh.dll and mh_harness.dll ship as separate
    // files and a player can copy one of them. Refusing costs the run its instrument and says so.
    if (host == nullptr || host->size != (unsigned)sizeof(MH_HarnessModuleHost) ||
        host->abi != MH_HARNESS_MODULE_ABI)
        return 0;
    (void)host->run_dir; // the paths come from MH_Core_ArmPaths(), one composition per process (Q1)
    g_configured = host->configured;
    g_init_calls++;

    // THE HOST ROWS. mh.dll is in the process by construction: it is holding our handle. A failure
    // here is not a configuration, it is a MISMATCHED PAIR of files, and it refuses the same way.
    g_host_rows = mh_harness_bind_host();
    if (g_host_rows != MH_HARNESS_HOST_COUNT)
        spine_refuse("mh.dll does not export the host contract this build was compiled against "
                     "(a mismatched mh.dll / mh_harness.dll pair)",
                     g_host_rows, MH_HARNESS_HOST_COUNT);

    // THE SPINE ROWS -- ruling Q4. `spine_bound` is mh.dll's own verdict, so the module and the
    // loader cannot disagree about which configuration the run is in; the GetModuleHandle below is
    // then a lookup of a module we have been told is there.
    if (host->spine_bound) {
        g_spine_rows = mh_harness_bind_spine();
        if (g_spine_rows != MH_HARNESS_SPINE_COUNT)
            spine_refuse("libmh.dll is bound but does not export every spine symbol the harness "
                         "reads (a mismatched libmh.dll / mh_harness.dll pair)",
                         g_spine_rows, MH_HARNESS_SPINE_COUNT);
    } else {
        spine_refuse("there is no libmh.dll in this process -- CONFIGURATION (1)", 0,
                     MH_HARNESS_SPINE_COUNT);
    }
    return (int)MH_HARNESS_MODULE_ABI;
}

extern "C" void MH_HarnessModule_Probe(MH_HarnessModuleProbe *out) {
    if (out == nullptr) return;
    // `size` and `abi` are filled BY THE MODULE and checked by mh.dll, so a shape change across a
    // mismatched pair is caught before any other field is read. Deliberately callable before
    // MH_HarnessModule_Init -- mh.dll probes first, precisely so the R2 numbers describe the LOAD and
    // nothing this module was subsequently told.
    out->size         = (unsigned)sizeof(MH_HarnessModuleProbe);
    out->abi          = MH_HARNESS_MODULE_ABI;
    out->attach_calls = g_attach_calls;
    out->attach_tid   = g_attach_tid;
    out->init_calls   = g_init_calls;
    out->attach_qpc   = g_attach_qpc;
    out->host_rows    = g_host_rows;
    out->spine_rows   = g_spine_rows;
    out->host_total   = MH_HARNESS_HOST_COUNT;
    out->spine_total  = MH_HARNESS_SPINE_COUNT;
}

// Read by harness.cpp's arm gate: a refusal above means this run is NOT instrumented whatever the ini
// says, and MH_Harness_Init returns 0 rather than arming half an instrument.
extern "C" int MH_HarnessModule_Refused(void) { return g_refused; }
