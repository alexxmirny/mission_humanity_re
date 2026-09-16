#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_unit_create_soldier @0x00463ac6. Returns the newly-placed unit's slot index (1..99), or 0 on
// failure (population cap hit, no placeable tile found within `width` outward-scan attempts).
//
// is_special_flag: `param_5 == 1` reduces the population-cap the direct-placement slot search stays
// under by 9 (slot search bound becomes `100 - 9` instead of `100`) -- name/purpose of the "9" is not
// otherwise recovered; kept as a bare literal matching the asm's own `MOV ..., 0x9` (not a
// Ghidra-named constant anywhere in the image).
//
// PARAMETER ORDER matches the committed `sig_llm_unit_create_soldier` / addr/mh_calls.gen.h prototype
// exactly: (tile_x, tile_y, unit_proto_id, player, is_special_flag).
uint32_t create_soldier(const sim_view &v, sim_store &own, uint32_t tile_x, uint32_t tile_y,
                        uint16_t unit_proto_id, uint16_t player, char is_special_flag);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// `__mh_watcall_ecx_ebx_volatile` shape (sig_llm_unit_create_soldier) -- see the header note above on
// why no special marshalling is needed for the "_ecx_ebx_volatile" tag itself.
uint32_t unit_create_soldier(uint32_t tile_x, uint32_t tile_y, uint16_t unit_proto_id, uint16_t player,
                             char is_special_flag);

namespace detail {
} // namespace detail

} // namespace mh::sim
