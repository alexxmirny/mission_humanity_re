#pragma once
//
// dist LA4 -- the crash marker. See mh/seams/crash_marker.cpp for the mechanism and
// mh_common/include/mh_crash_marker.h for the file format and the environment contract.
//
// Two calls, both from DllMain, and the SECOND ONE IS THE REASON THIS HEADER EXISTS. mh.dll's
// DllMain had no DLL_PROCESS_DETACH arm at all before LA4: nothing it installed needed unwinding,
// because a byte patch or a trampoline dies with the process that owns it. A vectored exception
// handler does not. It is a node in a process-wide list the OS walks on every exception, and if
// mh.dll is ever unloaded while ours is still in that list, the next exception in the process --
// any exception, including one the game handles routinely -- jumps into unmapped memory. That is
// the whole reason RemoveVectoredExceptionHandler exists, and it is why plan decision D12 names
// the removal as part of the design rather than leaving it implied.
//
#ifdef __cplusplus
extern "C" {
#endif

// Install the handler. Reads the two MH_CRASH_* environment variables (whether a launcher is
// listening, and where it wants the marker) and the `[debug] crash_after_ms` knob, then registers
// the vectored handler LAST in the chain. Returns 1 when the handler is installed, 0 when it is
// not. Safe to call from DllMain: no LoadLibrary, no wait, nothing that needs the loader lock it
// is already holding.
int MH_CrashMarker_Init(void);

// Remove the handler. Called from DLL_PROCESS_DETACH; idempotent, and a no-op when Init declined.
void MH_CrashMarker_Shutdown(void);

#ifdef __cplusplus
}
#endif
