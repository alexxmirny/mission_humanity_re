#include "sim/sim_prod_shuttle_bay_unload_all.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_bay_unload_all_calls &live_prod_shuttle_bay_unload_all_calls() {
    static const prod_shuttle_bay_unload_all_calls c = {
        MH_LIBMH_BIND(llm_prod_shuttle_unload_resource),
        MH_LIBMH_BIND(llm_prod_shuttle_unload_passengers),
        MH_LIBMH_BIND(llm_strat_prod_unload_cargo_unit),
    };
    return c;
}

namespace {

// One cargo-manifest entry's stride and leading-word offset -- same array/entry layout as
// sim_prod_unload_cargo_unit.cpp's kCargoEntryStride/kCargoEntryProtoIdOffset. Named locally rather
// than shared per the "one function in, one function out" rule (this site only TESTS the leading
// word for nonzero; it never reads/masks the control-group nibble that sibling does).
inline constexpr uint32_t kCargoEntryStride            = 0xe;
inline constexpr uint32_t kCargoEntryLeadingWordOffset = 0x0;

// resource_id's range: 1..9 inclusive -- loop starts at 1 (`MOV dword ptr [EBP-0x20],0x1` @0x0048f7e0)
// and exits once the counter reaches 10 (`CMP ..,0xa` @0x0048f7e7).
inline constexpr int32_t kFirstResourceId = 1;
inline constexpr int32_t kResourceIdBound = 10;

// The fixed "unload everything" amount both llm_prod_shuttle_unload_resource and
// llm_prod_shuttle_unload_passengers are called with (`MOV ECX/EBX,0xffffffff` @0x0048f7f7/0x0048f82a).
inline constexpr int32_t kUnloadAllCap = -1;

// Cargo-manifest walk bound: 0..49 inclusive (`CMP dword ptr [EBP-0x18],0x32` @0x0048f849).
inline constexpr int32_t kCargoManifestEntryCount = 0x32;

} // namespace

namespace detail {

void prod_shuttle_bay_unload_all(const sim_view &v, const prod_shuttle_bay_unload_all_calls &c,
                                 uint16_t player, int32_t building_index) {
    // 0x0048f7c3-0x0048f7dd: shuttle_slot, read ONCE off the BUILDING record and reused throughout --
    // the original never re-reads the building record.
    const building &b            = building_of(v, player, building_index);
    const uint32_t  shuttle_slot = b.shuttle_slot;

    // 0x0048f7e0-0x0048f80c: unload all nine resource cargo types, resource_id = 1..9 inclusive.
    for (int32_t resource_id = kFirstResourceId; resource_id < kResourceIdBound; ++resource_id) {
        c.unload_resource(player, building_index, (uint16_t)resource_id, kUnloadAllCap);
    }

    // 0x0048f80e-0x0048f836: gate the passengers unload on the shuttle slot's .passengers_reserved.
    // Row addressing: _G_LLM_PROD_SHUTTLE_SLOTS[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot]
    // -- see the header's ADDRESSING note for the literal cross-check against the two field offsets.
    const prod_shuttle_slot &slot =
        v.prod_shuttle_slots[(uint32_t)player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot];
    if (slot.passengers_reserved != 0) {
        c.unload_passengers(player, building_index, kUnloadAllCap);
    }

    // 0x0048f83b-0x0048f891: walk every cargo-manifest entry; unload the ones whose leading int16 is
    // nonzero (the same "occupied" flag sim_prod_unload_cargo_unit.cpp's kCargoEntryProtoIdOffset
    // reads/masks -- this site only TESTS it, never touches the control-group nibble or masks it out).
    for (int32_t cargo_index = 0; cargo_index < kCargoManifestEntryCount; ++cargo_index) {
        const uint32_t entry_off = (uint32_t)cargo_index * kCargoEntryStride;
        int16_t        leading;
        std::memcpy(&leading, &slot.cargo_manifest_raw[entry_off + kCargoEntryLeadingWordOffset],
                    sizeof(leading));
        if (leading != 0) {
            // 0x0048f87c-0x0048f88c: llm_strat_prod_unload_cargo_unit(player, building_index,
            // cargo_index) (ORIGINAL, via `c`; this ring's sibling). The mask-to-16-bits matches the
            // Ghidra .c draft's own `local_1c & 0xffff` -- inert here since cargo_index never exceeds
            // 49, kept for literal fidelity with the source.
            c.unload_cargo_unit(player, building_index, (uint32_t)cargo_index & 0xffffu);
            // 0x0048f88c: the original ADDs the return value into a stack accumulator (`local_1c`)
            // that is compared but never branched on -- see the header's DEAD ACCUMULATOR note. Not
            // reproduced; the function returns void and nothing reads the accumulated value.
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void prod_shuttle_bay_unload_all(uint16_t player, int32_t building_index) {
    const sim_view v = state().read;
    detail::prod_shuttle_bay_unload_all(v, live_prod_shuttle_bay_unload_all_calls(), player,
                                        building_index);
}


} // namespace mh::sim
