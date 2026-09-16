//
// sim_prod_shuttle_slot_bind_default_selftest.cpp -- `simtest` oracle for
// llm_prod_shuttle_slot_bind_default @0x0048dea7 (sim/sim_prod_shuttle_slot_bind_default.h/.cpp,
// RI-SIM / SIM1D, batch D).
//
// WHY THIS FILE IS THE ONLY EVIDENCE THIS FUNCTION WILL EVER HAVE (per the ledger, 2026-08-14
// reimpl-verify): the row made ZERO calls across 15000- AND 30000-step all-AI soaks -- three of its
// four trigger paths (order-dispatch cases 0x12/0x13/0x15) are SESSION_SP-only and unreachable from
// any skirmish/soak rig, and the fourth (case 0xf) needs an AI to actually operate an
// A_SHUTTLE/H_SHUTTLE building. There is no shadow-arm evidence coming for this site; this offline
// oracle is the primary proof, not a supplement.
//
// EXPECTED BEHAVIOUR from the header's own derivation (sim_prod_shuttle_slot_bind_default.h):
//   0x0048dec4-0x0048defa: buildings[p16][building_index].shuttle_slot != 0 -> return 0 immediately,
//     no scan, no callee call.
//   0x0048defa-0x0048e033: scan slot=1..9 (PROD_SHUTTLE_SLOTS_PER_PLAYER==10, so slot<10) for the
//     first _G_LLM_PROD_SHUTTLE_SLOTS[p16][slot] with type_ref_id==0; slot 0 is NEVER tested (the
//     loop's own init is slot=1).
//   0x0048df2e-0x0048df3a: on the first free slot, calls the ORIGINAL callee (EAX=p16, EDX=slot)
//     UNCONDITIONALLY, before any of this function's own field writes.
//   0x0048df3a-0x0048e026: building_id is RE-READ FRESH off buildings[p16][building_index] AFTER
//     that call (not reused from the guard read above) and used for BOTH type_ref_id and the
//     cfg_buildings[building_id].type lookup; then five field writes in asm order (type_ref_id,
//     src_building_type, src_building_index=(int16_t)building_index, origin_planet=
//     (int16_t)*planet_index, status=0xca) and buildings[...].shuttle_slot=(uint8_t)slot; returns
//     slot.
//   0x0048e033: scan exhausts (slots 1..9 all occupied) -> return -1, callee never called.
//   `player` (EAX) is re-read as 16 bits (p16) at EVERY use (guard, loop math, callee arg, all field
//   writes); `building_index` (EDX) stays a full dword everywhere except the one narrowing store into
//   src_building_index.
//
// KNOWN, ALREADY-SETTLED, NOT RE-OPENED: the callee at 0x0046318d is
// llm_strat_prod_shuttle_slot_release (addr/mh_calls.gen.h's CURRENT name) despite the .asm's stale
// inline comment calling it llm_strat_production_spawn_unit -- 2026-08-14 reimpl-verify already
// settled this; this file calls the header's `slot_release` member and does not relitigate the name.
//
#include "sim/sim_prod_shuttle_slot_bind_default.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct release_call {
    int32_t player;
    int32_t slot;
};
std::vector<release_call> g_release_calls;

// Set (non-null) only by the T6 order case: when non-null, the mock mutates *this* building's
// building_id to g_mutate_building_id_new_value the moment it fires -- i.e. AFTER the guard read but
// BEFORE this function's own post-call re-read. A translation that cached building_id BEFORE calling
// the callee would still observe the OLD value; the correct one (per the header's derivation) observes
// the NEW one.
building *g_mutate_building_id_target    = nullptr;
uint16_t  g_mutate_building_id_new_value = 0;

void rec_slot_release(int32_t player, int32_t slot) {
    g_release_calls.push_back({player, slot});
    if (g_mutate_building_id_target != nullptr)
        g_mutate_building_id_target->building_id = g_mutate_building_id_new_value;
}

const prod_shuttle_slot_bind_default_calls g_calls = {
    &rec_slot_release,
};

void reset_recorders() {
    g_release_calls.clear();
    g_mutate_building_id_target    = nullptr;
    g_mutate_building_id_new_value = 0;
}

// Distinct, non-symmetric fixture values -- see sim_test_support.h's own banner on why.
constexpr uint16_t PLAYER      = 5;
constexpr int32_t  BIDX        = 37;
constexpr uint16_t BUILDING_ID = 13;
constexpr uint8_t  CFG_TYPE    = 201;
constexpr int32_t  PLANET      = 9;

// The common "guard would pass, nothing yet occupied" seed: shuttle_slot unbound, building_id/cfg
// type/planet set to values distinct from every slot index and from each other so a mixed-up field
// assignment is visible.
void seed_common(sim_fixture &fx) {
    fx.b(PLAYER, BIDX).shuttle_slot    = 0;
    fx.b(PLAYER, BIDX).building_id     = BUILDING_ID;
    fx.cfg_buildings[BUILDING_ID].type = CFG_TYPE;
    fx.planet_index                    = PLANET;
}

prod_shuttle_slot &slot_at(sim_fixture &fx, uint16_t player, int32_t slot) {
    return fx.prod_shuttle_slots[(size_t)player * (size_t)PROD_SHUTTLE_SLOTS_PER_PLAYER + (size_t)slot];
}

} // namespace

void run_prod_shuttle_slot_bind_default_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- already bound (shuttle_slot != 0): return 0 immediately. The scan/callee never run, even
    // though slot 1 LOOKS free (type_ref_id==0 from reset()) -- proves the guard short-circuits before
    // the loop, not merely "the loop happens to find nothing".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_common(fx);
        fx.b(PLAYER, BIDX).shuttle_slot = 7; // already bound (nonzero)

        sim_store own = fx.store();
        int32_t   rv  = detail::prod_shuttle_slot_bind_default(fx.view(), own, g_calls, PLAYER, BIDX);

        ck_eq((uint32_t)rv, 0u, "T1: already-bound guard returns 0, 0x0048dee1-0x0048deee");
        ck_eq((uint32_t)g_release_calls.size(), 0u,
              "T1: slot_release never called on the already-bound path");
        ck_eq((uint32_t)slot_at(fx, PLAYER, 1).type_ref_id, 0u,
              "T1: slot 1 untouched -- the guard never reaches the scan/write, 0x0048dee5 JZ taken");
        ck_eq((uint32_t)fx.b(PLAYER, BIDX).shuttle_slot, 7u,
              "T1: building.shuttle_slot left as-is on the already-bound path");
    }

    // =================================================================================================
    // T2 -- happy path, nothing occupied: binds slot 1, NOT slot 0 (the loop's own init is slot=1,
    // 0x0048def3 `MOV [EBP-0x24],1`) -- and every one of the five field writes lands with its own
    // distinct value, so a mixed-up assignment (e.g. status into origin_planet) is caught.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_common(fx);

        sim_store own = fx.store();
        int32_t   rv  = detail::prod_shuttle_slot_bind_default(fx.view(), own, g_calls, PLAYER, BIDX);

        ck_eq((uint32_t)rv, 1u, "T2: first free slot is 1, not 0, 0x0048def3 loop init");
        ck((g_release_calls.size() == 1) && g_release_calls[0].player == (int32_t)PLAYER &&
               g_release_calls[0].slot == 1,
           "T2: slot_release(p16, 1) called unconditionally before any write, 0x0048df35");
        const prod_shuttle_slot &rec = slot_at(fx, PLAYER, 1);
        ck_eq((uint32_t)rec.type_ref_id, (uint32_t)BUILDING_ID,
              "T2: type_ref_id = buildings[].building_id, 0x0048df67");
        ck_eq((uint32_t)rec.src_building_type, (uint32_t)CFG_TYPE,
              "T2: src_building_type = cfg_buildings[building_id].type, 0x0048dfa9");
        ck_eq((uint32_t)(uint16_t)rec.src_building_index, (uint32_t)(uint16_t)BIDX,
              "T2: src_building_index = (int16_t)building_index, 0x0048dfc6");
        ck_eq((uint32_t)(uint16_t)rec.origin_planet, (uint32_t)(uint16_t)PLANET,
              "T2: origin_planet = (int16_t)*planet_index, 0x0048dfe7");
        ck_eq((uint32_t)(uint16_t)rec.status, 0xcau, "T2: status = 0xca (\"just bound\"), 0x0048e001");
        ck_eq((uint32_t)fx.b(PLAYER, BIDX).shuttle_slot, 1u,
              "T2: buildings[].shuttle_slot = (uint8_t)slot, 0x0048e020");
    }

    // =================================================================================================
    // T3 -- scan skips occupied slots: 1 and 2 occupied (distinct nonzero type_ref_id sentinels), so
    // slot 3 is bound. The occupied slots' own records are NOT touched by the skip.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_common(fx);
        slot_at(fx, PLAYER, 1).type_ref_id = 0xaaaa;
        slot_at(fx, PLAYER, 2).type_ref_id = 0xbbbb;

        sim_store own = fx.store();
        int32_t   rv  = detail::prod_shuttle_slot_bind_default(fx.view(), own, g_calls, PLAYER, BIDX);

        ck_eq((uint32_t)rv, 3u, "T3: first slot with type_ref_id==0 is 3, occupied 1/2 skipped, 0x0048df20-0x0048df28");
        ck((g_release_calls.size() == 1) && g_release_calls[0].slot == 3,
           "T3: slot_release(p16, 3) called for the found slot, not an occupied one");
        ck_eq((uint32_t)slot_at(fx, PLAYER, 1).type_ref_id, 0xaaaau,
              "T3: occupied slot 1 left untouched by the skip, 0x0048df05-0x0048df0b advance");
        ck_eq((uint32_t)slot_at(fx, PLAYER, 2).type_ref_id, 0xbbbbu,
              "T3: occupied slot 2 left untouched by the skip");
        ck_eq((uint32_t)slot_at(fx, PLAYER, 3).type_ref_id, (uint32_t)BUILDING_ID,
              "T3: slot 3 gets the bind write, type_ref_id = building_id");
    }

    // =================================================================================================
    // T4 -- exhaustion: slots 1..9 (the FULL scan range, slot<10) all occupied with distinct sentinel
    // values -> -1, callee never called, building.shuttle_slot never written.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_common(fx);
        for (int32_t slot = 1; slot < PROD_SHUTTLE_SLOTS_PER_PLAYER; ++slot)
            slot_at(fx, PLAYER, slot).type_ref_id = (uint16_t)(0x1000 + slot);

        sim_store own = fx.store();
        int32_t   rv  = detail::prod_shuttle_slot_bind_default(fx.view(), own, g_calls, PLAYER, BIDX);

        ck_eq((uint32_t)rv, 0xffffffffu, "T4: all nine slots occupied -> -1, 0x0048e033");
        ck_eq((uint32_t)g_release_calls.size(), 0u,
              "T4: slot_release never called when the scan exhausts, 0x0048df00 JMP past the call");
        ck_eq((uint32_t)fx.b(PLAYER, BIDX).shuttle_slot, 0u,
              "T4: building.shuttle_slot never written on the exhausted path");
    }

    // =================================================================================================
    // T5 -- `player`'s upper 16 bits are garbage: every use (guard read, loop's implicit row, the
    // callee arg, every field write) goes through the SAME p16 = (uint16_t)player narrowing, so the
    // result must be identical to the plain-PLAYER happy path (T2), and the callee must see the
    // TRUNCATED value, not the raw 32-bit one.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_common(fx);
        const uint32_t garbled_player = 0xbeef0000u | (uint32_t)PLAYER; // low 16 bits == PLAYER

        sim_store own = fx.store();
        int32_t   rv  = detail::prod_shuttle_slot_bind_default(fx.view(), own, g_calls, garbled_player, BIDX);

        ck_eq((uint32_t)rv, 1u, "T5: garbled upper 16 bits of player still bind slot 1, 0x0048dec4 MOVZX word");
        ck((g_release_calls.size() == 1) && g_release_calls[0].player == (int32_t)PLAYER,
           "T5: slot_release sees the TRUNCATED p16 (5), not the raw garbled 32-bit player value");
        const prod_shuttle_slot &rec = slot_at(fx, PLAYER, 1);
        ck_eq((uint32_t)rec.type_ref_id, (uint32_t)BUILDING_ID,
              "T5: field writes land in player row 5 (p16), matching the plain-player case");
        ck_eq((uint32_t)fx.b(PLAYER, BIDX).shuttle_slot, 1u,
              "T5: buildings[p16][building_index].shuttle_slot written using the truncated row");
    }

    // =================================================================================================
    // T6 -- ORDER: building_id is read AFTER the callee runs, not cached before it (header: "re-read
    // fresh (post-call), not reused from the guard above"). The mock mutates building_id the moment it
    // fires; the recorded type_ref_id (and the cfg-type lookup it drives) must reflect the NEW value.
    // A translation that cached building_id before calling slot_release would still see the OLD one.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t OLD_BUILDING_ID = 11;
        constexpr uint16_t NEW_BUILDING_ID = 42;
        constexpr uint8_t  OLD_CFG_TYPE    = 77;
        constexpr uint8_t  NEW_CFG_TYPE    = 188;

        fx.b(PLAYER, BIDX).shuttle_slot        = 0;
        fx.b(PLAYER, BIDX).building_id         = OLD_BUILDING_ID;
        fx.cfg_buildings[OLD_BUILDING_ID].type = OLD_CFG_TYPE;
        fx.cfg_buildings[NEW_BUILDING_ID].type = NEW_CFG_TYPE;
        fx.planet_index                        = PLANET;

        g_mutate_building_id_target    = &fx.b(PLAYER, BIDX);
        g_mutate_building_id_new_value = NEW_BUILDING_ID;

        sim_store own = fx.store();
        int32_t   rv  = detail::prod_shuttle_slot_bind_default(fx.view(), own, g_calls, PLAYER, BIDX);

        ck_eq((uint32_t)rv, 1u, "T6: still binds slot 1 -- the mutation is only to building_id");
        const prod_shuttle_slot &rec = slot_at(fx, PLAYER, 1);
        ck_eq((uint32_t)rec.type_ref_id, (uint32_t)NEW_BUILDING_ID,
              "T6: type_ref_id reflects the POST-call building_id (42), not the pre-call one (11) -- "
              "0x0048df60 re-read, after the 0x0048df35 CALL");
        ck_eq((uint32_t)rec.src_building_type, (uint32_t)NEW_CFG_TYPE,
              "T6: src_building_type's cfg lookup ALSO uses the post-call building_id (its own "
              "independent re-read in the asm, 0x0048df81), agreeing with type_ref_id's");
    }
}

} // namespace mh::sim::test
