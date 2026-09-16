#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game_e_race member values this function compares `race` against. Read directly off this
// function's own CMP immediates at 0x0045ee2c (0x2, the ALIEN arm) and 0x0045ee42 (0x1, the HUMAN
// arm); independently cross-checked against ai_player_tick.cpp's own comment on this EXACT parameter
// ("race = 2 (alien) or 1 (human)"), derived there from a different call site's literal register
// loads (0x004e8c0d/0x004e8c14 in llm_strat_ai_player_tick). Ghidra's own decompile of THIS function
// renders both comparisons as the symbolic `ALIEN`/`HUMAN` (game_e_race enum members), not raw
// literals -- see tmp/decomp/game_GetStartingUnit_0045eded.c -- so per naming-convention rule 17a
// these are that enum's own member names, not local invention. No generated C++ enum exists for
// game_e_race (mh_structs.gen.h types the one struct field carrying this type as a bare `uint32_t`
// with only a `// [game_e_race]` bracket comment -- the same posture as cfg_enum_E_UNIT_TYPE before
// sim_unit_type_predicates.h named its members; see that file's header note), so they are named as
// inline constexpr here rather than left as bare literals at the two comparison sites.
// DECLARED NEED: confirm (or attach) a real game_e_race Ghidra enum type with these two members, and
// hoist these two constants to a shared sim header if/when a second caller needs them (same "hoist on
// third user" precedent sim_unit_type_predicates.h's own header note records for UNIT_TYPE_A_HELI_MOTHER).
inline constexpr uint32_t RACE_ALIEN = 2u;
inline constexpr uint32_t RACE_HUMAN = 1u;

namespace detail {

// game_GetStartingUnit @0x0045eded. Scans cfg_units[1..99] (index 0 never checked) for the first
// prototype whose `.type` matches the race's mother-heli constant -- ALIEN -> UNIT_TYPE_A_HELI_MOTHER,
// HUMAN -> UNIT_TYPE_H_HELI_MOTHER (sim_unit_type_predicates.h) -- and returns that index, or 0 if
// none match. Pure read: touches no sim_store region and calls nothing.
int32_t game_get_starting_unit(const sim_view &v, uint32_t race);

} // namespace detail

// Public wrapper. Signature matches the committed export/call/shadow shape
// (sig_game_GetStartingUnit in addr/mh_export.gen.h): int32_t(uint32_t race).
int32_t game_get_starting_unit(uint32_t race);

namespace detail {
} // namespace detail

} // namespace mh::sim
