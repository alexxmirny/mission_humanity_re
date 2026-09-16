//
// sim/sim_storage_exit_query.cpp -- see sim_storage_exit_query.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_exit_tile_is_clear_00489cff.asm), address-by-address, per house
// rules.
//
#include "sim/sim_storage_exit_query.h"

namespace mh::sim {

namespace detail {

uint32_t storage_exit_tile_is_clear(const sim_view &v, uint16_t player, int32_t storage_slot) {
    // 0x00489d16-0x00489d2f / 0x00489d38-0x00489d4b / 0x00489d5c-0x00489d6f / 0x00489d78-0x00489d8b:
    // the original re-reads exit_tile_x/exit_tile_y from storage_of() FOUR times total (twice each,
    // once per gate below) rather than caching them in a register across the two gates -- reading each
    // field once here is behaviourally identical since nothing between the two gates can write
    // unit_storage (no CALL in this body at all).
    const unit_storage &st = storage_of(v, player, storage_slot);
    const int32_t       x  = st.exit_tile_x;
    const int32_t       y  = st.exit_tile_y;

    // 0x00489d2f-0x00489d5a: gate 1 -- the tile must be passable. `(x<<8)+y` is the SAME
    // element-index arithmetic tile_at()'s own `(tile_x<<8)|tile_y` uses, here against the 1-byte-
    // stride `passable` plane rather than the 8-byte-stride tile_objects plane. If the byte is zero
    // (blocked), the original jumps straight to the reject path -- gate 2 is never reached.
    if (v.passable[(x << 8) | y] == 0) return 0;

    // 0x00489d5c-0x00489d9e: gate 2 -- reached only if gate 1 passed. The tile must have no building on
    // it (tile_at(x, y).building == 0). A UNIT occupying the tile is not checked here at all -- this
    // body never reads tile_at().unit.
    if (tile_at(v, x, y).building != 0) return 0;

    return 1;
}

} // namespace detail


uint32_t storage_exit_tile_is_clear(uint16_t player, int32_t storage_slot) {
    const sim_view v = state().read;
    return detail::storage_exit_tile_is_clear(v, player, storage_slot);
}

} // namespace mh::sim
