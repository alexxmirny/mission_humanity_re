//
// sim_prod_cargo_selftest.cpp -- `simtest` oracle for TWO functions bundled by TU
// (sim/sim_prod_cargo.h/.cpp, RI-SIM / SIM1-G4):
//
//   llm_strat_prod_unload_cargo_manifest @0x0048e2c0 -- run_prod_unload_cargo_manifest_tests()
//   llm_strat_prod_try_start_unit        @0x00492843 -- run_prod_try_start_unit_tests()
//
// arm_ready:false for BOTH -- shadow_region_closure.py's closure reaches game_SetEvent's huge
// UI-event surface via callees this TU calls through its OWN prod_cargo_calls seam (mockable), so
// neither gets a live shadow arm this slice. This offline oracle is their evidence. Every check below
// pins a mechanism to the .asm instruction address it comes from (tmp/decomp_sim/
// llm_strat_prod_unload_cargo_manifest_0048e2c0.asm, tmp/decomp_sim/llm_strat_prod_try_start_unit_
// 00492843.asm) -- NOT the sibling Ghidra .c drafts, per the header banner's own warning that the
// first function's draft materially lied (merged an argument-load register with an unrelated, dead
// accumulator local under one name).
//
#include "sim/sim_prod_cargo.h"

#include <cstring>
#include <string>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared call-order log, used by BOTH functions' cases to prove interleaving (which callee ran
// before/after which), not just call counts. ----------------------------------------------------
std::vector<std::string> g_call_log;

// ---- llm_strat_prod_unload_cargo_manifest's two callees --------------------------------------------

struct unload_call {
    uint16_t player;
    int32_t  building_index;
    uint32_t cargo_index;
};
std::vector<unload_call> g_unload_calls;
uint32_t                 g_unload_return = 0;
uint32_t                 rec_unload_cargo_unit(uint16_t player, int32_t building_index,
                                               uint32_t cargo_index) {
    g_unload_calls.push_back({player, building_index, cargo_index});
    g_call_log.push_back("unload");
    return g_unload_return;
}

int32_t g_notify_calls = 0;
void    rec_transfer_notify_noop() {
    ++g_notify_calls;
    g_call_log.push_back("notify");
}

// ---- llm_strat_prod_try_start_unit's three callees --------------------------------------------------

struct spend_call {
    int32_t player;
    int32_t resource_id;
    int32_t amount;
};
std::vector<spend_call> g_spend_calls;
// T18 (cache-before-mutation) support: when set, the FIRST spend_resource call mutates the target
// int32_t (a live cfg_unit::soldier_count field) then clears the flag so only that one call mutates.
bool     g_mutate_soldier_count_on_first_spend = false;
int32_t *g_soldier_count_mutate_target         = nullptr;
void     rec_spend_resource(int32_t player, int32_t resource_id, int32_t amount) {
    g_spend_calls.push_back({player, resource_id, amount});
    g_call_log.push_back("spend");
    if (g_mutate_soldier_count_on_first_spend && g_soldier_count_mutate_target != nullptr) {
        *g_soldier_count_mutate_target        = 999;
        g_mutate_soldier_count_on_first_spend = false;
    }
}

struct pop_remove_call {
    uint32_t player;
    int32_t  count;
};
std::vector<pop_remove_call> g_pop_remove_calls;
void                         rec_population_remove(uint32_t player, int32_t count) {
    g_pop_remove_calls.push_back({player, count});
    g_call_log.push_back("pop_remove");
}

struct housing_add_call {
    int32_t player;
    int32_t unit_proto_id;
};
std::vector<housing_add_call> g_housing_add_calls;
void                          rec_unit_housing_count_add(int32_t player, int32_t unit_proto_id) {
    g_housing_add_calls.push_back({player, unit_proto_id});
    g_call_log.push_back("housing_add");
}

// ONE calls struct, all 5 seams recorded, shared by BOTH run_* functions below. Each function's
// cases assert the OTHER function's callees are NEVER touched (0 calls) -- a wrong-callee-invoked
// bug in either translation would show up as a nonzero count on the wrong side.
const prod_cargo_calls g_calls = {
    &rec_unload_cargo_unit,
    &rec_transfer_notify_noop,
    &rec_spend_resource,
    &rec_population_remove,
    &rec_unit_housing_count_add,
};

void reset_recorders() {
    g_call_log.clear();
    g_unload_calls.clear();
    g_unload_return = 0;
    g_notify_calls  = 0;
    g_spend_calls.clear();
    g_mutate_soldier_count_on_first_spend = false;
    g_soldier_count_mutate_target         = nullptr;
    g_pop_remove_calls.clear();
    g_housing_add_calls.clear();
}

} // namespace

// =====================================================================================================
// llm_strat_prod_unload_cargo_manifest @0x0048e2c0
// =====================================================================================================
void run_prod_unload_cargo_manifest_tests() {
    sim_fixture fx;

    constexpr uint16_t PLAYER         = 3;
    constexpr int32_t  BUILDING_INDEX = 6;
    constexpr uint32_t SHUTTLE_SLOT   = 4;

    // =================================================================================================
    // T1 -- shuttle_slot==0 (unbound): return 0 immediately, notify_noop NEVER called (not even the
    // pre-loop one), unload_cargo_unit never called. 0x0048e2fa-0x0048e307.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot = 0;

        int32_t r = detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)r, 0u, "T1: shuttle_slot==0 -> return 0 (unbound), 0x0048e2fa-0x0048e307");
        ck_eq((uint32_t)g_notify_calls, 0u,
              "T1: transfer_notify_noop never called on the unbound early-out");
        ck_eq((uint32_t)g_unload_calls.size(), 0u, "T1: unload_cargo_unit never called");
        ck_eq((uint32_t)g_spend_calls.size() + g_pop_remove_calls.size() + g_housing_add_calls.size(),
              0u, "T1: no cross-talk into the OTHER function's three callees");
    }

    // =================================================================================================
    // T2 -- bound (shuttle_slot!=0), but slot.status != 0xca ("just bound"): notify_noop called
    // EXACTLY ONCE (the unconditional pre-check call at 0x0048e30c), then early return 0 WITHOUT a
    // second notify or any unload, 0x0048e336-0x0048e33d.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot = SHUTTLE_SLOT;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].status =
            0x99; // NOT 0xca

        int32_t r = detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)r, 0u, "T2: status != 0xca -> return 0, 0x0048e336-0x0048e33d");
        ck_eq((uint32_t)g_notify_calls, 1u,
              "T2: notify called exactly ONCE (unconditional pre-check call), 0x0048e30c");
        ck_eq((uint32_t)g_unload_calls.size(), 0u,
              "T2: unload_cargo_unit never called on the wrong-status early-out");
    }

    // =================================================================================================
    // T3 -- bound + status==0xca, EMPTY manifest (all 50 leading words 0, the reset()-zeroed default):
    // notify called TWICE, no unload calls, return 1 UNCONDITIONALLY (the CMP at 0x0048e3a4 is dead --
    // the very next instruction is an unconditional store of 1), 0x0048e398-0x0048e3a8.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot                                           = SHUTTLE_SLOT;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT].status = 0xca;

        int32_t r = detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)r, 1u, "T3: empty manifest -> return 1 unconditionally, 0x0048e3a8");
        ck_eq((uint32_t)g_notify_calls, 2u,
              "T3: notify called twice (pre-loop 0x0048e30c + post-loop 0x0048e39f)");
        ck_eq((uint32_t)g_unload_calls.size(), 0u,
              "T3: no occupied entries -> unload_cargo_unit never called");
    }

    // =================================================================================================
    // T4 -- ONE occupied manifest entry at a mid-array index (27): unload_cargo_unit called exactly
    // once with (player, building_index, 27), notify still twice, return 1. 0x0048e383-0x0048e38e.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot = SHUTTLE_SLOT;
        prod_shuttle_slot &slot =
            fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT];
        slot.status            = 0xca;
        const uint16_t leading = 0x1234; // any nonzero value marks the entry occupied
        std::memcpy(&slot.cargo_manifest_raw[27 * 14], &leading, sizeof(leading));

        int32_t r = detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)r, 1u, "T4: occupied slot -> return 1");
        ck_eq((uint32_t)g_unload_calls.size(), 1u, "T4: unload_cargo_unit called exactly once");
        ck(g_unload_calls[0].player == PLAYER && g_unload_calls[0].building_index == BUILDING_INDEX &&
               g_unload_calls[0].cargo_index == 27u,
           "T4: unload_cargo_unit(player, building_index, 27), 0x0048e387-0x0048e38e");
    }

    // =================================================================================================
    // T5 -- full-width walk boundary: entries 0 AND 49 (kManifestEntryCount=50) both occupied. Also
    // pins the header banner's `local_18` finding: player/building_index are FRESH register reloads
    // at EACH call site (0x0048e387-0x0048e38a), not a value carried in some stale accumulated local,
    // so both calls must see the IDENTICAL player/building_index.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot = SHUTTLE_SLOT;
        prod_shuttle_slot &slot =
            fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT];
        slot.status            = 0xca;
        const uint16_t leading = 7;
        std::memcpy(&slot.cargo_manifest_raw[0 * 14], &leading, sizeof(leading));
        std::memcpy(&slot.cargo_manifest_raw[49 * 14], &leading, sizeof(leading));

        int32_t r = detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)r, 1u, "T5: both boundary entries occupied -> return 1");
        ck_eq((uint32_t)g_unload_calls.size(), 2u,
              "T5: exactly 2 unload calls (indices 0 and 49), kManifestEntryCount=50, no OBOB");
        ck(g_unload_calls[0].cargo_index == 0u && g_unload_calls[1].cargo_index == 49u,
           "T5: walked in ascending index order, entry 0 then entry 49, 0x0048e350-0x0048e396");
        ck(g_unload_calls[0].player == PLAYER && g_unload_calls[0].building_index == BUILDING_INDEX &&
               g_unload_calls[1].player == PLAYER && g_unload_calls[1].building_index == BUILDING_INDEX,
           "T5: every call gets the SAME (player, building_index), freshly reloaded each site");
    }

    // =================================================================================================
    // T6 -- the .c draft's local_18 (an accumulator summing unload_cargo_unit's return codes) is
    // PROVABLY DEAD per the header banner: even with a nonzero callee return on every call, the
    // function's own return is still the unconditional 1. 0x0048e3a4-0x0048e3a8.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot = SHUTTLE_SLOT;
        prod_shuttle_slot &slot =
            fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT];
        slot.status            = 0xca;
        const uint16_t leading = 1;
        std::memcpy(&slot.cargo_manifest_raw[10 * 14], &leading, sizeof(leading));
        g_unload_return = 5; // nonzero -- would poison a "sum discarded returns" translation

        int32_t r = detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)r, 1u,
              "T6: return is 1 regardless of unload_cargo_unit's return value (dead accumulator), "
              "0x0048e3a4-0x0048e3a8");
    }

    // =================================================================================================
    // T_order -- exact call SEQUENCE across the whole body: notify BEFORE the loop, both unload calls
    // interleaved in ascending walk order, notify AFTER the loop.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.b(PLAYER, BUILDING_INDEX).shuttle_slot = SHUTTLE_SLOT;
        prod_shuttle_slot &slot =
            fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT];
        slot.status            = 0xca;
        const uint16_t leading = 1;
        std::memcpy(&slot.cargo_manifest_raw[2 * 14], &leading, sizeof(leading));
        std::memcpy(&slot.cargo_manifest_raw[5 * 14], &leading, sizeof(leading));

        detail::prod_unload_cargo_manifest(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        const std::vector<std::string> want = {"notify", "unload", "unload", "notify"};
        ck(g_call_log == want,
           "T_order: notify(pre) -> unload(2) -> unload(5) -> notify(post), exact interleave");
    }
}

// =====================================================================================================
// llm_strat_prod_try_start_unit @0x00492843
// =====================================================================================================
void run_prod_try_start_unit_tests() {
    sim_fixture fx;

    constexpr uint16_t PLAYER     = 2;
    constexpr int32_t  UNIT_PROTO = 15;

    // Prepares the invention gate (available) and the queue-header cap (order well under 0x5b) so a
    // case can override just the ONE gate it means to exercise. Returns the cfg_unit by reference so
    // the case can set type/soldier_count/resource[] directly.
    auto ready_cu = [&]() -> cfg_unit & {
        cfg_unit &cu                                                      = fx.cfg_units[UNIT_PROTO];
        cu.invention                                                      = 3;
        fx.progress[PLAYER * PROGRESS_ROW_COUNT + cu.invention].available = 1;
        fx.u(PLAYER, 0).order                                             = 0;
        return cu;
    };

    // =================================================================================================
    // T1 -- invention not researched (progress.available==0): return 0x13, NOTHING else touched (no
    // calls at all, order untouched). 0x00492884-0x0049288d.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                                      = ready_cu();
        fx.progress[PLAYER * PROGRESS_ROW_COUNT + cu.invention].available = 0; // undo ready_cu's grant

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0x13u, "T1: invention.available==0 -> return 0x13, 0x00492886");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "T1: no resource spend on the invention gate");
        ck_eq((uint32_t)g_pop_remove_calls.size(), 0u, "T1: no population_remove on the invention gate");
        ck_eq((uint32_t)g_housing_add_calls.size(), 0u, "T1: no housing_count_add on the invention gate");
        ck_eq((uint32_t)fx.u(PLAYER, 0).order, 0u, "T1: header-row order untouched on the invention gate");
    }

    // =================================================================================================
    // T2 -- production-queue header-row cap: order==0x5a(90) -> order+1(91) is NOT < 0x5b(91) -> fail,
    // return 6, order untouched. 0x00492892-0x004928b0.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        ready_cu();
        fx.u(PLAYER, 0).order = 0x5a;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 6u, "T2: order+1 not < 0x5b(91) -> return 6, 0x004928a9");
        ck_eq((uint32_t)fx.u(PLAYER, 0).order, 0x5au, "T2: order untouched on the queue-cap gate");
    }

    // =================================================================================================
    // T3 -- type==UNIT_TYPE_UNDEFINED(0): the outer `type != UNDEFINED` gate means NEITHER the
    // soldier NOR the vehicle housing check ever runs, even though both are seeded to fail decisively
    // if evaluated. 0x004928f4-0x00492906.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu         = ready_cu();
        cu.type              = UNIT_TYPE_UNDEFINED;
        cu.soldier_count     = 0;
        housing_stats &hs    = fx.unit_housing[PLAYER];
        hs.cap_prev_soldiers = 1;
        hs.used_soldiers     = 50; // would fail (1 < 50+0) if the soldier check ran at all
        hs.cap_prev_vehicles = 0;
        hs.used_vehicles     = 99; // would fail (0 <= 99) if the vehicle check ran at all

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u,
              "T3: UNIT_TYPE_UNDEFINED -> NO housing check at all, success, 0x004928f4-0x00492906");
    }

    // =================================================================================================
    // T4 -- soldier housing (type in [1,0xa]) FAILS: cap_prev_soldiers < used_soldiers+soldier_count.
    // 0x004929b7.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu         = ready_cu();
        cu.type              = 5; // soldier class
        cu.soldier_count     = 3;
        housing_stats &hs    = fx.unit_housing[PLAYER];
        hs.cap_prev_soldiers = 5;
        hs.used_soldiers     = 3; // 3+3=6 > cap 5 -> fail

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0x10u,
              "T4: soldier cap_prev < used+soldier_count -> return 0x10, 0x004929b7");
    }

    // =================================================================================================
    // T5 -- soldier housing EQUAL boundary passes: cap_prev_soldiers == used_soldiers+soldier_count
    // (the JLE at 0x004929b5 treats equality as success -- `!(cap < used+count)`).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                = ready_cu();
        cu.type                     = 5;
        cu.soldier_count            = 3;
        housing_stats &hs           = fx.unit_housing[PLAYER];
        hs.cap_prev_soldiers        = 6;
        hs.used_soldiers            = 3;  // 3+3==6==cap -> boundary pass
        fx.population[PLAYER].human = 50; // must clear the LATER population gate (0x00492a7e) too, or
                                          // the function returns 9 there before this boundary matters

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T5: soldier cap boundary EQUAL -> passes, 0x004929b5");
    }

    // =================================================================================================
    // T6 -- vehicle housing (type in [0xb,0xe]) FAILS: cap_prev_vehicles <= used_vehicles. 0x00492927.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu         = ready_cu();
        cu.type              = 12; // vehicle class
        housing_stats &hs    = fx.unit_housing[PLAYER];
        hs.cap_prev_vehicles = 4;
        hs.used_vehicles     = 4; // equal -> <= triggers fail

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0xfu, "T6: vehicle cap_prev <= used -> return 0xf, 0x00492927");
    }

    // =================================================================================================
    // T7 -- heli housing (type in [0xf,0x10]) FAILS: cap_prev_helis <= used_helis. 0x00492954.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu      = ready_cu();
        cu.type           = UNIT_TYPE_A_HELI; // 0xf
        housing_stats &hs = fx.unit_housing[PLAYER];
        hs.cap_prev_helis = 2;
        hs.used_helis     = 5;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0x11u, "T7: heli cap_prev <= used -> return 0x11, 0x00492954");
    }

    // =================================================================================================
    // T8 -- plane housing (type in [0x11,0x12]) FAILS: cap_prev_planes <= used_planes. 0x0049297e.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu       = ready_cu();
        cu.type            = UNIT_TYPE_A_PLANE; // 0x11
        housing_stats &hs  = fx.unit_housing[PLAYER];
        hs.cap_prev_planes = 1;
        hs.used_planes     = 3;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0x12u, "T8: plane cap_prev <= used -> return 0x12, 0x0049297e");
    }

    // =================================================================================================
    // T9 -- type >= UNIT_TYPE_A_HELI_MOTHER(0x13): the same "no housing check" fallthrough as
    // UNDEFINED, over ALL FOUR classes seeded to fail decisively. 0x004928e5-0x004929c3.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu         = ready_cu();
        cu.type              = UNIT_TYPE_A_HELI_MOTHER; // 0x13
        cu.soldier_count     = 0;
        housing_stats &hs    = fx.unit_housing[PLAYER];
        hs.cap_prev_soldiers = 0;
        hs.used_soldiers     = 99;
        hs.cap_prev_vehicles = 0;
        hs.used_vehicles     = 99;
        hs.cap_prev_helis    = 0;
        hs.used_helis        = 99;
        hs.cap_prev_planes   = 0;
        hs.used_planes       = 99;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u,
              "T9: type>=A_HELI_MOTHER(0x13) -> no housing check, same fallthrough as UNDEFINED, "
              "0x004928e5-0x004929c3");
    }

    // =================================================================================================
    // T10 -- pass-1 resource-shortage scan, a SINGLE missing resource: shortage = id+0x89.
    // 0x00492a23-0x00492a3a. Pass 2 never runs (no spend_resource calls) once shortage != 0.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                            = ready_cu();
        cu.type                                                 = UNIT_TYPE_UNDEFINED;
        cu.soldier_count                                        = 0;
        cu.resource[0].id                                       = 5;
        cu.resource[0].val                                      = 10;
        cu.resource[1].id                                       = 0; // terminate
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 5] = 3; // insufficient (3<10)

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, (uint32_t)(5 + 0x89),
              "T10: single shortage -> return resource_id+0x89, 0x00492a23-0x00492a3a");
        ck_eq((uint32_t)g_spend_calls.size(), 0u,
              "T10: shortage != 0 -> pass 2 never runs, no spend_resource calls");
    }

    // =================================================================================================
    // T11 -- multi-shortage COLLAPSE (non-short-circuiting scan): a SECOND shortage overwrites the
    // first's id-carrying code with the bare sentinel 0x89, so the final return does NOT name either
    // resource id. 0x00492a23-0x00492a3a.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                            = ready_cu();
        cu.type                                                 = UNIT_TYPE_UNDEFINED;
        cu.soldier_count                                        = 0;
        cu.resource[0].id                                       = 3;
        cu.resource[0].val                                      = 10;
        cu.resource[1].id                                       = 4;
        cu.resource[1].val                                      = 5;
        cu.resource[2].id                                       = 0;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 3] = 1; // shortage #1 (id 3)
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 4] = 1; // shortage #2 (id 4)

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0x89u,
              "T11: SECOND shortage collapses the first (3+0x89=0x8c) to the bare sentinel 0x89 -- "
              "the scan does NOT short-circuit on the first miss, 0x00492a27-0x00492a2f");
    }

    // =================================================================================================
    // T12 -- resource[0].id==0 (UNDEFINED sentinel): the shortage scan never runs at all, success.
    // 0x004929e9.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu      = ready_cu();
        cu.type           = UNIT_TYPE_UNDEFINED;
        cu.soldier_count  = 0;
        cu.resource[0].id = 0;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T12: resource[0].id==0 -> no shortage scan at all, success, 0x004929e9");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "T12: no resources to charge");
    }

    // =================================================================================================
    // T13 -- ALL 7 resource[] slots occupied and sufficient: the walk hits i==CFG_RESOURCE_SLOTS(7),
    // whose `.id` read lands one struct field past resource[] into resource_2[0] (0x004929e0 /
    // 0x00492ade) -- default-zeroed here, so the id==0 sentinel stops the walk cleanly at exactly 7,
    // not 8, charges.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu     = ready_cu();
        cu.type          = UNIT_TYPE_UNDEFINED;
        cu.soldier_count = 0;
        for (int32_t i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
            cu.resource[i].id                                             = (uint32_t)(i + 1);
            cu.resource[i].val                                            = (i + 1) * 2;
            fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + (i + 1)] = (i + 1) * 2; // exact
        }
        // resource_2[0] (the aliasing OOB neighbour) is left at its reset()-zeroed default.

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T13: all 7 slots sufficient -> success, 0x00492b4d");
        ck_eq((uint32_t)g_spend_calls.size(), 7u,
              "T13: exactly 7 spend_resource calls -- the resource[7]/resource_2[0] OOB read "
              "(0x004929e0/0x00492ade) terminates the walk, does not charge an 8th");
    }

    // =================================================================================================
    // T13b -- same 7-slot setup, but resource_2[0].id is DELIBERATELY nonzero: proves the i<7 bound
    // check (evaluated SECOND, per the header banner) still stops the walk at 7 even when the OOB
    // read's id is nonzero -- the id==0 check alone would NOT have stopped it here.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu     = ready_cu();
        cu.type          = UNIT_TYPE_UNDEFINED;
        cu.soldier_count = 0;
        for (int32_t i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
            cu.resource[i].id                                             = (uint32_t)(i + 1);
            cu.resource[i].val                                            = (i + 1) * 2;
            fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + (i + 1)] = (i + 1) * 2;
        }
        cu.resource_2[0].id                                     = 9; // nonzero -- would be read as an 8th resource if id==0 were the
        cu.resource_2[0].val                                    = 1; // ONLY stop condition
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 9] = 1;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T13b: still success (7 slots sufficient)");
        ck_eq((uint32_t)g_spend_calls.size(), 7u,
              "T13b: the i<CFG_RESOURCE_SLOTS(7) bound stops the walk even with a nonzero OOB id -- "
              "bound evaluated SECOND, 0x004929ed-0x004929f3");
    }

    // =================================================================================================
    // T14 -- crew gate, population insufficient: population[player].human < soldier_count -> return 9.
    // 0x00492a7e.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                = ready_cu();
        cu.type                     = UNIT_TYPE_UNDEFINED; // isolate the population gate from housing
        cu.soldier_count            = 4;
        cu.resource[0].id           = 0;
        fx.population[PLAYER].human = 3; // 3 < 4 -> fail

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 9u, "T14: population[player].human < soldier_count -> return 9, 0x00492a7e");
    }

    // =================================================================================================
    // T15a -- crew gate, soldier roster full, player_race==1: return 0xa. 0x00492aad.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                             = ready_cu();
        cu.type                                                  = UNIT_TYPE_UNDEFINED;
        cu.soldier_count                                         = 5;
        cu.resource[0].id                                        = 0;
        fx.population[PLAYER].human                              = 50; // sufficient
        fx.soldiers[PLAYER * SOLDIERS_PER_PLAYER + 0].owner_unit = 95; // used slots
        // 95 + 5 + 1 = 101 > 99 -> roster full
        fx.player_race = 1;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0xau, "T15a: roster full, player_race==1 -> return 0xa, 0x00492aad");
    }

    // =================================================================================================
    // T15b -- same roster-full setup, player_race!=1: return 0xaf. 0x00492ab6.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                             = ready_cu();
        cu.type                                                  = UNIT_TYPE_UNDEFINED;
        cu.soldier_count                                         = 5;
        cu.resource[0].id                                        = 0;
        fx.population[PLAYER].human                              = 50;
        fx.soldiers[PLAYER * SOLDIERS_PER_PLAYER + 0].owner_unit = 95;
        fx.player_race                                           = 2; // NOT 1

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0xafu, "T15b: roster full, player_race!=1 -> return 0xaf, 0x00492ab6");
    }

    // =================================================================================================
    // T15c -- owner_unit width semantics: the field is a signed int16_t but the original reads it
    // MOVZX (zero-extended) regardless. -1 as int16_t is 0xffff, which zero-extends to 65535 (not to
    // -1) -- so a translation that read it SIGNED would compute -1+5+1=5<=99 and WRONGLY pass; the
    // correct zero-extended reading gives 65535+5+1, decisively > 99. 0x00492aa4-0x00492ab6.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                             = ready_cu();
        cu.type                                                  = UNIT_TYPE_UNDEFINED;
        cu.soldier_count                                         = 5;
        cu.resource[0].id                                        = 0;
        fx.population[PLAYER].human                              = 50;
        fx.soldiers[PLAYER * SOLDIERS_PER_PLAYER + 0].owner_unit = (int16_t)-1;
        fx.player_race                                           = 0;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0xafu,
              "T15c: owner_unit==-1 read MOVZX (zero-extended to 65535) -> roster gate fails, "
              "0x00492aa4-0x00492ab6 (translator brief rule 7: width is semantic)");
    }

    // =================================================================================================
    // T16 -- soldier_count==0: the ENTIRE population+roster gate block is skipped (not merely
    // vacuously true) -- population_remove is NEVER called -- while unit_housing_count_add and the
    // header-row increment remain UNCONDITIONAL. 0x00492a66-0x00492aa2, 0x00492b1e-0x00492b2b.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu         = ready_cu();
        cu.type              = 12; // vehicle class, no crew
        cu.soldier_count     = 0;
        housing_stats &hs    = fx.unit_housing[PLAYER];
        hs.cap_prev_vehicles = 10;
        hs.used_vehicles     = 2;
        cu.resource[0].id    = 0;

        const uint16_t before_order = fx.u(PLAYER, 0).order;
        sim_store      own          = fx.store();
        int32_t        r            = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T16: soldier_count==0 -> success");
        ck_eq((uint32_t)g_pop_remove_calls.size(), 0u,
              "T16: population_remove NEVER called when soldier_count==0, 0x00492b1e-0x00492b2b");
        ck_eq((uint32_t)g_housing_add_calls.size(), 1u,
              "T16: unit_housing_count_add IS unconditional though, 0x00492b30-0x00492b37");
        ck_eq((uint32_t)fx.u(PLAYER, 0).order, (uint32_t)(before_order + 1),
              "T16: header-row order incremented on success regardless, 0x00492b4d");
    }

    // =================================================================================================
    // T17 -- grand-finale full success WITH crew: exact resource-charge call sequence/args (pass 2),
    // population_remove(player, soldier_count) once, unit_housing_count_add(player, unit_proto_id)
    // once, header-row order incremented by exactly 1, and the OVERALL call ORDER is
    // charges-then-population_remove-then-housing_add.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                             = ready_cu();
        cu.type                                                  = 3; // soldier class [1,0xa]
        cu.soldier_count                                         = 4;
        housing_stats &hs                                        = fx.unit_housing[PLAYER];
        hs.cap_prev_soldiers                                     = 10;
        hs.used_soldiers                                         = 2; // 2+4=6<=10 passes
        cu.resource[0].id                                        = 1;
        cu.resource[0].val                                       = 5;
        cu.resource[1].id                                        = 2;
        cu.resource[1].val                                       = 3;
        cu.resource[2].id                                        = 4;
        cu.resource[2].val                                       = 7;
        cu.resource[3].id                                        = 0; // terminate
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 1]  = 5;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 2]  = 3;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 4]  = 7;
        fx.population[PLAYER].human                              = 10; // 10>=4 passes
        fx.soldiers[PLAYER * SOLDIERS_PER_PLAYER + 0].owner_unit = 50; // 50+4+1=55<=99 passes
        fx.player_race                                           = 0;
        fx.u(PLAYER, 0).order                                    = 20;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T17: all gates pass -> success, 0x00492b54");
        ck_eq((uint32_t)g_spend_calls.size(), 3u, "T17: exactly 3 spend_resource calls (pass 2), 0x00492b11");
        if (g_spend_calls.size() == 3) {
            ck(g_spend_calls[0].player == (int32_t)PLAYER && g_spend_calls[0].resource_id == 1 &&
                   g_spend_calls[0].amount == 5,
               "T17: spend_resource call #1 == (player, id=1, val=5)");
            ck(g_spend_calls[1].player == (int32_t)PLAYER && g_spend_calls[1].resource_id == 2 &&
                   g_spend_calls[1].amount == 3,
               "T17: spend_resource call #2 == (player, id=2, val=3)");
            ck(g_spend_calls[2].player == (int32_t)PLAYER && g_spend_calls[2].resource_id == 4 &&
                   g_spend_calls[2].amount == 7,
               "T17: spend_resource call #3 == (player, id=4, val=7)");
        }
        ck_eq((uint32_t)g_pop_remove_calls.size(), 1u, "T17: population_remove called exactly once");
        if (!g_pop_remove_calls.empty()) {
            ck(g_pop_remove_calls[0].player == PLAYER && g_pop_remove_calls[0].count == 4,
               "T17: population_remove(player, soldier_count=4), 0x00492b24-0x00492b2b");
        }
        ck_eq((uint32_t)g_housing_add_calls.size(), 1u, "T17: unit_housing_count_add called exactly once");
        if (!g_housing_add_calls.empty()) {
            ck(g_housing_add_calls[0].player == (int32_t)PLAYER &&
                   g_housing_add_calls[0].unit_proto_id == UNIT_PROTO,
               "T17: unit_housing_count_add(player, unit_proto_id), 0x00492b30-0x00492b37");
        }
        ck_eq((uint32_t)fx.u(PLAYER, 0).order, 21u, "T17: header-row order 20 -> 21, 0x00492b4d");

        const std::vector<std::string> want = {"spend", "spend", "spend", "pop_remove", "housing_add"};
        ck(g_call_log == want,
           "T17: call ORDER is pass-2 charges, then population_remove, then housing_count_add");
    }

    // =================================================================================================
    // T18 -- cache-before-mutation: `count` (cu.soldier_count) must be read/cached BEFORE pass 2's
    // spend_resource calls run, not re-read afterward. The mock mutates the SAME cfg_unit's
    // soldier_count field from inside the FIRST spend_resource callback; population_remove must still
    // see the ORIGINAL value (4), not the mutated one (999).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        cfg_unit &cu                                             = ready_cu();
        cu.type                                                  = 3;
        cu.soldier_count                                         = 4;
        housing_stats &hs                                        = fx.unit_housing[PLAYER];
        hs.cap_prev_soldiers                                     = 10;
        hs.used_soldiers                                         = 2;
        cu.resource[0].id                                        = 1;
        cu.resource[0].val                                       = 5;
        cu.resource[1].id                                        = 0;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 1]  = 5;
        fx.population[PLAYER].human                              = 10;
        fx.soldiers[PLAYER * SOLDIERS_PER_PLAYER + 0].owner_unit = 50;
        fx.player_race                                           = 0;

        g_mutate_soldier_count_on_first_spend = true;
        g_soldier_count_mutate_target         = &cu.soldier_count;

        sim_store own = fx.store();
        int32_t   r   = detail::prod_try_start_unit(fx.view(), own, g_calls, PLAYER, UNIT_PROTO);

        ck_eq((uint32_t)r, 0u, "T18: still succeeds");
        ck_eq((uint32_t)g_pop_remove_calls.size(), 1u, "T18: population_remove still called once");
        if (!g_pop_remove_calls.empty()) {
            ck_eq((uint32_t)g_pop_remove_calls[0].count, 4u,
                  "T18: population_remove gets the ORIGINAL cached soldier_count(4), NOT the "
                  "mid-pass-2 mutated value(999) -- `count` is read/cached before pass 2 runs");
        }
    }
}

} // namespace mh::sim::test
