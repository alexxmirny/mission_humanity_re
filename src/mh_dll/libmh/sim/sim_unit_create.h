#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the module's usual reason (a direct mh::call:: inside a detail:: body reaches into
// the live game image, making the body untestable by net_selftest.exe simtest) -- see the header's
// CALLEES note above for why this applies doubly here (two of the five are sibling translations in
// this very batch).
struct unit_create_calls {
    // map_unit_Add @0x00461e9e -- adds the unit to the roster/map bookkeeping; stamps
    // units[player][slot].unit_proto_id (everything below re-reads it off the record rather than
    // reusing the unit_proto_id parameter -- see the header hazard). First arg is the SLOT
    // (addr/mh_calls.gen.h names it `unit`), second is the proto id (`unit_proto`).
    void (*unit_add)(uint32_t slot, int32_t unit_proto_id, uint16_t player);

    // map_unit_PutOnMap @0x00486c6a -- places the unit at tile (x,y), BOTH truncated to a byte here
    // (unlike the later fow_update_plus call below) -- see the header hazard.
    void (*unit_put_on_map)(uint16_t player, uint16_t slot, uint8_t x, uint8_t y);

    // llm_strat_population_add @0x00491328 -- global/economy population bookkeeping, gated on
    // Unit[proto].human != 0 (see the header hazard).
    void (*population_add)(uint16_t player, int32_t count);

    // map_fow_UpdateFoWPlus @0x0049681a -- reveals the new unit's sight radius. x/y here are the
    // FULL untruncated parameters, unlike unit_put_on_map above.
    void (*fow_update_plus)(uint32_t player, uint32_t x, uint32_t y, uint8_t sight);

    // llm_strat_ai_notify_unit_lifecycle @0x004dbb38 -- kind 4 = "unit appeared" (same bare literal
    // sim_unit_create_soldier.h / sim_unit_spawn_on_tile.h / sim_unit_recruit.h already use; no
    // generated C++ enum exists for this domain -- see those siblings' identical note). Last call.
    void (*ai_notify_unit_lifecycle)(uint16_t player, uint16_t unit_type, uint32_t unit_id,
                                     uint32_t kind);
};

const unit_create_calls &live_unit_create_calls();

namespace detail {

// llm_strat_unit_create @0x00463860. Returns the newly-placed unit's roster slot (1..99, or 1..90
// when is_ship), or 0 on failure (no free slot within the search bound -- see the header hazard).
uint32_t create_unit(const sim_view &v, sim_store &own, const unit_create_calls &c, uint32_t x,
                     uint32_t y, uint16_t unit_proto_id, uint16_t player, uint8_t is_ship);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// `__mh_watcall_ecx_ebx_volatile` shape (addr/mh_calls.gen.h's llm_strat_unit_create).
uint32_t unit_create(uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player, uint8_t is_ship);

namespace detail {
} // namespace detail

} // namespace mh::sim
