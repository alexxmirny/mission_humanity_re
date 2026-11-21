// Manual Exit All's accepted transition, not a change to generic unit_force_disembark.
#pragma once
#include <cstdint>

namespace mh::sim {
struct sim_view;
class sim_store;
struct storage_cancel_pending_docked_calls;

namespace detail {
// Calls c.unit_force_disembark exactly once, with the sweep's already-narrowed player and full
// unit index. Returns true only when that call synchronously changes PARKED(0x1f) to
// EXIT_BEGIN(0x20). The caller can observe acceptance independently of any presentation feature.
// Rally/cancellation use own's existing synchronous policy observer; there is no second owner,
// queued order, retained snapshot, new callback operation, schema field or ABI payload.
bool storage_bulk_force_disembark(const sim_view &v, sim_store &own,
                                  const storage_cancel_pending_docked_calls &c,
                                  uint32_t player, int32_t building_index, int32_t unit_index);
} // namespace detail
} // namespace mh::sim
