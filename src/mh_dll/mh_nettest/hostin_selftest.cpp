//
// hostin_selftest.cpp -- the INBOUND (host -> libmh) ABI's binding contract, off the rig
// (LIB-REF-IN; the LIB-REF plan section 3.5).
//
// THE SIBLING OF hostapi_selftest.cpp, AND DELIBERATELY THE SAME SHAPE. That file proves the
// OUTBOUND table's contract; this one proves the inbound surface's, arm for arm: the version
// handshake refuses a stale host WITHOUT clobbering a working open, the unbound-walk names a gap
// rather than counting it, and a call into a holed slot TRAPS BY NAME instead of returning a quiet
// zero that reads like a working entry which happened to do nothing.
//
// WHAT THIS SUITE DOES NOT DO, and why that is not a gap. It never calls an entry that would reach
// a real body. net_selftest is not injected into mh.exe, so the stock region bases the surface
// binds are not mapped in this process -- forwarding into a wrapper would fault on the first read,
// and the fault would be the harness's, not a finding. The entries' FORWARDING is proven somewhere
// better anyway: the hosted configuration routes all 21 sim_hostreach promotion seams through these
// same entries, so the UI suite and the determinism run are the forwarding oracle. What can only be
// proven HERE is everything that happens BEFORE the forward -- the gate, the walk and the trap --
// and every arm below stops at that boundary on purpose.
//
// THE HOLE IS PUNCHED IN THE RUNTIME TABLE, not in generated code: mh::libmh_in::test_unbind_*
// nulls one slot of the copy libmh_in_open built, exactly as hostapitest nulls one member of a
// copy of the host-callback struct. libmh_in_open rebuilds from the generated data, so the suite
// puts the surface back by re-opening it.
//
#include "../libmh/state/host_bind.h" // libmh_bind_regions / libmh_bound_count -- the open's gate
#include "../libmh/state/host_in.h"

#include <cstdio>
#include <cstring>

namespace {

int  g_checks = 0, g_fails = 0;
void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

int  g_walk_reports = 0;
char g_walk_last[128];
void on_in_unbound(const char *name) {
    ++g_walk_reports;
    std::snprintf(g_walk_last, sizeof(g_walk_last), "%s", name);
}

void walk_reset() {
    g_walk_reports = 0;
    g_walk_last[0] = '\0';
}

} // namespace

int run_hostintest() {
    printf("hostintest: the inbound (host -> libmh) ABI binding contract "
           "(%u entries + %u order ids, version 0x%08X)\n",
           (unsigned)LIBMH_IN_ENTRY_COUNT, (unsigned)LIBMH_ORD_COUNT,
           (unsigned)LIBMH_HOST_IN_VERSION);

    // ---- the open gate: refused while the state ABI is unanswered ---------------------------
    //
    // This arm runs FIRST and only means anything first: it needs a process where the regions are
    // not yet bound, and every later arm binds them. -2 rather than a warning, for the same reason
    // libmh_default_binds refuses standalone rather than handing back zeroed bases -- a surface
    // opened over an unanswered registry is a surface aimed at nothing.
    if (libmh_bound_count() < (int)libmh_region_count()) {
        ck(libmh_in_open(LIBMH_HOST_IN_VERSION) == LIBMH_IN_E_STATE,
           "open is refused with -2 while the state ABI is not fully bound");
        ck(libmh_in_is_open() == 0, "the refused open did not open the surface");
        ck(libmh_in_unbound(nullptr) == LIBMH_IN_E_CLOSED, "the walk refuses on a closed surface");
    } else {
        printf("  note: regions were already bound on entry -- the unbound-state open arm is "
               "not exercised in this run\n");
    }

    // Answer for every region with the stock table. In this process those bases are mh.exe's .bss
    // addresses and nothing is mapped there -- which is fine and is the reason no arm below calls
    // an entry that forwards. What the bind buys is a legal open.
    mh::state::bind_stock();
    ck(libmh_bound_count() == (int)libmh_region_count(),
       "the stock bind answers for every region (the open's precondition)");

    // ---- version handshake -------------------------------------------------------------------
    ck(libmh_in_open(LIBMH_HOST_IN_VERSION + 1) == LIBMH_IN_E_CLOSED,
       "a stale host version is refused with -1");
    ck(libmh_in_is_open() == 0, "a refusal before any successful open leaves the surface closed");
    ck(libmh_in_open(LIBMH_HOST_IN_VERSION) == LIBMH_IN_OK, "the matching version opens");
    ck(libmh_in_is_open() == 1, "the surface reports itself open");

    // THE CLAUSE THAT MATTERS: a refusal must not clobber a WORKING open. This is the
    // libmh_set_host_api shape, and it is the half a naive implementation gets wrong (close first,
    // then validate), so it is asserted against a surface that is currently open.
    ck(libmh_in_open(LIBMH_HOST_IN_VERSION - 1) == LIBMH_IN_E_CLOSED,
       "a stale version is still refused while a surface is open");
    ck(libmh_in_is_open() == 1, "the refused open KEPT the working surface");

    // ---- the walk: full table is zero --------------------------------------------------------
    walk_reset();
    ck(libmh_in_unbound(on_in_unbound) == 0, "every entry and order id is bound (walk == 0)");
    ck(g_walk_reports == 0, "a zero walk reports nothing");

    // ---- trap by name: a holed ENTRY ---------------------------------------------------------
    //
    // libmh_post_event is the row 35 front-end sites reach, so it is the entry whose silent no-op
    // would be least visible and most damaging -- which is exactly why the arm uses it.
    mh::libmh_in::test_unbind_entry(LIBMH_IN_ENTRY_POST_EVENT);
    walk_reset();
    ck(libmh_in_unbound(on_in_unbound) == 1, "one holed entry -> walk == 1");
    ck(g_walk_reports == 1 && strcmp(g_walk_last, "libmh_post_event") == 0,
       "the holed entry is reported BY NAME (libmh_post_event)");

    mh::libmh_in::trap_reset();
    ck(libmh_post_event(0) == 0, "a holed entry returns its closed default rather than forwarding");
    ck(mh::libmh_in::trap_count() == 1, "calling a holed entry traps exactly once");
    ck(strcmp(mh::libmh_in::last_trap(), "libmh_post_event") == 0,
       "the trap NAMES the entry it refused (libmh_post_event)");

    // ---- trap by name: a holed ORDER ID ------------------------------------------------------
    //
    // The order table is the surface's genuinely holeable half: libmh_issue_order dispatches
    // through generated constructor slots, so "an id with no implementation" is a reachable state
    // rather than a hypothetical one. Two different names must come back -- the entry's above and
    // the ORDER's here -- or the trap is naming a category instead of a slot.
    mh::libmh_in::test_unbind_order(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER);
    walk_reset();
    ck(libmh_in_unbound(on_in_unbound) == 2, "a holed entry AND a holed order id -> walk == 2");

    mh::libmh_in::trap_reset();
    {
        const int32_t argv[3] = {0, 0, 0};
        ck(libmh_issue_order(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER, argv, 3u) == LIBMH_IN_E_UNBOUND,
           "a holed order id is refused with -5");
        ck(mh::libmh_in::trap_count() == 1 &&
               strcmp(mh::libmh_in::last_trap(), "llm_strat_order_ctrlgrp_flash_member") == 0,
           "the order trap names the ORDER (llm_strat_order_ctrlgrp_flash_member), not the entry");
    }

    // Re-opening rebuilds both tables from the generated data -- the positive control that says the
    // two arms above were reading a hole and not a permanently broken surface.
    ck(libmh_in_open(LIBMH_HOST_IN_VERSION) == LIBMH_IN_OK, "re-open succeeds");
    walk_reset();
    ck(libmh_in_unbound(on_in_unbound) == 0, "re-open rebuilt both tables (walk back to 0)");

    // The seam's own restore, checked rather than assumed: hole both tables again and put them back
    // WITHOUT re-opening, so a later arm that needs the surface to stay open can rely on it.
    mh::libmh_in::test_unbind_entry(LIBMH_IN_ENTRY_POST_EVENT);
    mh::libmh_in::test_unbind_order(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER);
    ck(libmh_in_unbound(nullptr) == 2, "both holes are back");
    mh::libmh_in::test_restore_all();
    ck(libmh_in_unbound(nullptr) == 0, "test_restore_all rebuilds both tables without re-opening");
    ck(libmh_in_is_open() == 1, "restore left the surface open");

    // ---- the order table's introspection + argv contract -------------------------------------
    //
    // A GDExtension binding generator emits typed wrappers from exactly these two, so a wrong
    // arity here is a wrong wrapper there. The arities themselves are generated from the committed
    // prototypes, so what is checked is that the accessors agree with the table and that the
    // out-of-range answers are distinguishable from real ones.
    ck(libmh_order_arity(LIBMH_ORD_COUNT) == -1, "arity of an out-of-range id is -1");
    ck(libmh_order_name(LIBMH_ORD_COUNT) == nullptr, "name of an out-of-range id is null");
    ck(libmh_order_name(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER) != nullptr &&
           strcmp(libmh_order_name(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER),
                  "llm_strat_order_ctrlgrp_flash_member") == 0,
       "an in-range id names its owned body");
    ck(libmh_order_arity(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER) == 3,
       "the ctrl-group flash order declares arity 3 (side, unit_id, group_index)");
    {
        int nonzero = 0;
        for (uint32_t i = 0; i < LIBMH_ORD_COUNT; ++i)
            if (libmh_order_name(i) != nullptr && libmh_order_arity(i) >= 0) ++nonzero;
        ck(nonzero == (int)LIBMH_ORD_COUNT, "every order id has a name and a declared arity");
    }

    // The refusals that keep a binding bug from becoming a desync. All three stop BEFORE dispatch,
    // so none of them forwards -- which is why they are safe in this process.
    {
        const int32_t argv[4] = {0, 0, 0, 0};
        ck(libmh_issue_order(LIBMH_ORD_COUNT, argv, 0u) == LIBMH_IN_E_ARG,
           "an out-of-range order id is refused with -2");
        ck(libmh_issue_order(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER, argv, 4u) == LIBMH_IN_E_ARITY,
           "argc != the declared arity is refused with -3");
        ck(libmh_issue_order(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER, nullptr, 3u) == LIBMH_IN_E_ARG,
           "a null argv with a non-zero argc is refused with -2 and never dispatches");
    }

    // ---- the entry-name table ----------------------------------------------------------------
    ck(libmh_in_entry_name(LIBMH_IN_ENTRY_COUNT) == nullptr,
       "entry_name past the end is null, not a garbage pointer");
    {
        int named = 0;
        for (uint32_t i = 0; i < LIBMH_IN_ENTRY_COUNT; ++i)
            if (libmh_in_entry_name(i) != nullptr && libmh_in_entry_name(i)[0] == 'l') ++named;
        ck(named == (int)LIBMH_IN_ENTRY_COUNT,
           "every entry id resolves to a libmh_-prefixed name (the walk's naming source)");
    }

    printf("hostintest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
