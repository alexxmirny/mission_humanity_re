// Destination: src/mh_dll/mh/seams/order_tasks_native.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "include/mh_order_tasks_reservations.h"
namespace mh::order_tasks::native {
// Existing hook owners call these INSIDE their saved-machine-state/FX envelopes.
bool            guard_dispatch(const uint8_t *native_ebp);
void            applied_dispatch(const uint8_t *native_ebp);
void            finish_dispatch(const uint8_t *native_ebp); // after native non-retention/source finalization
bool            finish_deferred(const uint8_t *native_ebp); // false means discard: never publish applied
bool            accepted_near(uint32_t player, uint32_t slot, bool on_goal);
void            projection_result(bool failure); // extend existing merge-goal owner's return
void            preparation_finished();          // extend existing takeoff owner before its original setter
extern "C" void order_tasks_projection_gateway();
extern "C" void order_tasks_retry_gateway();
extern "C" void order_tasks_planner_return_gateway();
extern "C" void order_tasks_no_slot_gateway();
bool            source_issue_is_active(actor_key, target_key); // exact actor/target/world native outer scope
void            begin_borrowed(const reservations::issue_scope &, bool managed);
bool            finish_borrowed(const reservations::issue_scope &, bool recorded, bool source_entered, bool suppressed);
void            source_sink_observed(uint32_t player, uint32_t slot, bool suppressed);
void            lost_records(uint32_t lane, uint32_t old_count); // AFTER actual logical loss; bytes retained
void            retained_record(const uint8_t *native_ebp, uint32_t lane, uint32_t source_index, bool copied_to_retained_prefix);
void            before_queue_overwrite(const uint8_t *native_ebp);
void            before_world_publication(); // forget transient tickets only, no canonical reset
struct site {
    uintptr_t      address, owner;
    const uint8_t *expected;
    uint32_t       length;
    void          *destination;
};
// New disjoint sites only. Existing hook owners are extended by integration.patch.
const site *sites(size_t &count);
bool        install(); // all preflight first, caps-aware writes, exact reverse-order rollback
// Existing native source stub calls this gateway after witness source capture; on stale record
// it returns directly into native iteration-end, so original compaction drops exactly that record.
extern "C" void order_tasks_dispatch_guard_gateway();
} // namespace mh::order_tasks::native
