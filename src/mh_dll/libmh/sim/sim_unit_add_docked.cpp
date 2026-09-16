//
// sim/sim_unit_add_docked.cpp -- see sim_unit_add_docked.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_add_docked_0046443a.asm), not from the Ghidra .c draft -- the draft's
// overall shape (free-slot scan, storage lookup, map_unit_Add + dock-at-building on success) reads
// correctly and was used as a map, but every index expression, field offset, and the two calls'
// differing unit_proto_id widths were independently re-walked against the raw
// MOVZX/MOV/IMUL/CMP/JZ/JNZ opcodes per house rules.
//
#include "sim/sim_unit_add_docked.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_add_docked_calls &live_unit_add_docked_calls() {
    static const unit_add_docked_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_find_home_for_unit),
        MH_LIBMH_BIND(map_unit_Add),
        MH_LIBMH_BIND(llm_strat_storage_dock_unit_at_building),
    };
    return c;
}

namespace detail {

int32_t unit_add_docked(const sim_view &v, const unit_add_docked_calls &c, uint32_t unit_proto_id,
                        uint16_t player, uint32_t probe_slot) {
    // 0x00464459-0x0046448b: linear scan units[player][1..0x5a] for a free slot (unit_proto_id==0).
    // Bound: the asm tests `unit_index < 0x5b` to continue (0x00464460-0x00464466), i.e. visits
    // 1..90 (0x5a) inclusive and falls to the "no slot" exit once it would reach 91 -- see header note
    // (same 91-slot bound the spawn_docked/spawn_on_tile siblings use, not the full 100-slot extent).
    int32_t unit_index = 1;
    while (unit_index <= 0x5a) {
        if (unit_of(v, static_cast<uint32_t>(player), unit_index).unit_proto_id == 0) break;
        ++unit_index;
    }
    if (unit_index > 0x5a) return 0; // 0x00464466/0x004644d7: search exhausted, no free slot

    // 0x0046448d-0x004644a6: ask the storage picker for an accepting building. `unit_proto_id` is
    // narrowed to its low 16 bits for THIS call only (MOVZX word, 0x00464490) -- the map_unit_Add call
    // below reads the SAME parameter back at full 32-bit width instead (see header note), a genuine
    // width difference reproduced exactly rather than smoothed into one shared local.
    const int32_t storage_slot =
        c.storage_find_home_for_unit(static_cast<uint32_t>(player), unit_proto_id & 0xffffu,
                                     static_cast<int32_t>(probe_slot));
    if (storage_slot == 0) return 0; // 0x004644a4/0x004644a6: no accepting storage building

    // 0x004644af-0x004644be: create the record (map::unit::Add variant -- no soldier-cap gate, no
    // crew-link step, unlike the llm_strat_unit_init_record llm_strat_unit_spawn_docked uses).
    c.unit_add(static_cast<uint32_t>(unit_index), static_cast<int32_t>(unit_proto_id), player);

    // 0x004644be-0x004644cd: dock + park the new unit into the accepting slot.
    c.storage_dock_unit_at_building(player, unit_index, storage_slot);

    return unit_index; // 0x004644cd-0x004644d0
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_add_docked(uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot) {
    const sim_view v = state().read;
    return detail::unit_add_docked(v, live_unit_add_docked_calls(), unit_proto_id, player, probe_slot);
}


} // namespace mh::sim
