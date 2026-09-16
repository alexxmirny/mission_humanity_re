//
// libmh_selftest.cpp -- `libmh_selftest.exe`, the SPINE's own offline oracle (fork F5I).
//
// WHAT THIS EXE IS, AND WHY IT IS A SECOND ONE. Until F5I one executable ran all 32 gate suites:
// `net_selftest.exe`, built from mh_nettest.vcxproj in the HOSTED arm (no MH_LIBMH_BUILD), i.e. the
// configuration where every promotion seam and hook row compiles in and MH_CRT() names the original
// binary's Watcom CRT at a fixed VA. That is the right arm for mh.dll's own machinery -- the
// transport, the marshalling thunks, the patch and tombstone instruments, the hook table -- and it
// is the WRONG arm for the 15 suites that are about libmh itself, because the question those ask on
// LIB-REF's behalf is "does the spine still behave the same way when it IS the whole program".
//
// So this image compiles the SAME 628 roster TUs and the SAME 338 test TUs with MH_LIBMH_BUILD +
// MH_SPINE_IN_IMAGE and runs the spine's 15. It COMPILES the roster rather than linking libmh.lib
// on purpose: /fsanitize=address is a compile flag, so an archive built without it would give the
// ASan pass a green report over uninstrumented spine objects.
//
// THE SPLIT IS BY SUBJECT, NOT BY SUITE (rulings R6 and R8). `statetest`, `bindtest` and
// `hostintest` are the three that look like they belong here and do not, and the reason is the same
// each time: their precondition is the STOCK bind. statetest's subject is ST2, a region moving off
// its STOCK VA; bindtest asserts the HOSTED answer to "where does each region live"; hostintest's
// first arm is an inbound open being REFUSED over an unanswered registry. In this arm every stock
// base is 0 by design -- mh_regions.gen.h's MH_STOCK_BASE, so that no original VA reaches
// libmh.lib's initialised data -- so all three would assert over a table of zeros and print green.
// They stay net-side with their TUs; the standalone side of that same question is run_gate's libref
// unit, which binds a real arena, opens the inbound surface for real, and replays a recording.
//
// MODELLED ON libref_host, NOT ON net_selftest. Its binding sequence is the standalone host's; what
// was kept, dropped, and why, is at main(). The ONE property that overrides every other
// consideration here is that main() PRINTS NOTHING on the way in: F5I's oracle for this whole split
// is per-suite transcript identity against a baseline recorded from the hosted exe
// (tools/prove_suite_identity.py), and a banner line in main would make all 15 differ for a reason
// that has nothing to do with the arm.
//
// THERE IS NO DEFAULT MODE. `net_selftest.exe` bare means `selftest`, its transport loopback
// round-trip; this exe has no transport suite at all, so a bare invocation has nothing to default TO
// and prints the mode list and exits 2 -- the same answer a typo gets, which is the correct one when
// there is no documented convenience to distinguish it from.
//
#include "selftest_dispatch.h" // F5I: the suite table mechanism, shared with mh_nettest

#include "hostapi_selftest_support.h" // the GENERATED selftest host tables (both), bound in main()
#include "../libmh/state/host_api.h"  // LIB-ABI: libmh_set_host_api + the unbound walks
#include "../libmh/include/libmh.h"   // LIB-ABI: libmh_abi_version, libmh_install_fp_precision

#include <stdio.h>

// ---- THE 15 SPINE SUITES -------------------------------------------------------------------------
//
// Declared rather than headered, as in net_selftest.cpp: each is one function with one caller, and
// a header per suite would outweigh the declaration. The TUs are this project's own
// (src/mh_dll/libmh_test/), which is what makes these 15 the set that moved.

// orders_selftest.cpp -- the order container's logic over heap buffers (branches no rig can reach)
int run_orderstest();
// issue_selftest.cpp -- O4-0: the order-ISSUE wrappers, one layer ABOVE the container. Drives each
// reimplemented wrapper with concrete inputs and compares what it handed the container against a
// golden EXTRACTED FROM THE ORIGINAL's disassembly (order_issue_golden.gen.h), so the expectations
// are not authored by the same reading that authored the translation.
int run_issuetest();
// lockstep_selftest.cpp -- the turn engine's horizon/barrier logic, same arrangement (L1)
int run_lockstest();
int run_resynctest();
// net_session_selftest.cpp -- NET-SESSION: the process-level lockstep bootstrap
// (llm_net_session_globals_reset) and the mode-8 leader resync frame (llm_wait_screen_frame).
int run_netsessiontest();
// lib_trans_selftest.cpp -- LT0: the lib_trans domain oracle's expectation layer (RNG-family golden
// vectors + batch-A wrapper contracts, pinned against the verified rng_next body).
int run_libtranstest();
// save_selftest.cpp -- the save FORMAT over buffers: batch A's version gate and batch B's block
// layer + LZW codec. Optional argv[2] = a real .sav to push every block of through the layer.
int run_savetest(int argc, char **argv);
// boot_snapshot_selftest.cpp -- LIB-BOOT: the post-cfg snapshot import path and its schema guards.
int run_boottest(int argc, char **argv);
// world_snapshot_selftest.cpp -- LIB-WORLD: the step-0 world fixture. `worldtest` runs a
// synthesised blob through the format, the refusals and all three coverage arms; `worldtest <blob>`
// imports a REAL step-0 capture into a poisoned arena and reproduces the recording peer's lockstep
// hash -- the done_when's oracle, in a process with no game to reproduce it from.
int run_worldtest(int argc, char **argv);
// nav_trailer_selftest.cpp -- LIB-REF: the world blob's FORMAT 2 nav trailer, round-tripped
// SLOT-EXACT (both list orders, not just membership).
int run_navtest();
// ai_selftest.cpp -- AI0: the strategic AI's decision logic over heap buffers. Most of the ~215-
// function AI cluster is pure over its state, so this is the oracle it is verified with; a rig run
// cannot reach an empty candidate list, a negative damage tally, or the player-7 record overrun.
int run_aitest();
// sim_selftest.cpp -- SIM0: the strategic sim's logic over heap buffers, the sibling of aitest and
// the lever that keeps the 307-function sim migration off the rig.
int run_simtest();
// tact_selftest.cpp -- TACT-DOMAIN: the OFFLINE half of the tactical oracle. The trajectory oracle
// compares a COMBINED hash over 14 slices and therefore cannot audit its own coverage; this is
// where all 14 slices are proven to be read and the arena's strides proven to match the manifest.
int run_tacttest();
// crt_sprintf_selftest.cpp -- LIB-CRT: the vendored sprintf family. A standalone libmh cannot reach
// the binary's Watcom CRT at a VA, so crt/crt_sprintf.h supplies the 12 shapes and this is the
// oracle that says they mean the same thing. THIS exe is the arm where that actually matters: it
// defines MH_LIBMH_BUILD, so its own MH_CRT() sites resolve to the shapes under test.
int run_crttest();
// crt_vendor_selftest.cpp -- LIB-CRT, the other five vendored headers (crt_string / crt_math /
// crt_heap / crt_rand / crt_qsort). Same suite name, separate TU: its reference arm is the ORIGINAL
// MACHINE CODE transcribed into naked functions, so `crttest` runs both halves and sums.
int run_crt_vendor_test();
// fp_x87_selftest.cpp -- CRT-X87 step 2: mh/fp/x87.h's helpers were assembly and are now C++, and
// this keeps a verbatim copy of the assembly as the reference arm so the equality is re-proved every
// run. Runs at BOTH x87 precision settings, setting the control word itself at each step -- which is
// why main()'s PC=53 install below cannot perturb it.
int run_fptest();

// The one row that does not fit a shared adapter shape.
static int adapt_crttest(const suite_args &) {
    // LIB-CRT has TWO halves and both must run: the sprintf family (expectations written out from
    // the standard) and the other five vendored headers (expectations transcribed from the original
    // machine code). Run both and SUM -- returning the first non-zero would hide a failure in the
    // second.
    const int a = run_crttest();
    const int b = run_crt_vendor_test();
    return (a != 0 || b != 0) ? 1 : 0;
}

// clang-format off
// THE TABLE. Every row is gate-flagged: unlike net_selftest.cpp this exe has no transport modes, no
// spawned children and no known-red suite, so `gate` is uniformly true here and the flag exists only
// because the row type is shared. The ORDER is tools/data/selftest_roster.json's, which is
// src/mh_dll/README.md's.
static const suite_row SUITE_TABLE[] = {
    {"orderstest",     true, adapt_void<run_orderstest>},
    {"issuetest",      true, adapt_void<run_issuetest>},
    // The three argc/argv suites: each takes an OPTIONAL real-capture path as argv[2]. The gate runs
    // them bare (the synthesised-fixture arm); the fixture arm is a hand invocation.
    {"boottest",       true, adapt_argv<run_boottest>},
    {"worldtest",      true, adapt_argv<run_worldtest>},
    {"navtest",        true, adapt_void<run_navtest>},
    {"aitest",         true, adapt_void<run_aitest>},
    {"simtest",        true, adapt_void<run_simtest>},
    {"tacttest",       true, adapt_void<run_tacttest>},
    {"crttest",        true, adapt_crttest},
    {"fptest",         true, adapt_void<run_fptest>},
    {"lockstest",      true, adapt_void<run_lockstest>},
    {"resynctest",     true, adapt_void<run_resynctest>},
    {"netsessiontest", true, adapt_void<run_netsessiontest>},
    {"libtranstest",   true, adapt_void<run_libtranstest>},
    {"savetest",       true, adapt_argv<run_savetest>},
};
// clang-format on

static const size_t SUITE_COUNT = sizeof(SUITE_TABLE) / sizeof(SUITE_TABLE[0]);

namespace {
// The unbound walk's sink. It RECORDS rather than prints, because main() prints nothing on a green
// path (see the file banner); the name is only rendered if the walk actually found a hole.
int  g_unbound_n = 0;
char g_unbound_first[128];

extern "C" void on_unbound(const char *name) {
    if (g_unbound_n++ == 0) snprintf(g_unbound_first, sizeof(g_unbound_first), "%s", name);
}
} // namespace

int main(int argc, char **argv) {
    // ANSWERED BEFORE ANYTHING IS BOUND, for the same reason net_selftest.cpp answers it first:
    // tools/run_selftests.py reads `--list-suites` to assert this exe's roster equals the committed
    // one, so it must not be able to fail for a reason unrelated to the roster -- a host-table
    // version skew below exits 3, and the assertion would read that as "the exe lists nothing".
    const char *mode = (argc > 1) ? argv[1] : "";
    if (strcmp(mode, "--list-suites") == 0) return selftest_list_suites(SUITE_TABLE, SUITE_COUNT);

    // ---- the standalone host's binding sequence, libref_host/main.cpp's, step by step -----------
    //
    // KEPT: the ABI version check. It is the cheapest possible statement that the header this exe
    // compiled against and the spine it links are the same generation, and in THIS image the two
    // cannot skew independently -- which is exactly why it must be here rather than assumed: if it
    // ever fires, something in the generated ABI layer is out of step with its own generator.
    if (libmh_abi_version() != LIBMH_ABI_VERSION) {
        printf("FATAL: libmh ABI %u, this host's header %u\n", (unsigned)libmh_abi_version(),
               (unsigned)LIBMH_ABI_VERSION);
        return 3;
    }

    // KEPT: both host tables, bound BEFORE any suite runs module code. Module code reaches host
    // callbacks only through mh::host(), which fail-fast aborts if nothing is bound, and the
    // generated selftest tables are the no-op-host arm -- notify entries do nothing, REQUIRED
    // entries name themselves and die (mh_hostapi_trap below). Same two calls net_selftest.cpp
    // makes; libref_host makes them with a real host's tables, which is the only difference.
    if (libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION) != 0) {
        printf("FATAL: selftest host table refused by libmh_set_host_api (version skew?)\n");
        return 3;
    }
    if (libmh_set_tact_host_api(&mh_hostapi_selftest_tact_table(), LIBMH_TACT_HOST_API_VERSION) !=
        0) {
        printf("FATAL: selftest tact host table refused by libmh_set_tact_host_api "
               "(version skew?)\n");
        return 3;
    }

    // KEPT: both unbound walks. The tables are generated, so a hole is a generator bug rather than a
    // typo -- but it is the class of bug that produces a NULL entry a suite then calls, and libmh's
    // dispatch does not check per call. Silent when clean; the name is what makes it actionable.
    if (libmh_host_api_unbound(on_unbound) != 0 || libmh_tact_host_api_unbound(on_unbound) != 0) {
        printf("FATAL: %d unbound selftest host entr(y/ies), first: %s\n", g_unbound_n,
               g_unbound_first);
        return 3;
    }

    // KEPT, as an ASSERTION rather than an action. Under MH_LIBMH_BUILD libmh_set_host_api has
    // ALREADY installed the PC=53 guarantee (state/host_api.cpp: standalone installs, hosted
    // inherits), so this call is idempotent by contract and cannot move the control word here. It
    // stays because libref_host makes it and because a 1 back from it is the only in-process
    // evidence that the guarantee the eleven C++ x87 helpers depend on is actually in force.
    // It does NOT perturb `fptest`: that suite sets the control word itself before each sweep and
    // its arm P deliberately starts from PC=64.
    if (libmh_install_fp_precision() != 1) {
        printf("FATAL: could not install the PC=53 guarantee\n");
        return 3;
    }

    // DROPPED: `libmh_in_open()`. libref_host must open the inbound surface -- every MH_IN_GUARD'd
    // entry refuses until it does, and a refusal RETURNS THE CLOSED DEFAULT, which is how a
    // footprint check silently answered "not clear" for 289 steps of a replay. This exe must NOT.
    // The suite that pins that behaviour, `hostintest`, is net-side (R8) precisely because its first
    // arm is an open being refused over an UNANSWERED registry -- a precondition about the stock
    // bind -- but the rule outlives it: an exe that opened the inbound surface in main() would leave
    // every guard arm in every suite asserting nothing while still printing green. net_selftest.cpp
    // does not call it either.
    //
    // DROPPED: the arena (`bind_all_regions`), the fixture load, the replay contract's
    // `set_suppress_enqueue`, the nav report, the vectored fault handler and the unbuffered stdout.
    // All of those belong to REPLAYING A RECORDING, which is libref_host's job and not a suite's;
    // `bindtest` and `statetest` stand the registry up themselves, from their own fixtures, and
    // would be measuring main()'s arena rather than their own if one existed.
    //
    // DROPPED: `mh::rebind::arm_none()` and `MH_LibMH_BindHookApi()`, the two calls net_selftest.cpp
    // makes here. The second is genuinely absent from this image: it is defined in
    // mh/seams/libmh_hook_host.cpp, which is mh.dll's own code and in no roster (grepped: the only
    // definition in the tree), so calling it would not link. The first is a subtler case and the
    // reason is NOT "absent" -- state/rebind_arming.cpp is a roster TU and `arm_none()` compiles and
    // links here perfectly well. It is that under MH_LIBMH_BUILD `armed()` returns true
    // unconditionally ("standalone binds unconditionally") and consults the bitmap arm_none() fills
    // for nothing, so the call cannot change one branch in this image. Making it anyway would state
    // a fact about the arming that is not true of this arm.

    const suite_args a = selftest_args(argc, argv);
    if (const suite_row *row = selftest_find(SUITE_TABLE, SUITE_COUNT, mode)) return row->run(a);

    // NO DEFAULT MODE, including for the bare invocation -- see the file banner. `net_selftest.exe`
    // may default to `selftest` because that row is a documented convenience which cannot be
    // confused with a typo; here there is no transport suite to default to, so every unrecognised
    // argument (the empty one included) gets the list and exit 2.
    return selftest_unknown_mode(mode, SUITE_TABLE, SUITE_COUNT);
}
