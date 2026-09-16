//
// sim/sim_bldg_uses_workers.cpp -- see sim_bldg_uses_workers.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_uses_workers_004988b0.asm), not from the Ghidra .c draft's plate prose
// (which is internally inconsistent about the field's name -- see the header's DECLARED NEED note);
// the draft's BODY line itself was confirmed against the raw bytes and matches exactly.
//
#include "sim/sim_bldg_uses_workers.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t bldg_uses_workers(const sim_view &v, uint32_t player, int32_t building_index) {
    // 0x004988cd-0x004988e4: buildings[player][building_index].building_id, re-read fresh every call
    // (translator brief rule 16: `const` on sim_view is not a promise of stability) via the standard
    // roster helper.
    const building &b = building_of(v, player, building_index);

    // 0x004988e4-0x004988f1: cfg_buildings[b.building_id].staffs_workers -- the per-TYPE 0/1 flag (see
    // the header's DECLARED NEED: this is the field the plate calls "field48/49_0x6f5"; renamed in
    // Ghidra 2026-08-13 from the auto-generated `_unnamed_0x6f5` placeholder to `staffs_workers`).
    // Widened byte -> int32_t, matching the MOVZX/return.
    return (int32_t)v.cfg_buildings[b.building_id].staffs_workers;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t bldg_uses_workers(uint32_t player, int32_t building_index) {
    const sim_view v = state().read;
    return detail::bldg_uses_workers(v, player, building_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
