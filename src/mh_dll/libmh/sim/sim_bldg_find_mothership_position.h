//
// sim/sim_bldg_find_mothership_position.h -- llm_strat_bldg_find_mothership_position @0x0048fdd0
// (0x11f bytes), translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_find_mothership_position_0048fdd0.asm), not from the Ghidra .c draft
// (which is structurally right on every branch and store, but uses the NON-NaN-safe `0.0 <
// energy` form for the FLDZ/FCOMP/FNSTSW/SAHF/JNC idiom -- re-derived from the raw instruction
// sequence per translator brief rule 16/9, same correction sim_bldg_mother_reelect_primary.cpp's
// two energy gates already document for the identical idiom).
//
// Scans a player's building roster for the FIRST occupied A_MOTHER(0x06)/H_MOTHER(0x1a) building
// (cfg type) and returns its tile (x,y) via the two OUT params. cf. `find_mother_position_indexed`
// (0x004d3fd0, NOT in this batch), which does the same job via a different (`.index`-based) walk.
//
// THE ACTIVE-COUNT IDIOM (same shape as sim_bldg_mother_reelect_primary.cpp's building search, and
// sim_bldg_refresh_all_buildings.h's `.index`-keyed sibling): `buildings[player][0]` is the
// roster's SENTINEL/COUNT slot, never a real building -- its `.energy` field (NOT a real building's
// HP/charge; see docs/conventions.md#energy-is-not-power) holds the live-building headcount as a double,
// truncated toward zero (utils_math_trunc @0x004d0596, ST0-in/ST0-out, inlined here rather than
// called -- see the .cpp's trunc_to_int32(), the sim_bldg_mother_reelect_primary.cpp /
// sim_unit_population_remove.cpp precedent's exact instruction sequence, confirmed at THIS site's
// own FISTP opcode bytes `db 5d ec` @0x0048fe01 -> 0xDB ModRM 0x5D, reg field 3 -> 0xDB /3 ==
// FISTP m32int, 32-bit). The walk starts at slot 1 (slot 0 is the count field itself) and is
// bounded by TWO ANDed conditions, either ending it: `slot < 100` (BUILDINGS_PER_PLAYER) and
// `remaining_count != 0` -- remaining_count is decremented only on a live slot (energy > 0), so an
// inaccurate count drives it to 0 early or leaves slots unvisited; NOT clamped, matching the
// sibling idiom's own "preserved, not clamped" note.
//
// THE ENERGY GATE IS `!(energy <= 0.0)`, NOT `0.0 < energy` -- see sim_bldg_mother_reelect_primary
// .cpp's identical FLDZ/FCOMP/FNSTSW/SAHF/JNC precedent: fallthrough (CF=1, no jump) is the "alive"
// path and matches on a NaN energy where the Ghidra draft's `0.0 < energy` form would not; no
// reachable state is known to put a NaN in this field, so this is a zero-cost faithfulness fix.
//
// PER-CANDIDATE ORDER, read directly off the asm rather than assumed: energy gate first: if alive,
// remaining_count is decremented UNCONDITIONALLY (0x0048fe45-0x0048fe48) BEFORE the type gate --
// preserved in that order even though it has no further effect once the type gate fails, matching
// sim_bldg_mother_reelect_primary.cpp's identical ordering note.
//
// TYPE GATE: cfg_buildings[building.building_id].type == BUILDING_TYPE_A_MOTHER(0x06) (checked
// first) OR == BUILDING_TYPE_H_MOTHER(0x1a) (checked second, only if the first missed) -- both
// constants reused from sim_order_enqueue.h's BUILDING_TYPE_* block (same reuse
// sim_bldg_mother_reelect_primary.cpp already established), NOT redeclared locally. The building_id
// is re-fetched independently for each of the two CMPs (0x0048fe71-0x0048fe95 recomputes the row/
// slot address and re-reads building_id from scratch rather than reusing the first read) -- observed
// here as a single reference is value-identical (no write can occur between the two reads in this
// straight-line body), same posture sim_bldg_get_coords.cpp's header documents for its own
// redundant re-fetches.
//
// ON THE FIRST MATCH: writes `*out_x = building.x` and `*out_y = building.y` (both uint8_t fields,
// MOVZX BYTE at 0x0048fea7/0x0048fec3, zero-extended into the uint32_t out params -- translator
// brief rule 7, no widening beyond the struct's own field type) and returns 1, WITHOUT visiting any
// further slot (the loop's `break` -- reassembled from the asm's straight fallthrough into
// LAB_0048fe97 from either type-match branch). If the walk exhausts (slot>=100 or remaining_count
// hits 0) with no match, returns 0 and leaves *out_x/*out_y UNTOUCHED (matches the asm: the exit
// path at LAB_0048fedd only ever sets the result-flag local, never touches EDX/EBX's out-pointers).
//
// PURE / NO-WRITE, NO OUTWARD CALL (other than the inlined trunc): the body's only real CALL is the
// inert utils_assert_stack_capacity prologue (translator brief rule 6 -- omitted) plus the inlined
// utils_math_trunc sequence. No `_calls` indirection struct is needed, same posture as
// sim_bldg_get_coords.h's four-function family.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_find_mothership_position @0x0048fdd0. Signature mirrors the committed export/call
// shape (out params are raw uint32_t* here, matching the asm header's own `uint *out_x, *out_y` --
// the public wrapper below carries the same `uint32_t*` pointee (TACT1-P C6, 2026-09-04), matching
// addr/mh_calls.gen.h's `llm_strat_bldg_find_mothership_position(int32_t, uint32_t*, uint32_t*)`).
uint32_t bldg_find_mothership_position(const sim_view &v, int32_t player, uint32_t *out_x, uint32_t *out_y);

} // namespace detail

// Live wrapper: the logic applied to state().read. Signature matches the committed __watcall shape
// already in addr/mh_calls.gen.h and addr/mh_export.gen.h exactly.
uint32_t bldg_find_mothership_position(int32_t player, uint32_t *out_x, uint32_t *out_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
