// Destination: src/mh_dll/mh_common/include/mh_order_tasks_dispatch.h
#pragma once
#include "mh_order_tasks_host.h"
#include "mh_order_tasks_reservations.h"
namespace mh::order_tasks::host_protocol {
inline bool equivalent_native_task(const entry &a, const entry &b) {
    return a.kind == b.kind && a.requested.x == b.requested.x && a.requested.y == b.requested.y &&
           a.target.kind == b.target.kind && a.target.object.player == b.target.object.player &&
           a.target.object.slot == b.target.object.slot && a.target.object.incarnation == b.target.object.incarnation &&
           a.group == b.group;
}
// All pointers borrowed for this call. Canonical ownership remains exclusively routes.
struct dispatch_context {
    state  *routes;
    backend live;
    void   *context;
    actor_key (*identity)(void *, object_kind, uint32_t player, uint32_t slot);
    bool (*decode_applied)(void *, actor_key, uint16_t code, int16_t param0,
                           const int32_t (&args)[13], entry &);
    reservations::access accounting;
};
// Shared native/translated dispatcher. No executor-specific policy or event history.
int32_t dispatch(dispatch_context &, const mh_order_tasks_request &, mh_order_tasks_result &);
} // namespace mh::order_tasks::host_protocol
