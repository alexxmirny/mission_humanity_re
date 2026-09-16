//
// sim/sim_bldg_free_record.cpp -- see sim_bldg_free_record.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_free_record_0047ba6c.asm); no Ghidra .c draft was consulted (none
// was exported for this batch).
//
#include "sim/sim_bldg_free_record.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void bldg_free_record(sim_store &own, uint32_t player, int32_t building_index) {
    // CORRECTED 2026-08-22 (reimpl-verify): the ORIGINAL truncates `player` to its low 16 bits
    // (MOVZX word) at all four address-recomputation sites -- the function's real contract is a
    // 16-bit player id despite the EAX:4 parameter storage (same convention the sibling
    // sim_bldg_liftoff_anim.cpp already follows). Use player16, not the raw 32-bit parameter.
    const uint16_t player16 = static_cast<uint16_t>(player);

    // Writes 1/2 (building_id, index) hit buildings[player][building_index] -- their address
    // computation is the full IMUL(player,0x6aa4) + IMUL(building_index,0x111) + ADD pair
    // (0x0047ba89-0x0047ba9a / 0x0047baa5-0x0047bab6).
    building &b = own.building_at(player16, building_index);
    // 0x0047ba9c: building_id = 0.
    b.building_id = 0;
    // 0x0047bab8: index = 0.
    b.index = 0;

    // Writes 3/4 (state, index) hit buildings[player][0], NOT buildings[player][building_index] --
    // CORRECTED 2026-08-22 (reimpl-verify): 0x0047bac1-0x0047bacb and 0x0047bad2-0x0047badc each
    // compute ONLY MOVZX(player)*0x6aa4, with NO `IMUL ...,building_index,0x111` / `ADD` step
    // (unlike writes 1/2 above) -- the function contains exactly two `IMUL ...,0x111` instructions
    // total, both already spent on writes 1/2. So these two writes always target row 0 of the
    // player's building array, regardless of building_index.
    building &b0 = own.building_at(player16, 0);
    // 0x0047bacb: state -= 1.
    b0.state -= 1;
    // 0x0047badc: index -= 1 -- SAME field as write 2 above ONLY when building_index==0; otherwise a
    // DIFFERENT record (buildings[player][0].index) than the one write 2 just set to 0. Net value on
    // buildings[player][0].index is whatever it held minus 1 (not necessarily -1, since write 2 may
    // have touched a different row).
    b0.index -= 1;

    // 0x0047bae3-0x0047baec: `CMP G_PLANET_INDEX,2; JLE <epilogue>; CMP building_index,5` then falls
    // straight into the epilogue with no jump after the second CMP -- both arms of the JLE land on
    // the identical epilogue state, so this block is dead code with zero observable effect and is not
    // reproduced. See the header banner's "THE TRAILING DEAD-CODE BLOCK" note.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_free_record(uint32_t player, int32_t building_index) {
    sim_state st = state();
    detail::bldg_free_record(st.own, player, building_index);
}


} // namespace mh::sim
