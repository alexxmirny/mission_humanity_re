#pragma once
//
// libmh_module.h -- THE mh.dll <-> libmh.dll MODULE-LEVEL CONTRACT (fork F4D).
//
// libmh.h declares WHAT the spine offers. This header declares the three entries that are not part
// of the spine at all: the ABI handshake, the loader-order probe, and the crossing readout. They
// are the direct siblings of mh_common/include/mh_net_module.h's MH_NetModule_Init/Probe, and they
// exist for the same reasons -- read docs/dll-split.md first (F4A ruled the mechanism, F4B shipped
// the first satellite with it).
//
// WHY THE 100-ROW SPINE CONTRACT IS *NOT* HERE. mh_net.dll's 23 symbols live in one X-macro a human
// reads and maintains. libmh's are ~100 MANGLED C++ names, and docs/dll-split.md says in as many
// words that such a list "should be BOUND BY A GENERATOR, not by hand" -- so it is derived from the
// two images' objects by tools/gen_libmh_contract.py, into libmh_dll/libmh.def and
// mh/seams/libmh_contract.gen.{h,cpp}. What stays here is the small part a generator cannot derive,
// because it is a decision rather than a measurement.
//
// ---- WHAT ABSENCE MEANS FOR THIS SATELLITE ------------------------------------------------------
//
// Not "degraded", and not "broken": CONFIGURATION (1), the shipped one in which the game runs the
// original binary's own bodies (F4 ruling Q10, ratified). Every generated forwarding thunk answers
// zero when unbound, and for this contract zero IS config (1): no promotion installs, no observers,
// no instrumentation. The four rows where zero would be the wrong answer are hand-written in
// mh/seams/libmh_bind.cpp and each carries its reason in the generator's SEMANTIC_HAND table.
//
// So the refusal line does not say something failed. It says which configuration the run is in --
// which is exactly what docs/dll-split.md's inheritance table asks of F4D and of nobody else.
//
#ifndef LIBMH_MODULE_H
#define LIBMH_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything in this header changes shape. mh.dll REFUSES a module whose ABI
// disagrees rather than calling into it: two DLLs that ship separately can be mismatched by a
// player copying one file, and a silent mismatch is a crash in somebody else's stack frame.
//
// IT IS NOT THE CONTRACT'S VERSION. The ~100 spine rows are checked ROW BY ROW at bind time (every
// name must resolve or the module is refused whole), which is strictly stronger than a number both
// sides have to remember to bump -- and it is the check a generated list can actually keep true.
#define LIBMH_MODULE_ABI 0xF4D00001u

// What mh.dll hands the module at bind time. The same shape as MH_NetModuleHost and the same Q1
// rule behind it: the satellite owns no path, no ini and no file handle, because exactly one place
// in the process knows where this run's logs live.
typedef struct libmh_module_host {
    unsigned    size;    /* sizeof(libmh_module_host) as MH.DLL was compiled                 */
    unsigned    abi;     /* LIBMH_MODULE_ABI as MH.DLL was compiled                          */
    const char *run_dir; /* "<exedir>\logs\<runid>_<role>\" -- borrowed, valid for the run    */
} libmh_module_host;

// What the module's own DllMain observed. The R2 measurement, kept for every satellite and not only
// for the spike that proved it: a statically-imported module's DLL_PROCESS_ATTACH runs BEFORE its
// importer's, and that ordering is the fact "every satellite DllMain must be inert" rests on.
// Reading it every boot is how a build that silently acquired a static import gets caught by its
// own log instead of by a 0xC0000409 nobody can reproduce.
typedef struct libmh_module_probe {
    unsigned  size;         /* sizeof(libmh_module_probe) as the MODULE was compiled         */
    unsigned  abi;          /* LIBMH_MODULE_ABI as the MODULE was compiled                   */
    unsigned  attach_calls; /* DLL_PROCESS_ATTACH count -- must be exactly 1                 */
    unsigned  attach_tid;   /* the thread its DllMain ran on (compare with mh.dll's)         */
    unsigned  init_calls;   /* libmh_module_init count -- must be exactly 1                  */
    long long attach_qpc;   /* QueryPerformanceCounter at DLL_PROCESS_ATTACH                 */
} libmh_module_probe;

/* Hand the module its run context. Returns LIBMH_MODULE_ABI (the module's own), or 0 if `host` is
 * malformed. Called by mh.dll's DllMain immediately after the exports resolve, before any spine
 * call. Safe to call once; a second call is ignored. */
int libmh_module_init(const libmh_module_host *host);

/* Fill *out with the DllMain observations above. Safe to call before libmh_module_init. */
void libmh_module_probe_read(libmh_module_probe *out);

#ifdef __cplusplus
}
#endif

#endif // LIBMH_MODULE_H
