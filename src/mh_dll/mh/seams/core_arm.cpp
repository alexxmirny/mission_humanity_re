//
// mh/seams/core_arm.cpp -- THE CORE ARM'S PHASE 1, and mh.dll's own half of what used to be
// harness.cpp's preamble (fork F4E -- the FILE cut F3B named and could not make).
//
// F3B gave this code its true name and its own call site in DllMain ("this is not harness work and
// never was") but moved no bytes: MH_Core_Arm_Early still lived in seams/harness.cpp, above
// `harness_enabled()`. F4E takes harness.cpp out of mh.dll entirely, so the line F3B drew is now a
// FILE boundary and this is the mh.dll side of it. Nothing about the boot sequence moved: DllMain
// still calls MH_Core_Arm_Early() immediately before MH_Harness_Init() and the two are still in the
// order G104 requires (see include/mh_core_arm_export.h for the measurement that fixed it).
//
// WHAT IS HERE, AND WHY EACH PIECE IS MH.DLL'S RATHER THAN THE INSTRUMENT'S:
//
//   build_paths()        composes the ini path and every per-run output path from the process image
//                        and MH_RunDir(). ONE composition of the run folder in the process (ruling
//                        Q1: run_context.cpp stays mh.dll-side), handed to the harness through
//                        MH_Core_ArmPaths() so there is never a second answer to "where does this
//                        run write".
//   harness_enabled()    the `[harness] enable=1` gate. mh.dll needs it for the two decisions below;
//                        the harness asks the same question of the same file (the note beside its
//                        own copy says why that is not a duplication to clean up).
//   the relocating bind  SB-HOSTFREE. `[harness] relocate_state` moves every relocatable region into
//                        a DLL-owned arena BEFORE anything resolves one, which is why it has to
//                        happen inside MH_Core_Arm_Early and therefore inside mh.dll. The arena, the
//                        report and MH_Harness_ReportRelocation come with it -- and that last one is
//                        why the mh.dll -> harness contract is TWELVE rows and not thirteen: it
//                        stopped crossing the boundary by moving to the side that owns its data.
//   MH_Core_Arm_Early()  the arm itself, unchanged.
//
// Two things this file deliberately does NOT own: the buffered log writer (that is the harness's and
// it holds a file handle for the whole run -- see append_line_once() for why mh.dll writes its
// handful of lines unbuffered instead), and `[harness]`'s other 120-odd keys, which are the
// instrument's configuration and are read in the instrument's image.
//
#include <windows.h>

#include "include/mh_core_arm_export.h"
#include "include/mh_core_arm_paths.h"  // F4E: the paths the harness copies at MH_Harness_Init
#include "include/mh_harness_export.h"  // MH_Harness_ReportRelocation lives here now
#include "include/mh_hostapi_bind.h"    // LIB-ABI: the thunk-backed host-callback table
#include "include/mh_libmh_hook_bind.h" // F4D-PRE: the hook-service table -- libmh's outbound edge
#include "include/mh_run_context.h"     // MH_RunDir (per-run log folder)
#include "state/host_api.h"             // LIB-ABI: libmh_set_host_api
#include "state/host_bind.h"            // SB-BIND: the state ABI (bind_stock / bind_relocated)
#include "state/host_in.h"              // LIB-REF-IN: libmh_in_open
#include "hook/host_event_sink.h"       // LIFT-EVQ: bind_host_event_sink (G104 placement)
#include "addr/mh_rebind.gen.h"         // LIB-REBIND R11: load_gates at the named init point
#include "addr/mh_regions.gen.h"        // ST2M: the state region registry
#include "config/config.h"              // F2A: the D11 selector gate policy derives from
#include "save/save_live.h"             // the four save-walker promotion predicates
#include "seams/net_internal.h"         // SHIP_REBIND_DEFAULT -- the R11 gate policy load_gates takes

namespace {

// ---- the paths ------------------------------------------------------------------------------------
//
// Composed here, ONCE, and read by two images. mh.dll uses g_ini_path (the `[harness]` reads below)
// and g_log_path (the arm-time lines); mh_harness.dll copies the whole struct at MH_Harness_Init and
// keeps its own char arrays, so its ~350 uses of these names are unchanged by the split.
char g_dir[MAX_PATH]; // exe directory, trailing backslash
char g_log_path[MAX_PATH];
char g_ini_path[MAX_PATH];
// Paired paths: *_path = INPUT, next to the exe (where a human/rig drops a banked recording);
// *_out = OUTPUT, in the per-run log folder. See build_paths.
char g_seed_path[MAX_PATH], g_seed_out[MAX_PATH];
char g_boot_snap_out[MAX_PATH];
char g_world_out[MAX_PATH]; // LIB-WORLD: the step-0 world blob, in the run folder
char g_orders_path[MAX_PATH], g_orders_out[MAX_PATH];
char g_clock_path[MAX_PATH], g_clock_out[MAX_PATH];

MH_CoreArmPaths g_paths;

// LIB-REF-IN: the inbound refusal baseline, taken at the open site below and read by the harness at
// its first hashed step (through MH_Core_TrapsAtOpen -- the count is mh.dll's, taken at a moment when
// only mh.dll is executing).
int g_in_traps_at_open = 0;

// ---- the unbuffered line writer --------------------------------------------------------------------
//
// mh.dll writes at most a handful of lines into mh_harness.log (the two rebind yields and the [reloc]
// report); the harness writes thousands. The harness's writer HOLDS ITS HANDLE OPEN for the run, so a
// second holder in this image would make one of the two CreateFileA calls fail and the loser would be
// whichever ran second. Open-write-close is what that costs us: three syscalls on a path taken a
// handful of times per boot, against a class of failure that shows up as a silently missing log.
void append_line_once(const char *path, const char *s) {
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, s, (DWORD)lstrlenA(s), &wrote, nullptr);
    CloseHandle(h);
}
#define append_line append_line_once

void build_paths() {
    GetModuleFileNameA(nullptr, g_dir, MAX_PATH); // ...\mh.exe
    char *slash = g_dir;
    for (char *p = g_dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';                              // keep trailing backslash
    wsprintfA(g_ini_path, "%smh_net.ini", g_dir); // config INPUT (next to exe)
    // OUTPUTS all land in the per-run log folder (2026-07-25). They used to be written next to the
    // exe, which littered the game directory and -- worse -- let one run silently overwrite the
    // previous run's recording. INPUTS still resolve next to the exe: the run folder is timestamped,
    // so a prior run's folder is not addressable at load time. Both paths are logged at arm, so
    // replaying a recording is "copy mh_orders.bin from the run folder next to the exe", not a guess.
    wsprintfA(g_log_path, "%smh_harness.log", MH_RunDir());
    wsprintfA(g_seed_out, "%smh_harness_seed.bin", MH_RunDir());
    wsprintfA(g_boot_snap_out, "%smh_boot_snapshot.bin", MH_RunDir());
    wsprintfA(g_world_out, "%smh_world.bin", MH_RunDir());
    wsprintfA(g_orders_out, "%smh_orders.bin", MH_RunDir());
    wsprintfA(g_clock_out, "%smh_clock.bin", MH_RunDir());
    wsprintfA(g_seed_path, "%smh_harness_seed.bin", g_dir); // seed INPUT (inject)
    wsprintfA(g_orders_path, "%smh_orders.bin", g_dir);     // order INPUT (replay)
    wsprintfA(g_clock_path, "%smh_clock.bin", g_dir);       // clock-track INPUT (replay)
}

// ---- THE ARMING SIGNAL (fork F2G, ruling Q6) -----------------------------------------------------
//
// `[harness] enable=1` in mh_net.ini, and nothing else. Default OFF when the key or the whole
// section is absent, so a plain install and every ordinary UI run are unharnessed exactly as before.
//
// IT USED TO BE THE EXISTENCE OF A FILE. `mh_harness.ini` beside the exe armed the harness by being
// there; there was no `enable=` key at all. That is a configuration written in the filesystem rather
// than in the config file, and it failed in the way such things do: a stale copy on a rig machine
// decided the real stop_step of a determinism gate that believed it was running 800 steps and was
// actually comparing 200 (2026-07-25). Presence-of-a-file and presence-of-a-
// SECTION are the same fragility, which is why the merge did not simply become "arm if [harness]
// exists": a fragment that sets one tactical knob would then also arm the whole instrument.
//
// The old file is not merely ignored now -- mh::config::refuse_stray_harness_ini kills any run that
// still has one beside the exe, so the migration cannot fail quietly.
//
// READ FRESH FROM THE INI, NOT FROM g_cfg, and that is not redundancy: this is asked twice before
// load_config() has run (the rebind yield below, and the arm gate itself), so g_cfg.* is still zero
// at both call points. It is one GetPrivateProfileInt against a file the OS caches; the cost is not
bool harness_enabled() { return GetPrivateProfileIntA("harness", "enable", 0, g_ini_path) != 0; }

// ---- the RELOCATING state bind (SB-HOSTFREE) -------------------------------------------------
//
// Read OUT OF BAND, before load_config(), because it has to happen before anything resolves a
// region -- which is the whole of MH_Harness_Init's G104 ordering argument, and load_config runs
// later. Two keys:
//
//   [harness] relocate_state=1        move every relocatable region into a DLL-owned arena
//   [harness] relocate_pin=<name>     ...except this one, which stays at its stock base
//
// The pin is the mutation the acceptance demands: with it, the run MUST diverge and the report must
// name that region. A relocation arm that could not go red would be the same vacuous gate this item
// was opened to close.
//
// The arena is static, on ai::island_move's precedent: ~390 KB of DLL .bss, deterministic, with no
// allocation failure to handle from inside an init hook that has no logger yet.
alignas(16) uint8_t g_reloc_arena[mh::state::RELOCATABLE_BYTES + 16u * mh::state::RID_COUNT];
char                         g_reloc_pin[64];
char                         g_reloc_corrupt[64];
mh::state::relocation_report g_reloc_report;
bool                         g_reloc_active = false;
// Set when bind_relocated_from_ini() refuses BEFORE calling the binder, so report_relocation()
// can name the reason -- g_reloc_report.refused only exists once the binder ran.
const char *g_reloc_refused_early = nullptr;
bool        g_reloc_poison        = true;

// Which ORIGINAL save-block walkers this run has promoted -- the mask bind_relocated() needs to
// decide whether a region a walker names may move (dead-ends G139).
//
// THE D6 DERIVATION (fork F2B, ruling Q1): `brokered AND save-closure-owned => ours, else 0`. It is
// now ONE call -- mh::config::save_walkers_ours() -- because F2E deleted the four per-walker
// `SHIP_PROMOTE_*` constants and the `[promote]` keys that overrode them, and what is left of that
// pair is the single compile-time `kSaveClosureOwned`, false today. So this mask is 0 in EVERY mode,
// which is the behaviour-preserving answer the ruling requires; when the save closure earns its
// evidence, one `constexpr bool` flips and this follows with no change here.
//
// IT IS A DERIVATION, NOT A CONSTANT, AND THAT MATTERS TO THE READER. The compiler folds it to 0
// today. What the fold does not remove is the STATEMENT that the relocation mask is a function of
// who owns the save walkers -- which is the thing a future author must not get wrong, since a mask
// that says "ours" while the ORIGINAL walker still runs relocates 29 regions out from under it
// (dead-ends G139). The same call answers the INSTALL decision in reimpl_probe, so the two cannot
// disagree; before F2E they were two independent ini reads that happened to agree.
//
// THE NEGATIVE CASE IS A UNIT TEST NOW, not an ini fragment. `[promote] save=1` (and
// tools/uiscripts/ini/promote_save_walkers.ini, deleted with it) used to be how an armed mask was  // CITATION-OK
// demonstrated; with no runtime override the arming case is proven by net_selftest's config fixture
// suite calling the derivation with the ownership half forced true and asserting 0x0f. An
// always-0 gate that nothing can ever show non-zero is exactly what F2B refused to ship.
//
// NOT save_live's INSTALL STATE, and the ordering is why: the promotions install in MH_Seam_Init,
// which runs AFTER the bind that is the second statement of MH_Harness_Init. Asking
// `load_promotion_active()` here would answer "no" in every configuration, including the ones that
// go on to promote -- a gate that is always closed is as useless as one that is always open, and
// quieter about it. The SELECTOR needs no such care: mh::config composes its own path from the
// module file name, so it answers correctly at this point in init by construction (G104).
//
// AND `[save] verify` NO LONGER CLEARS IT, because it cannot arise: a verifying run installs through
// a trampoline so the original stays callable, which would make every baked address live again --
// but verify is only read INSIDE an install that `save_walkers_ours()` already gated, so a run that
// reaches verify=1 is a run this mask already answered 0 for.
uint8_t armed_save_walkers() {
    return mh::state::save_walker_mask(mh::config::save_walkers_ours());
}

// Returns the number of regions relocated; 0 means "not asked for, or refused" and the caller falls
// back to bind_stock(). The report is kept for the log line, which cannot be written yet -- there is
// no logger at this point in init, which is exactly why [hostapi] reports late too.
int bind_relocated_from_ini() {
    // F2G: THE ARM GATE COMES FIRST, and it is a behaviour-preserving addition rather than a new
    // rule. Before the merge these keys lived in mh_harness.ini, so reading one at all already
    // implied an armed harness -- the file's existence was the arm. In the merged file `[harness]
    // relocate_state=1` can now sit in an ini that never says `enable=1`, and relocating the world
    // for a harness that will not run is a mutation with no instrument watching it.
    if (!harness_enabled()) return 0;
    if (!GetPrivateProfileIntA("harness", "relocate_state", 0, g_ini_path)) return 0;
    // F2B follow-up (conductor ruling, 2026-09-12). Under `[config] mode=original` nothing of ours
    // is armed, and the census's `relocatable` flag means "a translated body EXISTS", never "that
    // body is armed" -- so a relocation moves-and-poisons regions that only ORIGINAL bodies will
    // read, at their stock addresses. Measured: 372 regions relocate and the process dies before
    // report_relocation() can say anything. A diagnostic that poisons its own run is REFUSED here,
    // loudly, instead of attempted.
    if (mh::config::mode() == mh::config::mode_t::original) {
        g_reloc_refused_early = "[config] mode=original: no ours body is armed, so relocation "
                                "poisons every region the original bodies read";
        return 0;
    }
    mh::state::relocation_opts o;
    // `relocate_poison=0` is a DIAGNOSTIC, and it must be read as one. With poison off, a consumer
    // that never followed the bind reads a stale but CORRECT copy of the bytes and agrees with
    // itself -- so a green run in that configuration says the copy and the bind work, and says
    // NOTHING about whether anything still reads the abandoned address. It is how you separate
    // "the relocation is broken" from "something still points at .bss"; it is not the acceptance.
    o.poison       = GetPrivateProfileIntA("harness", "relocate_poison", 1, g_ini_path) != 0;
    g_reloc_poison = o.poison;
    GetPrivateProfileStringA("harness", "relocate_pin", "", g_reloc_pin, sizeof(g_reloc_pin),
                             g_ini_path);
    if (g_reloc_pin[0]) o.pin_stock = g_reloc_pin;
    // `relocate_corrupt=<region>` is THE mutation -- it fills that region's ARENA copy with 0xCD,
    // so a run with it set must DIVERGE and the report must name the region. `relocate_pin` is not
    // a mutation and never was: measured, a pinned run matched the golden over 3000 steps, because
    // answering at the stock base is exactly what an unrelocated run does. See host_bind.h.
    GetPrivateProfileStringA("harness", "relocate_corrupt", "", g_reloc_corrupt,
                             sizeof(g_reloc_corrupt), g_ini_path);
    if (g_reloc_corrupt[0]) o.corrupt_arena = g_reloc_corrupt;
    o.armed_walkers = armed_save_walkers();
    const int n     = mh::state::bind_relocated(g_reloc_arena, sizeof(g_reloc_arena), o,
                                                &g_reloc_report);
    g_reloc_active  = n > 0;
    return n;
}

// Written once the logger exists, beside [hostapi]'s. Silent unless the run asked for a relocation:
// a line saying "0 relocated" on every ordinary run would be noise, and worse, would train a reader
// to skim past the line that matters.
void report_relocation() {
    if (!harness_enabled()) return; // F2G: same gate as the bind above, or this reports on nothing
    if (!GetPrivateProfileIntA("harness", "relocate_state", 0, g_ini_path)) return;
    char line[512];
    if (!g_reloc_active) {
        wsprintfA(line, "; [reloc] REFUSED (%s) -- the run is on the STOCK bind, so nothing it "
                        "reports about relocation means anything\n",
                  g_reloc_refused_early    ? g_reloc_refused_early
                  : g_reloc_report.refused ? g_reloc_report.refused
                                           : "no regions eligible");
        append_line(g_log_path, line);
        return;
    }
    wsprintfA(line,
              "; [reloc]%s %d region(s) relocated, %u bytes; %d blocked by a live original accessor, "
              "%d held by an unpromoted save-block walker (armed mask 0x%02x), "
              "%d zero-size, %d overrunning, %d under an overrunning window, %d already moved; "
              "hash slices now reading relocated bytes: %d strat + %d tact%s%s\n",
              g_reloc_poison ? "" : " NO-POISON (diagnostic; a stale reader agrees with itself)",
              g_reloc_report.relocated, g_reloc_report.bytes, g_reloc_report.blocked,
              g_reloc_report.walker_held, (unsigned)armed_save_walkers(),
              g_reloc_report.zero_size, g_reloc_report.overrunning, g_reloc_report.under_overrun,
              g_reloc_report.already_moved, g_reloc_report.hash_slices, g_reloc_report.tact_slices,
              g_reloc_report.corrupted_name ? "; ARENA CORRUPTED (MUTATION, expect divergence): "
              : g_reloc_report.pinned_name  ? "; pinned to stock (no-op arm): "
                                            : "",
              g_reloc_report.corrupted_name ? g_reloc_report.corrupted_name
              : g_reloc_report.pinned_name  ? g_reloc_report.pinned_name
                                            : "");
    append_line(g_log_path, line);
    // THE INI SAID SO; DID IT ACTUALLY HAPPEN? bind_relocated() trusts the ini's `[promote]` keys,
    // because it runs before the installs and has nothing else to ask. That is a DECLARATION, and
    // the only failure mode of the whole walker gate that points the unsafe way is a declaration
    // the install did not honour -- a seam whose install_export failed, a trampoline that refused --
    // because then 29 regions moved out from under an ORIGINAL body that is still executing, and
    // the damage is confined to a save or a load, i.e. to bytes the determinism hash never reads.
    //
    // HERE is where it becomes checkable: report_relocation() is called after MH_Seam_Init, so
    // save_live's install flags are final. This is the one place the two can be compared, and a
    // mismatch is stated as a corrupted-run warning rather than a note -- there is no configuration
    // in which it is benign.
    {
        using namespace mh::state;
        uint8_t actual = 0;
        if (mh::save::promotion_active()) actual |= WALK_PLANET_SAVE;
        if (mh::save::load_promotion_active()) actual |= WALK_PLANET_LOAD;
        if (mh::save::container_promotion_active()) actual |= WALK_CONTAINER_SAVE;
        if (mh::save::container_load_promotion_active()) actual |= WALK_CONTAINER_LOAD;
        const uint8_t declared = armed_save_walkers();
        if (declared & ~actual) {
            wsprintfA(line,
                      "; [reloc] WALKER MASK MISMATCH -- the bind trusted 0x%02x, the seams actually "
                      "installed 0x%02x. Regions this run relocated are still walked by an ORIGINAL "
                      "save-block body at its baked addresses; any save or load in this run is "
                      "CORRUPT and its verdict means nothing.\n",
                      (unsigned)declared, (unsigned)actual);
            append_line(g_log_path, line);
        }
    }
    // THE NON-VACUITY LINE, separate and loud. A relocation the determinism hash cannot see is
    // green over bytes nobody watches -- the exact failure H0 just repaired in this same oracle --
    // so the run says outright when it has moved nothing observable rather than leaving a reader to
    // divide two numbers.
    if (g_reloc_report.hash_slices == 0)
        append_line(g_log_path, "; [reloc] VACUOUS -- not one hashed region moved; a green run "
                                "here proves nothing about relocation\n");
}

} // namespace

// SB-HOSTFREE: the relocating bind's evidence, written where the logger exists. Called from
// net_seams' arm-time report block, beside [statebind] -- same reason [hostapi] reports there.
extern "C" void MH_Harness_ReportRelocation(void) {
    report_relocation();
}

// ==== MH_Core_Arm_Early -- the core arm's EARLY phase (fork F3B / plan D2) =======================
//
// THIS IS NOT HARNESS WORK and never was. It is mh.dll core: the ini/log paths, the state bind, the
// two host-api tables, the inbound surface, the session seed, the event sink and the rebind arm --
// every one of them unconditional, every one of them required in a run with no harness at all. It
// opened MH_Harness_Init only because MH_Harness_Init is the first thing DllMain calls that runs
// ahead of the no-ini early return, i.e. because of WHEN it had to happen, not WHOSE it is. F3B
// gives it its own name and its own call site in DllMain, immediately before MH_Harness_Init, so
// the boot order is byte-for-byte what it was and `harness_enabled()` a few lines below is now the
// clean file cut F4 needs: everything above this comment is core, everything below the gate is the
// instrument.
//
// IT RUNS BEFORE THE HARNESS, NOT INSIDE THE CORE ARM'S OWN FUNCTION, and that is the whole G104
// argument restated: every calls-struct binder reads mh::host() on first use and fail-fast aborts
// unbound, the harness proper's own installs are arm-path code that can reach one, and the first
// placement of these binds (the end of MH_Seam_Init) was MEASURED too late -- every boot died at
// 0xC0000409 with the abort line only on stderr (2026-09-02). Moving them into MH_Core_Arm, which
// runs from MH_Seam_Init, would put them back on the wrong side of MH_Harness_Init. So the core arm
// has two phases with the instrument between them, and DllMain states that order in three lines.
extern "C" void MH_Core_Arm_Early(void) {
    // SB-BIND T1: the STATE bind comes first, ahead of even the host-callback table, because it is
    // the more fundamental answer -- "where does the state live" precedes "who do I call". mh.dll
    // is the host here and its answer is the stock .bss, so this is a no-op BY CONSTRUCTION
    // (mh::state::bind derives `moved` from whether the base actually differs, so every flag stays
    // false and translate() never leaves its identity path). It runs anyway, in the shipping
    // configuration, so the entry point a standalone host will use cannot rot unexercised.
    // The same G104 earliest-common-point reasoning as the two binds below applies with more force:
    // a module binder that resolved a region before the host answered would be reading an
    // unanswered registry. Report line lives beside [hostapi]'s, where the logger exists.
    //
    // SB-HOSTFREE: build_paths() MOVED UP to here, ahead of the state bind, so the ini can be
    // consulted before we answer. It only reads GetModuleFileNameA and MH_RunDir (which
    // self-initializes), so it depends on nothing the binds below establish -- and the alternative,
    // reading the relocation keys after the bind, would relocate AFTER the module binders that
    // resolve on first use, which is the exact G104 ordering hazard this comment block is about.
    build_paths();
    if (bind_relocated_from_ini() == 0) mh::state::bind_stock();
    // LIB-ABI: bind the host-callback table BEFORE anything else in the arm sequence. Module
    // code runs in EVERY config (ship defaults promote translated bodies with no ini), and its
    // calls-struct binders read mh::host() on first use -- which fail-fast aborts unbound. The
    // first placement of this bind (end of MH_Seam_Init) was measurably too late: arm-path code
    // reached a module binder first and every boot died at 0xC0000409 with the abort line only
    // on stderr (caught by the UI suite cycling launch-retries, 2026-09-02). MH_Harness_Init is
    // the earliest common point: DllMain calls it before MH_Seam_Init, ahead of the no-ini
    // early-return so the ship config is covered too. The [hostapi] report line stays at the
    // end of MH_Seam_Init, where the logger exists.
    libmh_set_host_api(&mh::hostapi::mhdll_table(), LIBMH_HOST_API_VERSION);
    libmh_set_tact_host_api(&mh::hostapi::mhdll_tact_table(), LIBMH_TACT_HOST_API_VERSION);
    // F4D-PRE: the HOOK-SERVICE table, beside the two above and for the same G104 reason one level
    // over. These five rows are what the 627-TU spine calls OUT through -- entry ownership, the two
    // entry installers, and the two determinism-harness questions -- and every [promote] installer
    // in MH_Seam_Init reaches one. A table bound later would leave a promotion silently un-installed
    // with nothing in the log saying why, which is quieter than the abort the host-api bind gets and
    // therefore worse. NOT reported here: the arm log's structure is a gated artifact
    // (check_arm_order) and this item does not edit baselines -- the binding is asserted by
    // `net_selftest.exe hostapitest` and by tools/check_libmh_outbound.py instead.
    MH_LibMH_BindHookApi();
    // LIB-REF-IN: open the INBOUND surface, here, for the SAME G104 reason as the two binds above
    // and not one line later. sim_hostreach's 21 promotion adapters now forward through the C
    // entries in libmh_host_in.h, and every one of those entries refuses until this has returned 0
    // -- so an open that happened at the end of MH_Seam_Init would leave a window in which a
    // front-end call into a promoted leaf was a silent no-op. It needs the state bind (which is
    // three lines up) and nothing else. The rc is REPORTED at arm time beside [hostapi]'s, where
    // the logger exists (via libmh_in_is_open / libmh_in_unbound, which are readable later --
    // no need to carry the rc in a global); a closed surface is a FAILED arm there.
    libmh_in_open(LIBMH_HOST_IN_VERSION);
    // The same refusal baseline libref_host takes, for the same reason: mh::libmh_in::trap() counts
    // closed-default answers and until LIB-REF nobody ever read the counter -- which is exactly how
    // a host running on invented answers stayed invisible for 289 steps. PRINTED, NOT GATED here,
    // and that asymmetry is deliberate: this arm legitimately runs pre-open boot frames, so a raw
    // count is not a defect, while a refusal AFTER open is. Baselining makes the printed number mean
    // the second thing. Left as a report rather than a failure because no hosted refusal has been
    // observed yet and a gate whose red has never been seen is not a gate.
    g_in_traps_at_open = mh::libmh_in::trap_count();
    // LIFT-TABLE S5: the strategic RNG's wall-clock seed is a PUSHED init parameter now, not a
    // host-table pull. Pushed through the ORIGINAL entry on purpose -- that is what keeps the
    // harness `pin_strat_seed` trampoline in the path, since it replaces the function, not the
    // call site. A standalone host hands both multiplayer peers the same number instead.
    // LIFT-TABLE S5: the strategic RNG's wall-clock seed is a PUSHED init parameter now, not a
    // host-table pull, so mh.dll -- the host here -- has to supply one.
    //
    // GetLocalTime().wSecond, NOT the original llm_strat_rng_seed_wallclock_seconds, AND THAT IS
    // NOT A SHORTCUT. The first version of this line called the original through mh::call:: so the
    // pin_strat_seed trampoline would stay in the path; it killed every boot. This runs in DllMain,
    // where mh.exe's own Watcom CRT has not been initialised yet, and the original is time() +
    // _localtime(). The whole UI suite went red at once -- 19 of 19, none presenting a frame -- which
    // is what a DllMain-time call into an uninitialised foreign CRT looks like from outside.
    // GetLocalTime is a plain Win32 call, safe under the loader lock, and .wSecond IS what the
    // original returns (tm_sec). The pin is preserved DIRECTLY instead, beside pin_strat_seed's own
    // arm below -- a better place for it, since it no longer depends on a trampoline being reachable.
    SYSTEMTIME st_now;
    GetLocalTime(&st_now);
    libmh_set_session_seed((int32_t)st_now.wSecond);
    // LIFT-EVQ: the event-channel sink binds at the same earliest common point, for the same
    // G104 reason -- an emit from arm-path module code must dispatch synchronously from the
    // first instruction that can reach one. Report line beside [hostapi]'s, where the logger is.
    mh::hook::bind_host_event_sink();
    // (build_paths() ran above, ahead of the state bind -- see the SB-HOSTFREE note there.)
    // LIB-REBIND R11: the rebind's arm bitmap, filled ONCE, here -- the same earliest-common-point
    // reasoning as the two binds above, and for the same G104 reason. Every calls-struct binder
    // consults mh::rebind::armed() on first use, so an arm placed any later would have arm-path
    // module code binding the ORIGINAL for rows this configuration selected, and the run would then
    // report a clean trajectory that proves nothing. Called unconditionally: armed() distinguishes
    // "armed for nothing" from "never armed", and the second COMPLAINS for the whole run.
    //
    // F2E: THE INI IS OUT OF THIS BLOCK ENTIRELY. It used to compose mh_net.ini's path here to read
    // a `[rebind]` section of per-row overrides -- and the path mattered enough to have its own
    // paragraph, because the first draft pointed at mh_harness.ini and a rig run caught it in the
    // only way it could (the fragment armed a row, the report still said "armed 0", and there was no
    // UNKNOWN-row line either: the section was not missing a key, the FILE was the wrong one). With
    // the section deleted there is no path to get wrong: the selector composes its own, from the
    // module file name, for exactly the ordering reason that used to force the work to happen here.
    {
        // THE ROW SET FOLLOWS THE SELECTOR AND NOTHING ELSE. `[config] mode=original` arms nothing --
        // every calls-struct binder takes mh::call and the game's own bodies run -- and brokered arms
        // every row. The rows an instrument OWNS are cleared below; that is not configuration, it is
        // the entry-ownership rule, and it is the only remaining per-row decision.
        mh::rebind::arm_from_config(mh::config::ours_run());

        // THE WALL-CLOCK PIN OWNS time_GetCurrentTime'S ENTRY, so the rebind must yield that row
        // exactly as the lib_trans PROMOTION of the same entry already yields via
        // MH_Harness_WantsWallclockPin. A rebound caller calls our body DIRECTLY and never reaches
        // the hooked entry, so the pin silently stops reaching it: `pin_wallclock=1` still reports
        // armed while those callers read the real clock. That is not a hypothetical -- with the
        // gate default armed it is the ONE row of 676 that reds tact_panel (1.798% on the selected
        // frame, 0.000% on idle), found by bisection.
        //
        // Read straight from the ini rather than g_cfg: this block runs BEFORE load_config, by
        // R11's own requirement that the arming precede any calls-struct binder, so g_cfg is still
        // zero here.
        //
        // F2G: THE HAND-BUILT PATH IS GONE, and the init-order argument that demanded one is what
        // says it may go. It was composed locally because it pointed at a DIFFERENT file from
        // g_ini_path -- and the first draft pointed it at the wrong one of the two, which a rig run
        // caught in the only way it could (the fragment armed a row, the report still said "armed
        // 0", and there was no UNKNOWN-row line either: the section was not missing a key, the FILE
        // was the wrong one). With one file there is one path, and build_paths() is the FIRST
        // statement of MH_Harness_Init -- ahead of the state bind, for the SB-HOSTFREE reason noted
        // there -- so g_ini_path is filled before this block can run. The G104 hazard the local
        // compose avoided was never about this file: it is mh::config's, and mh::config still
        // composes its own path from the module file name for exactly that reason.
        if (GetPrivateProfileIntA("harness", "pin_wallclock", 0, g_ini_path) != 0) {
            if (mh::rebind::set_armed("time_GetCurrentTime", false))
                append_line(g_log_path,
                            "; [rebind] time_GetCurrentTime YIELDED to the wall-clock pin "
                            "(pin_wallclock=1 owns that entry)\n");
        }

        // THE HARNESS-DETOURED ROOTS YIELD WHENEVER THE HARNESS IS ARMED AT ALL, for the reason
        // ROOTS-LIVE already settled one mechanism over: the detour carries the per-step hash that
        // IS the trajectory oracle, so anything taking that entry VOIDS a determinism run rather
        // than failing it. A rebound caller calls our body directly and never reaches the detour,
        // so the harness hashes nothing -- measured as the MP host sitting at step 0 with an empty
        // mh_harness.log, and bisected to llm_strat_sim_step out of all 676 rows.
        //
        // Same shape as the wall-clock row above, and the pair is the whole class: an entry some
        // instrument OWNS cannot also be rebound, because the rebind is a direct call and the
        // ownership is an entry hook.
        //
        // THE LIST USED TO LIVE HERE as `kHarnessOwned[]` -- two string literals plus a comment asking
        // future authors to remember to extend it, which is the G106 hand-list shape one level below
        // the census. SIM1-P clause 6: it is now DERIVED, in the block after the installs, from the
        // entries the detours actually claimed. It cannot run HERE because it needs those claims to
        // exist, and this block deliberately precedes every install (R11 wants the gates loaded before
        // any calls-struct binder).
        //
        // F4E: THE FLAG IT USED TO SET IS THE INSTRUMENT'S AND WENT WITH IT. `g_yield_claimed_rebinds`
        // lives in harness.cpp, in the other image, and is now set by the FIRST statements of
        // MH_Harness_Init -- which DllMain calls immediately after this function, before anything can
        // read a gate, so the value and the moment are both unchanged. It was only ever
        // `harness_enabled()` evaluated here; it is `harness_enabled()` evaluated one call later.
    }
}

// ---- what the harness reads out of this file --------------------------------------------------------

// The paths, as pointers into this image's statics. BORROWED and valid for the run -- the same rule
// every other cross-image row in this project follows (libmh's last_save_path, the module hosts'
// run_dir): two static CRTs mean two heaps, so nothing crosses owning memory. The harness COPIES the
// strings into its own arrays at MH_Harness_Init rather than holding these pointers, which also means
// a harness that never armed never reads them.
extern "C" const MH_CoreArmPaths *MH_Core_ArmPaths(void) {
    g_paths.size          = (unsigned)sizeof(MH_CoreArmPaths);
    g_paths.exe_dir       = g_dir;
    g_paths.ini_path      = g_ini_path;
    g_paths.log_path      = g_log_path;
    g_paths.seed_in       = g_seed_path;
    g_paths.seed_out      = g_seed_out;
    g_paths.boot_snap_out = g_boot_snap_out;
    g_paths.world_out     = g_world_out;
    g_paths.orders_in     = g_orders_path;
    g_paths.orders_out    = g_orders_out;
    g_paths.clock_in      = g_clock_path;
    g_paths.clock_out     = g_clock_out;
    return &g_paths;
}

// The inbound refusal count AT THE MOMENT libmh_in_open returned -- a fact about mh.dll's arm, taken
// before the harness exists. The harness subtracts it from the live count at its first hashed step to
// report refusals SINCE open; see the open site above for why the baseline is what makes that report
// mean anything.
extern "C" int MH_Core_TrapsAtOpen(void) { return g_in_traps_at_open; }
