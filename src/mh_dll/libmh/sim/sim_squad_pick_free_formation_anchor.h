#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_squad_pick_free_formation_anchor @0x004899ed.
//
// The candidate table is the squad-placement offset table (`v.squad_placement_offset_table`,
// byte[288] = llm_squad_placement_offset[6][6] @0xae3618 -- REBASED 2026-09-02 by LT0 from the old
// interior byte[248] view @+0x28). This loop reads the fixed col-5 column, indexed by SLOT (= row)
// 1..5 inclusive (slot 0 is skipped by the original -- the loop counter is seeded to 1, not 0);
// x is the byte at 0x28+slot*0x30+0, y at 0x28+slot*0x30+4, both read as signed bytes (the compare
// against the scratch table sign-extends via MOVSX).
//
// For each candidate slot in order, scan `v.squad_anchor_scratch[0 .. existing_count-1]` (the
// in-use list) for an entry whose (x, y) equals the candidate's. The first candidate with NO match
// is written out through `out_x`/`out_y` (a raw byte store, matching the original's `char *` out
// params -- not `int32_t *`) and the function returns.
//
// If ALL 5 candidates are already in use, the original falls out of the loop without ever writing
// through either out pointer -- `*out_x`/`*out_y` are left untouched by this function in that case.
// That is the original's exact behaviour (there is no "return false" signal; the caller is trusted
// not to reach this path with an already-full in-use list), so it is preserved here, not "fixed"
// with a default write.
void squad_pick_free_formation_anchor(const sim_view &v, int32_t existing_count, char *out_x, char *out_y);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the original's __watcall shape
// (EAX=existing_count, EDX=out_x, EBX=out_y).
void squad_pick_free_formation_anchor(int32_t existing_count, char *out_x, char *out_y);

} // namespace mh::sim
