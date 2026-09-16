//
// sim/sim_storage_dock_unit_at_building.cpp -- see sim_storage_dock_unit_at_building.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_dock_unit_at_building_004636bc.asm), not
// from the Ghidra .c draft -- the draft's overall shape (docked-list append, occupancy weighting,
// park-and-refresh) reads correctly and was used as a map, but every index expression/field offset
// was independently re-walked against the raw IMUL/MOVZX/CMP/JZ opcodes per house rules, which is
// what surfaced the park_y offset hazard documented in the header.
//
#include "sim/sim_storage_dock_unit_at_building.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_dock_unit_at_building_calls &live_storage_dock_unit_at_building_calls() {
    static const storage_dock_unit_at_building_calls c = {
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// map_object_unit.state -- docked, idle. Same value/field every other sim TU touching this domain
// re-derives locally (no backing Ghidra enum on this order/state domain, per sim_dock_slot_is_busy.h/
// sim_storage_cancel_pending_docked.cpp's identical finding); not shared across TUs by established
// convention.
constexpr uint16_t UNIT_STATE_PARKED = 0x1f;

// game::e::event member 7 -- BUILD_PROJECTS_REFRESH. Kept FILE-LOCAL (anonymous namespace, not
// `mh::sim` scope) deliberately: sim_unit_population_remove.h already declares an identically-named,
// identically-valued `inline constexpr uint32_t EVENT_BUILD_PROJECTS_REFRESH = 7;` at mh::sim
// namespace scope, and mh/seams/reimpl_probe.cpp includes both that header and this TU's own header
// in one translation unit -- a second namespace-scope declaration of the same name would be a hard
// redefinition error there (same situation sim_bldg_notify_ui.cpp's identical constant documents).
constexpr uint32_t kEventBuildProjectsRefresh = 7;

} // namespace

namespace detail {

void storage_dock_unit_at_building(const sim_view &v, sim_store &own,
                                   const storage_dock_unit_at_building_calls &c, uint16_t player,
                                   int32_t unit_idx, int32_t storage_slot) {
    // 0x004636db-0x00463716: append unit_idx at the CURRENT docked_count -- UNCHECKED against the
    // 50-entry docked_units[] capacity, matching every other docked_units writer in this closure
    // (sim_storage_cancel_pending_docked.cpp's reader-side walk is the only bound this array gets
    // anywhere in the migration set).
    unit_storage &storage                      = own.storage_at(player, storage_slot);
    storage.docked_units[storage.docked_count] = unit_idx;

    // 0x00463729: docked_count += 1, AFTER the append above (the asm re-derives the unit_storage row/
    // elem base a third time for this one INC; collapsed to the single `storage` reference here since
    // nothing between the two writes changes player/storage_slot).
    storage.docked_count += 1;

    // 0x0046372f-0x004637b2: occupancy weighting, mutually exclusive (the asm's JZ/JMP pair runs
    // exactly one arm) -- an unarmed/crewless unit (soldier_count == 0) still occupies ONE nominal
    // slot; a crewed unit adds its FULL soldier_count instead, never both. Same "weight by
    // soldier_count, else 1" shape sim_unit_tick.cpp's own identical soldier_count==0 branch
    // documents for a different field (there: tick-budget delta; here: storage.occupancy).
    unit &u = own.unit_at(player, unit_idx);
    if (v.cfg_units[u.unit_proto_id].soldier_count == 0) {
        storage.occupancy += 1;
    } else {
        storage.occupancy += v.cfg_units[u.unit_proto_id].soldier_count;
    }

    // 0x004637c5-0x004637d8: home_storage_slot = (byte)storage_slot (asm loads only the low 8 bits of
    // the stack slot, an explicit narrowing), then state = PARKED.
    u.home_storage_slot = static_cast<uint8_t>(storage_slot);
    u.state             = UNIT_STATE_PARKED;

    // 0x004637fd-0x00463848: snap the unit onto the building's park tile. park_x is CONFIRMED at the
    // right offset; park_y is NOT -- see the header's CRITICAL HAZARD note before trusting this line's
    // Y half. Written here as instructed (a named-field access, not raw offset math) with the
    // discrepancy declared rather than worked around.
    u.x = storage.park_x;
    u.y = storage.park_y;

    // 0x0046384e-0x00463858: fire the UI refresh event, unconditionally, every call.
    c.set_event(kEventBuildProjectsRefresh);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void storage_dock_unit_at_building(uint16_t player, int32_t unit_idx, int32_t storage_slot) {
    sim_state st = state();
    detail::storage_dock_unit_at_building(st.read, st.own, live_storage_dock_unit_at_building_calls(),
                                          player, unit_idx, storage_slot);
}


} // namespace mh::sim
