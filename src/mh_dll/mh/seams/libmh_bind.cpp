//
// mh/seams/libmh_bind.cpp -- THE SPINE BIND, and the eight contract rows a generator cannot write.
//
// Read mh/include/mh_module_bind.h first (F4A's ruling: LoadLibrary on an absolute path beside
// mh.dll, GetProcAddress per symbol, from the first statement of DLL_PROCESS_ATTACH, with an inert
// satellite DllMain and mh.dll as the sole orchestrator), then tools/gen_libmh_contract.py's header
// (why the ~100 rows are DERIVED from the two images' objects rather than typed, and why the
// forwarding shims carry no types). docs/dll-split.md carries the measurements.
//
// This file is the sibling of seams/module_bind.cpp one module over, and it does three things:
//
//   1. BINDS libmh.dll from DLL_PROCESS_ATTACH and fills the generated table
//      (seams/libmh_contract.gen.cpp). One loud log line whatever happens.
//   2. DEFINES the eight contract rows that cannot be a naked forwarding thunk -- four because
//      their RETURN TYPE makes `xor eax,eax` a null dereference, four because zero is a legal
//      value of the right type and the WRONG answer. Every one of them is a written decision about
//      what configuration (1) means.
//   3. REPORTS the crossing counters once, on the first present. That report is F4D's standing
//      arm: the recorded miss this item exists to close is that LIB-SPINE-API proved the spine
//      crossing live ONCE and never made it a gate.
//
// ---- WHAT "ABSENT" MEANS HERE, AND WHY IT IS NOT A DEGRADATION ----------------------------------
//
// For mh_net.dll, absence means no transport: a real loss the player can see, dressed as a notice.
// For libmh.dll it means CONFIGURATION (1) -- the game runs the original binary's own bodies (F4
// ruling Q10, ratified by the user). Nothing is lost, because nothing was ever promoted.
//
// That is what makes the generated thunks' uniform zero the right answer rather than a convenience.
// Read down the contract and every row is one of: an installer (`install_promotion*` -> 0, "not
// installed", so the original entry stays), a predicate about an install (`*_active`, `*_promoted`,
// `*_requested` -> false), an observer/logger registration (-> no-op, a callback nothing will
// invoke), an entry thunk (-> nullptr, so the installer that would patch it refuses), or harness
// instrumentation (`rng_trace_*`, `state::capture` -> 0, which is Q4's uninstrumented config (1)
// stated in numbers). The three rows that reach live gameplay code -- mh::tact::mission_start,
// group_issue_order and unit_enqueue_command -- are all reached ONLY from the harness's own
// force-entry verbs (launch.cpp's --tactical, harness.cpp's journal replay); the shipping route
// into tactical mode is llm_strat_try_enter_tactical_mission, which in config (1) was never
// promoted and therefore runs the original.
//
// THE REFUSAL LINE SAYS WHICH CONFIGURATION THE RUN IS IN. It does not say something failed, and
// docs/dll-split.md's inheritance table asks that of this satellite and of no other.
//
// ---- WHY THERE IS NO `[modules] libmh` KEY ------------------------------------------------------
//
// F4A ruling (a): a real satellite is unconditional and ABSENCE IS THE CONFIGURATION. A per-module
// enable key would rebuild the "off or absent?" ambiguity F3B spent a whole item removing from
// `[net] enable`, and it would additionally be a lie here: with the spine in another file, "off"
// and "not there" are the same state of the process. mh_net.dll has `[net] module=none` only
// because F3F shipped that key before the module existed.
//
#include <windows.h>

#include "include/mh_module_bind.h"
#include "seams/libmh_contract.gen.h"
#include "../../libmh/include/libmh_module.h"
#include "mh_run_context.h" // MH_RunDir + mh_log_stamp (mh_common; self-initialising)

// The headers whose declarations the eight hand rows must match EXACTLY. Including them is the
// point: the compiler checks each signature against the real one, so a hand-written forwarder
// cannot drift the way a transcribed prototype can.
#include "addr/mh_regions.gen.h"  // mh::state::live_table, live()
#include "state/region_owner.h"   // mh::state::owner_slot, owner_table(), owner_count()
#include "state/hook_api.h"       // mh::hosthook::install_export_ok
#include "hook/export.h"          // mh::hook::install_export_ok -- the absent answer for that row
#include "lockstep/turn_engine.h" // mh::lockstep::reimpl_fixes, fixes()
#include "tact/tact_state.h"      // mh::tact::tact_state, state()

namespace {

const char *const MODULE_FILE = "libmh.dll";
const char *const MODULE_TAG  = "libmh"; // what the log lines call it

// THE HANDLE IS HELD FOR THE LIFE OF THE PROCESS AND DELIBERATELY NEVER READ AGAIN -- the same rule
// module_bind.cpp states for mh_net.dll. It is released only on the REFUSAL paths, where nothing
// has been bound yet. On the success path there is no FreeLibrary and there must not be: every slot
// of the generated table is a raw pointer into that image.
HMODULE g_module   = nullptr;
bool    g_done     = false;
bool    g_bound    = false;
bool    g_reported = false;

// mh.dll's OWN DLL_PROCESS_ATTACH timestamp. The R2 measurement is the SIGN of
// (module attach_qpc - this): negative means the module's DllMain ran FIRST, which is what a static
// import produces and what the inert-DllMain rule has to hold against.
long long g_self_qpc = 0;

void mod_log(const char *s) {
    // mh_net.log, composed locally from MH_RunDir() rather than taken from net_internal.h's g_log:
    // that is filled by build_paths(), which runs inside MH_Core_Arm_Early -- AFTER this. A bind
    // that logged through it would write to an empty path, and the one line that reports whether
    // the spine exists would say nothing. (Q9: mh_net.log keeps its name and is the CORE-ARM log.)
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

// Compose "<dir of the module `self`>\<name>". NEXT TO MH.DLL rather than next to the exe, and the
// distinction is deliberate even though the two directories are the same in every deployment we
// ship (R7): configuration belongs to the installation, a sibling MODULE belongs to the BUILD. A
// bare name would run the whole search order and could bind a stranger -- msvfw32's Rule 2, which
// exists because that shim's own bare-name load found ITSELF. With exactly one path tried, "the
// module is not here" is a fact rather than a search that quietly succeeded elsewhere.
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
        const DWORD err = GetLastError();
        char        b[MAX_PATH + 420];
        // THE CONFIGURATION LINE. Loud, named, and followed by a boot that continues -- and note
        // what it does NOT say: nothing failed. Every generated thunk now answers its absent value,
        // no promotion installs, and the game runs the original binary's own bodies.
        wsprintfA(b,
                  "; [modules] %s: NOT BOUND -- LoadLibrary(%s) failed, Win32 error %u (%s). This "
                  "run is CONFIGURATION (1): mh.dll does not contain the spine, so the game runs "
                  "the original binary's own bodies. Nothing is promoted and nothing is "
                  "instrumented; that is the configuration, not a failure of the boot.\n",
                  MODULE_TAG, path, err,
                  err == ERROR_MOD_NOT_FOUND ? "the file is not there"
                                             : "see the Win32 error code");
        mod_log(b);
        return;
    }

    // Resolve EVERY contract row before committing to any of them. A partially bound spine is the
    // worst of the outcomes -- some calls reach the module and some answer zero -- so the table is
    // filled into a local and only adopted whole.
    void *t[MH_LIBMH_CONTRACT_COUNT];
    int   got = 0;
    char  missing[420];
    missing[0] = '\0';
    for (int i = 0; i < MH_LIBMH_CONTRACT_COUNT; ++i) {
        t[i] = (void *)GetProcAddress(h, g_libmh_fn_name[i]);
        if (t[i] != nullptr) {
            ++got;
        } else if (lstrlenA(missing) < 340) {
            lstrcatA(missing, missing[0] ? " " : "");
            lstrcatA(missing, g_libmh_fn_name[i]);
        }
    }

    typedef int(__cdecl * PFN_ModInit)(const libmh_module_host *);
    typedef void(__cdecl * PFN_ModProbe)(libmh_module_probe *);
    PFN_ModInit  mod_init  = (PFN_ModInit)GetProcAddress(h, "libmh_module_init");
    PFN_ModProbe mod_probe = (PFN_ModProbe)GetProcAddress(h, "libmh_module_probe_read");

    if (got != MH_LIBMH_CONTRACT_COUNT || mod_init == nullptr || mod_probe == nullptr) {
        char b[MAX_PATH + 760];
        // A module that loaded but does not carry the contract is a WRONG module, not a missing
        // one, and it gets a different sentence. Naming the symbols turns "your libmh.dll is old"
        // from a guess into a diff -- and with a DERIVED export list, the names it prints are
        // exactly the rows tools/gen_libmh_contract.py would regenerate.
        wsprintfA(b,
                  "; [modules] %s: LOADED BUT REFUSED -- %s resolved %d of %d contract symbols "
                  "(init=%d probe=%d); missing: %s. Continuing in configuration (1).\n",
                  MODULE_TAG, path, got, (int)MH_LIBMH_CONTRACT_COUNT, mod_init != nullptr,
                  mod_probe != nullptr, missing[0] ? missing : "(none by name)");
        mod_log(b);
        FreeLibrary(h);
        return;
    }

    // THE R2 MEASUREMENT, asked BEFORE init so the numbers describe the LOAD and nothing else.
    libmh_module_probe p;
    p.size = 0;
    p.abi  = 0;
    mod_probe(&p);
    if (p.size != (unsigned)sizeof(libmh_module_probe) || p.abi != LIBMH_MODULE_ABI) {
        char b[300];
        wsprintfA(b,
                  "; [modules] %s: ABI MISMATCH -- module reports size=%u abi=%08X, this build "
                  "wants size=%u abi=%08X. Continuing in configuration (1).\n",
                  MODULE_TAG, p.size, p.abi, (unsigned)sizeof(libmh_module_probe), LIBMH_MODULE_ABI);
        mod_log(b);
        FreeLibrary(h);
        return;
    }

    libmh_module_host host;
    host.size            = (unsigned)sizeof(libmh_module_host);
    host.abi             = LIBMH_MODULE_ABI;
    host.run_dir         = MH_RunDir();
    const int module_abi = mod_init(&host);

    g_module = h;
    for (int i = 0; i < MH_LIBMH_CONTRACT_COUNT; ++i) g_libmh_fn[i] = t[i];
    g_bound = true;

    // ONE LINE, and its being one line is a constraint rather than terseness: this bind lands at the
    // very head of the arm window check_arm_order gates, so every line it writes is a structural
    // baseline entry in every committed template. F4A pre-ruled the cost of a DllMain arm as exactly
    // one inserted step; spending two here would also have made the bound and absent arms
    // structurally different for no reason -- they arm identically, so they should read identically.
    //
    //   THE CALL-THROUGH. GetProcAddress succeeding is not evidence that a CALL across the boundary
    //   works. mh::state::live() is the right probe: it is the process-wide region registry this
    //   split had to make single-instance (see the hand rows below), its answer at
    //   DLL_PROCESS_ATTACH is KNOWN -- nothing has rebased anything, so region 0 sits at its stock
    //   base -- and asking it is also the first thing that proves the boundary carries a REFERENCE
    //   return correctly, which every generated thunk deliberately cannot do.
    //
    //   THE LOADER ORDER, in the form that survives a log read months later: the SIGN and the
    //   microseconds. A dynamically loaded module always reads AFTER; a BEFORE means somebody
    //   acquired a static import, and that is R2 announcing itself in a log instead of in a
    //   0xC0000409 nobody can reproduce.
    LARGE_INTEGER freq;
    freq.QuadPart = 0;
    QueryPerformanceFrequency(&freq);
    const long long delta = p.attach_qpc - g_self_qpc;
    long long       us    = 0;
    if (freq.QuadPart != 0) us = (delta * 1000000) / freq.QuadPart;
    const bool probe_ok =
        mh::state::live().base[0] == mh::state::REGIONS[0].base && !mh::state::live().moved[0];
    char b[560];
    wsprintfA(b,
              "; [modules] %s: BOUND at DllMain (under the loader lock) -- %d exports resolved, "
              "init returned %08X, call-through %s; module DLL_PROCESS_ATTACH was %s mh.dll's by "
              "%d us (attach_calls=%u attach_tid=%u, mh.dll tid=%u)\n",
              MODULE_TAG, (int)MH_LIBMH_CONTRACT_COUNT, module_abi,
              probe_ok ? "ok" : "WRONG -- the boundary call is broken", delta >= 0 ? "AFTER" : "BEFORE",
              (int)(us < 0 ? -us : us), p.attach_calls, p.attach_tid, (unsigned)GetCurrentThreadId());
    mod_log(b);
}

// Every hand row bumps the same counter the generated thunks do, so the witness counts CALLS rather
// than call sites and no row is silently exempt from the arm.
inline void *slot(int i) {
    void *f = g_libmh_fn[i];
    if (f != nullptr)
        ++g_libmh_crossings;
    else
        ++g_libmh_absent_calls;
    return f;
}

} // namespace

extern "C" void MH_LibmhBind_Early(HMODULE self) {
    if (g_done) return;
    g_done = true;
    // OUR timestamp first, so the loader-order comparison is against the earliest instant mh.dll
    // can name about itself.
    LARGE_INTEGER t;
    if (QueryPerformanceCounter(&t)) g_self_qpc = (long long)t.QuadPart;
    bind(self);
}

extern "C" int MH_LibmhModule_IsBound(void) { return g_bound ? 1 : 0; }

// ---- THE STANDING ARM ----------------------------------------------------------------------------
//
// Emitted ONCE, on the first present -- which is net_lockstep.cpp's on_present, "the first place
// that is both off the loader lock and guaranteed to run". Two properties make that the right home
// and neither is convenience:
//
//   * It is AFTER the arm window check_arm_order gates (F4A measured mechanism B's lines landing at
//     1161-1164 against a window ending at 1152), so this line costs no baseline edit in any of the
//     committed templates -- and a witness that forced a baseline edit on every config would have
//     been a witness nobody could add.
//   * By the first present the WHOLE arm has run: every [promote] installer, the harness, the seam
//     wiring. So the number it reports is the arm's crossings, not a prefix of them.
//
// WHAT IT IS FOR. A brokered lane that binds and then never calls is indistinguishable, in every
// gate this project had before F4D, from one that crossed 400 times: the bind line is identical and
// the game runs. tools/check_module_bind.py --libmh reads this line and requires crossings > 0 with
// absent-calls == 0 on a lane that deploys the module, and the inverse on one that does not. That
// is the recorded miss from LIB-SPINE-API turned into an arm rather than a one-time proof.
extern "C" void MH_Libmh_OnPresent(void) {
    if (g_reported) return;
    g_reported = true;
    char b[320];
    wsprintfA(b,
              "; [libmh] crossings=%u absent-calls=%u -- the spine boundary was %s in this run "
              "(configuration %s)\n",
              g_libmh_crossings, g_libmh_absent_calls,
              g_libmh_crossings != 0 ? "ENTERED" : "NOT ENTERED", g_bound ? "(2)" : "(1)");
    mod_log(b);
}

// ---- THE EIGHT HAND-DEFINED CONTRACT ROWS --------------------------------------------------------
//
// tools/gen_libmh_contract.py emits a naked forwarding thunk for the other 92 and REFUSES to emit
// one for these. Four are mechanical (a reference or by-value-struct return, where the generated
// `xor eax,eax` would hand the caller a null to dereference); four are semantic (zero is a legal
// value of the right type and the wrong answer). The generator carries each reason next to the row.
//
// Every one of them includes the module's real header above, so the SIGNATURE is checked by the
// compiler rather than transcribed -- which is the property the whole generated-thunk mechanism
// gives up in exchange for not needing signatures at all, bought back here where it matters.

namespace mh::state {

// THE STATE REGION REGISTRY -- ONE TABLE PER PROCESS, and this row is why the split is not a pure
// packaging change. Until F4D `live()` was an unconditional magic static in addr/mh_regions.gen.h,
// which is exactly right while the spine and the injection layer are one image: the linker folds
// the COMDAT and every reader shares the object. Two images make that silently false.
// mh::ai::island_move() rebases 48 regions from inside libmh; mh.dll's harness.cpp, desync_watch.cpp,
// net_lockstep.cpp, launch.cpp, ui_drive.cpp, gfx_overlay.cpp, net_diag.cpp and net_seams.cpp read
// live_base() for dozens. With two tables the rebases land in one and the reads come from the other,
// the determinism hash walks the abandoned addresses, and NOTHING GOES RED -- not the arm log, not
// the UI suite, not even a two-peer determinism run, because both peers would be wrong identically.
//
// So the image that CONTAINS the spine owns the table (MH_SPINE_IN_IMAGE, set by every project that
// compiles the roster) and mh.dll reaches it across the boundary like every other row.
//
// ABSENT: mh.dll's own table, and it is not a fallback but the correct answer. In configuration (1)
// there is no spine, nothing can claim or rebase a region, and this static -- seeded from REGIONS[]
// by its own constructor -- is the whole truth about where the state is.
live_table &live() {
    if (void *f = slot(MH_LIBMH_SLOT_mh_state_live))
        return *((live_table * (__cdecl *)(void)) f)();
    static live_table t;
    return t;
}

// Region OWNERSHIP, the same argument one layer over: claim() is called from roster TUs
// (ai_state.cpp claims 48 regions for the island move, order_queue.cpp 2) and owner_of() /
// owner_serves() are read from desync_watch.cpp and harness.cpp. Two tables and the hash silently
// takes the raw-bytes path for every claimed region.
//
// ABSENT: mh.dll's own empty table, which is again the correct answer rather than a degradation --
// with no spine, nothing has claimed anything, so an empty table is what is true.
owner_slot *owner_table() {
    if (void *f = slot(MH_LIBMH_SLOT_mh_state_owner_table))
        return ((owner_slot * (__cdecl *)(void)) f)();
    static owner_slot t[MAX_OWNED]{};
    return t;
}

int &owner_count() {
    if (void *f = slot(MH_LIBMH_SLOT_mh_state_owner_count))
        return *((int *(__cdecl *)(void))f)();
    static int n = 0;
    return n;
}

} // namespace mh::state

namespace mh::hosthook {

// EVERY MH_EXPORT_REPLACE SITE ROUTES THROUGH THIS ACCESSOR, and after F4D-PRE that includes
// mh.dll's own two (harness.cpp:2325's wall-clock pin and reimpl_probe.cpp:96's utils_w_strlen).
// The accessor lives in libmh and forwards to mh.dll's hook table, so bound it is a round trip:
// mh.dll -> libmh -> libmh_hook_api -> mh::hook::install_export_ok.
//
// ABSENT: call mh::hook::install_export_ok DIRECTLY. This is the one row where the generated zero
// would have been actively wrong rather than merely inert -- returning false would refuse mh.dll's
// OWN installs, which have nothing to do with the spine and which configuration (1) has no reason
// to lose. The answer is identical to the bound path's, minus the round trip.
bool install_export_ok(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8) {
    if (void *f = slot(MH_LIBMH_SLOT_mh_hosthook_install_export_ok))
        return ((bool(__cdecl *)(uintptr_t, void *, const char *, uint64_t))f)(target, thunk, name,
                                                                               expect_entry8);
    return mh::hook::install_export_ok(target, thunk, name, expect_entry8);
}

} // namespace mh::hosthook

namespace mh::lockstep {

// MECHANICAL: returns `const reimpl_fixes &`. The knob values read once from the ini at init.
// ABSENT: a default-constructed set, i.e. every fix OFF -- which is what configuration (1) means
// for this row, since the fixes are reimplementation behaviour and there is no reimplementation.
// A static rather than a temporary because the caller gets a reference.
const reimpl_fixes &fixes() {
    if (void *f = slot(MH_LIBMH_SLOT_mh_lockstep_fixes))
        return *((const reimpl_fixes *(__cdecl *)(void))f)();
    static const reimpl_fixes k{};
    return k;
}

} // namespace mh::lockstep

// THE TWO rng_trace_* REFERENCE ROWS WERE HERE AND F4E DELETED THEM WITH THEIR LAST CALLER.
// `mh::sim::rng_trace_at` and `rng_trace_note_at` return a const reference into the trace ring, so
// they were mechanically hand-defined for the same reason the four above are. Their ONLY mh.dll
// caller was seams/harness.cpp, which is mh_harness.dll now -- so they stopped being contract rows
// (the derivation measured 101 -> 81 across the split) and these definitions would have named slot
// enumerators the generator no longer emits. They are gone rather than kept "in case": a stale
// adjudication reads exactly like a live decision, which is F4D-PRE's own lesson. The instrument
// reaches both rows out of libmh.dll directly now, through its own spine table, where the absent
// path is a loud trap rather than a zeroed static -- see tools/gen_harness_contract.py for why an
// instrument may not answer plausibly.

namespace mh::tact {

// MECHANICAL: returns tact_state BY VALUE. A by-value struct comes back through a hidden pointer
// the caller supplies, so a naked stub that only sets EAX leaves the caller's storage untouched --
// uninitialised, not zero. ABSENT: a value-initialised tact_state, i.e. every view and store
// pointer null, which is what "no tactical mode is running" already looks like to its one mh.dll
// reader (host_event_sink.cpp).
tact_state state() {
    if (void *f = slot(MH_LIBMH_SLOT_mh_tact_state)) return ((tact_state(__cdecl *)(void))f)();
    // The zeroed storage is spelled this way rather than as `tact_state{}` because tact_store has
    // a user-declared constructor (the 60-pointer registry binder) and therefore no default one --
    // by design, since a half-bound store is the bug that class exists to prevent. Constructing one
    // here would mean reproducing that binder against a registry that, in configuration (1), has
    // nothing tactical bound in it anyway. So the absent answer is the all-null state, copied out
    // of zeroed storage: every pointer null, which is what "no tactical session" already looks
    // like to this row's one mh.dll reader (host_event_sink.cpp).
    alignas(tact_state) static const unsigned char zeroed[sizeof(tact_state)] = {};
    return *reinterpret_cast<const tact_state *>(zeroed);
}

} // namespace mh::tact
