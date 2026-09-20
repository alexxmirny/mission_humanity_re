//
// sim/sim_bldg_pay_costs.h -- two 'can-afford-and-pay' resource checks bundled by address
// neighborhood AND by shared shape (RI-SIM / SIM1B building_tick machinery slice):
//
//   llm_bldg_pay_build_cost       @0x00492eb1 (0x148 bytes)
//   llm_strat_bldg_pay_cycle_inputs @0x00492ff9 (0x133 bytes)
//
// Both walk a 7-slot (id,val) cfg resource list (CFG_RESOURCE_SLOTS, sim_state.h) against
// v.player_resources (player_resource_of()) and, if every slot is affordable, charge via
// game_SpendResource (mh_calls.gen.h, already committed) -- a REAL outward effect: player_resources
// is written only through this call in the sim closure (see sim_state.h's own comment on the
// `progress`/`player_resources` view members, added for exactly this pair of functions). Indirected
// through a one-member `pay_costs_calls` struct, same reasoning as every other module here (a direct
// mh::call:: inside detail:: would be untestable by net_selftest.exe simtest / the offline fixture).
//
// THE TWO DIFFER IN THREE WAYS, all load-bearing:
//
//   (1) pay_build_cost gates on the prerequisite invention BEFORE the resource walk at all:
//       `progress_of(v, player, Building[building_type_id].invention).available == 0` returns 0x13
//       immediately, no resource walk (0x00492ecb-0x00492efb). pay_cycle_inputs has NO such gate --
//       it goes straight from the building lookup into the resource walk.
//   (2) pay_build_cost indexes Building[] DIRECTLY by `building_type_id` (a cfg TYPE id, passed by
//       the caller, not a roster index). pay_cycle_inputs takes a roster `b_index`, reads
//       `buildings[player][b_index].building_id` (the INSTANCE's cfg type), and indexes Building[]
//       by THAT.
//   (3) pay_build_cost walks `Building[type].resource[]`; pay_cycle_inputs walks
//       `Building[type].resource_2[]` -- a DIFFERENT array on the SAME cfg_building record (resource
//       ends at struct offset 0x1df/56 bytes = 7*8, `build_time_2` (a double) sits at +0x38..+0x40,
//       then resource_2 begins at +0x40 -- confirmed by the two functions' own id-field addresses,
//       0xd9f376 vs 0xd9f3b6, exactly 0x40 apart). Do not conflate the two arrays.
//
// BOTH share the identical error-accumulation shape over their own resource array, which is
// deliberate multi-shortage signalling and is reproduced literally, not "fixed":
//
//   - The scan does NOT short-circuit on the first unaffordable slot -- it keeps walking every slot
//     up to 7 (or until `.id == UNDEFINED(0)`, cfg_enum_E_RESOURCE member 0 -- see
//     sim_bldg_defense_cost.h's own precedent for the same sentinel, no C++ enum committed for this
//     domain in this codebase yet).
//   - The FIRST unaffordable slot sets the accumulator to `resource_id + 0x89`.
//   - Any SECOND (or later) unaffordable slot OVERWRITES the accumulator to the bare sentinel
//     `0x89`, discarding which resource(s) were short. This is read directly off the asm (both
//     functions: `CMP accumulator,0 / JZ / (set to id+0x89) ... else (set to bare 0x89)`) -- not a
//     bug, a deliberate "multiple things are short, don't bother saying which" collapse.
//   - If the walk finishes with the accumulator still 0 (everything affordable), a SECOND pass over
//     the SAME resource array actually calls `game_SpendResource(player, id, val)` per slot, and the
//     function returns 0. If the accumulator is nonzero, that second pass never runs and the
//     accumulator is returned as the error/reason code.
//
// THE id-READ-BEFORE-BOUND-CHECK ORDER (both loops, both passes): the asm reads `resource[i].id`
// FIRST (unconditionally on loop-body entry) and tests it against UNDEFINED, THEN tests `i < 7`
// second (0x00492f26/0x00492f2c for pay_build_cost's first pass; same shape in its second pass and in
// both of pay_cycle_inputs' passes) -- same evaluation order sim_unit_refund.cpp's walk documents.
// Reproduced literally as an unbounded `for(;;)` with two ordered breaks, matching that precedent.
// UNLIKE that precedent, here it is PROVABLY behaviourally inert regardless of what a shipped cfg
// contains: since the loop index only ever increments by 1 from 0, `i` can equal exactly 7 for the
// first time only when every one of slots 0..6 had a non-UNDEFINED id (i.e. the id-check never fired
// early) -- and at that exact point the SECOND check (`i < 7`) is guaranteed false, so the loop
// exits unconditionally on the very next check regardless of what garbage bytes the out-of-bounds
// `resource[7].id` read happens to contain (it reads into `build_time_2`'s low bytes for
// pay_build_cost's `resource` array, and into `build_time_d`'s low bytes for pay_cycle_inputs'
// `resource_2` array -- confirmed adjacent by addr/mh_structs.gen.h's own layout). So the OOB read
// has no discoverable effect on the return value or on which resources get charged; noted here rather
// than silently clamped, per house style, but NOT carried into uncertainties[] since the equivalence
// is proven rather than assumed.
//
// Both functions truncate `player` to its low 16 bits before EVERY use (roster/progress/
// player_resources indexing and the game_SpendResource call) -- `MOVZX ..., word ptr [player]` at
// every site in both functions (e.g. 0x00492edf/0x00492fd5 for pay_build_cost; 0x00493016/0x00493108
// for pay_cycle_inputs). Reproduced as `player & 0xffffu` once per function, matching the .c drafts'
// own `player & 0xffff` at every site.
//
// Both return `int` (0 = success, nonzero = an error/reason code -- 0x13 is the "invention not
// researched" code, 0x89.. codes are cfg::G_TEXT_PTRS-indexed resource-shortage reasons per
// sim_bldg_placement_preview.h's sibling family and llm_bldg_pay_build_cost's own existing plate).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call both functions make. See the header banner on why this is a one-member struct
// rather than a direct `mh::call::` inside `detail::`. game_SpendResource is the sim closure's ONLY
// writer of player_resources (sim_state.h's own comment on the `player_resources` view member) --
// this struct is that write, made testable.
struct pay_costs_calls {
    void (*spend_resource)(int32_t player, int32_t resource_id, int32_t amount); // game_SpendResource @0x00497f94
};

const pay_costs_calls &live_pay_costs_calls();

namespace detail {

// mp:D25 (2026-09-19) -- PASS 1 of llm_bldg_pay_build_cost ALONE: the invention gate + the
// 7-slot shortage scan, returning the SAME code pay_build_cost would (0 = affordable, 0x13 =
// invention not researched, id+0x89 / bare 0x89 = short), and charging NOTHING. pay_build_cost is
// this followed by the charge pass. It exists for the one caller that needs the verdict without the
// payment: the HUD build-menu click (sim/resid/sim_bldg_try_begin_placement) -- see that header.
int32_t bldg_can_afford_build_cost(const sim_view &v, uint32_t player, int32_t building_type_id);

// llm_bldg_pay_build_cost @0x00492eb1. See the header banner for the full derivation.
int32_t bldg_pay_build_cost(const sim_view &v, const pay_costs_calls &gc, uint32_t player,
                            int32_t building_type_id);

// llm_strat_bldg_pay_cycle_inputs @0x00492ff9. See the header banner for the full derivation.
int32_t bldg_pay_cycle_inputs(const sim_view &v, const pay_costs_calls &gc, uint32_t player,
                              uint32_t b_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

int32_t bldg_pay_build_cost(uint32_t player, int32_t building_type_id);
int32_t bldg_pay_cycle_inputs(uint32_t player, uint32_t b_index);
// Not an original: the side-effect-free half of bldg_pay_build_cost (mp:D25). Same return codes.
int32_t bldg_can_afford_build_cost(uint32_t player, int32_t building_type_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
