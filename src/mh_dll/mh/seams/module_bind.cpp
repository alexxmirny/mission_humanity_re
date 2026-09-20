//
// mh/seams/module_bind.cpp -- THE SIBLING-DLL BIND, and mh.dll's whole surface onto mh_net.dll.
//
// Read mh/include/mh_module_bind.h first (the problem, the three mechanisms, F4A's ruling) and
// mh_common/include/mh_net_module.h second (the contract: the 26 bound symbols and what each one
// answers when the module is not there). docs/dll-split.md carries the measurements.
//
// This file does three things and they are deliberately in one TU:
//
//   1. BINDS mh_net.dll from DLL_PROCESS_ATTACH -- LoadLibrary on an absolute path beside mh.dll,
//      GetProcAddress per symbol, an ABI handshake, and one loud log line whatever happens.
//   2. DEFINES all 26 MH_Net_* / MH_Key_* symbols INSIDE mh.dll as forwarding shims over the bound
//      table. This is the part that makes the split cost the rest of the DLL nothing: 75 call sites
//      across 8 files are unchanged, and the 18 that had no transport-present test in front
//      (launch.cpp 12, ui_drive.cpp 1, gfx_overlay.cpp 1, harness.cpp 1, desync_watch 3 which ARE
//      gated) are safe BY CONSTRUCTION rather than by 18 new guards somebody has to maintain.
//   3. ANSWERS `MH_NetModule_IsBound()`, which is what mh::net::transport_present() reads.
//
// ---- WHAT THIS FILE IS CAREFUL ABOUT, AND WHY EACH ONE IS LOAD-BEARING --------------------------
//
// 1. ABSOLUTE PATH, NEXT TO MH.DLL. LoadLibraryA with a bare name would run the whole search order
//    (application dir, system32, %PATH%) and could bind a stranger that happens to share the name --
//    msvfw32's Rule 2, which exists because that shim's own bare-name load found ITSELF. Composing
//    the path from OUR module handle also makes ABSENCE deterministic: exactly one path is tried,
//    so "the module is not here" is a fact rather than a search that quietly succeeded elsewhere.
//
//    NEXT TO MH.DLL RATHER THAN NEXT TO THE EXE, and the distinction is deliberate even though the
//    two directories are the same in every deployment we ship. R7: mh::config composes its INI path
//    from the PROCESS image (GetModuleFileNameA(nullptr)) on purpose, because configuration belongs
//    to the installation. A sibling MODULE belongs to the BUILD, so it is resolved relative to the
//    build artefact that needs it. One rule each, neither derived from the other.
//
// 2. `[net] module=none` IS CHECKED BEFORE THE LOAD, NOT AFTER IT. F3F's key survives the F4 move
//    with its meaning intact -- "behave as though there is no network module" -- and the honest
//    implementation of that is to not attempt the load at all. A run that asked for the no-module
//    configuration and then loaded the module anyway would be a lie in both directions: the file
//    would be mapped, its DllMain would have run, and only the answers would pretend otherwise.
//    A TYPO IS REFUSED, not defaulted (F2E's rule), through mh::config's own mechanism.
//
// 3. THE BIND FAILURE PATH IS LOUD AND NON-FATAL. A missing satellite logs a line naming the path
//    and the Win32 error and the boot CONTINUES. The asymmetry against rule 2 is the whole point of
//    the item: a mis-SPELLED configuration is the author being wrong, a MISSING module is the
//    shipped degradation we are building.
//
// 4. THE LOG IS mh_net.log, COMPOSED HERE. Same file, same stamp format as the transport's own
//    logf() and the seams' seam_log(), and it has to be composed locally for the same reason
//    mh::config composes its own ini path: net_internal.h's `g_log` is filled by build_paths(),
//    which runs inside MH_Core_Arm_Early -- AFTER this. A bind that logged through g_log from
//    DllMain would write to an empty path, and the one line that reports whether the transport
//    exists would say nothing. (Q9: mh_net.log keeps its name and is the CORE-ARM log -- 49 of the
//    69 structural lines in a brokered boot are mh.dll's, 0 are mh_net.dll's.)
//
// 5. THE WS2_32 ANCHOR. See its comment below; it is the subset rule staying true after the
//    transport left, and it is the one piece of this file that looks like nothing and is not.
//
#include <winsock2.h> // htons -- the WS2_32 anchor, see ws2_anchor() below
#include <windows.h>

#include "include/mh_module_bind.h"
#include "config/config.h"  // exe_dir + refuse -- one composition rule, one refusal mechanism
#include "mh_net_module.h"  // the contract: MH_NET_MODULE_SYMBOLS, the host/probe structs, the ABI
#include "mh_run_context.h" // MH_RunDir + mh_log_stamp (mh_common; self-initialising)
#include "mh_version.h"     // MH_VERSION_FULL -- the build stamp, from src/mh_dll/mh_version.props

#pragma comment(lib, "ws2_32.lib")

namespace {

// THE TAG IS THE ROLE, NOT THE FILE, and that distinction is load-bearing since mp:T1 gave the role
// two candidate files. `mh_net` names THE NET MODULE -- the satellite this bind is about -- and
// every outcome line, every baseline in check_arm_order and every `--module mh_net` invocation of
// check_module_bind reads that word. Renaming it per transport would have made a UDP run's log
// unreadable to the two gates that read this log, for no gain: which FILE was bound is stated by the
// transport line below and by the path in the failure line.
const char *const MODULE_TAG = "mh_net";
const char *const FILE_TCP   = "mh_net.dll";
const char *const FILE_UDP   = "mh_net_udp.dll";

// THE HANDLE IS HELD FOR THE LIFE OF THE PROCESS AND DELIBERATELY NEVER READ AGAIN. It is released
// only on the REFUSAL paths (missing export / ABI mismatch), where nothing has been bound yet and
// dropping the reference is what stops a rejected module staying mapped. On the success path there
// is no FreeLibrary and there must not be: the exports mh.dll resolved are raw pointers into that
// image, and the whole point of binding at DLL_PROCESS_ATTACH is that they stay valid for the run.
HMODULE g_module = nullptr;
bool    g_done   = false;
bool    g_bound  = false;

// mh.dll's OWN DLL_PROCESS_ATTACH timestamp, taken before anything else. The R2 measurement is the
// SIGN of (module attach_qpc - this): negative means the module's DllMain ran FIRST, which is what a
// static import produces and what every satellite's DllMain has to be inert against.
long long g_self_qpc = 0;

// ---- the bound table -----------------------------------------------------------------------------
//
// One function pointer per contract row, expanded from the ONE list in mh_net_module.h. Nothing here
// is hand-written, which is the point: a hand-maintained binder table is the G106 hand-list shape --
// it looks complete, nobody checks it, and the first forgotten symbol is a call through null.
#define MH_NET_BIND_FIELD(ret, name, params, args, absent) ret(__cdecl *name) params;
struct bound_t {
    MH_NET_MODULE_SYMBOLS(MH_NET_BIND_FIELD)
};
bound_t g_b = {};

// ---- the log -------------------------------------------------------------------------------------

void mod_log(const char *s) {
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

// ---- THE WS2_32 ANCHOR ---------------------------------------------------------------------------
//
// This is the subset rule, kept true by construction, and it is the one thing about F4B that a
// reader would delete as dead code. The rule says a satellite is safe to load from DllMain when its
// static imports are a SUBSET of mh.dll's own -- because then the loader snaps its import table
// against modules it has ALREADY initialised, and there is no second DllMain to run under the lock
// we hold. mh_net.dll imports WS2_32. mh.dll imported WS2_32 too, for exactly one reason: the
// transport was compiled into it. The transport has now left.
//
// So without this call mh.dll's import table loses WS2_32, the subset rule breaks on the FIRST
// satellite it was written for, and loading mh_net.dll from DLL_PROCESS_ATTACH would drag a fresh
// ws2_32.dll in behind it and run its DllMain under our loader lock -- the open-ended-dependency
// hazard net_lockstep.cpp:335-337 is right about. Keeping the import costs one instruction and a
// line in the IAT.
//
// IT IS A REAL CALL WITH ITS RESULT STORED, NOT AN ADDRESS IN A GLOBAL, and that is a MEASURED
// correction rather than a style choice: at F4A the static-import arm was first anchored by storing
// `&Export` in a non-const global (the msvfw32 shim's technique, chosen there because a call could
// be optimised away), and under mh.dll's Release /GL + /LTCG the unreferenced global was eliminated,
// the import descriptor was never emitted, and dumpbin showed the ordinary four system DLLs. The
// measurement silently measured the wrong thing. A call from a function DllMain reaches, whose
// result is written to a volatile, is what survives whole-program optimisation.
//
// htons specifically: a pure byte-swap, documented as usable without WSAStartup, no Winsock state
// touched, no failure mode. The gate that keeps this honest is check_module_bind.py --subset, which
// compares both PEs' import tables and fails if mh_net.dll imports anything mh.dll does not.
volatile unsigned short g_ws2_anchor = 0;
void                    ws2_anchor(void) { g_ws2_anchor = htons(0xF4B0); }

// ---- `[net] module`: the one key that still gates the load ---------------------------------------
//
// F3F introduced it as a stand-in ("pretend there is no transport", because the transport was
// compiled in and could not actually be absent). It survives the move with the meaning it always
// documented -- `none` = do not even attempt the load -- and it is now the ONLY thing besides the
// file's existence that decides. `auto`, the default, means "use the transport this build has".
bool module_declined(void) {
    char dir[MAX_PATH];
    char ini[MAX_PATH];
    mh::config::detail::exe_dir(dir);
    wsprintfA(ini, "%smh_net.ini", dir);

    char v[32] = {0};
    GetPrivateProfileStringA("net", "module", "auto", v, sizeof(v), ini);
    if (lstrcmpiA(v, "auto") == 0) return false;
    if (lstrcmpiA(v, "none") == 0) return true;
    char what[512];
    wsprintfA(what, "`[net] module=%s` in %s is not a value this build implements.", v, ini);
    mh::config::detail::refuse(
        dir, what,
        "The values are `auto` (the shipping default -- load the transport file from beside mh.dll, and "
        "degrade loudly if it is not there) and `none` (do not even attempt the load: the transport "
        "seams, the control-frame handler binds and the bootstrap protocol stubs are not installed, "
        "and the MP browser says why). It is NOT `[net] enable`, which is the operator's off switch "
        "for a transport that IS present. A misspelling is refused rather than defaulted because "
        "this key exists to be believed: a lane asking for the no-module configuration and silently "
        "getting the full transport would report a graceful degradation nobody exercised.");
    return false; // unreachable; refuse() terminated
}

// ---- `[net] transport`: WHICH net module this run binds (tracker mp:T1) --------------------------
//
// A THIRD AXIS beside `module` and `enable`, and a different question from both: `module` asks
// whether this build has a transport at all, `enable` is the operator's off switch for one it has,
// and this asks WHICH of the two implementations of the same 26-export contract answers. `udp` is
// mh_net_udp.dll, T0's packet format over one socket per process, and THE SHIPPING DEFAULT since
// 2026-09-20 (user ruling: it is the transport the relay needs, the one the launcher configures and
// the one the determinism gate boots its peers with -- so an absent key and an absent ini both
// mean udp). `tcp` is mh_net.dll, the original restoration and the reference the UDP transport was
// measured against; it is direct dial only (no relay), so it is the explicit choice now.
//
// A TYPO IS REFUSED, NOT DEFAULTED, and that is this item's own acceptance clause as much as it is
// F2E's standing rule. Silently falling back to the default on `transport=udo` would produce a run
// that reports one configuration in every log line the operator reads and plays another -- the
// failure shape where a knob is believed and does nothing. refuse() writes mh_config_refused.log
// beside the exe, prints to stderr and OutputDebugString, and terminates: three channels, because
// this happens before any logger exists and the refusal's whole job is to be found.
//
// Returns the file name to bind, or nullptr on `transport=udp` meaning "the default, say nothing
// extra". The nullptr distinction exists so a DEFAULT run's arm window is byte-identical to what it
// was before T1: check_arm_order baselines every structural line of the boot, and a new line emitted
// unconditionally would red that gate on every lane for a knob nobody set. The 2026-09-20 flip kept
// that invariant by moving WHICH value is silent, not by adding a line: a plain lane still emits
// nothing here, and the explicit `transport=tcp` is the one that now names its file.
const char *transport_file(void) {
    char dir[MAX_PATH];
    char ini[MAX_PATH];
    mh::config::detail::exe_dir(dir);
    wsprintfA(ini, "%smh_net.ini", dir);

    char v[32] = {0};
    GetPrivateProfileStringA("net", "transport", "udp", v, sizeof(v), ini);
    if (lstrcmpiA(v, "udp") == 0) return nullptr;
    if (lstrcmpiA(v, "tcp") == 0) return FILE_TCP;
    char what[512];
    wsprintfA(what, "`[net] transport=%s` in %s is not a transport this build implements.", v, ini);
    mh::config::detail::refuse(
        dir, what,
        "The values are `udp` (the shipping default -- mh_net_udp.dll, the UDP transport: one socket "
        "per process, T0's authenticated packet format, redundant step inputs, the relay) and `tcp` "
        "(mh_net.dll, the TCP client-server star, direct dial only). Both answer the same 26-export "
        "module contract, so the only thing this key changes is which file mh.dll binds. A "
        "misspelling is refused rather than defaulted because this key exists to be believed: a run "
        "asking for tcp and silently getting udp would report a transport it never used in every "
        "log line an operator reads.");
    return nullptr; // unreachable; refuse() terminated
}

// Compose "<dir of the module `self`>\<name>". `self` is mh.dll's own handle (see rule 1 above).
void sibling_path(HMODULE self, const char *name, char *out) {
    out[0] = '\0';
    if (GetModuleFileNameA(self, out, MAX_PATH) == 0) return;
    char *slash = nullptr;
    for (char *p = out; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (slash != nullptr) slash[1] = '\0';
    lstrcatA(out, name);
}

void bind(HMODULE self, const char *module_file) {
    char path[MAX_PATH];
    sibling_path(self, module_file, path);

    HMODULE h = path[0] != '\0' ? LoadLibraryA(path) : nullptr;
    if (h == nullptr) {
        const DWORD err = GetLastError();
        char        b[MAX_PATH + 360];
        // THE DEGRADATION LINE. Loud, named, and followed by a boot that continues. Every shim below
        // now answers its absent value, transport_present() answers false, the nine net arm steps
        // skip and the MP browser carries the standing no-module notice (ruling Q2).
        wsprintfA(b,
                  "; [modules] %s: NOT BOUND -- LoadLibrary(%s) failed, Win32 error %u (%s). "
                  "Continuing WITHOUT the module: this is the absent-tolerant path, not a failure "
                  "of the boot.\n",
                  MODULE_TAG, path, err,
                  err == ERROR_MOD_NOT_FOUND ? "the file is not there"
                                             : "see the Win32 error code");
        mod_log(b);
        return;
    }

    // Resolve EVERY contract symbol before committing to any of them. A partially bound transport is
    // the worst of the three outcomes -- some calls reach the module and some do not -- so the table
    // is filled into a local and only adopted whole.
    bound_t t   = {};
    int     got = 0;
    char    missing[512];
    missing[0] = '\0';
#define MH_NET_BIND_RESOLVE(ret, name, params, args, absent)  \
    t.name = (ret(__cdecl *) params)GetProcAddress(h, #name); \
    if (t.name != nullptr) {                                  \
        ++got;                                                \
    } else if (lstrlenA(missing) < 440) {                     \
        lstrcatA(missing, missing[0] ? " " : "");             \
        lstrcatA(missing, #name);                             \
    }
    MH_NET_MODULE_SYMBOLS(MH_NET_BIND_RESOLVE)
#undef MH_NET_BIND_RESOLVE

    typedef int(__cdecl * PFN_ModInit)(const MH_NetModuleHost *);
    typedef void(__cdecl * PFN_ModProbe)(MH_NetModuleProbe *);
    PFN_ModInit  mod_init  = (PFN_ModInit)GetProcAddress(h, "MH_NetModule_Init");
    PFN_ModProbe mod_probe = (PFN_ModProbe)GetProcAddress(h, "MH_NetModule_Probe");

    if (got != MH_NET_MODULE_SYMBOL_COUNT || mod_init == nullptr || mod_probe == nullptr) {
        char b[MAX_PATH + 700];
        // A module that loaded but does not carry the contract is a WRONG module, not a missing one,
        // and it gets a different sentence -- the distinction msvfw32's resolver also draws. Naming
        // the symbols is what turns "your mh_net.dll is old" from a guess into a diff.
        wsprintfA(b,
                  "; [modules] %s: LOADED BUT REFUSED -- %s resolved %d of %d contract symbols "
                  "(init=%d probe=%d); missing: %s. Continuing without it.\n",
                  MODULE_TAG, path, got, (int)MH_NET_MODULE_SYMBOL_COUNT, mod_init != nullptr,
                  mod_probe != nullptr, missing[0] ? missing : "(none by name)");
        mod_log(b);
        FreeLibrary(h);
        return;
    }

    // THE R2 MEASUREMENT, asked BEFORE init so the numbers describe the LOAD and nothing else.
    MH_NetModuleProbe p;
    p.size = 0;
    p.abi  = 0;
    mod_probe(&p);
    if (p.size != (unsigned)sizeof(MH_NetModuleProbe) || p.abi != MH_NET_MODULE_ABI) {
        char b[260];
        wsprintfA(b,
                  "; [modules] %s: ABI MISMATCH -- module reports size=%u abi=%08X, this build "
                  "wants size=%u abi=%08X. Continuing without it.\n",
                  MODULE_TAG, p.size, p.abi, (unsigned)sizeof(MH_NetModuleProbe),
                  MH_NET_MODULE_ABI);
        mod_log(b);
        FreeLibrary(h);
        return;
    }

    // Hand the module its run context (ruling Q1: run_context.cpp stays mh.dll-side, so the module is
    // GIVEN the log directory rather than composing one). Adopt the table only after this succeeds.
    MH_NetModuleHost host;
    host.size            = (unsigned)sizeof(MH_NetModuleHost);
    host.abi             = MH_NET_MODULE_ABI;
    host.run_dir         = MH_RunDir();
    const int module_abi = mod_init(&host);

    g_module = h;
    g_b      = t;
    g_bound  = true;

    // ONE LINE, FOUR FACTS, and the fact that it is ONE line is a deliberate constraint rather than
    // terseness. This bind lands at the very head of the arm window check_arm_order gates, so every
    // line it writes is a structural baseline entry in all three committed templates; F4A measured
    // and pre-ruled the cost as exactly one inserted step, and an outcome that spent two lines in
    // the bound arm and one in the absent arm would also have made the three arms structurally
    // different for no reason -- they arm identically, they should read identically.
    //
    //   THE CALL-THROUGH. GetProcAddress succeeding is not evidence that a CALL across the boundary
    //   works (F4A's spike proved the same thing with 2+3=5). MH_Net_IsStarted() is the right probe:
    //   pure, no side effect, and at DLL_PROCESS_ATTACH its answer is KNOWN -- nothing has started a
    //   transport yet, so 0. A wrong answer means the boundary is broken and this line says so.
    //
    //   THE LOADER ORDER, in the form that survives a log read months later: the SIGN and the
    //   microseconds, plus whether the module's DllMain ran on our thread. A dynamically loaded
    //   module always reads AFTER; a BEFORE means somebody acquired a static import, and that is R2
    //   announcing itself in a log instead of in a 0xC0000409 nobody can reproduce.
    LARGE_INTEGER freq;
    freq.QuadPart = 0;
    QueryPerformanceFrequency(&freq);
    const long long delta = p.attach_qpc - g_self_qpc;
    long long       us    = 0;
    if (freq.QuadPart != 0) us = (delta * 1000000) / freq.QuadPart;
    const int started = MH_Net_IsStarted();
    char      b[520];
    wsprintfA(b,
              "; [modules] %s: BOUND at DllMain (under the loader lock) -- %d exports resolved, "
              "init returned %08X, call-through %s; module DLL_PROCESS_ATTACH was %s mh.dll's by "
              "%d us (attach_calls=%u attach_tid=%u, mh.dll tid=%u)\n",
              MODULE_TAG, (int)MH_NET_MODULE_SYMBOL_COUNT, module_abi,
              started == 0 ? "ok" : "WRONG -- the boundary call is broken",
              delta >= 0 ? "AFTER" : "BEFORE", (int)(us < 0 ? -us : us), p.attach_calls,
              p.attach_tid, (unsigned)GetCurrentThreadId());
    mod_log(b);
}

} // namespace

extern "C" void MH_ModuleBind_Early(HMODULE self) {
    if (g_done) return;
    g_done = true;
    // OUR timestamp first, before the ini read, so the loader-order comparison is against the
    // earliest instant mh.dll can name about itself.
    LARGE_INTEGER t;
    if (QueryPerformanceCounter(&t)) g_self_qpc = (long long)t.QuadPart;
    // THE BUILD STAMP, and it is the FIRST line of mh_net.log on purpose (tracker TL-CI1). A bug
    // report quotes a log, and until this line a log could not say which build produced it. It is
    // written AFTER the QPC capture above so the loader-order delta is not measured across a file
    // write, and it is ONE line for the same reason the bind outcome below is one line.
    //
    // NOT AN ARM STEP. tools/check_arm_order.py FOLDS IT OUT rather than baselining it: its
    // normalizer masks addresses and digit runs to make a template version-independent, and a
    // banner whose text IS the release number cannot be masked to a constant (0.1.0 and 0.1.0-rc1
    // normalize differently, and a short sha of pure letters survives the hex mask as itself). A
    // baselined step that changes with every tag would red the gate on every release, which is the
    // opposite of what that gate is for. The stamp's own gate is tools/release_package.py, which
    // reads it back out of the built binaries.
    {
        char vb[192];
        wsprintfA(vb, "; [build] mh %s\n", MH_VERSION_FULL);
        mod_log(vb);
    }
    ws2_anchor(); // the subset rule -- see its comment; do not delete
    if (module_declined()) {
        // NOT ATTEMPTED is its own outcome, distinct from NOT BOUND, because the two are different
        // facts about the run: one says the file was not there, the other says this run was told not
        // to look. Collapsing them is the `[net] enable` conflation F3B had to undo one level up.
        mod_log("; [modules] mh_net: NOT ATTEMPTED -- `[net] module=none` in mh_net.ini, so mh.dll "
                "does not look for the module at all. Continuing WITHOUT a transport: this is the "
                "requested configuration, not a failure.\n");
        return;
    }
    // WHICH transport, decided before the load and refused on a typo. A non-default choice gets one
    // extra line naming the file, so a log can say which of the two implementations this run ran;
    // the default (udp since 2026-09-20) emits nothing, which is what keeps every existing arm-order
    // baseline valid.
    const char *file = transport_file();
    if (file != nullptr) {
        char b[MAX_PATH + 200];
        wsprintfA(b,
                  "; [modules] %s: `[net] transport=tcp` -- binding %s instead of the default %s\n",
                  MODULE_TAG, file, FILE_UDP);
        mod_log(b);
    }
    bind(self, file != nullptr ? file : FILE_UDP);
}

extern "C" int MH_NetModule_IsBound(void) { return g_bound ? 1 : 0; }

// ---- THE FORWARDING SHIMS ------------------------------------------------------------------------
//
// One definition per contract row, and between them they are the ENTIRE surface mh.dll has onto the
// transport. Every MH_Net_* / MH_Key_Load call in this DLL -- 75 of them, across 8 files -- links
// against these, unchanged, and none of the callers knows the transport moved. (The F4 surface pass
// counted 80; re-measured at F4B's HEAD it is 75, the difference being comment mentions and the two
// dead exports this item deleted. R9: re-derive every inherited number.)
//
// THIS IS WHY THE 18 UNGATED CALL SITES DID NOT NEED 18 GUARDS. The alternative shape considered and
// rejected was to wrap each ungated site in `if (transport_present())`. That makes absence a
// property every caller has to remember, it is unverifiable by anything but reading, and it grows
// with every new call site. Putting the answer in the surface makes the ungated-site count zero by
// construction: there is no way to reach the module except through a shim that already knows.
//
// The absent branch is NOT a stub of convenience -- each one is the value the REAL body returns when
// g_started is 0 (the table and the reasoning are in mh_net_module.h), so an absent module and an
// un-started transport are indistinguishable to every caller, which is exactly the property the
// callers were already written against.
#define MH_NET_BIND_SHIM(ret, name, params, args, absent) \
    extern "C" ret name params {                          \
        if (!g_bound) absent;                             \
        return g_b.name args;                             \
    }
MH_NET_MODULE_SYMBOLS(MH_NET_BIND_SHIM)
#undef MH_NET_BIND_SHIM
