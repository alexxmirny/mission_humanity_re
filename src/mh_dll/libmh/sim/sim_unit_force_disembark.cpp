//
// sim/sim_unit_force_disembark.cpp -- see sim_unit_force_disembark.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_unit_force_disembark_0046d0cd.asm), not from Ghidra's C: the draft's overall shape
// (single guarded release) reads correctly, but the default_op_code/exit_tile_x/exit_tile_y field
// widths were independently re-walked against the raw MOVZX/MOV-byte sequences per the translator
// brief -- see the header's WIDTH NOTES.
//
#include "sim/sim_unit_force_disembark.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// llm_strat_unit_state members this function tests/sets (map_object_unit.state, uint16_t). Ghidra's
// cfg-enum tag on this field is documentation only, not a real generated enum (see
// sim_order_enqueue.h's identical finding on the same domain) -- re-derived locally here per the
// translator brief rather than shared, same move as every other sim TU touching this field
// (sim_order_enqueue.h / sim_order_dispatch.cpp / sim_unit_update_rotation.cpp each keep their own
// copy).
constexpr uint16_t UNIT_STATE_PARKED             = 0x1f; // map_object_unit.state -- docked, idle
constexpr uint16_t UNIT_STATE_EXIT_STORAGE_BEGIN = 0x20; // map_object_unit.state -- forced out, begin exit

// map_object_building.built_flags -- bit 0x1 connected, bit 0x2 staffed; the "== 3" idiom is
// "fully operational" (see addr/mh_structs.gen.h's field comment and sim_order_enqueue.h's
// BUILT_FLAGS_OPERATIONAL on the same domain, re-derived locally here per the same convention).
constexpr uint8_t BUILT_FLAGS_OPERATIONAL = 0x3;

} // namespace

namespace detail {

void unit_force_disembark(const sim_view &v, sim_store &own, uint32_t unit_player, int32_t unit_index) {
    // The asm narrows unit_player to its low 16 bits before every row-index multiply (`MOVZX EAX, word
    // ptr [...]`, never the full dword) -- the exported .c draft's own `unit_player & 0xffff` reads the
    // same narrowing. MAX_PLAYERS is 8, so this only matters for a caller passing a player value with
    // high bits set, but the width is reproduced faithfully rather than assumed harmless.
    const uint16_t player = static_cast<uint16_t>(unit_player);

    // 0x0046d0ea-0x0046d105: guard 1 -- state must be PARKED.
    if (unit_of(v, player, unit_index).state != UNIT_STATE_PARKED) return;

    // 0x0046d10b-0x0046d125: home_storage_slot, a BYTE field, read ONCE and reused (see header note).
    const uint8_t home_storage_slot = unit_of(v, player, unit_index).home_storage_slot;

    // 0x0046d128-0x0046d158: guard 2 -- the unit's host storage building must be fully operational.
    const unit_storage &storage = storage_of(v, player, home_storage_slot);
    const building     &host    = building_of(v, player, storage.b_index);
    if (host.built_flags != BUILT_FLAGS_OPERATIONAL) return;

    // Both guards passed -- force the unit out of PARKED and reposition it to the storage's exit tile.
    unit &wu = own.unit_at(player, unit_index);

    wu.state = UNIT_STATE_EXIT_STORAGE_BEGIN; // 0x0046d15e-0x0046d171

    // 0x0046d17a-0x0046d18d: unit_proto_id read fresh off the roster here (unaffected by the state
    // write above -- a different field of the same record), matching the assembly's own read order.
    const uint16_t unit_proto_id = wu.unit_proto_id;

    // 0x0046d18d-0x0046d1b5: default_op_code is a uint8_t cfg field, read as a single BYTE and
    // zero-extended into the unit's uint16_t `order` -- see the header's WIDTH NOTES.
    wu.order = static_cast<uint16_t>(v.cfg_units[unit_proto_id].default_op_code);

    // 0x0046d1bc-0x0046d1e8 / 0x0046d1ee-0x0046d21a: exit_tile_x/y are int32_t unit_storage fields,
    // each read via a single-byte load (the low-order byte only) and stored into the unit's uint8_t
    // goal_x/goal_y -- an intentional narrowing (docs/conventions.md#coordinates: tile coords are
    // STORED as byte and computed as int), not a bug.
    wu.goal_x = static_cast<uint8_t>(storage.exit_tile_x);
    wu.goal_y = static_cast<uint8_t>(storage.exit_tile_y);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_force_disembark(uint32_t unit_player, int32_t unit_index) {
    sim_state st = state();
    detail::unit_force_disembark(st.read, st.own, unit_player, unit_index);
}


} // namespace mh::sim
