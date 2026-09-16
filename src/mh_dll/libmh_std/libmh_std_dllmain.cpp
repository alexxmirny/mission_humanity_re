//
// libmh_std/libmh_std_dllmain.cpp -- the STANDALONE libmh.dll's entry point (fork F4G).
//
// This file is four lines of code and a page of why, because the why is the only thing here that a
// reader could get wrong.
//
// ---- WHAT THIS MODULE IS -------------------------------------------------------------------------
//
// `Release\standalone\libmh.dll` is CONFIGURATION (3)'s spine: the reimplemented engine as a real
// dynamic library, loaded by a host that has no game image in the process (today `libref_host.exe`,
// which replays the committed fixtures against it). It is built by linking `libmh.lib` -- the
// STANDALONE arm of the roster, compiled WITH MH_LIBMH_BUILD -- with /WHOLEARCHIVE, so it is the
// same object code as the archive and not a second compilation of it.
//
// It is NOT configuration (2)'s `libmh.dll`. That one is libmh_dll.vcxproj's, built WITHOUT
// MH_LIBMH_BUILD, and the difference is not a flag preference: `MH_CRT()` (crt/crt_select.h) reads
// that macro to choose between the vendored CRT and the ORIGINAL BINARY's Watcom CRT at a fixed VA.
// The hosted build's allocator is an address that exists only while mh.exe is mapped, so a
// standalone host running it faults on its first malloc. Two deployments, two files, one name --
// and a swapped pair is caught immediately and by name, because mh.dll resolves every contract row
// before adopting any of them and this module exports a different set entirely
// (`LOADED BUT REFUSED -- 0 of N contract symbols`, the f4d_wrong shape).
//
// ---- WHY THIS DllMain IS INERT, WHICH IS NOT THE SATELLITES' REASON -----------------------------
//
// mh_net.dll / libmh.dll / mh_harness.dll are inert at attach because mh.dll LoadLibrary()s them
// from inside its own DLL_PROCESS_ATTACH, under the loader lock (docs/dll-split.md, the subset
// rule). Nothing loads THIS module under a lock: its host imports it statically and the loader
// initialises it before the host's own entry point, in the ordinary way. So the reason here is the
// other one, and it is just as binding: `tools/check_module_bind.py --dllmain-inert` is a lint_repo
// row that finds EVERY DllMain in src/ and exempts exactly one by name. A module in this tree does
// not get to be the second exception, and it should not want to be -- the arm sequence belongs to
// the host that knows what it is arming, which for this module is the host's `main()`.
//
// ---- ONE CRT, ONE HEAP, AND THE HOST MUST STILL NOT ASSUME IT ----------------------------------
//
// This DLL and its host both link the static CRT, so there are two heaps here exactly as there are
// between mh.dll and libmh.dll -- and the same standing rule applies: nothing crosses this boundary
// owning memory. The host lends the arena and the fixture blob, and libmh hands back PODs and
// const char * into its own static storage. It is the property that made the F4D split possible and
// it is what makes this one work unchanged.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (dwReason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}
