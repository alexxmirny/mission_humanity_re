#include "sim/sim_prod_shuttle_depart.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-default, non-symmetric so a swapped arg/field lands in a different record and is
// caught. sub_id and shuttle_slot are DELIBERATELY different values (and different from BIDX/PLAYER)
// -- the header banner is explicit that these are two different building-offset fields (0xc6 vs
// 0xc5) feeding two different array lookups (unit_storage vs prod_shuttle_slots), so a translation
// that swapped them must fail here.
constexpr uint16_t PLAYER          = 5;
constexpr int32_t  BIDX            = 8;
constexpr uint8_t  SUB_ID          = 6;
constexpr uint8_t  SHUTTLE_SLOT    = 3;
constexpr int32_t  NEIGHBOR_SLOT   = SHUTTLE_SLOT + 1; // adjacent record; must stay untouched
constexpr int32_t  VIEWED_PLANET   = 2;                // *v.planet_index for the "on-planet" tests
constexpr int32_t  DEST_OFF_PLANET = 9;                // != VIEWED_PLANET -> the manifest-clear branch

unit_storage &storage_of_f(sim_fixture &f, uint32_t player, int32_t sub_id) {
    return f.storage[player * STORAGE_PER_PLAYER + (uint32_t)sub_id];
}

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)slot];
}

// ---- recorder -------------------------------------------------------------------------------------
// Captureless lambdas convert to the plain function pointers prod_shuttle_depart_calls holds, same
// shape as every other sim/ oracle's g_rec (e.g. sim_prod_shuttle_unload_selftest.cpp's `recorder`).
struct ev3 {
    int32_t a, b, c;
};

struct dep_recorder {
    std::vector<ev3> load_into_cargo; // (player, building_index, unit_id) -- llm_strat_unit_load_into_shuttle_cargo
    std::vector<ev3> finalize;        // (player, building_index, dest_planet) -- llm_prod_bldg_depart_finalize
    // control knobs
    int32_t load_ret     = 0;
    int32_t finalize_ret = 0;
    // Set by test_depart_docked_count_cached_before_loop_not_rereadlive only: if non-null, every
    // load_into_cargo call stomps this record's docked_count, simulating a callee that (in a buggy
    // translation reading the field live) would shrink the loop bound mid-flight. A correct
    // translation ignores this entirely because docked_count was cached BEFORE the loop started.
    unit_storage *mutate_docked_count_target = nullptr;

    void reset() { *this = dep_recorder{}; }
};
dep_recorder g_dep;

const prod_shuttle_depart_calls &rec_dep_calls() {
    static const prod_shuttle_depart_calls c = {
        [](uint16_t player, int32_t building_idx, uint16_t unit_idx) -> int32_t {
            g_dep.load_into_cargo.push_back({(int32_t)player, building_idx, (int32_t)unit_idx});
            if (g_dep.mutate_docked_count_target != nullptr)
                g_dep.mutate_docked_count_target->docked_count = 0;
            return g_dep.load_ret;
        },
        [](uint16_t player, int32_t building_index, int32_t dest_planet) -> int32_t {
            g_dep.finalize.push_back({(int32_t)player, building_index, dest_planet});
            return g_dep.finalize_ret;
        },
    };
    return c;
}

// ---- STEP 1: the docked-unit detach loop (0x0048e189-0x0048e20d) ----------------------------------
// sub_id read once (building offset 0xc6), docked_count read once from
// unit_storage[player][sub_id].docked_count (offset 0x4) and CACHED before the loop; docked_units[0]
// (offset 0x8, a 32-bit field read as a 16-bit word) is reloaded fresh every iteration, always index
// 0. All three cases below use an ON-PLANET dest_planet (== *v.planet_index) so STEP 2's
// manifest-clear branch is skipped, isolating the loop's own behavior.

void test_depart_zero_docked_units_no_load_calls() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id                     = SUB_ID;
    f.planet_index                               = VIEWED_PLANET;
    storage_of_f(f, PLAYER, SUB_ID).docked_count = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, VIEWED_PLANET);

    ck_eq((uint32_t)g_dep.load_into_cargo.size(), 0u,
          "depart(docked_count=0): zero unit_load_into_shuttle_cargo calls");
    ck_eq((uint32_t)g_dep.finalize.size(), 1u,
          "depart(docked_count=0): finalize is still called exactly once -- STEP 3 is unconditional");
    ck_eq((uint32_t)r, 1u, "depart(docked_count=0): always returns 1");
}

void test_depart_loop_runs_docked_count_times_with_truncated_unit_id() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id = SUB_ID;
    f.planet_index           = VIEWED_PLANET;
    unit_storage &st         = storage_of_f(f, PLAYER, SUB_ID);
    st.docked_count          = 3;
    // 0x0048e1f5: MOVZX EBX, word ptr [...] -- a 16-bit load of docked_units[0]'s low half. Seed the
    // full 32-bit field with garbage above bit 15 so a translation that forgot the truncation and
    // passed the raw 32-bit value is caught.
    st.docked_units[0] = (int32_t)0x00170005; // low 16 bits == 5

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, VIEWED_PLANET);

    ck_eq((uint32_t)g_dep.load_into_cargo.size(), 3u,
          "depart(docked_count=3): three unit_load_into_shuttle_cargo calls");
    bool all_ok = true;
    for (const ev3 &e : g_dep.load_into_cargo)
        if (e.a != PLAYER || e.b != BIDX || e.c != 5) all_ok = false;
    ck(all_ok,
       "depart: every call is unit_load_into_shuttle_cargo(player=5, building_index=8, "
       "unit_id=truncated-low16(0x00170005)=5) -- always index 0, always the same truncated id since "
       "this stub never shifts the list");
}

void test_depart_docked_count_cached_before_loop_not_rereadlive() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id = SUB_ID;
    f.planet_index           = VIEWED_PLANET;
    unit_storage &st         = storage_of_f(f, PLAYER, SUB_ID);
    st.docked_count          = 3;
    st.docked_units[0]       = 11;
    // Every load_into_cargo call stomps docked_count to 0. Per the header banner, the loop condition
    // at 0x0048e1d0-0x0048e1d6 compares against the value CACHED in a stack local before the loop
    // starts, not a fresh memory read each pass -- so this stomp must have NO effect on the iteration
    // count. A translation that (incorrectly) re-read storage_of(...).docked_count live inside the
    // loop condition would stop after 1 iteration instead of 3.
    g_dep.mutate_docked_count_target = &st;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, VIEWED_PLANET);

    ck_eq((uint32_t)g_dep.load_into_cargo.size(), 3u,
          "depart: docked_count is CACHED before the loop -- a callee that stomps the live field to 0 "
          "does not shrink the iteration count below the original 3");
}

// ---- STEP 2: off-planet-only cargo-manifest claim-nibble clear (0x0048e242-0x0048e29d) ------------
// Gated on dest_planet != *v.planet_index. shuttle_slot (building offset 0xc5, DISTINCT from sub_id's
// 0xc6) is read fresh every iteration; for i in [0,50), cargo_manifest_raw[i*14+1] &= 0xf.

void test_depart_on_planet_dest_equals_viewed_skips_manifest_clear() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id       = SUB_ID;
    f.b(PLAYER, BIDX).shuttle_slot = SHUTTLE_SLOT;
    f.planet_index                 = VIEWED_PLANET;
    prod_shuttle_slot &s           = slot_of(f, PLAYER, SHUTTLE_SLOT);
    for (int32_t i = 0; i < 50; ++i) s.cargo_manifest_raw[(size_t)(i * 14 + 1)] = 0xff;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, VIEWED_PLANET); // dest == viewed

    bool untouched = true;
    for (int32_t i = 0; i < 50; ++i)
        if (s.cargo_manifest_raw[(size_t)(i * 14 + 1)] != 0xff) untouched = false;
    ck(untouched,
       "depart(dest_planet == *v.planet_index): STEP 2 does not run at all -- every manifest byte "
       "stays 0xff, none masked to 0xf");
}

void test_depart_off_planet_clears_upper_nibble_of_every_entrys_second_byte() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id       = SUB_ID;
    f.b(PLAYER, BIDX).shuttle_slot = SHUTTLE_SLOT;
    f.planet_index                 = VIEWED_PLANET;
    prod_shuttle_slot &s           = slot_of(f, PLAYER, SHUTTLE_SLOT);
    for (int32_t i = 0; i < 50; ++i) {
        s.cargo_manifest_raw[(size_t)(i * 14 + 0)] = 0xaa; // a DIFFERENT byte of the same entry
        s.cargo_manifest_raw[(size_t)(i * 14 + 1)] = 0xa5; // the targeted byte: hi nibble 0xa, lo 0x5
    }

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, DEST_OFF_PLANET); // off-planet

    bool masked_ok = true, byte0_untouched = true;
    for (int32_t i = 0; i < 50; ++i) {
        if (s.cargo_manifest_raw[(size_t)(i * 14 + 1)] != 0x5) masked_ok = false;
        if (s.cargo_manifest_raw[(size_t)(i * 14 + 0)] != 0xaa) byte0_untouched = false;
    }
    ck(masked_ok,
       "depart(off-planet): all 50 entries' second byte (i*14+1) masked to (value & 0xf) == 0x5, "
       "including the boundary entries 0 and 49");
    ck(byte0_untouched,
       "depart(off-planet): the entry's FIRST byte (i*14+0) is a different field entirely and is left "
       "untouched by the AND-mask");
}

void test_depart_off_planet_uses_shuttle_slot_field_not_sub_id_and_spares_neighbor() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id       = SUB_ID;       // 6
    f.b(PLAYER, BIDX).shuttle_slot = SHUTTLE_SLOT; // 3 -- distinct from sub_id
    f.planet_index                 = VIEWED_PLANET;
    // Seed the TARGET slot (shuttle_slot=3), a slot indexed by SUB_ID instead (6, must stay
    // untouched -- proves the manifest clear reads shuttle_slot, not sub_id), and the numerically
    // adjacent slot (4, must also stay untouched -- proves it targets exactly shuttle_slot).
    slot_of(f, PLAYER, SHUTTLE_SLOT).cargo_manifest_raw[1]  = 0xf3;
    slot_of(f, PLAYER, SUB_ID).cargo_manifest_raw[1]        = 0xf7;
    slot_of(f, PLAYER, NEIGHBOR_SLOT).cargo_manifest_raw[1] = 0xf9;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, DEST_OFF_PLANET);

    ck_eq((uint32_t)slot_of(f, PLAYER, SHUTTLE_SLOT).cargo_manifest_raw[1], 0x3u,
          "depart(off-planet): the shuttle_slot(3) record's entry-0 byte masked 0xf3 -> 0x3");
    ck_eq((uint32_t)slot_of(f, PLAYER, SUB_ID).cargo_manifest_raw[1], 0xf7u,
          "depart(off-planet): the sub_id(6)-indexed slot record is UNTOUCHED -- proves the clear "
          "reads building.shuttle_slot(0xc5), not building.sub_id(0xc6)");
    ck_eq((uint32_t)slot_of(f, PLAYER, NEIGHBOR_SLOT).cargo_manifest_raw[1], 0xf9u,
          "depart(off-planet): the numerically adjacent slot(4) record is UNTOUCHED -- proves the "
          "clear targets exactly shuttle_slot, not a range");
}

// ---- STEP 3: delegate, then always return 1 (0x0048e29f-0x0048e2bf) -------------------------------

void test_depart_delegates_to_finalize_with_exact_args_and_ignores_its_return() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id       = SUB_ID;
    f.b(PLAYER, BIDX).shuttle_slot = SHUTTLE_SLOT;
    f.planet_index                 = VIEWED_PLANET;
    g_dep.finalize_ret             = 42; // nonzero, non-1 -- must not leak into the function's own return

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, DEST_OFF_PLANET);

    ck_eq((uint32_t)g_dep.finalize.size(), 1u,
          "depart: llm_prod_bldg_depart_finalize called exactly once");
    ck(g_dep.finalize.size() == 1 && g_dep.finalize[0].a == PLAYER && g_dep.finalize[0].b == BIDX &&
           g_dep.finalize[0].c == DEST_OFF_PLANET,
       "depart: finalize(player=5, building_index=8, dest_planet=9) -- exact args, matching the "
       "callee's own committed signature");
    ck_eq((uint32_t)r, 1u,
          "depart: return is the LITERAL 1 from 0x0048e2ae -- finalize's return value (42) is "
          "discarded, not propagated");
}

// ---- the two re-derived dead computations: prove they are NOT externally observable ---------------
// The header banner documents the accumulator at [EBP-0x14] (written every loop iteration, never
// read) and the discarded CMP at 0x0048e239-0x0048e23c (accumulator vs. passengers_reserved,
// unconditionally clobbered before any Jcc reads it) as verified-dead: no write to tracked state, no
// effect on control flow. This case drives both toward extreme/adversarial values and confirms the
// function's observable behavior (call count, manifest write, return value) is unaffected -- if
// either were secretly live, this is the case that would catch it.
void test_depart_dead_accumulator_and_discarded_cmp_have_no_observable_effect() {
    sim_fixture f;
    g_dep.reset();
    f.b(PLAYER, BIDX).sub_id       = SUB_ID;
    f.b(PLAYER, BIDX).shuttle_slot = SHUTTLE_SLOT;
    f.planet_index                 = VIEWED_PLANET;
    unit_storage &st               = storage_of_f(f, PLAYER, SUB_ID);
    st.docked_count                = 5;
    st.docked_units[0]             = 77;
    // passengers_reserved is the field the discarded CMP compares the (dead) accumulator against
    // (struct offset 0x58, cross-checked in the header banner). Set it to an extreme value distinct
    // from anything the accumulator could plausibly sum to.
    slot_of(f, PLAYER, SHUTTLE_SLOT).passengers_reserved = 0x7fffffff;
    for (int32_t i = 0; i < 50; ++i)
        slot_of(f, PLAYER, SHUTTLE_SLOT).cargo_manifest_raw[(size_t)(i * 14 + 1)] = 0xff;
    // Each call's return value is what the real accumulator (ADD dword ptr [EBP-0x14],EAX) would sum
    // -- large and distinct per call, so if the accumulator's value secretly reached anything
    // downstream, a huge/varying sum would be the most likely thing to disturb it.
    g_dep.load_ret = 1000003;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_shuttle_depart(v, own, rec_dep_calls(), PLAYER, BIDX, DEST_OFF_PLANET);

    ck_eq((uint32_t)g_dep.load_into_cargo.size(), 5u,
          "depart(dead-computation stress): loop still runs the full cached docked_count(5) times");
    bool masked_ok = true;
    for (int32_t i = 0; i < 50; ++i)
        if (slot_of(f, PLAYER, SHUTTLE_SLOT).cargo_manifest_raw[(size_t)(i * 14 + 1)] != 0xf)
            masked_ok = false;
    ck(masked_ok,
       "depart(dead-computation stress): the manifest clear still runs normally regardless of "
       "passengers_reserved's value or the load calls' return values");
    ck_eq((uint32_t)g_dep.finalize.size(), 1u,
          "depart(dead-computation stress): finalize still called exactly once");
    ck_eq((uint32_t)r, 1u, "depart(dead-computation stress): still returns 1 unconditionally");
}

} // namespace

void run_prod_shuttle_depart_tests() {
    test_depart_zero_docked_units_no_load_calls();
    test_depart_loop_runs_docked_count_times_with_truncated_unit_id();
    test_depart_docked_count_cached_before_loop_not_rereadlive();

    test_depart_on_planet_dest_equals_viewed_skips_manifest_clear();
    test_depart_off_planet_clears_upper_nibble_of_every_entrys_second_byte();
    test_depart_off_planet_uses_shuttle_slot_field_not_sub_id_and_spares_neighbor();

    test_depart_delegates_to_finalize_with_exact_args_and_ignores_its_return();

    test_depart_dead_accumulator_and_discarded_cmp_have_no_observable_effect();
}

} // namespace mh::sim::test
