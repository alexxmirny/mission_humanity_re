//
// mh/seams/harness_bind.cpp -- THE INSTRUMENT BIND, and mh.dll's whole surface onto mh_harness.dll.
//
// The third satellite, and the sibling of seams/module_bind.cpp (mh_net.dll) and seams/libmh_bind.cpp
// (libmh.dll). Read mh/include/mh_module_bind.h first (F4A's ruling: LoadLibrary on an absolute path
// beside mh.dll + GetProcAddress per symbol, from the first statement of DLL_PROCESS_ATTACH, with an
// inert satellite DllMain and mh.dll as the sole orchestrator), then mh/include/mh_harness_module.h
// (the thirteen rows and what each answers with no instrument). docs/dll-split.md carries the
// measurements.
//
// This file does three things:
//
//   1. BINDS mh_harness.dll from DLL_PROCESS_ATTACH -- after libmh, because the instrument has to be
//      TOLD which configuration the run is in (MH_HarnessModuleHost::spine_bound) and mh.dll does not
//      know that until libmh's own bind has run. One loud log line whatever happens.
//   2. DEFINES all thirteen MH_Harness_* symbols inside mh.dll as forwarding shims over the bound
//      table. Same construction as F4B's, and for the same reason: net_lockstep, video, sim_step,
//      reimpl_probe, net_seams and turn_engine call these unchanged, byte for byte, and "is the
//      instrument there" stops being a property every caller has to remember.
//   3. ANSWERS the one case that is NOT silent -- `[harness] enable=1` with no module.
//
// ---- WHY ABSENCE IS SILENT AND THE MISCONFIGURATION IS NOT ----------------------------------------
//
// A missing mh_net.dll is a degradation a player can see. A missing libmh.dll is configuration (1).
// A missing mh_harness.dll is neither: the game is BIT-FOR-BIT the game it always was, because the
// only thing the instrument does is observe. So the thirteen shims answer what the un-armed bodies
// already answered and nothing anywhere behaves differently -- which is exactly why the one case
// where somebody ASKED for the instrument must be loud. G178 is the recorded cost of the opposite: a
// determinism run with the instrument silently never installed produced a log nobody could tell from
// a healthy one.
//
// REFUSED, NOT FATAL, and the F2E scoping rule is what decides it. mh::config::detail::refuse
// terminates because a misspelled `[config] mode` is an author believing something false about WHICH
// BODIES RUN. This is not that: the instrument installs nothing, so its absence cannot change which
// bodies run -- it changes whether the run is MEASURED. And the measurement's absence is not quiet
// either way: mh_harness.log is not written, so mp_analyze, check_arm_order's harness channel and
// every --determinism consumer red on a file that is not there. Killing the process would turn a
// stray ini key into a dead game for no evidence gained.
//
#include <windows.h>

#include "include/mh_module_bind.h"
#include "include/mh_harness_module.h"
#include "config/config.h"  // exe_dir + refuse's three channels -- one composition rule, one refusal
#include "mh_run_context.h" // MH_RunDir + mh_log_stamp (mh_common; self-initialising)

namespace {

const char *const MODULE_FILE = "mh_harness.dll";
const char *const MODULE_TAG  = "mh_harness"; // what the log lines call it

// THE HANDLE IS HELD FOR THE LIFE OF THE PROCESS AND DELIBERATELY NEVER READ AGAIN -- the same rule
// the two sibling binds state. Released only on the REFUSAL paths, where nothing has been bound yet.
HMODULE g_module = nullptr;
bool    g_done   = false;
bool    g_bound  = false;

// mh.dll's OWN DLL_PROCESS_ATTACH timestamp. The R2 measurement is the SIGN of
// (module attach_qpc - this): negative means the module's DllMain ran FIRST, which is what a static
// import produces and what the inert-DllMain rule has to hold against.
long long g_self_qpc = 0;

#define MH_HARNESS_BIND_FIELD(ret, name, params, args, absent) ret(__cdecl *name) params;
struct bound_t {
    MH_HARNESS_MODULE_SYMBOLS(MH_HARNESS_BIND_FIELD)
};
bound_t g_b = {};

void mod_log(const char *s) {
    // mh_net.log, composed locally from MH_RunDir() -- Q9: it keeps its name and is the CORE-ARM
    // log. net_internal.h's g_log is filled by build_paths(), which runs inside MH_Core_Arm_Early,
    // AFTER this; a bind that logged through it would write to an empty path.
    char path[MAX_PATH];
    wsprintfA(path, "%smh_net.log", MH_RunDir());
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    char  stamp[24];
    int   sn = mh_log_stamp(stamp);
    WriteFile(h, stamp, sn, &wrote, nullptr);
    WriteFile(h, s, lstrlenA(s), &wrote, nullptr);
    CloseHandle(h);
}

// `[harness] enable` read straight from the ini, composed the way mh::config composes its own: from
// the PROCESS image, because configuration belongs to the installation. It is asked HERE, before
// build_paths() has run, which is why this does not use g_ini_path.
bool harness_configured() {
    char dir[MAX_PATH];
    char ini[MAX_PATH];
    mh::config::detail::exe_dir(dir);
    wsprintfA(ini, "%smh_net.ini", dir);
    return GetPrivateProfileIntA("harness", "enable", 0, ini) != 0;
}

// THE LOUD REFUSAL -- all three channels mh::config::detail::refuse uses, and deliberately NOT its
// TerminateProcess (see the file banner). A file beside the exe is what an automated lane reads back
// after the process is gone; OutputDebugString is what a debugger sees; stderr is what a console host
// shows. The log line goes into mh_net.log too, so a reader following the bind outcomes finds it in
// the place the other two satellites report.
void refuse_configured_but_absent(const char *path) {
    char dir[MAX_PATH];
    mh::config::detail::exe_dir(dir);
    char line[MAX_PATH + 900];
    wsprintfA(line,
              "mh.dll: `[harness] enable=1` IS SET AND THERE IS NO INSTRUMENT.\r\n"
              "LoadLibrary(%s) failed -- the file is not beside mh.dll.\r\n"
              "THIS RUN IS NOT INSTRUMENTED: no mh_harness.log, no per-step hashes, no journal "
              "record or replay, no relocating state bind. The game itself is unaffected and the "
              "boot continues, because the harness only ever observes -- but every verdict that "
              "reads mh_harness.log (determinism, --ui-abc, --sp-determinism, check_arm_order's "
              "harness channel) has nothing to read and will say so.\r\n"
              "Deploy mh_harness.dll beside mh.dll, or remove `enable=1` from the `[harness]` "
              "section so the configuration and the run agree.\r\n",
              path);
    OutputDebugStringA(line);
    {
        char p2[MAX_PATH];
        wsprintfA(p2, "%smh_harness_refused.log", dir);
        HANDLE h = CreateFileA(p2, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
            CloseHandle(h);
        }
    }
    // stderr without the CRT: the console handle, if this process has one (a lane launched from a
    // console redirects it to a file, which is the R2 discipline's whole evidence channel).
    {
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
        if (e != nullptr && e != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(e, line, lstrlenA(line), &wrote, nullptr);
        }
    }
    // NOTHING GOES INTO mh_net.log FROM HERE, and that is the "one line, N facts" constraint both
    // sibling binds state rather than a decision to be quiet. This bind lands at the head of the arm
    // window check_arm_order gates, so a SECOND `[modules] mh_harness:` line would be a structural
    // step that exists in one boot shape and not the others -- an unbaselined arm log for a
    // configuration that is an operator's mistake, which is the worst possible thing to make a gate
    // argue about. The one line the bind already writes carries the extra sentence instead (see
    // bind()), so every shape is structurally identical and the difference is TEXT, i.e. an `alt`.
}

// Compose "<dir of the module `self`>\<name>" -- next to MH.DLL rather than next to the exe (R7: a
// sibling MODULE belongs to the BUILD), by absolute path so that "the module is not here" is a fact
// rather than a search that quietly succeeded elsewhere (msvfw32's Rule 2).
void sibling_path(HMODULE self, const char *name, char *out) {
    out[0] = '\0';
    if (GetModuleFileNameA(self, out, MAX_PATH) == 0) return;
    char *slash = nullptr;
    for (char *p = out; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (slash != nullptr) slash[1] = '\0';
    lstrcatA(out, name);
}

void bind(HMODULE self) {
    char path[MAX_PATH];
    sibling_path(self, MODULE_FILE, path);

    HMODULE h = path[0] != '\0' ? LoadLibraryA(path) : nullptr;
    if (h == nullptr) {
        const DWORD err        = GetLastError();
        const bool  configured = harness_configured();
        char        b[MAX_PATH + 560];
        // ONE LINE, whichever shape this is. The tail sentence is the only difference between an
        // uninstrumented run nobody asked to instrument and an operator who set `[harness] enable=1`
        // and will not get one -- so check_arm_order sees one structural step with two spellings
        // (an `alt`) rather than a step that appears in one boot shape and not the others.
        wsprintfA(b,
                  "; [modules] %s: NOT BOUND -- LoadLibrary(%s) failed, Win32 error %u (%s). This "
                  "run is UNINSTRUMENTED: no per-step hashes, no journal, no determinism log. The "
                  "game is unaffected -- the harness only observes -- so the boot continues.%s\n",
                  MODULE_TAG, path, err,
                  err == ERROR_MOD_NOT_FOUND ? "the file is not there"
                                             : "see the Win32 error code",
                  configured ? " `[harness] enable=1` IS SET, so this run asked for an instrument "
                               "and will not get one -- see mh_harness_refused.log beside the exe."
                             : "");
        mod_log(b);
        // ...and if this run ASKED for an instrument, say so where it cannot be missed.
        if (configured) refuse_configured_but_absent(path);
        return;
    }

    // Resolve EVERY contract row before committing to any of them. A partially bound instrument is
    // the worst outcome -- some calls reach the module and some answer the un-armed value -- so the
    // table is filled into a local and only adopted whole.
    bound_t t   = {};
    int     got = 0;
    char    missing[420];
    missing[0] = '\0';
#define MH_HARNESS_BIND_RESOLVE(ret, name, params, args, absent) \
    t.name = (ret(__cdecl *) params)GetProcAddress(h, #name);    \
    if (t.name != nullptr) {                                     \
        ++got;                                                   \
    } else if (lstrlenA(missing) < 340) {                        \
        lstrcatA(missing, missing[0] ? " " : "");                \
        lstrcatA(missing, #name);                                \
    }
    MH_HARNESS_MODULE_SYMBOLS(MH_HARNESS_BIND_RESOLVE)
#undef MH_HARNESS_BIND_RESOLVE

    typedef int(__cdecl * PFN_ModInit)(const MH_HarnessModuleHost *);
    typedef void(__cdecl * PFN_ModProbe)(MH_HarnessModuleProbe *);
    PFN_ModInit  mod_init  = (PFN_ModInit)GetProcAddress(h, "MH_HarnessModule_Init");
    PFN_ModProbe mod_probe = (PFN_ModProbe)GetProcAddress(h, "MH_HarnessModule_Probe");

    if (got != MH_HARNESS_MODULE_SYMBOL_COUNT || mod_init == nullptr || mod_probe == nullptr) {
        char b[MAX_PATH + 760];
        wsprintfA(b,
                  "; [modules] %s: LOADED BUT REFUSED -- %s resolved %d of %d contract symbols "
                  "(init=%d probe=%d); missing: %s. Continuing UNINSTRUMENTED.\n",
                  MODULE_TAG, path, got, (int)MH_HARNESS_MODULE_SYMBOL_COUNT, mod_init != nullptr,
                  mod_probe != nullptr, missing[0] ? missing : "(none by name)");
        mod_log(b);
        FreeLibrary(h);
        return;
    }

    // THE R2 MEASUREMENT, asked BEFORE init so the numbers describe the LOAD and nothing else.
    MH_HarnessModuleProbe p = {};
    mod_probe(&p);
    if (p.size != (unsigned)sizeof(MH_HarnessModuleProbe) || p.abi != MH_HARNESS_MODULE_ABI) {
        char b[300];
        wsprintfA(b,
                  "; [modules] %s: ABI MISMATCH -- module reports size=%u abi=%08X, this build "
                  "wants size=%u abi=%08X. Continuing UNINSTRUMENTED.\n",
                  MODULE_TAG, p.size, p.abi, (unsigned)sizeof(MH_HarnessModuleProbe),
                  MH_HARNESS_MODULE_ABI);
        mod_log(b);
        FreeLibrary(h);
        return;
    }

    // Hand it the run context AND the spine verdict. The init call is where the module binds its own
    // two contract tables -- the host rows out of mh.dll and the spine rows out of libmh.dll -- which
    // is why this is the moment `spine_bound` has to be true or false and not unknown.
    MH_HarnessModuleHost host;
    host.size        = (unsigned)sizeof(MH_HarnessModuleHost);
    host.abi         = MH_HARNESS_MODULE_ABI;
    host.run_dir     = MH_RunDir();
    host.spine_bound = MH_LibmhModule_IsBound();
    // ...and whether anybody asked for an instrument at all. See MH_HarnessModuleHost's note: with
    // no `[harness] enable=1` a failed spine bind is recorded and not shouted, because a player in
    // configuration (1) asked for nothing and should be told nothing.
    host.configured      = harness_configured() ? 1 : 0;
    const int module_abi = mod_init(&host);

    g_module = h;
    g_b      = t;
    g_bound  = true;

    // Re-probe AFTER init: the row counts are what the module's own two binds resolved, and they are
    // the fact Q4's refusal is checkable by. A run that says `spine=0/N` is configuration (1) with
    // the instrument declining to arm, which is a different sentence from `spine=N/N`.
    MH_HarnessModuleProbe q = {};
    mod_probe(&q);

    // THE CALL-THROUGH, and it is a different claim from either of the two above. GetProcAddress
    // succeeding is not evidence that a CALL across the boundary works, and neither is the module's
    // init returning its ABI -- that is a call into the module-level entry, not into the CONTRACT.
    // MH_Harness_WantsPresentTick is the right probe for the three reasons MH_Net_IsStarted is one
    // module over: it is pure, it has no side effect, and at DLL_PROCESS_ATTACH its answer is KNOWN
    // -- it reads the UI-journal arm and `boot_snapshot`, neither of which load_config has filled
    // yet, so it must be 0. A wrong answer means the boundary is broken and this line says so.
    // tools/check_module_bind.py refuses a BOUND line that does not carry this, for exactly that
    // reason: resolving an export is not the same as being able to call it.
    const int probe_tick = t.MH_Harness_WantsPresentTick();

    // ONE LINE, and its being one line is a constraint rather than terseness -- the same one both
    // sibling binds state. This lands at the head of the arm window check_arm_order gates, so every
    // line it writes is a structural baseline entry in every committed template; F4A pre-ruled the
    // cost of a DllMain arm as exactly one inserted step.
    LARGE_INTEGER freq;
    freq.QuadPart = 0;
    QueryPerformanceFrequency(&freq);
    const long long delta = p.attach_qpc - g_self_qpc;
    long long       us    = 0;
    if (freq.QuadPart != 0) us = (delta * 1000000) / freq.QuadPart;
    char b[700];
    wsprintfA(b,
              "; [modules] %s: BOUND at DllMain (under the loader lock) -- %d exports resolved, "
              "init returned %08X, call-through %s, host=%d/%d spine=%d/%d; module "
              "DLL_PROCESS_ATTACH was %s mh.dll's by %d us (attach_calls=%u attach_tid=%u, "
              "mh.dll tid=%u)\n",
              MODULE_TAG, (int)MH_HARNESS_MODULE_SYMBOL_COUNT, module_abi,
              probe_tick == 0 ? "ok" : "WRONG -- the boundary call is broken", q.host_rows,
              q.host_total, q.spine_rows, q.spine_total, delta >= 0 ? "AFTER" : "BEFORE",
              (int)(us < 0 ? -us : us), p.attach_calls, p.attach_tid,
              (unsigned)GetCurrentThreadId());
    mod_log(b);
}

} // namespace

extern "C" void MH_HarnessBind_Early(HMODULE self) {
    if (g_done) return;
    g_done = true;
    // OUR timestamp first, so the loader-order comparison is against the earliest instant mh.dll can
    // name about itself.
    LARGE_INTEGER t;
    if (QueryPerformanceCounter(&t)) g_self_qpc = (long long)t.QuadPart;
    bind(self);
}

extern "C" int MH_HarnessModule_IsBound(void) { return g_bound ? 1 : 0; }

// ---- THE FORWARDING SHIMS --------------------------------------------------------------------------
//
// One definition per contract row, and between them they are the ENTIRE surface mh.dll has onto the
// instrument. Every MH_Harness_* call in this DLL links against these, unchanged, and none of the
// callers knows the harness moved. The absent branch is not a stub of convenience: each is the value
// the REAL body returns with no `[harness] enable=1` (the table and the reasoning are in
// mh_harness_module.h), so an absent instrument and a disarmed one are indistinguishable -- which is
// exactly the property every caller was already written against, because the harness has always
// shipped off by default.
#define MH_HARNESS_BIND_SHIM(ret, name, params, args, absent) \
    extern "C" ret name params {                              \
        if (!g_bound) absent;                                 \
        return g_b.name args;                                 \
    }
MH_HARNESS_MODULE_SYMBOLS(MH_HARNESS_BIND_SHIM)
#undef MH_HARNESS_BIND_SHIM
