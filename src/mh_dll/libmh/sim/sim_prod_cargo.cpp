#include "sim/sim_prod_cargo.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_cargo_calls &live_prod_cargo_calls() {
    static const prod_cargo_calls c = {
        MH_LIBMH_BIND(llm_strat_prod_unload_cargo_unit),
        MH_LIBMH_BIND(llm_bldg_transfer_notify_noop),
        MH_LIBMH_BIND(game_SpendResource),
        MH_LIBMH_BIND(llm_strat_population_remove),
        MH_LIBMH_BIND(llm_strat_unit_housing_count_add),
    };
    return c;
}

namespace {

// ---- llm_strat_prod_unload_cargo_manifest's own constants -----------------------------------------

// _G_LLM_PROD_SHUTTLE_SLOTS[player][slot].status == 0xca means "just bound" (per that field's own
// Ghidra comment) -- the only status this function proceeds past.
inline constexpr int16_t kSlotStatusJustBound = 0xca;

// cargo_manifest_raw walk bounds -- 50 entries, stride 0xe(14) bytes each. Same values
// sim_prod_unload_cargo_unit.cpp's kCargoEntryStride independently derives for the same array; not
// shared across TUs per this codebase's per-file-local-constant convention.
inline constexpr int32_t kManifestEntryCount  = 0x32;
inline constexpr int32_t kManifestEntryStride = 0xe;

// ---- llm_strat_prod_try_start_unit's own constants -------------------------------------------------

// units[player][0].order production-queue header-row cap (0x00492892-0x004928a7): admits a new
// request only while `order + 1 < 0x5b(91)`.
inline constexpr int32_t kQueueHeaderCap = 0x5b;

// units[player][0].order header-row increment (0x00492b3c-0x00492b4d, bare `INC word ptr`) -- same
// idiom and same name sim_prod_unload_cargo_unit.cpp uses for its own INC/DEC pair on the identical
// field; see the header banner's HEADER-ROW COUNTER note for the open enum-vs-counter question.
inline constexpr uint16_t kUnitZeroOrderHeaderIncrement = 1;

} // namespace

namespace detail {

int32_t prod_unload_cargo_manifest(const sim_view &v, const prod_cargo_calls &c, uint16_t player,
                                   int32_t building_index) {
    // 0x0048e2dd-0x0048e2f7: buildings[player][building_index].shuttle_slot.
    const uint32_t shuttle_slot = building_of(v, player, building_index).shuttle_slot;
    if (shuttle_slot == 0) {
        return 0; // 0x0048e2fa-0x0048e307
    }

    // 0x0048e30c-0x0048e313: notify (ORIGINAL, void(void) -- the args loaded before this call in the
    // .asm are dead, see the header banner).
    c.transfer_notify_noop();

    // 0x0048e318-0x0048e334: only proceed if the bound slot is in status 0xca.
    const prod_shuttle_slot &slot =
        v.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot];
    if (slot.status != kSlotStatusJustBound) {
        return 0; // 0x0048e336-0x0048e33d
    }

    // 0x0048e342-0x0048e398: walk all 50 manifest entries, unloading every occupied one. The
    // accumulator the original writes here (EBP-0x18, summing each call's return value) is PROVABLY
    // DEAD -- see the header banner's derivation -- and is not reproduced.
    for (int32_t i = 0; i < kManifestEntryCount; ++i) {
        uint16_t leading_word;
        std::memcpy(&leading_word, &slot.cargo_manifest_raw[i * kManifestEntryStride],
                    sizeof(leading_word));
        if (leading_word != 0) {
            // 0x0048e383-0x0048e38e: player and building_index are reloaded FRESH from their own
            // locals at this call site in the assembly -- NOT through the Ghidra .c draft's
            // `local_18` (see the header banner: that variable does not exist in the assembly).
            c.unload_cargo_unit(player, building_index, (uint32_t)i);
        }
    }

    // 0x0048e398-0x0048e3a8: notify again, then return 1 UNCONDITIONALLY (the CMP just before the
    // return-value store is dead -- see the header banner).
    c.transfer_notify_noop();
    return 1;
}

int32_t prod_try_start_unit(const sim_view &v, sim_store &own, const prod_cargo_calls &c,
                            uint16_t player, int32_t unit_proto_id) {
    const cfg_unit &cu = v.cfg_units[unit_proto_id];

    // 0x00492860-0x0049288d: prerequisite invention gate.
    if (progress_of(v, player, cu.invention).available == 0) {
        return 0x13;
    }

    // 0x00492892-0x004928b0: production-queue header-row count gate -- see the header banner's
    // HEADER-ROW COUNTER note.
    if (!(unit_of(v, player, 0).order + 1 < kQueueHeaderCap)) {
        return 6;
    }

    const uint32_t       type = cu.type;
    const housing_stats &hs   = v.unit_housing[player];

    // 0x004928b5-0x004929c3: per-class housing-cap gate, four mutually exclusive arms bounded by the
    // cfg_enum_E_UNIT_TYPE class boundaries already named in sim_unit_type_predicates.h /
    // sim_order_enqueue.h (naming-convention rule 17a).
    if (type < UNIT_TYPE_A_HELI) {
        if (type != UNIT_TYPE_UNDEFINED) {
            if (type < UNIT_TYPE_A_WALKER) {
                // 0x0049298c-0x004929be: soldier housing.
                if (hs.cap_prev_soldiers < hs.used_soldiers + cu.soldier_count) {
                    return 0x10;
                }
            } else {
                // 0x0049290b-0x0049292e: vehicle housing.
                if (hs.cap_prev_vehicles <= hs.used_vehicles) {
                    return 0xf;
                }
            }
        }
    } else if (type < UNIT_TYPE_A_PLANE) {
        // 0x00492938-0x0049295b: heli housing.
        if (hs.cap_prev_helis <= hs.used_helis) {
            return 0x11;
        }
    } else if (type < UNIT_TYPE_A_HELI_MOTHER) {
        // 0x00492962-0x00492985: plane housing.
        if (hs.cap_prev_planes <= hs.used_planes) {
            return 0x12;
        }
    }
    // type >= UNIT_TYPE_A_HELI_MOTHER (0x004929c3's direct fallthrough, same as UNDEFINED): no
    // housing check at all. (Corrected 2026-08-22 -- 0x00492ac8 is LAB_00492ac8, the pass-2
    // resource-charge loop's i=0 init, not this fallthrough target.)

    // ---- pass 1: resource-shortage scan over Unit[unit_proto_id].resource[] (0x004929d1-0x00492a45),
    // same non-short-circuiting multi-shortage collapse shape and id-before-bound evaluation order as
    // sim_bldg_pay_costs.cpp.
    int32_t shortage = 0;
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = cu.resource[i].id; // 0x004929e0 -- can read resource[7].
        if (resource_id == 0) break;                    // 0x004929e9: UNDEFINED
        if (!(i < CFG_RESOURCE_SLOTS)) break;           // 0x004929ed/0x004929f3: bound, evaluated SECOND

        if (player_resource_of(v, player, (int32_t)resource_id) < cu.resource[i].val) { // 0x00492a1b
            shortage = (shortage == 0) ? (int32_t)(resource_id + 0x89u) : 0x89;         // 0x00492a23-0x00492a3a
        }
    }
    if (shortage != 0) {
        return shortage; // 0x00492a45-0x00492a51
    }

    // 0x00492a56-0x00492aa2: population + soldier-roster gate, only when this unit type carries crew.
    const int32_t count = cu.soldier_count;
    if (count != 0) {
        if (v.population[player].human < count) {
            return 9; // 0x00492a7e-0x00492a85
        }
        // owner_unit is read MOVZX (zero-extended) in the assembly regardless of its declared signed
        // struct type -- record [0]'s field doubles as the roster's used-slot count here (translator
        // brief rule 7: width is semantic).
        const int32_t used_soldier_slots =
            (int32_t)(uint16_t)v.soldiers[player * v.caps.soldiers + 0].owner_unit;
        if (99 < used_soldier_slots + count + 1) {
            // 0x00492aa4-0x00492ac0: race-specific rejection code.
            return (*v.player_race == 1) ? 0xa : 0xaf;
        }
    }

    // ---- pass 2: actually charge every resource slot (0x00492acf-0x00492b19), same array as pass 1.
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = cu.resource[i].id; // 0x00492ade
        if (resource_id == 0) break;
        if (!(i < CFG_RESOURCE_SLOTS)) break;

        c.spend_resource((int32_t)player, (int32_t)resource_id, cu.resource[i].val); // 0x00492b11
    }

    if (count != 0) {
        c.population_remove(player, count); // 0x00492b24-0x00492b2b
    }

    c.unit_housing_count_add((int32_t)player, unit_proto_id); // 0x00492b30-0x00492b37

    // 0x00492b3c-0x00492b4d: the header-row increment -- see the header banner's HEADER-ROW COUNTER
    // note (same idiom sim_prod_unload_cargo_unit.cpp names kUnitZeroOrderHeaderIncrement).
    unit &roster_header = own.unit_at(player, 0);
    roster_header.order = (uint16_t)(roster_header.order + kUnitZeroOrderHeaderIncrement);

    return 0; // 0x00492b4d-0x00492b54
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t prod_unload_cargo_manifest(uint32_t player, int32_t building_index) {
    const sim_view v = state().read;
    // Truncated to the low 16 bits at EVERY use site in the original (0x0048e2dd et al.).
    return detail::prod_unload_cargo_manifest(v, live_prod_cargo_calls(), (uint16_t)(player & 0xffffu),
                                              building_index);
}

int32_t prod_try_start_unit(uint32_t player, int32_t unit_proto_id) {
    sim_state st = state();
    // Truncated to the low 16 bits at EVERY use site in the original (0x00492871 et al.).
    return detail::prod_try_start_unit(st.read, st.own, live_prod_cargo_calls(),
                                       (uint16_t)(player & 0xffffu), unit_proto_id);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
