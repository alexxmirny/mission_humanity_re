//
// sim_bldg_init_all_selftest.cpp -- `simtest` oracle for llm_strat_bldg_init_all @0x0045a017
// (sim/sim_bldg_init_all.h/.cpp), X-TL-DRAIN step 4.
//
// WHY AN OFFLINE ORACLE WHEN THE SHADOW SITE IS ARMABLE. Both, not either. The shadow site compares
// one region (`Building`, written by the callee) and is reachable exactly once per game boot, so it
// can say "the whole sweep agreed" and nothing about WHICH indices were visited or in what order.
// This file drives detail::bldg_init_all against a sim_fixture with a recording stub and observes the
// call SEQUENCE directly -- which is where all three of this function's decisions live (slot 0
// skipped, the 100 bound, the UNDEFINED gate).
//
// EVERY EXPECTED VALUE IS DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_init_all_0045a017.asm), re-cited inline per case, not from the C++ under
// test:
//     0x0045a02f  MOV dword ptr [EBP-0x18],0x1        i = 1   -- slot 0 never visited
//     0x0045a036  CMP dword ptr [EBP-0x18],0x64 / JL  i < 100 -- SIGNED, exclusive
//     0x0045a04a  CMP byte ptr [EAX + 0xd9ec88],0x0   Building[i].type == 0 (UNDEFINED)
//     0x0045a051  JZ                                  ... skip, no call
//     0x0045a053  MOVZX EAX,word ptr [EBP-0x18]       argument = the index, zero-extended from 16 bits
//
#include <vector>

#include "sim/sim_bldg_init_all.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorder ----------------------------------------------------------------------------------
// Captureless lambda -> plain function pointer, the same shape every other sim oracle in this tree
// uses for its stub table.
struct bia_recorder {
    std::vector<uint32_t> args; // one entry per llm_strat_bldg_init_defaults call, in order
    void                  reset() { args.clear(); }
};
bia_recorder g_bia;

const bldg_init_all_calls &rec_bia_calls() {
    static const bldg_init_all_calls c = {
        [](uint32_t building_id) { g_bia.args.push_back(building_id); },
    };
    return c;
}

// A fixture whose cfg_buildings are ALL undefined, so each case turns on exactly the types it sets.
// sim_fixture value-initialises its vectors, so `type` is already 0 everywhere; stated rather than
// assumed, because the whole gate under test is "type == 0".
void clear_types(sim_fixture &f) {
    for (auto &cb : f.cfg_buildings) cb.type = 0;
}

// ==== the UNDEFINED gate =========================================================================

void test_all_undefined_calls_nothing() {
    sim_fixture f;
    clear_types(f);
    g_bia.reset();

    sim_view v = f.view();
    detail::bldg_init_all(v, rec_bia_calls());

    ck(g_bia.args.empty(),
       "every Building[].type == UNDEFINED -> llm_strat_bldg_init_defaults is never called "
       "(0x0045a04a CMP byte,0x0 / 0x0045a051 JZ)");
}

void test_only_defined_types_are_seeded_and_in_index_order() {
    sim_fixture f;
    clear_types(f);
    // Deliberately non-contiguous and not in a run, so a translation that seeds a RANGE rather than
    // testing each entry cannot pass. Values are real cfg_enum_E_BUILDING members.
    f.cfg_buildings[1].type  = 1;  // A_PRODUCTION
    f.cfg_buildings[11].type = 11; // A_LAB
    f.cfg_buildings[25].type = 25; // H_TURRET
    f.cfg_buildings[99].type = 38; // H_CIVIL, at the LAST index the loop reaches
    g_bia.reset();

    sim_view v = f.view();
    detail::bldg_init_all(v, rec_bia_calls());

    ck(g_bia.args.size() == 4, "exactly one call per DEFINED type, none for the undefined ones");
    ck(g_bia.args.size() == 4 && g_bia.args[0] == 1u && g_bia.args[1] == 11u &&
           g_bia.args[2] == 25u && g_bia.args[3] == 99u,
       "the argument is the ARRAY INDEX in ascending order (0x0045a053 MOVZX EAX,word ptr "
       "[EBP-0x18]), not the type value and not the enumeration order");
}

// ==== the two loop bounds ========================================================================

void test_index_zero_is_never_visited() {
    sim_fixture f;
    clear_types(f);
    // Slot 0 DEFINED, and it is the only defined slot. A translation starting the loop at 0 would
    // call with 0; the original starts at 1 (0x0045a02f).
    f.cfg_buildings[0].type = 1;
    g_bia.reset();

    sim_view v = f.view();
    detail::bldg_init_all(v, rec_bia_calls());

    ck(g_bia.args.empty(),
       "Building[0] is DEFINED but slot 0 is never visited -- the loop starts at i = 1 "
       "(0x0045a02f MOV dword ptr [EBP-0x18],0x1)");
}

void test_upper_bound_is_exclusive_at_100() {
    sim_fixture f;
    clear_types(f);
    // Define EVERY index the array holds. The loop must produce exactly indices 1..99 -- 99 calls,
    // first 1, last 99. A `<=` bound would read cfg_buildings[100], one past the vector, and ASan
    // would take the run down rather than let it pass quietly.
    for (auto &cb : f.cfg_buildings) cb.type = 1;
    g_bia.reset();

    sim_view v = f.view();
    detail::bldg_init_all(v, rec_bia_calls());

    ck_eq((uint32_t)g_bia.args.size(), 99u,
          "all 100 cfg entries defined -> exactly 99 calls, indices 1..99 "
          "(0x0045a036 CMP ...,0x64 / JL -- exclusive, and slot 0 skipped)");
    ck(!g_bia.args.empty() && g_bia.args.front() == 1u && g_bia.args.back() == 99u,
       "... first call is index 1 and last is index 99");
}

// ==== the gate is on the byte, and the body writes nothing =======================================

void test_gate_reads_type_not_some_other_field() {
    sim_fixture f;
    clear_types(f);
    // `id`, `invention` and `ai_build` all precede `type` in the record (offsets 0/4/6 against
    // type's 8 -- mh_structs.gen.h). Setting them while leaving `type` at 0 must change nothing: a
    // translation probing the wrong offset would start seeding. This is the field-anchoring case
    // field-anchoring rule exists for: resolve the offset against the generated layout, never the index.
    f.cfg_buildings[5].id        = 0x7f7f7f7f;
    f.cfg_buildings[5].invention = 0x1234;
    f.cfg_buildings[5].ai_build  = 0x5678;
    g_bia.reset();

    sim_view v = f.view();
    detail::bldg_init_all(v, rec_bia_calls());

    ck(g_bia.args.empty(),
       "id / invention / ai_build set but type still 0 -> no call: the gate reads `type` at record "
       "offset 8 (Building base 0x00d9ec80, instruction disp32 0xd9ec88), not a neighbouring field");
}

void test_body_writes_nothing_of_its_own() {
    sim_fixture f;
    clear_types(f);
    f.cfg_buildings[7].type = 7; // A_BARRAKS
    // Sentinels across the regions a careless translation might touch. This function's own write set
    // is EMPTY (the state matrix attributes zero writes to it); all effect belongs to the callee,
    // which is stubbed out here.
    f.b(1, 3).building_id    = 0x4242;
    f.u(1, 3).unit_proto_id  = 0x2424;
    f.cfg_buildings[7].frame = 0x5a5a;
    g_bia.reset();

    sim_view v = f.view();
    detail::bldg_init_all(v, rec_bia_calls());

    ck_eq((uint32_t)g_bia.args.size(), 1u, "one defined type -> one call");
    ck_eq((uint32_t)f.b(1, 3).building_id, 0x4242u, "no write to `buildings`");
    ck_eq((uint32_t)f.u(1, 3).unit_proto_id, 0x2424u, "no write to `units`");
    ck_eq((uint32_t)f.cfg_buildings[7].frame, 0x5a5au,
          "no write to the cfg record either -- seeding it is the CALLEE's job, not this body's");
}

} // namespace

void run_bldg_init_all_tests() {
    test_all_undefined_calls_nothing();
    test_only_defined_types_are_seeded_and_in_index_order();

    test_index_zero_is_never_visited();
    test_upper_bound_is_exclusive_at_100();

    test_gate_reads_type_not_some_other_field();
    test_body_writes_nothing_of_its_own();
}

} // namespace mh::sim::test
