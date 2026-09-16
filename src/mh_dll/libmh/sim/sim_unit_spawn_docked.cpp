//
// sim/sim_unit_spawn_docked.cpp -- see sim_unit_spawn_docked.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_spawn_docked_0046434c.asm), not from the Ghidra .c draft: the
// draft's overall shape (housing-cap guard -> slot search -> storage-home lookup -> init+dock) reads
// correctly and was used as a map, but the housing-cap arithmetic's missing `+1` (a real difference
// from its sim_unit_create_soldier.cpp/sim_unit_recruit.cpp siblings), the early-return-with-no-retry
// on a failed storage-home lookup, and every register/call-argument order were independently
// re-walked against the raw CMP/JZ/JL/JNZ opcodes and the MOV-before-CALL sequences per the
// translator brief.
//
#include "sim/sim_unit_spawn_docked.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_spawn_docked_calls &live_unit_spawn_docked_calls() {
    static const unit_spawn_docked_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_init_record),
        MH_LIBMH_BIND(llm_strat_storage_find_home_for_unit),
        MH_LIBMH_BIND(llm_strat_storage_dock_unit_at_building),
    };
    return c;
}

namespace {

// The ASM's own IMMEDIATE slot-search bound (0x004643b0: `CMP dword ptr [...], 0x5b`) -- the SAME
// 0x5b (91) sim_unit_spawn_on_tile.h's SPAWN_ON_TILE_SLOT_SEARCH_BOUND documents at its own,
// textually-identical CMP -- deliberately NOT sim_state.h's UNITS_PER_PLAYER (100). See the header.
inline constexpr int32_t SPAWN_DOCKED_SLOT_SEARCH_BOUND = 0x5b;

} // namespace

namespace detail {

int32_t unit_spawn_docked(const sim_view &v, const unit_spawn_docked_calls &c, uint16_t unit_proto_id,
                          uint16_t player, uint32_t probe_slot) {
    // ---- housing-cap guard (0x0046436b-0x004643a4) -- see header derivation ----------------------
    const cfg_unit &proto         = v.cfg_units[unit_proto_id];
    const int32_t   soldier_count = proto.soldier_count;
    if (soldier_count != 0) {
        // ZERO-extend owner_unit (asm: MOVZX EAX, word ptr [...]) -- same cast order
        // sim_unit_create_soldier.cpp/sim_unit_recruit.cpp's identical guard uses; NO `+1` here (see
        // header note on the real, verbatim-reproduced difference from those two siblings).
        const int32_t existing_plus_new =
            (int32_t)(uint32_t)(uint16_t)v.soldiers[player * v.caps.soldiers + 0].owner_unit +
            soldier_count;
        if (existing_plus_new >= 100) return 0;
    }

    // ---- slot search (0x004643a9-0x00464428) -----------------------------------------------------
    for (int32_t slot = 1; slot < SPAWN_DOCKED_SLOT_SEARCH_BOUND; ++slot) {
        if (unit_of(v, player, slot).unit_proto_id != 0) continue; // occupied, keep looking

        // ---- storage-home lookup, COMMIT on failure (0x004643dd-0x004643fd) -- see header note ----
        const int32_t storage_slot =
            c.storage_find_home_for_unit((uint32_t)player, (uint32_t)unit_proto_id, (int32_t)probe_slot);
        if (storage_slot == 0) return 0; // NOT `continue` -- the original commits to this slot

        // ---- init the record, THEN dock it (0x004643ff-0x00464424) --------------------------------
        c.unit_init_record(slot, (uint32_t)unit_proto_id, (uint32_t)player);
        c.storage_dock_unit_at_building(player, slot, storage_slot);

        return slot;
    }

    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_spawn_docked(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot) {
    const sim_view v = state().read;
    return detail::unit_spawn_docked(v, live_unit_spawn_docked_calls(), unit_proto_id, player,
                                     probe_slot);
}


} // namespace mh::sim
