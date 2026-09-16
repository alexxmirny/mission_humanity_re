//
// state/rebind_shims.h -- R3's cast shims: the four seamless rows whose public wrapper is
// signature-COMPATIBLE but not signature-IDENTICAL to the committed callee shape.
//
// HAND-WRITTEN, deliberately, and small enough to stay that way. The libmh rebind notes R1 prefers the
// public live wrapper and R2 derives it; where a wrapper's signature differs, R1's arm-class
// exception routes through the MH_*_REPLACE adapter instead, because that adapter is pinned to
// `sig_<fn>` by its own macro. These four rows have NO seam and therefore no adapter -- they are the
// intersection of "seamless" and "wrapper types improved on the original's" -- so the bridge has to
// exist somewhere, and one reviewed file of four forwarders beats teaching the generator to invent
// casts. Every difference below is representation-preserving and named:
//
//   llm_strat_bldg_record_resource_expenditure_stats  uint32_t player_id  -> the wrapper's int32_t
//   llm_strat_order_integrity_check                   mh_llm_strat_order* -> the wrapper's
//                                                     `const order *` (same type, const added)
//   llm_strat_spawn_invasion_force                    the wrapper RETURNS uint32_t, the committed
//                                                     shape says int32_t (same 32 bits)
//   llm_strat_unit_calc_range_approach_point          committed uint32_t * out-params -> the callee
//                                                     body's int32_t * (TACT1-P C6, 2026-09-04: the
//                                                     wrapper now carries the committed pointee and
//                                                     reinterpret_casts into the callee)
//
// If a fifth row ever needs one, ask first whether its wrapper should simply be declared with the
// committed shape -- these four exist because the improved typing is worth keeping at the C++ call
// sites, not because a shim is a good default.
//
#pragma once
#include <cstdint>

namespace mh::game {
struct mh_llm_strat_order;
}

namespace mh::rebind::shim {

void    bldg_record_resource_expenditure_stats(uint32_t player_id, int32_t building_id);
void    order_integrity_check(::mh::game::mh_llm_strat_order *order, char *tag);
int32_t spawn_invasion_force(uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                             int32_t home_tile_y, int32_t invasion_points);
int32_t unit_calc_range_approach_point(uint32_t *io_target_x, uint32_t *io_target_y);

} // namespace mh::rebind::shim
