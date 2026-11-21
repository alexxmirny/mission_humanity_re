// Destination: src/mh_dll/mh_common/include/mh_order_tasks_reservations.h
#pragma once
#include "mh_order_tasks.h"
namespace mh::order_tasks::reservations {
struct actual_primary {
    object_kind kind   = object_kind::none;
    uint32_t    player = 0, slot = 0, cached_raw16 = 0, owned_bits = 0;
};
// No decisions, allocation, floating-point or target selection in these accessors.
// adjust skips a stale lifetime; write preserves every flag bit outside native0x8001.
struct access {
    void *context;
    bool (*read)(void *, actor_key, actual_primary &);
    bool (*write)(void *, actor_key, uint32_t cached_raw16, uint32_t owned_bits);
    actor_key (*identity)(void *, object_kind, uint32_t, uint32_t);
    bool (*adjust)(void *, target_key, uint32_t delta_raw16, bool subtract);
    bool (*clear_target)(void *, actor_key);
    bool (*retained_application)(void *, actor_key, target_key);
    bool (*retained_marker)(void *, actor_key, uint32_t magic, uint32_t word10, uint32_t word11, const entry &expected);
};
enum class issue_decision : uint32_t { unmanaged,
                                       allowed,
                                       denied,
                                       physical };
struct issue_scope {
    actor_key  actor{};
    target_key target{}, prior_target{};
    uint32_t   epoch = 0, prior_packed = 0;
    bool       active = false;
    world_key  world{};
};
// Proven passive PRIMARY only. physical tracks an unowned booking, never QOPA authority.
// unmanaged is original native fall-through with no captured accounting scope.
issue_decision begin_issue(state &, const backend &, const access &, actor_key, target_key, issue_scope &);
// Called after original issue-side accounting even when enqueue was replay-suppressed.
// accepted_or_suppressed=false rolls back the just-created reservation exactly once.
bool finish_issue(state &, const access &, const issue_scope &, bool accepted_or_suppressed);
// No ownership/QOPA authority: preserve physical bookkeeping after a managed route ends.
issue_decision begin_physical_issue(state &, const access &, actor_key, target_key, issue_scope &);
// Actual untagged F only. Matching pending reservation uses the existing handoff arithmetic;
// a different replacement rolls it back before original release/bind. Never stops ordinary F.
bool physical_application(state &, const access &, actor_key, target_key);
bool reconcile_lost_physical(state &, const access &, actor_key, target_key);
bool cancel_pending(state &, const access &, actor_key);
// Called after all actual-F guards, before initial native mode1 and field writes.
bool handoff(state &, const access &, actor_key, target_key, uint32_t epoch);
// true=handled: caller skips native mode1/mode0 arithmetic. false=native body must run.
bool release_before(state &, const access &, actor_key);
bool rebound_before(state &, const access &, actor_key);
// Actual native mode0/1 observation, including unmanaged ordinary bindings; metadata only there.
bool rebound_after(state &, const access &, actor_key);
void release_after(state &, const access &, actor_key);
// Actual legacy publication only, AFTER canonical roster-life import and empty QORD
// initialization. Import recognized current native bookings as nonowning epoch0 metadata;
// never writes native fields/counters, infers missing lives or replaces modern metadata.
// Dead/unresolvable refs stay unmanaged/refusable until actual native cleanup.
bool import_legacy_bindings(state &, const access &);
// First accepted append adopts existing accounting without estimating/adding again.
bool capture_binding(const state &, const access &, actor_key, primary_reservation &);
bool adopt_binding(state &, const access &, actor_key);
bool physical_record_current(const state &, const access &, actor_key, target_key, uint32_t expected_epoch);
// Actual-F only, after QOEX admission; an exact captured pending lease transfers without re-estimation.
bool captured_application(state &, const access &, actor_key);
// Pure QOEX classification: exact external ticket/ref/index, target life from its canonical head.
bool captured_record_target(const state &, actor_key, uint32_t route, uint32_t leg, uint32_t ref, uint32_t slot, target_key &);
// Post-F alternate capture requires the exact external head and actual BOUND booking.
bool capture_bound_alternate(const state &, const access &, actor_key, uint32_t native_state, uint32_t native_order, entry &);
// An observed generic AI cache overwrite makes an unowned old baseline unproven, even if
// the numeric estimate stays equal. No native bytes/control/route/epoch change.
void invalidate_nonowning_binding(state &, actor_key);
// Managed-only detach BEFORE retirement/reuse. Unmanaged bindings remain native behavior.
// Backend callback: native writes only; caller core clears matching descriptor/baseline.
void detach_primary(const state &, const access &, actor_key, target_key);
void retire_target(state &, const access &, target_key);
void retire_actor(state &, const access &, actor_key);
} // namespace mh::order_tasks::reservations
