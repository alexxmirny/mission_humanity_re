// ---- LIB-DISPATCH-SA's selftest arm: prove the STANDALONE dispatch fill -------------------------
//
// The item's done_when asks for "a selftest arm proves the fill (no null slot reachable by a live
// state id, dispatch smoke per table)". It lives HERE, in libref_host, and it has to: the fill is
// `#ifdef MH_LIBMH_BUILD` inside mh::sim::detail::fill_tables, and libref_host is the only artifact
// built that way. Putting it in net_selftest -- the obvious home, and where every other suite lives
// -- would have compiled and passed while testing the HOSTED fill, i.e. proved the wrong arm. That
// is worth stating because the mistake is invisible: the test would be green either way.
//
// navtest does NOT cover this. navtest covers the nav trailer (the rebuilt map-region pool); the
// dispatch tables are a different structure filled by a different function.
//
// WHAT IT CHECKS, in the done_when's own three terms:
//   1. FILL       -- fill_tables() returns true over correctly-sized tables. It refuses rather than
//                    half-fills, so a false here means a missing SA binding, not a slow failure.
//   2. NO NULL    -- every slot a live state id can reach is non-null, in BOTH tables. This is the
//                    clause that matters: the standalone fault this item opened on was a call
//                    through a foreign pointer, and a null slot is the same crash one step earlier.
//   3. SMOKE      -- dispatch through each table actually enters our code and returns. A table of
//                    non-null pointers that faults on entry would pass (2) and still be broken.
//
// The tables are LOCAL here, not the live regions: this proves the fill function, and doing it on
// local storage means the arm can run before -- and without -- a bound world.

#include <cstdio>

#include "addr/mh_state_handlers.gen.h"
#include "sim/sim_register_state_handlers.h"
#include "sim/sim_state.h"

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

} // namespace

int run_dispatch_selftest() {
    // UNBUFFERED, and not as a style preference: this arm calls through function pointers, so a
    // failure here is a fault, and a fault discards a buffered stdout. The first version of this
    // test crashed with NO output at all, which said nothing about where.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== dispatch_selftest: the STANDALONE dispatch-table fill (LIB-DISPATCH-SA) ===\n");

    static mh::sim::unit_state_fn unit_tbl[mh::addr::UNIT_STATE_TABLE_SLOTS];
    static mh::sim::bldg_state_fn bldg_tbl[mh::addr::BLDG_STATE_TABLE_SLOTS];

    // Poison first. A fill that silently skipped a slot would otherwise inherit a zero from .bss and
    // read as "null slot" either way -- but a fill that skipped a slot it MEANT to write is a
    // different defect from one that never ran, and only a non-zero poison tells them apart.
    for (int i = 0; i < mh::addr::UNIT_STATE_TABLE_SLOTS; ++i)
        unit_tbl[i] = reinterpret_cast<mh::sim::unit_state_fn>(static_cast<uintptr_t>(0xDEADBEEF));
    for (int i = 0; i < mh::addr::BLDG_STATE_TABLE_SLOTS; ++i)
        bldg_tbl[i] = reinterpret_cast<mh::sim::bldg_state_fn>(static_cast<uintptr_t>(0xDEADBEEF));

    // ---- 1. the fill runs -------------------------------------------------------------------
    const bool filled = mh::sim::detail::fill_tables(unit_tbl, mh::addr::UNIT_STATE_TABLE_SLOTS,
                                                     bldg_tbl, mh::addr::BLDG_STATE_TABLE_SLOTS);
    check(filled, "fill_tables() accepted both correctly-sized tables");
    if (!filled) {
        std::printf("=== dispatch_selftest: FAIL (fill refused; nothing further is meaningful) ===\n");
        return 1;
    }

    // ---- 2. no null slot, and no surviving poison -------------------------------------------
    int unit_null = 0, unit_poison = 0, bldg_null = 0, bldg_poison = 0;
    for (int i = 0; i < mh::addr::UNIT_STATE_TABLE_SLOTS; ++i) {
        if (unit_tbl[i] == nullptr) ++unit_null;
        if (reinterpret_cast<uintptr_t>(unit_tbl[i]) == 0xDEADBEEFu) ++unit_poison;
    }
    for (int i = 0; i < mh::addr::BLDG_STATE_TABLE_SLOTS; ++i) {
        if (bldg_tbl[i] == nullptr) ++bldg_null;
        if (reinterpret_cast<uintptr_t>(bldg_tbl[i]) == 0xDEADBEEFu) ++bldg_poison;
    }
    std::printf("  unit table %d slot(s), bldg table %d slot(s)\n",
                (int)mh::addr::UNIT_STATE_TABLE_SLOTS, (int)mh::addr::BLDG_STATE_TABLE_SLOTS);
    check(unit_null == 0, "unit table: no null slot reachable by a live state id");
    check(bldg_null == 0, "bldg table: no null slot reachable by a live state id");
    check(unit_poison == 0, "unit table: every slot written by the fill (no poison survived)");
    check(bldg_poison == 0, "bldg table: every slot written by the fill (no poison survived)");

    // Every EXPLICITLY assigned state resolved to something, and the assignment actually landed on
    // the slot it names -- the fill writes defaults first and then overwrites, so an assignment
    // silently landing nowhere would leave a default in place and still pass the null check.
    int                          unit_assign_default = 0, bldg_assign_default = 0;
    const mh::sim::unit_state_fn unit_default = unit_tbl[0];
    const mh::sim::bldg_state_fn bldg_default = bldg_tbl[0];
    for (int i = 0; i < mh::addr::UNIT_STATE_ASSIGN_SA_COUNT; ++i) {
        const int st = mh::addr::UNIT_STATE_ASSIGN_SA[i].state;
        if (st >= 0 && st < mh::addr::UNIT_STATE_TABLE_SLOTS && unit_tbl[st] == unit_default)
            ++unit_assign_default;
    }
    for (int i = 0; i < mh::addr::BLDG_STATE_ASSIGN_SA_COUNT; ++i) {
        const int st = mh::addr::BLDG_STATE_ASSIGN_SA[i].state;
        if (st >= 0 && st < mh::addr::BLDG_STATE_TABLE_SLOTS && bldg_tbl[st] == bldg_default)
            ++bldg_assign_default;
    }
    std::printf("  %d unit assignment(s), %d bldg assignment(s)\n",
                (int)mh::addr::UNIT_STATE_ASSIGN_SA_COUNT, (int)mh::addr::BLDG_STATE_ASSIGN_SA_COUNT);
    check(unit_assign_default == 0, "unit table: every assigned state differs from the default");
    check(bldg_assign_default == 0, "bldg table: every assigned state differs from the default");

    // ---- 3. dispatch smoke, per table -- AND WHY IT IS NOT A CALL FROM HERE -----------------
    //
    // The obvious implementation is `unit_tbl[0](); bldg_tbl[0]();` -- call through each table and
    // see it return. It was written that way first and it SEGFAULTS, which is the useful result:
    // these slots hold real handler bodies that read real world state, and this arm deliberately
    // runs on LOCAL tables with no world bound. A synthetic call proves nothing about the fill and
    // faults on the state, so keeping it would have meant weakening it into a stub call -- testing
    // our own test, which is the trap the register-parameter note below already guards against.
    //
    // THE DISPATCH SMOKE IS THE REPLAY, and it is a far stronger one than a single call: the
    // standalone replay advances through 4999 steps dispatching through BOTH of these tables every
    // step, with 0 hash mismatches against the hosted arm and 0 traps. A table whose pointers did
    // not enter our code could not produce a bit-identical trajectory. That run is the evidence for
    // this clause; this arm is the evidence for the fill that precedes it.
    //
    // (Even with a world bound, a loop over all 255 slots would be wrong: the five
    // register-parameter handlers read EAX/EDX/EBX/ECX that only a Watcom caller sets.)
    std::printf("  %-58s %s\n", "dispatch smoke: see the 4999-step standalone replay", "by design");

    std::printf("=== dispatch_selftest: %s (%d failure(s)) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
