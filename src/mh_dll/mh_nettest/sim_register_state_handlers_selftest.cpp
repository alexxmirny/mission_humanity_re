//
// sim_register_state_handlers_selftest.cpp -- `simtest` oracle for OUR registrar
// (sim/sim_register_state_handlers.h/.cpp, RI-SIM / SIM1-DISPATCH, 2026-08-22).
//
// WHAT THIS ORACLE IS FOR, AND WHY THE LIVE RUN IS NOT ENOUGH. The golden A/B says whether the game
// still behaves the same with 68 of our handlers dispatched for real. It cannot say WHICH slot got
// which handler: give unit state 0x0f the plot_turn_path handler instead of move_walker and the
// trajectory diverges -- but the divergence names a step and a region, not the swapped pair, and a
// scenario where neither state ever fires reports MATCHED with the swap still in place. The pairing
// is a table, and a table is exactly what an offline check can read entry by entry.
//
// So `detail::fill_tables` is deliberately separable from the live binding: it takes two arrays and
// fills them, and everything below drives it over LOCAL arrays. No game memory is touched here.
//
// THE ORDER MATTERS AND IS CHECKED. The original registrar fills 0..0xfe with a default and THEN
// overwrites 44 unit / 34 bldg states. Two mistakes survive a naive "every slot is one of ours"
// check: filling the assignments first and then flattening them with the default (case D would go
// red), and skipping the default fill so unassigned states keep whatever was in .bss (case C).
//
// EXPECTED CONTENT comes from addr/mh_state_handlers.gen.h, extracted by
// tools/gen_state_handler_table.py from llm_strat_register_state_handlers @0x0045f26a. This file
// deliberately re-derives NOTHING from it: it asserts that fill_tables reproduces the generated
// table, and separately (case G) pins two well-known pairings by NAME against the strategic-sim notes,
// so that a wholesale corruption of the generated table itself is still caught here.
//
#include "sim/sim_register_state_handlers.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "addr/mh_state_handlers.gen.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int32_t UNIT_SLOTS = mh::addr::UNIT_STATE_TABLE_SLOTS;
constexpr int32_t BLDG_SLOTS = mh::addr::BLDG_STATE_TABLE_SLOTS;

// A sentinel no thunk can ever equal, so "untouched" is distinguishable from "filled".
unit_state_fn sentinel_fn() { return reinterpret_cast<unit_state_fn>(static_cast<uintptr_t>(0xdeadbeefu)); }

const handler_binding *bind_of(uintptr_t va) { return detail::binding_for(va); }

// The name of whichever binding owns `fn`, or "<none>" -- so a failure message says WHICH handler
// landed in the slot rather than only that the pointer differed.
const char *name_of(unit_state_fn fn) {
    const handler_binding *b = detail::handler_bindings();
    for (int i = 0; i < detail::handler_binding_count(); ++i)
        if (b[i].ours == fn) return b[i].name;
    return "<none>";
}

bool state_is_assigned_unit(int s) {
    for (int i = 0; i < mh::addr::UNIT_STATE_ASSIGN_COUNT; ++i)
        if (mh::addr::UNIT_STATE_ASSIGN[i].state == s) return true;
    return false;
}
bool state_is_assigned_bldg(int s) {
    for (int i = 0; i < mh::addr::BLDG_STATE_ASSIGN_COUNT; ++i)
        if (mh::addr::BLDG_STATE_ASSIGN[i].state == s) return true;
    return false;
}

} // namespace

void run_register_state_handlers_tests() {
    printf("-- llm_strat_register_state_handlers (the two state-machine dispatch tables) --\n");

    // ---- A: the binding table itself (68 rows, one per distinct handler in the binary) ----------
    {
        const handler_binding *b = detail::handler_bindings();
        const int              n = detail::handler_binding_count();
        ck_eq((uint32_t)n, (uint32_t)mh::addr::STATE_HANDLER_DISTINCT_COUNT,
              "A1: one binding per distinct handler the registrar installs, 0x0045f26a");
        int nulls = 0, zero_va = 0, unnamed = 0;
        for (int i = 0; i < n; ++i) {
            if (b[i].ours == nullptr) ++nulls;
            if (b[i].original_va == 0) ++zero_va;
            if (b[i].name == nullptr || b[i].name[0] == '\0') ++unnamed;
        }
        ck_eq((uint32_t)nulls, 0u, "A2: every binding has an entry thunk (a null slot would be a "
                                   "CALL to 0 the first time that state ticks)");
        ck_eq((uint32_t)zero_va, 0u, "A3: every binding carries its original VA (mh::exp::addr_<fn>)");
        ck_eq((uint32_t)unnamed, 0u, "A4: every binding carries the original's symbol name");

        // Distinctness both ways. Two rows sharing a VA means the X-macro emitted a handler twice;
        // two rows sharing a THUNK means two originals were bound to one C++ wrapper -- which is the
        // copy-paste failure this whole file exists to catch, and it is invisible in a live run.
        int dup_va = 0, dup_fn = 0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                if (b[i].original_va == b[j].original_va) ++dup_va;
                if (b[i].ours == b[j].ours) ++dup_fn;
            }
        ck_eq((uint32_t)dup_va, 0u, "A5: no two bindings share an original VA");
        ck_eq((uint32_t)dup_fn, 0u, "A6: no two originals are bound to the SAME entry thunk");
    }

    // ---- B: every VA the generated tables name is bindable ---------------------------------------
    {
        ck(bind_of(mh::addr::UNIT_STATE_DEFAULT_VA) != nullptr,
           "B1: the unit default (llm_strat_unit_state_default_noop @0x0047e2ac) has a binding");
        ck(bind_of(mh::addr::BLDG_STATE_DEFAULT_VA) != nullptr,
           "B2: the bldg default (llm_strat_bldg_state_default_reset @0x004711c3) has a binding");
        int missing = 0;
        for (int i = 0; i < mh::addr::UNIT_STATE_ASSIGN_COUNT; ++i)
            if (bind_of(mh::addr::UNIT_STATE_ASSIGN[i].original_va) == nullptr) ++missing;
        for (int i = 0; i < mh::addr::BLDG_STATE_ASSIGN_COUNT; ++i)
            if (bind_of(mh::addr::BLDG_STATE_ASSIGN[i].original_va) == nullptr) ++missing;
        ck_eq((uint32_t)missing, 0u,
              "B3: every assigned state's handler VA resolves to a binding (78 assignments)");
        ck(bind_of(0x00400000u) == nullptr,
           "B4: an address that is NOT a state handler does not resolve -- binding_for is a lookup, "
           "not a fallback");
    }

    // ---- C: the fill, over local arrays ----------------------------------------------------------
    std::vector<unit_state_fn> unit(UNIT_SLOTS, sentinel_fn());
    std::vector<bldg_state_fn> bldg(BLDG_SLOTS, sentinel_fn());
    {
        ck(detail::fill_tables(unit.data(), UNIT_SLOTS, bldg.data(), BLDG_SLOTS),
           "C1: fill_tables accepts two correctly-sized tables");

        int unit_foreign = 0, bldg_foreign = 0;
        for (int i = 0; i < UNIT_SLOTS; ++i)
            if (!is_our_handler(reinterpret_cast<const void *>(unit[i]))) ++unit_foreign;
        for (int i = 0; i < BLDG_SLOTS; ++i)
            if (!is_our_handler(reinterpret_cast<const void *>(bldg[i]))) ++bldg_foreign;
        // This is the clause SIM1-DISPATCH's done_when turns on: not "the assigned states are ours"
        // but EVERY slot, because llm_strat_unit_tick indexes by a byte it does not range-check
        // (0x0047c8d6 MOVZX / 0x0047c8dd CALL) and an unfilled slot is a call into stale .bss.
        ck_eq((uint32_t)unit_foreign, 0u,
              "C2: all 255 unit slots hold one of OUR handlers -- the default fill really ran");
        ck_eq((uint32_t)bldg_foreign, 0u, "C3: all 255 bldg slots hold one of OUR handlers");
    }

    // ---- D: each assigned state holds ITS OWN handler, and not the default -----------------------
    {
        int                    wrong = 0, flattened = 0;
        const handler_binding *unit_default = bind_of(mh::addr::UNIT_STATE_DEFAULT_VA);
        for (int i = 0; i < mh::addr::UNIT_STATE_ASSIGN_COUNT; ++i) {
            const mh::addr::state_handler_slot &s = mh::addr::UNIT_STATE_ASSIGN[i];
            if (unit[s.state] != bind_of(s.original_va)->ours) ++wrong;
            // The default is a legitimate handler for a state only if the binary assigns it there;
            // it does not, for any of the 44. A fill that ran the default loop LAST would pass C2.
            if (unit[s.state] == unit_default->ours) ++flattened;
        }
        ck_eq((uint32_t)wrong, 0u,
              "D1: every one of the 44 assigned unit states holds the handler the registrar puts "
              "there (the pairing, slot by slot)");
        ck_eq((uint32_t)flattened, 0u,
              "D2: no assigned unit state was flattened back to the default -- the default fill runs "
              "FIRST (0x0045f282-0x0045f2a6), the assignments after");

        wrong = flattened                   = 0;
        const handler_binding *bldg_default = bind_of(mh::addr::BLDG_STATE_DEFAULT_VA);
        for (int i = 0; i < mh::addr::BLDG_STATE_ASSIGN_COUNT; ++i) {
            const mh::addr::state_handler_slot &s = mh::addr::BLDG_STATE_ASSIGN[i];
            if (bldg[s.state] != bind_of(s.original_va)->ours) ++wrong;
            if (bldg[s.state] == bldg_default->ours) ++flattened;
        }
        ck_eq((uint32_t)wrong, 0u,
              "D3: every one of the 34 assigned bldg states holds its own handler");
        ck_eq((uint32_t)flattened, 0u,
              "D4: no assigned bldg state was flattened back to the default (0x0045f53c-0x0045f560)");
    }

    // ---- E: every UNassigned state holds the default ---------------------------------------------
    {
        int wrong = 0;
        for (int s = 0; s < UNIT_SLOTS; ++s)
            if (!state_is_assigned_unit(s) && unit[s] != bind_of(mh::addr::UNIT_STATE_DEFAULT_VA)->ours)
                ++wrong;
        ck_eq((uint32_t)wrong, 0u,
              "E1: the 211 unassigned unit states hold llm_strat_unit_state_default_noop -- an "
              "unassigned state is a real call into the default, not a hole");
        wrong = 0;
        for (int s = 0; s < BLDG_SLOTS; ++s)
            if (!state_is_assigned_bldg(s) && bldg[s] != bind_of(mh::addr::BLDG_STATE_DEFAULT_VA)->ours)
                ++wrong;
        ck_eq((uint32_t)wrong, 0u,
              "E2: the 221 unassigned bldg states hold llm_strat_bldg_state_default_reset");
    }

    // ---- F: the two tables are genuinely different -----------------------------------------------
    //
    // A fill that wrote the unit table twice (a copy-paste in fill_tables, the plausible bug) passes
    // A through E for the unit half and would leave the building half looking self-consistent too if
    // the checks above only compared each table against itself.
    {
        ck(unit[0x0f] != bldg[0x0f],
           "F1: the unit and bldg tables are not the same content -- state 0x0f differs");
        ck(!state_is_assigned_bldg(0x0f),
           "F2: (premise of F1) 0x0f is assigned in the unit table and NOT in the bldg one");
    }

    // ---- G: two pairings pinned BY NAME against the strategic-sim notes ----------------------------
    //
    // Everything above compares fill_tables against the generated header. If the extraction itself
    // were wrong, all of it would agree and all of it would be wrong together. These two are the
    // independent anchor: both are documented in prose, from before this generator existed.
    {
        ck_eq((uint32_t)(strcmp(name_of(unit[0x0f]), "llm_strat_unit_state_move_walker") == 0), 1u,
              "G1: unit state 0x0f is the walker move handler (the strategic-sim notes; 0x0045f2a8)");
        ck_eq((uint32_t)(strcmp(name_of(bldg[0x64]), "llm_strat_bldg_state_construction") == 0), 1u,
              "G2: bldg state 0x64 is the construction handler (the strategic-sim notes; 0x0045f562)");
        ck_eq((uint32_t)(strcmp(name_of(bldg[0x8b]), "llm_strat_bldg_state_power_generate") == 0), 1u,
              "G3: bldg state 0x8b is power_generate (the strategic-sim notes)");
    }

    // ---- H: the refusals. fill_tables writes NOTHING when it refuses -----------------------------
    //
    // The failure this guards is a region that is unrebased or short: a fill that got half way would
    // leave a LIVE dispatch table part ours and part stale .bss, which is strictly worse than not
    // installing at all. So each refusal is checked for its return value AND for the arrays being
    // untouched.
    {
        std::vector<unit_state_fn> u2(UNIT_SLOTS, sentinel_fn());
        std::vector<bldg_state_fn> b2(BLDG_SLOTS, sentinel_fn());

        ck(!detail::fill_tables(u2.data(), UNIT_SLOTS - 1, b2.data(), BLDG_SLOTS),
           "H1: a unit table one slot short is REFUSED");
        ck(u2[0] == sentinel_fn() && u2[UNIT_SLOTS - 2] == sentinel_fn(),
           "H2: ...and nothing was written before the refusal");

        ck(!detail::fill_tables(u2.data(), UNIT_SLOTS, b2.data(), BLDG_SLOTS - 1),
           "H3: a bldg table one slot short is REFUSED -- the size check covers BOTH tables");
        ck(u2[0] == sentinel_fn() && b2[0] == sentinel_fn(),
           "H4: ...and neither table was written (the unit half is checked first, so a fill that "
           "wrote it before validating the bldg size would fail here)");

        ck(!detail::fill_tables(nullptr, UNIT_SLOTS, b2.data(), BLDG_SLOTS),
           "H5: a null unit table is REFUSED rather than dereferenced");
        ck(!detail::fill_tables(u2.data(), UNIT_SLOTS, nullptr, BLDG_SLOTS),
           "H6: a null bldg table is REFUSED");
        ck(b2[0] == sentinel_fn(), "H7: ...and still nothing written");

        // A table LARGER than the binary's is fine -- the registrar fills what the binary fills and
        // does not run off the end of its own table.
        std::vector<unit_state_fn> u3(UNIT_SLOTS + 8, sentinel_fn());
        std::vector<bldg_state_fn> b3(BLDG_SLOTS + 8, sentinel_fn());
        ck(detail::fill_tables(u3.data(), UNIT_SLOTS + 8, b3.data(), BLDG_SLOTS + 8),
           "H8: an over-sized table is accepted");
        ck(u3[UNIT_SLOTS] == sentinel_fn() && b3[BLDG_SLOTS] == sentinel_fn(),
           "H9: ...and the slots past the binary's 255 are NOT written -- the setter's own bound is "
           "`id < 0xff` (0x0045f13f), so slot 255 is not part of the table");
    }

    // ---- I: is_our_handler is a membership test, not a range test --------------------------------
    {
        ck(is_our_handler(reinterpret_cast<const void *>(unit[0x0f])),
           "I1: a filled slot reads as ours");
        ck(!is_our_handler(reinterpret_cast<const void *>(sentinel_fn())),
           "I2: the sentinel does not");
        ck(!is_our_handler(nullptr), "I3: nullptr does not");
        ck(!is_our_handler(reinterpret_cast<const void *>(&run_register_state_handlers_tests)),
           "I4: an unrelated DLL-resident function does not -- so the first-dispatch note cannot be "
           "fooled by any of our own code that happens to sit near a thunk");
    }
}

} // namespace mh::sim::test
