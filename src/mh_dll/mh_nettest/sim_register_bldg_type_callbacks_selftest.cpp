//
// sim_register_bldg_type_callbacks_selftest.cpp -- `simtest` oracle for OUR registrar
// (sim/sim_register_bldg_type_callbacks.h/.cpp, RI-SIM / SIM1-BLDGCB, 2026-08-23).
//
// WHAT THIS ORACLE IS FOR, AND WHY THE LIVE RUN IS NOT ENOUGH. Same argument as the state-table
// registrar's own oracle, one notch sharper. The golden A/B says whether the game still behaves the
// same with our 30 callbacks dispatched for real. It cannot say WHICH building got which callback:
// give the port kind the shuttle animation handler and the trajectory diverges -- but the
// divergence names a step and a region, not the swapped pair, and a scenario in which neither kind
// is ever built reports MATCHED with the swap still in place. The pairing is a table, and a table is
// what an offline check can read entry by entry.
//
// THE SHARPER PART: THIS REGISTRAR DOES NOT INDEX BY TYPE, IT SCANS BY IT. The original setters
// (llm_bldg_register_done_callback @0x0045f1ae, _tick2_ @0x0045f20c) loop over building ids 1..99
// and write the callback into every slot whose `Building[id].type` equals the type argument. So the
// property under test is not "slot N holds callback X" -- it is "the scan mapped a cfg table onto
// two tables the way the original's would". That needs a cfg table to scan, which is exactly what a
// live golden run cannot vary and this file can: every case below drives detail::
// fill_bldg_callback_tables over LOCAL arrays with a SYNTHETIC cfg. No game memory is touched here.
//
// FOUR MISTAKES THIS IS WRITTEN TO CATCH, each of which survives a green golden run:
//   * the pairing swapped or shifted           -> case D (and the by-name anchors, case J)
//   * the default fill run LAST, flattening the assignments -> case D2/D5
//   * the scan stopping at the first match, so only ONE of the several buildings sharing a type
//     gets the callback                        -> case K, which is the failure mode unique to this
//     registrar and has no counterpart in the state tables
//   * "helpfully" filling the ids whose type is outside the default range, which the original
//     leaves ALONE                             -> case F
//
// EXPECTED CONTENT comes from addr/mh_bldg_type_callbacks.gen.h, extracted by
// tools/gen_state_handler_table.py --extract-bldg-type from llm_strat_register_bldg_type_callbacks
// @0x0045f76a. This file deliberately re-derives NOTHING from it: it asserts that the fill
// reproduces the generated table, and separately (case J) pins four pairings by NAME against the
// registrar's own listing, so that a wholesale corruption of the generated table is still caught.
//
#include "sim/sim_register_bldg_type_callbacks.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "addr/mh_bldg_type_callbacks.gen.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int ID_LIMIT = mh::addr::BLDG_TYPE_ID_LIMIT;
constexpr int FIRST_ID = mh::addr::BLDG_TYPE_FIRST_ID;

// A sentinel no thunk can ever equal, so "untouched" is distinguishable from "filled".
bldg_done_fn sentinel_fn() {
    return reinterpret_cast<bldg_done_fn>(static_cast<uintptr_t>(0xdeadbeefu));
}

const bldg_type_callback_binding *bind_of(uintptr_t va) { return detail::bldg_type_binding_for(va); }

// The name of whichever binding owns `fn`, or "<none>" -- so a failure message says WHICH callback
// landed in the slot rather than only that the pointer differed.
const char *name_of(bldg_done_fn fn) {
    const bldg_type_callback_binding *b = detail::bldg_type_callback_bindings();
    for (int i = 0; i < detail::bldg_type_callback_binding_count(); ++i)
        if (b[i].ours == fn) return b[i].name;
    return "<none>";
}

bool done_type_is_assigned(int ty) {
    for (int i = 0; i < mh::addr::BLDG_DONE_ASSIGN_COUNT; ++i)
        if (mh::addr::BLDG_DONE_ASSIGN[i].type == ty) return true;
    return false;
}
bool tick2_type_is_assigned(int ty) {
    for (int i = 0; i < mh::addr::BLDG_TICK2_ASSIGN_COUNT; ++i)
        if (mh::addr::BLDG_TICK2_ASSIGN[i].type == ty) return true;
    return false;
}

// The scan's INPUT. `cfg_building` is 0x842 bytes, so the 100 records live on the heap (a stack
// array would be 211 KB); `sim_fixture` already owns one for the same reason and is reused rather
// than allocating a second.
//
// THE TYPE LAYOUT USED BY EVERY CASE BELOW, chosen so each property has a witness:
//   id 0                 -- type 0x0c, and the id loop starts at 1, so it must stay UNTOUCHED
//   ids 1..0x27          -- type == id: one building per type across the whole default-fill range,
//                           which makes every ASSIGN row reachable and every unassigned type too
//   ids 0x28..0x2b       -- types 0, 0x28, 0x29, 0xff: all OUTSIDE the default range, so the
//                           original never writes them (case F)
//   ids 0x2c, 0x2d       -- type 0x0c again, i.e. THREE ids share one type (0x0c, 0x2c, 0x2d).
//                           A scan that stops at the first match fills 0x0c and leaves these two
//                           at the sentinel (case K)
//   ids 0x2e..0x63       -- type 0x01, harmless filler that also proves the loop runs to the end
void seed_cfg(sim_fixture &fx) {
    for (int id = 0; id < ID_LIMIT; ++id) fx.cfg_buildings[id].type = 0x01;
    fx.cfg_buildings[0].type = 0x0c;
    for (int id = 1; id <= 0x27; ++id) fx.cfg_buildings[id].type = (uint8_t)id;
    fx.cfg_buildings[0x28].type = 0x00;
    fx.cfg_buildings[0x29].type = 0x28;
    fx.cfg_buildings[0x2a].type = 0x29;
    fx.cfg_buildings[0x2b].type = 0xff;
    fx.cfg_buildings[0x2c].type = 0x0c;
    fx.cfg_buildings[0x2d].type = 0x0c;
}

// Assemble the binder struct over local tables + the fixture's cfg. `done_slots`/`tick2_slots`/
// `cfg_count` are passed explicitly so the refusal cases can shrink one of them.
sim_bldg_callback_tables tables_over(sim_fixture &fx, std::vector<bldg_done_fn> &done,
                                     std::vector<bldg_tick2_fn> &tick2, int32_t done_slots,
                                     int32_t tick2_slots, int32_t cfg_count) {
    sim_bldg_callback_tables t{};
    t.done        = done.empty() ? nullptr : done.data();
    t.tick2       = tick2.empty() ? nullptr : tick2.data();
    t.cfg         = fx.cfg_buildings.data();
    t.done_slots  = done_slots;
    t.tick2_slots = tick2_slots;
    t.cfg_count   = cfg_count;
    return t;
}

} // namespace

void run_register_bldg_type_callbacks_tests() {
    printf("-- llm_strat_register_bldg_type_callbacks (the two per-building-TYPE tables) --\n");

    sim_fixture fx;
    fx.reset();
    seed_cfg(fx);

    // ---- A: the binding table itself (30 rows, one per distinct callback in the binary) ---------
    {
        const bldg_type_callback_binding *b = detail::bldg_type_callback_bindings();
        const int                         n = detail::bldg_type_callback_binding_count();
        ck_eq((uint32_t)n, (uint32_t)mh::addr::BLDG_TYPE_CALLBACK_DISTINCT_COUNT,
              "A1: one binding per distinct callback the registrar installs, 0x0045f76a");
        int nulls = 0, zero_va = 0, unnamed = 0;
        for (int i = 0; i < n; ++i) {
            if (b[i].ours == nullptr) ++nulls;
            if (b[i].original_va == 0) ++zero_va;
            if (b[i].name == nullptr || b[i].name[0] == '\0') ++unnamed;
        }
        ck_eq((uint32_t)nulls, 0u,
              "A2: every binding has an entry thunk (a null slot would be a CALL to 0 the first "
              "time a building of that kind ticks)");
        ck_eq((uint32_t)zero_va, 0u, "A3: every binding carries its original VA (mh::exp::addr_<fn>)");
        ck_eq((uint32_t)unnamed, 0u, "A4: every binding carries the original's symbol name");

        // Distinctness both ways. Two rows sharing a VA means the X-macro emitted a callback twice;
        // two rows sharing a THUNK means two originals were bound to one C++ wrapper -- the
        // copy-paste failure this file exists to catch, invisible in a live run.
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
        ck(bind_of(mh::addr::BLDG_DONE_DEFAULT_VA) != nullptr,
           "B1: the done default (llm_strat_done_default @0x0046ff2c) has a binding");
        ck(bind_of(mh::addr::BLDG_TICK2_DEFAULT_VA) != nullptr,
           "B2: the tick2 default (llm_strat_bldg_anim_tick @0x00476447) has a binding");
        int missing = 0;
        for (int i = 0; i < mh::addr::BLDG_DONE_ASSIGN_COUNT; ++i)
            if (bind_of(mh::addr::BLDG_DONE_ASSIGN[i].original_va) == nullptr) ++missing;
        for (int i = 0; i < mh::addr::BLDG_TICK2_ASSIGN_COUNT; ++i)
            if (bind_of(mh::addr::BLDG_TICK2_ASSIGN[i].original_va) == nullptr) ++missing;
        ck_eq((uint32_t)missing, 0u,
              "B3: every assigned type's callback VA resolves to a binding (50 assignments)");
        ck(bind_of(0x00400000u) == nullptr,
           "B4: an address that is NOT a bldg-type callback does not resolve -- "
           "bldg_type_binding_for is a lookup, not a fallback");
    }

    // ---- C: the fill, over local arrays and the synthetic cfg -------------------------------------
    std::vector<bldg_done_fn>  done(ID_LIMIT, sentinel_fn());
    std::vector<bldg_tick2_fn> tick2(ID_LIMIT, (bldg_tick2_fn)sentinel_fn());
    int                        filled_done = -1, filled_tick2 = -1;
    {
        sim_bldg_callback_tables t = tables_over(fx, done, tick2, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        ck(detail::fill_bldg_callback_tables(t, &filled_done, &filled_tick2),
           "C1: fill_bldg_callback_tables accepts two correctly-sized tables + a full cfg table");

        // The cfg seeded above puts 0x27 types in range at ids 1..0x27, plus ids 0x2c/0x2d (type
        // 0x0c) and ids 0x2e..0x63 (type 0x01) -- 39 + 2 + 54 = 95 ids. The four
        // out-of-range ids (0x28..0x2b) and id 0 are NOT among them. Spelled out rather than
        // recomputed from the same loop the fill uses, so a fill that scanned the wrong id range
        // cannot agree with its own arithmetic here.
        ck_eq((uint32_t)filled_done, 95u,
              "C2: the done table now holds OUR callback at exactly the 95 ids whose cfg type is in "
              "0x01..0x27 -- a CFG-DEPENDENT count, which is why the registrar logs one");
        ck_eq((uint32_t)filled_tick2, 95u, "C3: ...and the tick2 table at the same 95");
    }

    // ---- D: each id holds the callback ITS type is paired with, and not the default ---------------
    {
        int wrong = 0, flattened = 0;
        for (int i = 0; i < mh::addr::BLDG_DONE_ASSIGN_COUNT; ++i) {
            const mh::addr::bldg_type_callback_slot &a  = mh::addr::BLDG_DONE_ASSIGN[i];
            const int                                id = a.type; // the seed puts type==id for 1..0x27
            if (done[id] != bind_of(a.original_va)->ours) ++wrong;
            // The default is a legitimate callback for a type only if the binary assigns it there;
            // it does not, for any of the 30 done assignments. A fill that ran the default loop
            // LAST would pass C2 and fail here.
            if (done[id] == bind_of(mh::addr::BLDG_DONE_DEFAULT_VA)->ours) ++flattened;
        }
        ck_eq((uint32_t)wrong, 0u,
              "D1: every one of the 30 assigned done types holds the callback the registrar pairs "
              "with it (the pairing, type by type)");
        ck_eq((uint32_t)flattened, 0u,
              "D2: no assigned done type was flattened back to llm_strat_done_default -- the "
              "default fill runs FIRST (0x0045f782-0x0045f7a3), the assignments after");

        wrong = flattened = 0;
        for (int i = 0; i < mh::addr::BLDG_TICK2_ASSIGN_COUNT; ++i) {
            const mh::addr::bldg_type_callback_slot &a  = mh::addr::BLDG_TICK2_ASSIGN[i];
            const int                                id = a.type;
            if ((bldg_done_fn)tick2[id] != bind_of(a.original_va)->ours) ++wrong;
            if ((bldg_done_fn)tick2[id] == bind_of(mh::addr::BLDG_TICK2_DEFAULT_VA)->ours)
                ++flattened;
        }
        ck_eq((uint32_t)wrong, 0u,
              "D4: every one of the 20 assigned tick2 types holds its own callback");
        ck_eq((uint32_t)flattened, 0u,
              "D5: no assigned tick2 type was flattened back to llm_strat_bldg_anim_tick "
              "(0x0045f967-0x0045f988)");
    }

    // ---- E: every UNassigned in-range type holds the default -------------------------------------
    {
        int wrong_done = 0, wrong_tick2 = 0, seen_done = 0, seen_tick2 = 0;
        for (int ty = mh::addr::BLDG_DONE_DEFAULT_TYPE_LO; ty <= mh::addr::BLDG_DONE_DEFAULT_TYPE_HI;
             ++ty) {
            if (done_type_is_assigned(ty)) continue;
            ++seen_done;
            if (done[ty] != bind_of(mh::addr::BLDG_DONE_DEFAULT_VA)->ours) ++wrong_done;
        }
        for (int ty = mh::addr::BLDG_TICK2_DEFAULT_TYPE_LO;
             ty <= mh::addr::BLDG_TICK2_DEFAULT_TYPE_HI; ++ty) {
            if (tick2_type_is_assigned(ty)) continue;
            ++seen_tick2;
            if ((bldg_done_fn)tick2[ty] != bind_of(mh::addr::BLDG_TICK2_DEFAULT_VA)->ours)
                ++wrong_tick2;
        }
        // Non-vacuity for E itself: if the ASSIGN tables ever covered the whole range, the two loops
        // above would examine nothing and report clean.
        ck(seen_done > 0 && seen_tick2 > 0,
           "E1: (premise) both tables have types the registrar leaves on the default");
        ck_eq((uint32_t)wrong_done, 0u,
              "E2: an in-range done type with no assignment holds llm_strat_done_default -- it is a "
              "real call into the default, not a hole");
        ck_eq((uint32_t)wrong_tick2, 0u,
              "E3: an in-range tick2 type with no assignment holds llm_strat_bldg_anim_tick");
    }

    // ---- F: an id whose type is OUTSIDE the default range is LEFT ALONE ---------------------------
    //
    // This is the behaviour that differs from the state registrar, and the one a "robust"
    // reimplementation would quietly break by filling every slot. The original's default fill loops
    // over TYPES 1..0x27 (0x0045f789 CMP ...,0x28), so a building whose cfg type is 0 or >= 0x28 is
    // never named by any setter call and its slot keeps whatever was there.
    {
        ck(done[0x28] == sentinel_fn() && (bldg_done_fn)tick2[0x28] == sentinel_fn(),
           "F1: id 0x28 (cfg type 0x00, below the default range) is UNTOUCHED in both tables");
        ck(done[0x29] == sentinel_fn() && (bldg_done_fn)tick2[0x29] == sentinel_fn(),
           "F2: id 0x29 (cfg type 0x28, one past the range's 0x27) is UNTOUCHED");
        ck(done[0x2a] == sentinel_fn() && done[0x2b] == sentinel_fn(),
           "F3: ids 0x2a/0x2b (cfg types 0x29 and 0xff) are UNTOUCHED -- the type byte is MOVZX'd, "
           "so 0xff is 255 and out of range, never -1 and in it (0x0045f1e6)");
    }

    // ---- G: id 0 is never written -- the setters' id loop starts at 1 ----------------------------
    {
        ck_eq((uint32_t)FIRST_ID, 1u,
              "G1: (premise) the extracted id loop starts at 1 (0x0045f1cb MOV [EBP-0x14],0x1)");
        ck(done[0] == sentinel_fn() && (bldg_done_fn)tick2[0] == sentinel_fn(),
           "G2: id 0 is UNTOUCHED even though its cfg type (0x0c) is assigned -- slot 0 is outside "
           "the loop, not merely unmatched");
    }

    // ---- H: the two tables are genuinely different ------------------------------------------------
    //
    // A fill that wrote the done table twice (a copy-paste in fill_bldg_callback_tables, the
    // plausible bug) passes A through G for the done half and would leave the tick2 half looking
    // self-consistent if every check only compared a table against itself.
    {
        ck((bldg_done_fn)tick2[0x26] != done[0x26],
           "H1: at type 0x26 the two tables differ -- tick2 has an assignment there and done does "
           "not");
        ck(tick2_type_is_assigned(0x26) && !done_type_is_assigned(0x26),
           "H2: (premise of H1) 0x26 is assigned in the tick2 table (online_toggle, 0x0045fa57) and "
           "NOT in the done one");
    }

    // ---- I: THE SCAN IS A LOOP, NOT A LOOKUP -- every id sharing a type gets the callback ---------
    //
    // The failure mode unique to this registrar and with no counterpart in the state tables: an
    // implementation that resolved "the id for this type" instead of scanning would fill id 0x0c and
    // leave 0x2c/0x2d on the sentinel. Every check above would still pass, because every check above
    // looks at the id that happens to equal its type.
    {
        const bldg_done_fn port_done = bind_of(0x00470198u)->ours; // llm_strat_done_port
        ck(done[0x0c] == port_done && done[0x2c] == port_done && done[0x2d] == port_done,
           "I1: ALL THREE ids with cfg type 0x0c hold llm_strat_done_port -- the setter loops over "
           "every id (0x0045f1d2-0x0045f201), it does not resolve one");
        const bldg_done_fn port_tick2 = bind_of(0x004786f5u)->ours; // llm_strat_bldg_anim_state_port
        ck((bldg_done_fn)tick2[0x0c] == port_tick2 && (bldg_done_fn)tick2[0x2c] == port_tick2 &&
               (bldg_done_fn)tick2[0x2d] == port_tick2,
           "I2: ...and the same three in the tick2 table hold llm_strat_bldg_anim_state_port");
        // The default fill has to be a loop for the same reason. ids 0x2e..0x63 all carry type 0x01,
        // which IS assigned in done (done_production) and is NOT in tick2 -- so the tick2 side of
        // this is a default-fill-loop witness and the done side an assignment-loop one.
        int dflt_wrong = 0;
        for (int id = 0x2e; id < ID_LIMIT; ++id)
            if ((bldg_done_fn)tick2[id] != bind_of(mh::addr::BLDG_TICK2_DEFAULT_VA)->ours)
                ++dflt_wrong;
        ck_eq((uint32_t)dflt_wrong, 0u,
              "I3: the 54 ids with cfg type 0x01 all hold the tick2 DEFAULT -- the default fill "
              "scans every id too, not just the first of each type");
    }

    // ---- J: four pairings pinned BY NAME against the registrar's own listing ----------------------
    //
    // Everything above compares the fill against the generated header. If the extraction itself were
    // wrong, all of it would agree and all of it would be wrong together. These four are the
    // independent anchor, each read off llm_strat_register_bldg_type_callbacks' disassembly.
    {
        ck_eq((uint32_t)(strcmp(name_of(done[0x0c]), "llm_strat_done_port") == 0), 1u,
              "J1: building type 0x0c gets llm_strat_done_port (MOV EDX,0x470198 / MOV EAX,0xc / "
              "CALL 0x0045f1ae at 0x0045f7af)");
        ck_eq((uint32_t)(strcmp(name_of(done[0x15]), "llm_strat_done_production") == 0), 1u,
              "J2: type 0x15 -- the second race's production kind -- gets llm_strat_done_production "
              "(0x0045f8ea), the same callback as type 0x01");
        ck_eq((uint32_t)(strcmp(name_of((bldg_done_fn)tick2[0x05]),
                                "llm_strat_bldg_anim_state_turret") == 0),
              1u,
              "J3: type 0x05 gets llm_strat_bldg_anim_state_turret in the tick2 table (0x0045faa2) "
              "-- the callback that was FUN_00476605 when SIM1-BLDGCB opened");
        ck_eq((uint32_t)(strcmp(name_of((bldg_done_fn)tick2[0x12]),
                                "llm_strat_bldg_anim_state_online_toggle") == 0),
              1u,
              "J4: type 0x12 gets llm_strat_bldg_anim_state_online_toggle (0x0045fa48) -- one of "
              "the six types that share it");
    }

    // ---- K: the refusals. the fill writes NOTHING when it refuses --------------------------------
    //
    // The failure this guards is a region that is unrebased or short: a fill that got half way would
    // leave a LIVE dispatch table part ours and part stale .bss, strictly worse than not installing.
    // So each refusal is checked for its return value AND for the arrays being untouched.
    {
        std::vector<bldg_done_fn>  d2(ID_LIMIT, sentinel_fn());
        std::vector<bldg_tick2_fn> t2(ID_LIMIT, (bldg_tick2_fn)sentinel_fn());
        int                        fd = -1, ft = -1;

        sim_bldg_callback_tables t = tables_over(fx, d2, t2, ID_LIMIT - 1, ID_LIMIT, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K1: a done table one slot short is REFUSED");
        ck(d2[1] == sentinel_fn() && d2[ID_LIMIT - 2] == sentinel_fn(),
           "K2: ...and nothing was written before the refusal");
        ck(fd == 0 && ft == 0, "K3: ...and the reported fill counts are 0, not left stale");

        t = tables_over(fx, d2, t2, ID_LIMIT, ID_LIMIT - 1, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K4: a tick2 table one slot short is REFUSED -- the size check covers BOTH tables");
        ck(d2[1] == sentinel_fn() && (bldg_done_fn)t2[1] == sentinel_fn(),
           "K5: ...and NEITHER table was written (the done half is filled first, so a fill that "
           "wrote it before validating the tick2 size would fail here)");

        // The cfg table is an input, and a short one is the same class of failure: the original's
        // loop reads Building[1..99] unconditionally, so a 50-record cfg means reading past it.
        t = tables_over(fx, d2, t2, ID_LIMIT, ID_LIMIT, ID_LIMIT - 1);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K6: a cfg table shorter than the id loop is REFUSED -- it is scanned, so it is sized "
           "too");
        ck(d2[1] == sentinel_fn(), "K7: ...and nothing was written");

        std::vector<bldg_done_fn>  none_d;
        std::vector<bldg_tick2_fn> none_t;
        t = tables_over(fx, none_d, t2, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K8: a null done table is REFUSED rather than dereferenced");
        t = tables_over(fx, d2, none_t, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft), "K9: a null tick2 table is REFUSED");
        t     = tables_over(fx, d2, t2, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        t.cfg = nullptr;
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft), "K10: a null cfg table is REFUSED");
        ck(d2[1] == sentinel_fn() && (bldg_done_fn)t2[1] == sentinel_fn(),
           "K11: ...and still nothing written by any of the three");

        // Tables LARGER than the binary's are fine -- the registrar fills what the binary fills and
        // does not run off the end of its own table.
        std::vector<bldg_done_fn>  d3(ID_LIMIT + 8, sentinel_fn());
        std::vector<bldg_tick2_fn> t3(ID_LIMIT + 8, (bldg_tick2_fn)sentinel_fn());
        t = tables_over(fx, d3, t3, ID_LIMIT + 8, ID_LIMIT + 8, ID_LIMIT);
        ck(detail::fill_bldg_callback_tables(t, &fd, &ft), "K12: an over-sized table is accepted");
        ck(d3[ID_LIMIT] == sentinel_fn() && (bldg_done_fn)t3[ID_LIMIT] == sentinel_fn(),
           "K13: ...and the slots past the binary's 100 are NOT written -- the setter's own bound "
           "is `id < 0x64` (0x0045f1d2)");
    }

    // ---- L: is_our_bldg_type_callback is a membership test, not a range test ----------------------
    {
        ck(is_our_bldg_type_callback(reinterpret_cast<const void *>(done[0x0c])),
           "L1: a filled slot reads as ours");
        ck(!is_our_bldg_type_callback(reinterpret_cast<const void *>(sentinel_fn())),
           "L2: the sentinel does not");
        ck(!is_our_bldg_type_callback(nullptr), "L3: nullptr does not");
        ck(!is_our_bldg_type_callback(
               reinterpret_cast<const void *>(&run_register_bldg_type_callbacks_tests)),
           "L4: an unrelated DLL function does not -- membership against the 30, not an address "
           "range, so nearby DLL code cannot be mistaken for a callback of ours");
    }

    // ---- M: the cfg is READ, never written -------------------------------------------------------
    //
    // `sim_bldg_callback_tables::cfg` is a const pointer, so this cannot fail by a direct store; it
    // can by a const_cast, which is exactly the sort of thing a later edit adds "to normalise" a
    // type byte. Cheap to pin, and it states the direction of the dependency.
    {
        int changed = 0;
        for (int id = 1; id <= 0x27; ++id)
            if (fx.cfg_buildings[id].type != (uint8_t)id) ++changed;
        ck_eq((uint32_t)changed, 0u, "M1: the fill left the cfg type bytes untouched -- cfg is the "
                                     "scan's INPUT, never its output");
    }
}

} // namespace mh::sim::test
