#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// game_UpdateResourceStats @0x004dbeec. Writes player_at(player).resource_spent[res_id-1] through
// `own`, guarded exactly as above. No callees, so no `_calls` table is needed for this TU.
void update_resource_stats(sim_store &own, int32_t player, int32_t amount, uint32_t res_id);

} // namespace detail

// Live wrapper: the logic applied to state().own. Signature matches the committed prototype in
// addr/mh_export.gen.h (`sig_game_UpdateResourceStats`) and addr/mh_calls.gen.h exactly -- all three
// parameters uint32_t (Ghidra never recovered a narrower/signed type for any of them; the signed
// `amount > 0` guard is applied internally via detail::update_resource_stats's int32_t parameter).
void update_resource_stats(uint32_t player, uint32_t amount, uint32_t res_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
