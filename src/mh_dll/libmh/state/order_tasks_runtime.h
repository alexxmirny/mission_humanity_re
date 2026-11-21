// Destination: src/mh_dll/libmh/state/order_tasks_runtime.h
#pragma once
#include "include/mh_order_tasks_dispatch.h"
#include <cstdlib>
extern "C" {
// Borrowed canonical QORD and QCAM. Host owns arena, initialization and publication.
int32_t libmh_order_tasks_bind(void *routes, uint32_t route_bytes, void *campaign, uint32_t campaign_bytes);
int32_t libmh_order_tasks_invoke(const mh_order_tasks_request *, mh_order_tasks_result *);
int32_t libmh_order_tasks_enqueue_suppressed();
int32_t libmh_order_tasks_checkpoint_ready();
// Quarantine surrounds bulk publication. Abort only AFTER restoring/rebinding the old spans.
int32_t libmh_order_tasks_begin_load();
void    libmh_order_tasks_abort_load(int32_t previous_live);
void    libmh_order_tasks_published();
}
namespace mh::order_tasks::runtime {
// Shared bridge keeps strategic host-table calls out of the tactical namespace.
int32_t request_host(const mh_order_tasks_request *, mh_order_tasks_result *);
// Offline stores without the optional observer have no canonical campaign to mutate.
template <class Store>
void ensure_known_planet(Store &own, uint32_t planet) {
    if (!own.has_order_tasks_observer()) return;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(host_protocol::operation::ensure_known);
    q.args[0]   = planet;
    if (!own.order_tasks_request(q, r) || !r.values[0]) std::abort();
}
state          &bound_state();
campaign_state &bound_campaign();
// Actual standalone legacy-loader boundary only, after canonical lifetime import.
bool        import_legacy_bindings();
inline bool checkpoint_ready() { return libmh_order_tasks_checkpoint_ready() != 0; }
} // namespace mh::order_tasks::runtime
