//
// sim/sim_tile_neighbor_reverse_dir.cpp -- see sim_tile_neighbor_reverse_dir.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_tile_neighbor_reverse_dir_0048b308.asm). The two-level table
// indirection (dir_remap_table[dir].step_primary -> dir_step_offsets[step].{dx,dy}), the SUB-then-AND
// order, and the unsigned wrap were re-derived from the listing per house rules.
//
#include "sim/sim_tile_neighbor_reverse_dir.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void tile_neighbor_reverse_dir(const sim_view &v, int32_t tile_x, int32_t tile_y, int32_t dir_index,
                               uint32_t *out_x, uint32_t *out_y) {
    // 0x0048b329-0x0048b338: dir_remap_table[dir_index].step_primary, then it indexes
    // dir_step_offsets (the SHL 3 in the listing is the *8 stride of the {int dx; int dy;} entries,
    // which the typed array index below performs implicitly).
    const int32_t                 step = v.dir_remap_table[dir_index].step_primary;
    const mh::game::mh_llm_vec2i &off  = v.dir_step_offsets[step];

    // 0x0048b33b-0x0048b34c / 0x0048b35d-0x0048b371: subtract the step, then mask -- SUB binds before
    // AND. The mask makes the result unsigned-wrapped into [0, dim).
    *out_x = static_cast<uint32_t>(tile_x - off.dx) & map_width_mask(v);
    *out_y = static_cast<uint32_t>(tile_y - off.dy) & map_height_mask(v);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void tile_neighbor_reverse_dir(int32_t tile_x, int32_t tile_y, int32_t dir_index, uint32_t *out_x,
                               uint32_t *out_y) {
    const sim_view v = state().read;
    detail::tile_neighbor_reverse_dir(v, tile_x, tile_y, dir_index, out_x, out_y);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
