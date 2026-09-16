//
// sim/sim_bldg_roster_queries.h -- four small building-roster/cfg-table QUERIES (RI-SIM / SIM1B).
//
//   llm_strat_bldg_find_by_ai_build_and_type @0x004d36c4 (0x46)
//   llm_strat_bldg_count_by_type             @0x004d37f2 (0x9c)
//   llm_strat_bldg_count_by_id                @0x004d388e (0x89)
//   llm_strat_bldg_find_mother_position_indexed @0x004d44d0 (0xf0)
//
// All four take ONLY a `sim_view` -- no store, no callees at all (not even the inert stack-probe
// callee's siblings make an outward call here beyond utils_assert_stack_capacity, which is inert
// per the translator brief). Pure roster/cfg-table reads, so every shadow site below is safe to run
// for real in both arms with nothing to stub.
//
// THREE OF THE FOUR SHARE ONE WALK IDIOM (count_by_type / count_by_id / find_mother_position_
// indexed): `count = (uint16_t)buildings[player][0].index` is the number of OCCUPIED roster slots
// still to visit; walk slot indices 1, 2, 3... A slot whose building_id == 0 is INVISIBLE to the
// walk -- it is skipped without decrementing the remaining count, so only a slot with a nonzero
// building_id both tests the match and decrements the count. Reproduced identically (not
// consolidated into a shared helper per the one-function-in-one-function-out rule) in all three
// bodies below.
//
// find_by_ai_build_and_type is unrelated to that idiom: it walks the cfg Building TYPE table
// (v.cfg_buildings), not a player's roster, bounded INCLUSIVE by v.cfg_building_sec->total (the
// cfg parser's own Building-section record, `.total` only -- see sim_state.h's cfg_building_sec
// comment on why this is a SECOND, independent binding of the identical RID ai/ai_state.h's own
// cfg_unit_section sibling binds, and a DIFFERENT object from the cfg_buildings per-type TABLE).
//
#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER / BUILDING_TYPE_H_MOTHER -- already
                                   // named there (SIM1C, 2026-08-08); NOT redeclared here.
#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_bldg_find_by_ai_build_and_type @0x004d36c4. Searches the cfg Building TYPE table
// v.cfg_buildings[0..v.cfg_building_sec->total] INCLUSIVE (the asm's `CMP EDX,total / JBE` loop-back
// -- index == total is visited) for the first entry whose ai_build id equals ai_build_id AND whose
// type equals building_type; returns 0xffffffff if none. `unused` is the committed first register
// argument (EAX) and is genuinely dead in the body -- EBX/building_type is the real third register
// arg (see the plate); kept in the signature only to match the committed prototype.
uint32_t bldg_find_by_ai_build_and_type(const sim_view &v, uint32_t ai_build_id,
                                        uint32_t building_type);

// llm_strat_bldg_count_by_type @0x004d37f2. Counts how many of `player`'s occupied roster slots
// hold a building whose CFG type (v.cfg_buildings[building_id].type) equals bldg_type -- see the
// header note on the shared live-count walk idiom.
int32_t bldg_count_by_type(const sim_view &v, int32_t player, int32_t bldg_type);

// llm_strat_bldg_count_by_id @0x004d388e. Same walk idiom as bldg_count_by_type, but compares the
// roster slot's building_id DIRECTLY against building_id -- no cfg indirection (do not swap this
// comparison with count_by_type's).
int32_t bldg_count_by_id(const sim_view &v, int32_t player, int32_t building_id);

// llm_strat_bldg_find_mother_position_indexed @0x004d44d0. Same walk idiom again; matches on
// v.cfg_buildings[building_id].type == BUILDING_TYPE_A_MOTHER (0x6) or BUILDING_TYPE_H_MOTHER
// (0x1a) and, on the FIRST match, writes the roster slot's x/y into *out_x/*out_y and returns 1
// immediately (does not keep scanning). *out_x and *out_y are zeroed BEFORE the scan starts, so a
// not-found return (0) still leaves them defined, matching the .c draft and the asm's two `MOV
// dword ptr [.. ],0` at entry.
uint32_t bldg_find_mother_position_indexed(const sim_view &v, int32_t player, uint32_t *out_x,
                                           uint32_t *out_y);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly (the
// drift gate enforces this on export/shadow installation).
uint32_t bldg_find_by_ai_build_and_type(int32_t unused, uint32_t ai_build_id, uint32_t building_type);
int32_t  bldg_count_by_type(int32_t player, int32_t bldg_type);
int32_t  bldg_count_by_id(int32_t player, int32_t building_id);
uint32_t bldg_find_mother_position_indexed(int32_t param_1, uint32_t *param_2, uint32_t *a2);

namespace detail {
} // namespace detail

} // namespace mh::sim
