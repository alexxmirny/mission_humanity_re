//
// sim/sim_bldg_defense_cost.h -- two small building queries bundled by address neighborhood, not by
// shared logic (RI-SIM):
//
//   llm_strat_bldg_max_defense_radius_sq @0x004d3e61 (0x1e0 bytes) -- confidence: med
//   llm_strat_bldg_total_resource_cost   @0x004e2d52 (0x3f bytes)
//
// ---- llm_strat_bldg_max_defense_radius_sq ------------------------------------------------------
//
// `uint __mh_watcall_ebx_volatile llm_strat_bldg_max_defense_radius_sq(int player, int x, int y)`
// per the .asm header (player=EAX, x=EDX, y=EBX) -- matches mh_calls.gen.h's
// `llm_strat_bldg_max_defense_radius_sq(int32_t player, int32_t x, int32_t y)`.
//
// TWO INDEPENDENT SCANS OVER THE PLAYER'S BUILDING ROSTER, both using the SAME live-count idiom the
// ai domain's roster walkers already use (ai_nearest_flagged.cpp / ai_build_sources.cpp / etc.):
// `remaining = (uint16_t)buildings[player][0].index` is the live building count, decremented only
// when a non-empty slot (`building_id != 0`) is consumed -- a hole in the roster costs an index
// increment but not a budget decrement (read off 0x004d3ed8's JZ landing PAST the DEC, same shape as
// every ai:: precedent).
//
//   (1) 0x004d3e7c-0x004d3f33: only entered if the incoming `x < 0` -- the caller's signal to use the
//       player's own Mothership position instead of an explicit point. Walks the roster looking for a
//       building whose cfg type is H_MOTHER (0x1a) or A_MOTHER (0x6); on the first match, `x`/`y` are
//       overwritten with that building's own (byte) x/y and the scan stops immediately (break, not
//       continue -- the original does not keep looking for a second Mothership). If no match is found
//       before the live count is exhausted, `x`/`y` are left exactly as the caller passed them (still
//       negative).
//   (2) 0x004d3f3f: `if ((int)x < 0) return 0xffffffff` -- a SENTINEL, not a real squared distance,
//       reached either because the caller passed a negative point with no mothership search
//       (impossible: x<0 always triggers (1)) or, in practice, because (1) ran and found nothing. The
//       second scan below never runs in that case.
//   (3) 0x004d3f4f-0x004d4034: walks the roster a second time (same live-count idiom, independent
//       `remaining`/`i`), computing `llm_strat_toroidal_dist_sq(x, y, building.x, building.y)` for
//       every occupied slot whose cfg type is NONE of {A_MOTHER, H_MOTHER, A_MINE, H_MINE, A_TURRET,
//       H_TURRET, A_RELAY, H_RELAY} (eight CMPs, read directly off 0x004d3fc0-0x004d400a; the
//       BUILDING_TYPE_* constants are sim_order_enqueue.h's own, already pinned from a Ghidra enum
//       dump -- reused here rather than re-declared), and keeps the MAXIMUM (`CMP EAX,EDI; JBE` skips
//       the update on a tie or a smaller value, at 0x004d402a). Returns 0 if the roster has no
//       qualifying building at all (EDI initialised to 0 at 0x004d3f4f, never updated).
//
// `llm_strat_toroidal_dist_sq`'s argument order, read off the __cdecl push sequence (right-to-left:
// building.y, building.x, y, x pushed in that order, so the first/leftmost C argument is the LAST
// value pushed): `toroidal_dist_sq(origin_x, origin_y, building.x, building.y)`.
//
// mh_shadow.gen.h already records this function as having NO measured writable region (pure query),
// so the shadow arm below only adds a trace line, matching sim_unit_type_predicates.cpp's five
// predicates.
//
// ---- WHY toroidal_dist_sq IS INDIRECTED THROUGH A ONE-MEMBER CALLS STRUCT ------------------------
//
// llm_strat_toroidal_dist_sq is a committed, marshallable __cdecl wrapper in mh_calls.gen.h, and it is
// PURE (no state write, confirmed by every ai:: site that calls it "for real" under a shadow arm). A
// direct `mh::call::llm_strat_toroidal_dist_sq` call from inside `detail::` would still jump to a
// fixed VA that only exists inside the loaded game image, which is exactly the reason
// sim_facing24_from_points.h indirects its own single pure-math callee (`llm_math_atan`) through a
// one-member `_calls` struct despite it being just as marshallable and just as pure -- and it is the
// reason EVERY ai:: site that calls toroidal_dist_sq (ai_nearest_flagged.cpp, ai_target_query.cpp,
// ai_site_scan.cpp, ai_engage_scan.cpp, ...) does so through `gc.toroidal_dist_sq(...)`, never through
// `mh::call::` directly. Keeping that precedent here (rather than the literal "call it directly, no
// struct needed" phrasing this unit's brief suggested) is what keeps `detail::bldg_max_defense_radius_
// sq` drivable by `net_selftest.exe simtest` over heap buffers, with no game process required.
//
// ---- llm_strat_bldg_total_resource_cost --------------------------------------------------------
//
// `int __watcall llm_strat_bldg_total_resource_cost(int building_type)` per the .asm header
// (building_type=EAX) -- matches mh_calls.gen.h's `llm_strat_bldg_total_resource_cost(int32_t
// building_type)`. NO callees at all (not even the inert stack-probe is followed by any other CALL).
//
// Sums `Building[building_type].resource[i].val` for i = 0.. while `resource[i].id != 0` (the
// UNDEFINED sentinel, cfg_enum_E_RESOURCE member 0) AND i < CFG_RESOURCE_SLOTS (7) -- but unlike
// sim_unit_refund.cpp's cfg_unit walk, THIS loop tests `i < 7` FIRST: 0x004e2d66 jumps straight to the
// bottom-of-loop test (`CMP EDX,0x7 / JC`) before the body ever runs, so the body (which reads
// `resource[i].id`) is only entered for i in [0,6] -- `resource[7]` is never read, no out-of-bounds
// concern here (contrast sim_unit_refund.h's own note on why ITS walk can read one slot past the
// declared extent). On the first slot whose `id == 0`, the CMP at 0x004e2d75 jumps straight to the
// function's single exit (0x004e2d8a, `MOV EAX,EBX`) -- a BREAK, not a "skip this slot and keep
// scanning" continue, so a defined cost sitting behind an undefined slot contributes nothing. This
// matches the .c draft's own corrected plate exactly (see tmp/decomp/llm_strat_bldg_total_resource_
// cost_004e2d52.c) -- translated from the disassembly per house rules regardless.
//
// mh_shadow.gen.h records this one too as having no measured writable region.
//
#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/H_MOTHER/A_MINE/H_MINE/A_TURRET/H_TURRET/
                                   // A_RELAY/H_RELAY -- already pinned there (SIM1C), reused rather
                                   // than re-declared (would be a C2374/C2086 redefinition otherwise).
#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call llm_strat_bldg_max_defense_radius_sq makes. See the header banner on why this
// is a one-member struct rather than a direct `mh::call::` inside `detail::`.
struct bldg_defense_cost_calls {
    uint32_t (*toroidal_dist_sq)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // llm_strat_toroidal_dist_sq @0x00669f90
};

const bldg_defense_cost_calls &live_bldg_defense_cost_calls();

// The logic over an explicit view + calls table, matching every other sim TU's split.
namespace detail {

// llm_strat_bldg_max_defense_radius_sq @0x004d3e61. See the header banner for the full derivation.
uint32_t bldg_max_defense_radius_sq(const sim_view &v, const bldg_defense_cost_calls &c,
                                    int32_t player, int32_t x, int32_t y);

// llm_strat_bldg_total_resource_cost @0x004e2d52. See the header banner. No callees, no calls table.
int32_t bldg_total_resource_cost(const sim_view &v, int32_t building_type);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly.
uint32_t bldg_max_defense_radius_sq(int32_t player, int32_t x, int32_t y);
int32_t  bldg_total_resource_cost(int32_t building_type);

namespace detail {
} // namespace detail

} // namespace mh::sim
