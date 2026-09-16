#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra enum (rule 17a fallback) -----------
// A genuinely closed two-value domain (found / exhausted), but no `E_*` enum for it was found anywhere
// in the tree (checked docs/symbols.md, docs/structs.md, the DTM categories) -- declared_needs item,
// not a licence to invent one silently. Named to match the vocabulary the two existing callers already
// use in their own comments (see the header banner above), not re-spelled.
inline constexpr uint8_t UNIT_SELECT_WEAPON_FOUND     = 4;    // a weapon slot matched target_mask
inline constexpr uint8_t UNIT_SELECT_WEAPON_NOT_FOUND = 0x64; // loop exhausted (weapon_id==0 or 4 slots scanned)

namespace detail {

// llm_strat_unit_select_weapon @0x0048ba9e. See the header banner above for the full derivation.
uint8_t unit_select_weapon(const sim_view &v, uint16_t player, int32_t unit_index, uint32_t target_mask);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_unit_select_weapon) exactly.
uint8_t unit_select_weapon(uint16_t player, int32_t unit_index, uint32_t target_mask);

namespace detail {
} // namespace detail

} // namespace mh::sim
