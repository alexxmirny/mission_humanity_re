// Manual Exit All only: decorate the native force call at EN0046cb4a, never its shared callee.
#pragma once
#include <cstdint>

namespace mh::storage_policy::bulk_rally {
// Always runs the original force action exactly once. Returns true only for a synchronous
// PARKED(0x1f) -> EXIT_BEGIN(0x20) transition, including a departure without a rally.
// A rally changes only order/goal_x/goal_y/move_group_id. Queued-order fields stay native.
bool force_disembark(uint32_t player, int32_t unit_index, int32_t building_index);

// CALL-site adapter: EAX=player, EDX=unit; the enclosing sweep's [EBP-0x20] is its building.
// Preserve all integer/flags/x87/SSE state and return to EN0046cb4f. Register through the existing
// storage-controls guarded patch transaction, not as a global force_disembark replacement.
void native_force_disembark_thunk();
} // namespace mh::storage_policy::bulk_rally
