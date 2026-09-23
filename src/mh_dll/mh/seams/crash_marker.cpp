//
// crash_marker.cpp -- the game's half of the out-of-process crash capture (dist LA4, plan D12).
//
// WHAT HAPPENS WHEN THE GAME FAULTS, in order:
//
//   1. The vectored handler below sees the exception. If it is not FATAL (mh_crash_is_fatal --
//      severity ERROR, customer bit clear) it returns EXCEPTION_CONTINUE_SEARCH immediately and
//      nothing else in this file runs. That branch is the common one: a first-chance handler sees
//      every exception in the process, including the ones somebody is about to catch.
//   2. Otherwise it fills an MH_CrashFacts on its own stack -- what, where, which thread, which
//      module, which match -- and writes it, in ONE WriteFile of one pre-sized buffer, to the
//      marker path.
//   3. If a launcher told us it is listening (MH_CRASH_CHANNEL was in our environment at startup),
//      it signals that launcher and BLOCKS on the acknowledgement. The block is the point: the
//      launcher is about to call MiniDumpWriteDump on this process from outside, and both the
//      thread's register context and the EXCEPTION_POINTERS the marker names only exist while this
//      thread stays where it is.
//   4. Then EXCEPTION_CONTINUE_SEARCH, and the process dies exactly as it would have. The handler
//      never returns EXCEPTION_CONTINUE_EXECUTION and never swallows a fault: a crash report that
//      changed whether the game crashed would be an instrument that altered its subject.
//
// WHY A VECTORED HANDLER AND NOT SetUnhandledExceptionFilter. The unhandled filter is the tidier
// choice in a normal program -- it runs only when no SEH handler wanted the exception -- and it is
// the wrong choice here for two measured reasons. It is a single global slot, so anything else in
// the process (the game's own Watcom runtime, a debugger, an overlay injector) silently replaces
// ours; and the rig already installs vectored handlers in this same process for the tombstone
// sweep (mh/hook/tombstone.cpp) and the GAME_MODE write breakpoint (mh/seams/net_diag.cpp), so the
// mechanism is one this binary is already known to tolerate.
//
// AND IT IS REGISTERED **LAST**, which is the one flag on AddVectoredExceptionHandler that matters
// here. `First = 0` appends; `First = 1` prepends. tombstone.cpp registers with 1 because its
// whole job is to intercept its own planted traps and RESUME, and net_diag's breakpoint handler
// does the same for its single-step hits. Ours must see an exception only after both of them have
// declined it -- a tombstone trap is not a crash, and reporting one as a crash would turn a
// deliberate diagnostic into a bug report.
//
// THE DELIBERATE CRASH (`[debug] crash_after_ms=N`) is at the bottom of this file. LA4's acceptance
// needs a fault that is reproducible on demand and lands inside mh.dll, and the honest way to get
// one is to write one rather than to wait for a real bug.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string.h> // memcpy -- write_context()'s two raw-struct copies

#include "include/mh_crash_export.h"
#include "mh_crash_marker.h" // MH_CrashFacts, the text format, the env + event contract
#include "mh_run_context.h"  // MH_RunDir_SessionMatchId / MH_RunDir_UtcStamp (mh_common)
#include "mh_version.h"      // MH_VERSION_FULL -- the build stamp in the marker

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

// ---- what Init decided, read by the handler -----------------------------------------------------
// All plain scalars written once before the handler can fire and never written again, so the
// handler reads them without a lock. `g_busy` is the exception: two threads can fault at once.
PVOID  g_veh                       = nullptr;
HANDLE g_req                       = nullptr; // signalled by us; the launcher waits on it
HANDLE g_ack                       = nullptr; // signalled by the launcher once its dump is written
char   g_marker[MAX_PATH]          = {0};
char   g_build[MH_CRASH_BUILD_CAP] = {0};
LONG   g_busy                      = 0; // 0 = no crash reported yet; CAS'd to 1 by the first fatal fault

// ---- the module a fault address belongs to ------------------------------------------------------
//
// VirtualQuery FOR THE BASE, GetModuleFileName FOR THE NAME, and the split is not fussiness.
// `mbi.AllocationBase` of any address inside a mapped image IS that image's load base, and
// VirtualQuery takes no loader lock -- which matters in a handler that may be running on a thread
// that faulted while HOLDING the loader lock, where GetModuleHandleEx would deadlock. The name is
// then best-effort on top of a base we already have: if it does not come back, the marker still
// carries `module_base` and `address`, and a reader can do the subtraction itself.
void resolve_module(unsigned long address, MH_CrashFacts *f) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)(ULONG_PTR)address, &mbi, sizeof(mbi)) != sizeof(mbi)) return;
    if (mbi.AllocationBase == nullptr) return;
    f->module_base = (unsigned long)(ULONG_PTR)mbi.AllocationBase;

    char path[MAX_PATH];
    path[0] = '\0';
    if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH) == 0) return;
    const char *leaf = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    mh_sd_copy(f->module, MH_CRASH_MODULE_CAP, leaf);
}

// Write the marker. One CreateFile + one WriteFile + one CloseHandle, no allocation, no CRT.
// CREATE_ALWAYS rather than append: the last fault is the one being reported, and a marker that
// accumulated would leave the launcher reading whichever record it happened to parse first.
void write_marker(const MH_CrashFacts *f) {
    if (g_marker[0] == '\0') return;
    char text[MH_CRASH_MARKER_CAP];
    int  n = mh_crash_marker_text(f, text, (int)sizeof(text));
    if (n <= 0) return;
    HANDLE h = CreateFileA(g_marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text, (DWORD)n, &wrote, nullptr);
    FlushFileBuffers(h); // the process is about to die; a buffered marker is no marker
    CloseHandle(h);
}

// ---- the raw x86 EXCEPTION_RECORD + CONTEXT sidecar (dist LA5) ----------------------------------
//
// LA4 measured that a 64-bit MiniDumpWriteDump refuses a WOW64 target's EXCEPTION_POINTERS
// (ERROR_NOACCESS -- it reads the 32-bit pointers as 64-bit), so the dump the launcher writes has
// every thread and module but no Exception stream: nothing in it says WHICH thread faulted. This
// process has no such problem -- it is running natively as the same x86 the fault happened in, so
// `*ep->ExceptionRecord` and `*ep->ContextRecord` are ALREADY the bytes a 32-bit debugger would
// read. Writing them out verbatim, once, sidesteps the cross-bitness read the launcher cannot do;
// src/launcher/src/crash.rs `append_exception_stream()` turns them into a real minidump Exception
// stream after the fact. This function contributes only the bytes -- it knows nothing about the
// minidump format, on purpose, for the same reason the marker itself is plain text: whatever writes
// from inside a fault handler should know as little as possible about the format its reader parses.
//
// Sized statically rather than measured at runtime: a size that drifted would silently misalign
// every field crash.rs reads out of the sidecar, and a link error here is a much better failure than
// a launcher quietly misreading `Eip`.
static_assert(sizeof(EXCEPTION_RECORD) == 80, "x86 EXCEPTION_RECORD layout changed");
static_assert(sizeof(CONTEXT) == 716, "x86 CONTEXT layout changed");

// Returns 1 when the sidecar was written whole, 0 otherwise (no marker path yet, a null pointer, or
// the write failed) -- the caller folds this straight into MH_CrashFacts::has_context, which is
// what tells the launcher whether to go looking for the file at all.
int write_context(const EXCEPTION_RECORD *rec, const CONTEXT *ctx) {
    if (g_marker[0] == '\0' || rec == nullptr || ctx == nullptr) return 0;
    char path[MAX_PATH + 16];
    wsprintfA(path, "%s%s", g_marker, MH_CRASH_CTX_SUFFIX);

    unsigned char buf[sizeof(EXCEPTION_RECORD) + sizeof(CONTEXT)];
    memcpy(buf, rec, sizeof(EXCEPTION_RECORD));
    memcpy(buf + sizeof(EXCEPTION_RECORD), ctx, sizeof(CONTEXT));

    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    WriteFile(h, buf, (DWORD)sizeof(buf), &wrote, nullptr);
    FlushFileBuffers(h); // the process is about to die; a buffered sidecar is no sidecar
    CloseHandle(h);
    return wrote == (DWORD)sizeof(buf) ? 1 : 0;
}

LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (!mh_crash_is_fatal(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

    // ONE REPORT PER PROCESS. A fatal fault is frequently followed by more of them -- the unwind
    // faults, a second thread touches the same corrupt structure -- and each would overwrite the
    // marker and re-signal the launcher while it is mid-dump. The loser does not return early: it
    // falls through to CONTINUE_SEARCH, so it dies normally instead of racing.
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    MH_CrashFacts f;
    mh_crash_facts_clear(&f);
    f.code       = (unsigned long)ep->ExceptionRecord->ExceptionCode;
    f.flags      = (unsigned long)ep->ExceptionRecord->ExceptionFlags;
    f.process_id = (unsigned long)GetCurrentProcessId();
    f.thread_id  = (unsigned long)GetCurrentThreadId();
    f.address    = (unsigned long)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    f.pointers   = (unsigned long)(ULONG_PTR)ep;
    resolve_module(f.address, &f);
    mh_sd_copy(f.build, MH_CRASH_BUILD_CAP, g_build);
    mh_sd_copy(f.match_id, MH_SESSION_MATCH_HEX_CAP, MH_RunDir_SessionMatchId());
    MH_RunDir_UtcStamp(f.when, MH_SESSION_STAMP_CAP);

    // dist LA5. Before the marker, so `ctx=` in the text below reflects whether the sidecar really
    // landed -- a launcher must never go looking for a file that was never written.
    f.has_context = write_context(ep->ExceptionRecord, ep->ContextRecord);

    write_marker(&f);

    // The handoff. Only ever reached when a launcher named a channel at startup, which is why a
    // game run from Explorer pays none of this: no events, no wait, marker on disk and out.
    if (g_req != nullptr && g_ack != nullptr) {
        SetEvent(g_req);
        WaitForSingleObject(g_ack, MH_CRASH_ACK_TIMEOUT_MS);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- the deliberate fault -----------------------------------------------------------------------
//
// `[debug] crash_after_ms=N` (N > 0) faults on purpose N milliseconds after the DLL arms. It is
// what makes LA4's acceptance clause runnable: "a deliberate access violation in the game yields a
// zip whose dump names a frame inside mh.dll" needs a fault that happens when asked, at a known
// place, without waiting for a real bug.
//
// WHY A TIMER THREAD AND NOT `[debug] crash_at_step=N`, which is what the item's done_when suggests.
// A per-STEP trigger needs a per-step hook, and the only non-harness-gated one in this process is
// `mh::sim::set_sim_step_pre_hook` -- a SINGLE-SUBSCRIBER slot already owned by the D21 desync
// sampler in the ship configuration (mh/seams/net_seams.cpp `install_desync_watch`, which logs
// "THE DETECTOR IS ARMED AND SAMPLING NOTHING" when it loses that slot). Taking it for a debug knob
// would silently disarm the desync detector for the whole run -- the exact failure that comment
// exists to warn about -- so the trigger is scheduled on wall clock instead, on a thread of its own.
// It costs nothing in a normal run (the knob defaults to 0 and no thread is created) and the fault
// still lands inside mh.dll, which is what the acceptance clause is about.
//
// `volatile` on the pointer is load-bearing: without it the optimiser is entitled to notice the
// store is to a constant null and delete the whole function body as unreachable.
DWORD WINAPI crash_after_thread(LPVOID param) {
    Sleep((DWORD)(ULONG_PTR)param);
    OutputDebugStringA("mh.dll: [debug] crash_after_ms elapsed -- faulting on purpose now\n");
    volatile int *poison = (volatile int *)0x00000010;
    *poison              = 0x1badf00d;
    return 0;
}

} // namespace

extern "C" int MH_CrashMarker_Init(void) {
    if (g_veh != nullptr) return 1; // idempotent; DllMain calls it once but say so anyway

    // The exe directory, composed the way mh/seams/net_lockstep.cpp's ensure_key_once does -- from
    // the module file name, not from MH_ExeDir(). That is deliberate: this runs FIRST in DllMain,
    // ahead of everything, and MH_ExeDir()'s first caller is also what initialises the run context
    // (role detection, the log directory). Moving that initialisation earlier than it has ever
    // happened, in the one function whose job is to be installed before anything can go wrong, is a
    // change to boot ordering wearing a convenience's clothes.
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *slash = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';

    char ini[MAX_PATH];
    wsprintfA(ini, "%smh_net.ini", exe);

    // WHERE THE MARKER GOES. The launcher names it (it has to: it is the reader, and it must not
    // have to guess a path built from a pid it would then have to match). With no launcher the
    // default is `<exedir>\logs\mh_crash_<pid>.marker` -- under `logs\` because that is the
    // directory this project already treats as its output tree, and per-pid because the rig runs
    // several lanes whose exes all have the same name. dist LA13: a process started with
    // MH_LOG_ROOT (the launcher-owned logs root, see mh_common/run_context.cpp) and no marker path
    // puts the default under THAT root instead -- beside the exe it would be UAC-virtualized away
    // under Program Files, exactly the hole the root exists to close. Same root run_context uses,
    // resolved here independently (same reason MH_ExeDir() is not called: boot ordering).
    if (GetEnvironmentVariableA(MH_CRASH_ENV_MARKER, g_marker, MAX_PATH) == 0) {
        char  logs[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("MH_LOG_ROOT", logs, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            wsprintfA(logs, "%slogs", exe);
        } else {
            while (n > 1 && (logs[n - 1] == '\\' || logs[n - 1] == '/')) logs[--n] = '\0';
        }
        CreateDirectoryA(logs, nullptr); // idempotent; failure just means the path below will not open
        char leaf[64];
        wsprintfA(leaf, MH_CRASH_MARKER_LEAF, (unsigned long)GetCurrentProcessId());
        if (lstrlenA(logs) + 1 + lstrlenA(leaf) < MAX_PATH) wsprintfA(g_marker, "%s\\%s", logs, leaf);
    }

    // The handshake, IF a launcher is listening. Both events are created rather than opened so the
    // order the two processes start in does not matter: whoever is second gets a handle to the one
    // that already exists. Auto-reset, because each is signalled exactly once per crash and a
    // manual-reset event would stay hot for a second fault the handler has already declined to
    // report.
    char channel[128];
    if (GetEnvironmentVariableA(MH_CRASH_ENV_CHANNEL, channel, sizeof(channel)) != 0 && channel[0] != '\0') {
        char name[192];
        wsprintfA(name, "%s%s%s", MH_CRASH_EVENT_PREFIX, channel, MH_CRASH_EVENT_REQ);
        g_req = CreateEventA(nullptr, FALSE, FALSE, name);
        wsprintfA(name, "%s%s%s", MH_CRASH_EVENT_PREFIX, channel, MH_CRASH_EVENT_ACK);
        g_ack = CreateEventA(nullptr, FALSE, FALSE, name);
        if (g_req == nullptr || g_ack == nullptr) {
            // Degrade to marker-only rather than to a handler that waits on a handle it does not
            // have: the marker alone still gives the launcher module+offset+match_id.
            if (g_req) CloseHandle(g_req);
            if (g_ack) CloseHandle(g_ack);
            g_req = nullptr;
            g_ack = nullptr;
        }
    }

    mh_sd_copy(g_build, MH_CRASH_BUILD_CAP, MH_VERSION_FULL);

    // LAST IN THE CHAIN -- see the header note. This is the only argument to this call and it is
    // the whole interaction with the two handlers this process already installs.
    g_veh = AddVectoredExceptionHandler(0 /* First = FALSE: append */, crash_veh);
    if (g_veh == nullptr) return 0;

    const int crash_after_ms = GetPrivateProfileIntA("debug", "crash_after_ms", 0, ini);
    if (crash_after_ms > 0) {
        // CreateThread from DllMain is safe as long as nothing WAITS on the new thread here, which
        // nothing does -- the same pattern mh/seams/net_seams.cpp's marker_scan_thread already uses
        // from this same DllMain. The new thread blocks on the loader lock until DllMain returns,
        // which is exactly the delay Sleep() was going to impose anyway.
        HANDLE t = CreateThread(nullptr, 0, crash_after_thread,
                                (LPVOID)(ULONG_PTR)(DWORD)crash_after_ms, 0, nullptr);
        if (t != nullptr) CloseHandle(t);
    }
    return 1;
}

extern "C" void MH_CrashMarker_Shutdown(void) {
    // THE POINT OF HAVING A DETACH ARM AT ALL. A vectored handler outlives the module it lives in
    // unless it is removed, and the next exception after an unload -- any exception, in any thread,
    // including one the game was going to handle -- would then call into unmapped memory.
    if (g_veh != nullptr) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
    if (g_req != nullptr) {
        CloseHandle(g_req);
        g_req = nullptr;
    }
    if (g_ack != nullptr) {
        CloseHandle(g_ack);
        g_ack = nullptr;
    }
}
