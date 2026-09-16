//
// sim/sim_storage_release_door_held_by_unit.cpp -- see sim_storage_release_door_held_by_unit.h.
// Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_release_door_held_by_unit_00485466.asm), not from the Ghidra .c
// draft -- the draft's overall shape (scan a player's storage slots, clear a door-holder field on
// match) reads correctly and was used as a map, but the loop range, the field offsets, and the
// return-value derivation were independently re-walked against the raw CMP/IMUL/JZ/JNZ opcodes per
// house rules (see the header banner's note on why the draft's "iVar1 = player" fallback never
// actually executes).
//
#include "sim/sim_storage_release_door_held_by_unit.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t storage_release_door_held_by_unit(sim_store &own, int32_t player, int32_t unit_idx) {
    const uint32_t p = static_cast<uint32_t>(player);

    // 0x00485483: local_18 = 1. 0x0048548a-0x00485490: CMP local_18,0x19 / JL body else JMP exit -- an
    // ordinary bounded loop, slots 1..24 inclusive (STORAGE_PER_PLAYER is 25; slot 0 is never visited,
    // see header note). `last_slot` tracks what the loop tail (0x00485492) loads into EAX every visit,
    // which is what the function returns -- see the header's derivation of why this is always 24 at
    // exit rather than the draft's dead `player` fallback.
    int32_t last_slot = 1;
    for (int32_t slot = 1; slot < 0x19; ++slot) {
        // 0x0048549a-0x004854a8 / 0x004854b3-0x004854c1 / 0x004854ce-0x004854dc: the SAME
        // player*0x17d4 + slot*0xf4 row/elem address, re-materialised three separate times by the
        // assembly (Watcom's usual idiom) -- taken once here via a single reference, address-equivalent
        // since nothing between the three accesses can move or reallocate the region (no outward call
        // anywhere in this body).
        unit_storage &s = own.storage_at(p, slot);

        // 0x004854aa-0x004854b1: b_index != 0 (JZ skips both remaining tests when the slot is unbound).
        // 0x004854c3-0x004854cc: door_mutex_unit == unit_idx (JNZ skips the write otherwise).
        if (s.b_index != 0 && s.door_mutex_unit == unit_idx) {
            // 0x004854de: door_mutex_unit = 0 -- release the door.
            s.door_mutex_unit = 0;
        }

        // 0x00485492: EAX = local_18 (the CURRENT slot, before the increment at 0x00485495).
        last_slot = slot;
    }
    return last_slot;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t storage_release_door_held_by_unit(int32_t player, int32_t unit_idx) {
    sim_state st = state();
    return detail::storage_release_door_held_by_unit(st.own, player, unit_idx);
}


} // namespace mh::sim
