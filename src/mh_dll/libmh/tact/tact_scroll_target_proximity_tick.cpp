//
// tact/tact_scroll_target_proximity_tick.cpp -- see tact_scroll_target_proximity_tick.h. Translated
// from the DISASSEMBLY (tmp/decomp_tact/llm_tact_scroll_target_proximity_tick_00439005.asm), not
// from Ghidra's .c.
//
#include "tact/tact_scroll_target_proximity_tick.h"


namespace mh::tact {

namespace detail {

void scroll_target_proximity_tick(tact_store &own) {
    // @0x0043901d-0x00439046: the four col margins, then the four row margins, all four derived
    // from a SINGLE re-read of the target tile each (the original re-loads
    // _G_LLM_TACT_TARGET_TILE_COL/ROW from memory for every one of the eight SUB/ADD -- there is
    // nothing in between that could change it, so caching once here is not an observable
    // divergence).
    const int32_t target_col = own.target_tile_col();
    const int32_t target_row = own.target_tile_row();

    const int32_t col_hard_lo = target_col - 5;  // @0x00439022
    const int32_t col_soft_lo = target_col - 10; // @0x0043902d
    const int32_t col_hard_hi = target_col + 5;  // @0x00439038
    const int32_t col_soft_hi = target_col + 10; // @0x00439043
    const int32_t row_hard_lo = target_row - 7;  // @0x0043904e
    const int32_t row_soft_lo = target_row - 12; // @0x00439059
    const int32_t row_hard_hi = target_row + 7;  // @0x00439064
    const int32_t row_soft_hi = target_row + 12; // @0x0043906f

    // @0x00439075-0x0043907f and @0x004390af-0x004390b9: same re-derivation reasoning as above --
    // the blast marker cannot change mid-function, so one read of each suffices.
    const int32_t blast_col = own.blast_marker_col();
    const int32_t blast_row = own.blast_marker_row();

    // @0x00439075-0x004390a3: the hard-box test (col AND row, both signed, both inclusive).
    const bool in_hard_box = blast_col >= col_hard_lo && blast_col <= col_hard_hi &&
                             blast_row >= row_hard_lo && blast_row <= row_hard_hi;
    if (in_hard_box) {
        // @0x004390a3: inside the hard box -> hard reset.
        own.squad_bb_target_energy_pct() = 0;
        return;
    }

    // @0x004390af-0x004390d9: the soft-box test, reached only after the hard-box test failed.
    const bool in_soft_box = blast_col >= col_soft_lo && blast_col <= col_soft_hi &&
                             blast_row >= row_soft_lo && blast_row <= row_soft_hi;
    if (!in_soft_box) {
        // @0x004390db/0x004390cf/0x004390c3 -> 0x004390f9: outside BOTH boxes -- the third exit
        // path, and the only one that touches nothing at all.
        return;
    }

    // @0x004390dd-0x004390f9: inside the soft ring but not the hard box -- threshold-gated decay.
    if (own.squad_bb_target_energy_pct() < 0x32) {
        // @0x004390e6: below 50 -> hard reset, same as the hard-box case.
        own.squad_bb_target_energy_pct() = 0;
    } else {
        // @0x004390f2: at or above 50 -> subtract 50 (not a floor-to-zero clamp; the original's own
        // SUB instruction, reproduced literally).
        own.squad_bb_target_energy_pct() -= 0x32;
    }
}

} // namespace detail

void scroll_target_proximity_tick() {
    tact_state st = state();
    detail::scroll_target_proximity_tick(st.own);
}


} // namespace mh::tact
