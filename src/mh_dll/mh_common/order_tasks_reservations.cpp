// Destination: src/mh_dll/mh_common/order_tasks_reservations.cpp
#include "include/mh_order_tasks_reservations.h"
#include <cstdlib>
namespace mh::order_tasks::reservations {
namespace {
bool     same(actor_key a, actor_key b) { return a.player == b.player && a.slot == b.slot && a.incarnation == b.incarnation; }
bool     same(target_key a, target_key b) { return a.kind == b.kind && same(a.object, b.object); }
bool     same(world_key a, world_key b) { return a.system == b.system && a.planet == b.planet && a.incarnation == b.incarnation; }
bool     has(target_key t) { return t.kind != object_kind::none && t.object.incarnation; }
bool     fields(const actual_primary &a, target_key t) { return a.kind == t.kind && a.player == t.object.player && a.slot == t.object.slot; }
bool     current(const access &a, target_key t) { return has(t) && same(a.identity(a.context, t.kind, t.object.player, t.object.slot), t.object); }
uint32_t packed(const actual_primary &a) { return (a.cached_raw16 & 65535u) | ((a.owned_bits & 0x8001u) << 16); }
void     subtract(const access &a, target_key t, uint32_t delta) {
    if (current(a, t) && !a.adjust(a.context, t, delta & 65535u, true)) std::abort();
}
bool owned(const state &s, actor_key actor) {
    route_view r{};
    return s.read_route(actor, r) && r.count;
}
bool                cleanup_owned(const state &s, actor_key actor) { return owned(s, actor) || s.primary_cleanup_pending(actor); }
uint32_t            epoch(const state &s, actor_key actor) { return s.execution_epoch(actor); }
primary_reservation bound(target_key t, uint32_t delta, uint32_t baseline, uint32_t e) {
    primary_reservation r{};
    r.phase           = reservation_phase::bound;
    r.target          = t;
    r.delta_raw16     = delta & 65535u;
    r.prior_packed    = baseline;
    r.execution_epoch = e;
    return r;
}
bool put_none(state &s, actor_key actor) { return s.write_reservation(actor, {}); }
bool restore_prior(state &s, const access &a, actor_key actor, const primary_reservation &r) {
    const bool     prior_valid = has(r.prior_target) && current(a, r.prior_target);
    const uint32_t baseline    = (has(r.prior_target) && !prior_valid) ? 0 : r.prior_packed;
    if (!a.write(a.context, actor, baseline & 65535u, (baseline >> 16) & 0x8001u)) return false;
    if (has(r.prior_target) && !prior_valid && (cleanup_owned(s, actor) || r.execution_epoch) &&
        !a.clear_target(a.context, actor)) std::abort();
    return prior_valid ? s.write_reservation(actor, bound(r.prior_target, baseline & 65535u, baseline, r.execution_epoch)) : put_none(s, actor);
}
issue_decision begin_unowned_automatic(state &s, const access &a, actor_key actor,
                                       target_key target, issue_scope &scope) {
    scope = {};
    if (!s.valid() || !actor.incarnation ||
        !same(a.identity(a.context, object_kind::unit, actor.player, actor.slot), actor) ||
        !current(a, target) || s.primary_cleanup_pending(actor)) return issue_decision::unmanaged;
    primary_reservation previous{};
    s.read_reservation(actor, previous);
    if (previous.phase == reservation_phase::pending || previous.phase == reservation_phase::handoff)
        return issue_decision::denied;
    actual_primary actual{};
    if (!a.read(a.context, actor, actual)) return issue_decision::unmanaged;
    target_key prior{};
    if (actual.kind == object_kind::none) {
        if (previous.phase != reservation_phase::none || actual.cached_raw16 || actual.owned_bits)
            return issue_decision::unmanaged;
    } else {
        if (previous.phase != reservation_phase::bound || !fields(actual, previous.target) ||
            !current(a, previous.target) || previous.delta_raw16 != (actual.cached_raw16 & 65535u) ||
            previous.prior_packed != packed(actual)) return issue_decision::unmanaged;
        prior = previous.target;
    }
    uint32_t next = 0;
    if (!s.reserve_physical_epoch(actor, next)) return issue_decision::denied;
    scope = {actor, target, prior, next, packed(actual), true, s.world()};
    return issue_decision::physical;
}
} // namespace
issue_decision begin_issue(state &s, const backend &live, const access &a,
                           actor_key actor, target_key target, issue_scope &scope) {
    scope = {};
    if (!owned(s, actor)) return begin_unowned_automatic(s, a, actor, target, scope);
    route_view route{};
    s.read_route(actor, route);
    if (s.validate_excursion_application(live, actor, route.execution_epoch, target) != refusal::none)
        return issue_decision::denied;
    primary_reservation previous{};
    if (!s.read_reservation(actor, previous) || previous.phase == reservation_phase::pending ||
        previous.phase == reservation_phase::handoff) return issue_decision::denied;
    actual_primary actual{};
    if (!a.read(a.context, actor, actual)) return issue_decision::denied;
    target_key prior{};
    if (actual.kind != object_kind::none) {
        if (previous.phase != reservation_phase::bound || !fields(actual, previous.target) ||
            !current(a, previous.target) || previous.delta_raw16 != (actual.cached_raw16 & 65535u) ||
            previous.prior_packed != packed(actual)) return issue_decision::denied;
        prior = previous.target;
    } else if (previous.phase != reservation_phase::none) return issue_decision::denied;
    scope = {actor, target, prior, route.execution_epoch, packed(actual), true, s.world()};
    return issue_decision::allowed;
}
issue_decision begin_physical_issue(state &s, const access &a, actor_key actor, target_key target, issue_scope &scope) {
    scope = {};
    if (owned(s, actor)) return issue_decision::unmanaged;
    primary_reservation prior{};
    if (!s.read_reservation(actor, prior)) return issue_decision::unmanaged;
    if (prior.phase == reservation_phase::bound && !prior.execution_epoch) {
        invalidate_nonowning_binding(s, actor);
        return issue_decision::unmanaged;
    }
    if (prior.phase == reservation_phase::none) return issue_decision::unmanaged;
    const uint32_t current_epoch = epoch(s, actor);
    if (!current_epoch || current_epoch == UINT32_MAX || !current(a, target) || prior.phase == reservation_phase::handoff) return issue_decision::denied;
    if (prior.phase == reservation_phase::pending) {
        if (!cancel_pending(s, a, actor) || !s.read_reservation(actor, prior)) return issue_decision::denied;
    }
    actual_primary actual{};
    if (!a.read(a.context, actor, actual)) return issue_decision::denied;
    target_key previous{};
    if (actual.kind != object_kind::none) {
        if (prior.phase != reservation_phase::bound || !fields(actual, prior.target) ||
            prior.delta_raw16 != (actual.cached_raw16 & 65535u) || prior.prior_packed != packed(actual)) return issue_decision::denied;
        previous = prior.target;
    } else if (prior.phase != reservation_phase::none || actual.cached_raw16 || actual.owned_bits) return issue_decision::denied;
    uint32_t next = 0;
    if (!s.reserve_physical_epoch(actor, next)) return issue_decision::denied;
    scope = {actor, target, previous, next, packed(actual), true, s.world()};
    return issue_decision::allowed;
}
bool physical_application(state &s, const access &a, actor_key actor, target_key target) {
    primary_reservation pending{};
    if (!s.read_reservation(actor, pending) || pending.phase != reservation_phase::pending) return true;
    if (!owned(s, actor) && same(pending.target, target) && current(a, target))
        return handoff(s, a, actor, target, pending.execution_epoch);
    return cancel_pending(s, a, actor);
}
bool reconcile_lost_physical(state &s, const access &a, actor_key actor, target_key target) {
    primary_reservation pending{};
    if (owned(s, actor) || !s.read_reservation(actor, pending) ||
        pending.phase != reservation_phase::pending || !same(pending.target, target)) return true;
    return a.retained_application(a.context, actor, target) || cancel_pending(s, a, actor);
}
bool finish_issue(state &s, const access &a, const issue_scope &scope, bool accepted) {
    if (!scope.active) return true;
    if (!s.valid() || !same(scope.world, s.world())) return false; // stale stack may outlive world publication
    actual_primary actual{};
    if (!a.read(a.context, scope.actor, actual)) return false;
    primary_reservation r{};
    r.phase           = reservation_phase::pending;
    r.target          = scope.target;
    r.delta_raw16     = actual.cached_raw16 & 65535u;
    r.prior_target    = scope.prior_target;
    r.prior_packed    = scope.prior_packed;
    r.execution_epoch = scope.epoch;
    if (!s.write_reservation(scope.actor, r)) return false;
    if (!accepted || !current(a, r.target) || epoch(s, scope.actor) != scope.epoch) return cancel_pending(s, a, scope.actor);
    return true;
}
bool cancel_pending(state &s, const access &a, actor_key actor) {
    primary_reservation r{};
    if (!s.read_reservation(actor, r)) return true; // no metadata/pending lease
    if (r.phase == reservation_phase::none || r.phase == reservation_phase::bound) return true;
    if (r.phase == reservation_phase::handoff) return false; // synchronous bridge cannot be canceled/published
    subtract(a, r.target, r.delta_raw16);
    if (!restore_prior(s, a, actor, r)) std::abort();
    return true;
}
bool handoff(state &s, const access &a, actor_key actor, target_key target, uint32_t expected_epoch) {
    primary_reservation r{};
    actual_primary      actual{};
    if (!s.read_reservation(actor, r) || r.phase != reservation_phase::pending || !same(r.target, target) ||
        r.execution_epoch != expected_epoch || epoch(s, actor) != expected_epoch || !current(a, target) || !a.read(a.context, actor, actual)) return false;
    if (has(r.prior_target) && !fields(actual, r.prior_target)) return false;
    if (!has(r.prior_target) && actual.kind != object_kind::none) return false;
    if (has(r.prior_target) && !current(a, r.prior_target)) {
        r.prior_target = {};
        r.prior_packed = 0;
        if (!a.clear_target(a.context, actor)) std::abort();
    }
    if (!a.write(a.context, actor, r.prior_packed & 65535u, (r.prior_packed >> 16) & 0x8001u)) std::abort();
    r.phase = reservation_phase::handoff;
    if (!s.write_reservation(actor, r)) std::abort();
    return true;
}
bool release_before(state &s, const access &a, actor_key actor) {
    primary_reservation r{};
    if (!s.read_reservation(actor, r)) return false;
    const bool pending = r.phase == reservation_phase::pending;
    if (pending) {
        if (!cancel_pending(s, a, actor) || !s.read_reservation(actor, r)) std::abort();
    }
    if (r.phase == reservation_phase::handoff) {
        subtract(a, r.prior_target, r.prior_packed & 65535u);
        if (!a.write(a.context, actor, 0, 0)) std::abort();
        r.prior_target = {};
        r.prior_packed = 0;
        if (!s.write_reservation(actor, r)) std::abort();
        return true;
    }
    if (r.phase == reservation_phase::bound && (r.execution_epoch || owned(s, actor))) {
        subtract(a, r.target, r.delta_raw16);
        if (!a.write(a.context, actor, 0, 0) || !put_none(s, actor)) std::abort();
        return true;
    }
    return pending; // pending with no prior binding must not run native mode1 on a stale reference
}
bool rebound_before(state &s, const access &a, actor_key actor) {
    primary_reservation r{};
    actual_primary      actual{};
    if (!s.read_reservation(actor, r) || r.phase != reservation_phase::handoff) return false;
    if (!a.read(a.context, actor, actual) || !fields(actual, r.target) || !current(a, r.target) ||
        !a.write(a.context, actor, r.delta_raw16, 0x8001u)) std::abort();
    if (!s.write_reservation(actor, bound(r.target, r.delta_raw16, r.delta_raw16 | (0x8001u << 16), r.execution_epoch))) std::abort();
    return true;
}
bool rebound_after(state &s, const access &a, actor_key actor) {
    primary_reservation prior{};
    actual_primary      actual{};
    s.read_reservation(actor, prior); // a first actual binding initializes a previously zero row
    if (!a.read(a.context, actor, actual)) return false;
    if (prior.phase == reservation_phase::handoff || prior.phase == reservation_phase::pending) return false;
    if (actual.kind == object_kind::none) return put_none(s, actor);
    const target_key target{actual.kind, a.identity(a.context, actual.kind, actual.player, actual.slot)};
    if (!has(target)) return false;
    const uint32_t e = owned(s, actor) ? epoch(s, actor) : prior.phase == reservation_phase::bound ? prior.execution_epoch
                                                                                                   : 0;
    return s.write_reservation(actor, bound(target, actual.cached_raw16, packed(actual), e));
}
void release_after(state &s, const access &, actor_key actor) {
    primary_reservation r{};
    if (s.read_reservation(actor, r) && r.phase != reservation_phase::pending && r.phase != reservation_phase::handoff) put_none(s, actor);
}
bool import_legacy_bindings(state &s, const access &a) {
    if (!s.valid() || !a.read || !a.identity) return false;
    // This is an actual legacy-load migration, never a lazy first-click repair or a
    // modern-save merge. Refuse before any writes unless every actor row is pristine.
    for (uint32_t i = HEADER_BYTES; i < s.size(); ++i)
        if (s.data()[i]) return false;
    const auto d = s.shape();
    for (uint32_t p = 0; p < d.players; ++p) {
        for (uint32_t slot = 1; slot < d.units; ++slot) {
            const actor_key actor = a.identity(a.context, object_kind::unit, p, slot);
            if (!actor.incarnation) continue;
            actual_primary actual{};
            if (!a.read(a.context, actor, actual) ||
                (actual.kind != object_kind::unit && actual.kind != object_kind::building) ||
                actual.player >= d.players || !actual.slot ||
                actual.slot >= (actual.kind == object_kind::unit ? d.units : d.buildings)) continue;
            const target_key target{actual.kind, a.identity(a.context, actual.kind, actual.player, actual.slot)};
            // Legacy native cleanup may still hold a dead/unresolvable ref. Leave it
            // unmanaged and unqueueable; neither invent a life nor reject the whole load.
            if (!has(target)) continue;
            // Already counted in the loaded native target: metadata only, epoch0 means
            // NONOWNING until actual route acquisition. Preserve the raw flag baseline.
            if (!s.write_reservation(actor, bound(target, actual.cached_raw16, packed(actual), 0))) return false;
        }
    }
    return true;
}
bool capture_binding(const state &s, const access &a, actor_key actor, primary_reservation &out) {
    actual_primary actual{};
    if (!a.read(a.context, actor, actual)) return false;
    primary_reservation r{};
    s.read_reservation(actor, r);
    route_view route{};
    const bool route_owned = s.read_route(actor, route) && route.count;
    node_view  head{};
    const bool captured_head = route_owned && route.externally_pending &&
                               s.read_entry(actor, route.cursor, head) && same(head.task.target, r.target);
    if (r.phase == reservation_phase::pending && (!route_owned || captured_head) &&
        r.execution_epoch == epoch(s, actor) && r.execution_epoch && current(a, r.target) &&
        actual.cached_raw16 == r.delta_raw16 && actual.owned_bits == 0x8001u &&
        (has(r.prior_target) ? fields(actual, r.prior_target) : actual.kind == object_kind::none)) {
        out = r;
        return true;
    }
    if (actual.kind == object_kind::none) {
        if (r.phase != reservation_phase::none || actual.cached_raw16 || actual.owned_bits) return false;
        out = {};
        return true;
    }
    if (r.phase != reservation_phase::bound || !fields(actual, r.target) || !current(a, r.target) ||
        r.delta_raw16 != (actual.cached_raw16 & 65535u) || r.prior_packed != packed(actual)) return false;
    out = r;
    return true;
}
bool captured_record_target(const state &s, actor_key actor, uint32_t route_id, uint32_t leg,
                            uint32_t ref, uint32_t slot, target_key &out) {
    out = {};
    route_view     route{};
    node_view      head{};
    const uint32_t tag  = ref & 0xf0u;
    const auto     kind = tag == 0x40u ? object_kind::building : (tag == 0x80u || tag == 0x20u) ? object_kind::unit
                                                                                                : object_kind::none;
    if (ref > 255 || !slot || kind == object_kind::none ||
        !s.read_route(actor, route) || !route.count || !route.externally_pending ||
        route.active.route != route_id || route.active.leg != leg ||
        !s.read_entry(actor, route.cursor, head) ||
        (head.task.kind != action::attack_unit && head.task.kind != action::attack_building) ||
        head.task.target.kind != kind || head.task.target.object.player != (ref & 15u) ||
        head.task.target.object.slot != slot) return false;
    out = head.task.target;
    return out.object.incarnation != 0;
}
bool capture_bound_alternate(const state &s, const access &a, actor_key actor,
                             uint32_t native_state, uint32_t native_order, entry &out) {
    if (native_state != 27 && native_order != 27) return false;
    route_view          route{};
    node_view           head{};
    primary_reservation booking{};
    if (!s.read_route(actor, route) || !route.count || !route.externally_pending ||
        !s.read_entry(actor, route.cursor, head) ||
        (head.task.kind != action::attack_unit && head.task.kind != action::attack_building) ||
        !capture_binding(s, a, actor, booking) || booking.phase != reservation_phase::bound ||
        !same(head.task.target, booking.target)) return false;
    out                  = head.task;
    out.command_sequence = 0;
    return true;
}
bool physical_record_current(const state &s, const access &a, actor_key actor,
                             target_key target, uint32_t expected_epoch) {
    if (owned(s, actor) || !expected_epoch || epoch(s, actor) != expected_epoch ||
        !current(a, target)) return false;
    primary_reservation pending{};
    actual_primary      actual{};
    if (!s.read_reservation(actor, pending) || pending.phase != reservation_phase::pending ||
        pending.execution_epoch != expected_epoch || !same(pending.target, target) ||
        !a.read(a.context, actor, actual) || actual.cached_raw16 != pending.delta_raw16 ||
        actual.owned_bits != 0x8001u) return false;
    return has(pending.prior_target) ? fields(actual, pending.prior_target)
                                     : actual.kind == object_kind::none;
}
bool captured_application(state &s, const access &a, actor_key actor) {
    primary_reservation pending{};
    if (!s.read_reservation(actor, pending) || pending.phase != reservation_phase::pending) return true;
    route_view route{};
    node_view  head{};
    if (!s.read_route(actor, route) || !route.count || !route.externally_pending ||
        !s.read_entry(actor, route.cursor, head) || !same(head.task.target, pending.target) ||
        pending.execution_epoch != route.execution_epoch) return false;
    return handoff(s, a, actor, pending.target, route.execution_epoch);
}
bool adopt_binding(state &s, const access &a, actor_key actor) {
    primary_reservation r{};
    if (!capture_binding(s, a, actor, r)) return false;
    if (r.phase == reservation_phase::none) return true;
    r.execution_epoch = epoch(s, actor);
    return s.write_reservation(actor, r);
}
void invalidate_nonowning_binding(state &s, actor_key actor) {
    primary_reservation r{};
    if (!owned(s, actor) && !s.primary_cleanup_pending(actor) && s.read_reservation(actor, r) &&
        r.phase == reservation_phase::bound && r.execution_epoch == 0) s.clear_reservation(actor);
}
void detach_primary(const state &s, const access &a, actor_key actor, target_key target) {
    primary_reservation r{};
    if (!s.read_reservation(actor, r) || !(cleanup_owned(s, actor) || r.execution_epoch)) return;
    uint32_t delta = 0;
    if (r.phase == reservation_phase::bound && same(r.target, target)) {
        delta = r.delta_raw16;
    } else if ((r.phase == reservation_phase::pending || r.phase == reservation_phase::handoff) &&
               same(r.prior_target, target)) {
        delta = r.prior_packed & 65535u;
    } else {
        return;
    }
    subtract(a, target, delta);
    if (!a.clear_target(a.context, actor)) std::abort();
    // Pending's cache belongs to its new target, not the just-detached prior.
    // HANDOFF has already restored the prior cache synchronously.
    if (r.phase != reservation_phase::pending && !a.write(a.context, actor, 0, 0)) std::abort();
}
void retire_target(state &s, const access &a, target_key target) {
    const auto d = s.shape();
    for (uint32_t p = 0; p < d.players; ++p) {
        for (uint32_t i = 1; i < d.units; ++i) {
            const actor_key actor = a.identity(a.context, object_kind::unit, p, i);
            if (!actor.incarnation) continue;
            primary_reservation r{};
            if (!s.read_reservation(actor, r)) continue;
            if (r.phase == reservation_phase::pending && same(r.target, target)) {
                if (!cancel_pending(s, a, actor) || !s.read_reservation(actor, r)) std::abort();
            }
            const bool accounting_owned = cleanup_owned(s, actor) || r.execution_epoch;
            if (r.phase == reservation_phase::pending && same(r.prior_target, target) && accounting_owned) {
                detach_primary(s, a, actor, target);
                s.note_primary_detached(actor);
                r.prior_target = {};
                r.prior_packed = 0;
                if (!s.write_reservation(actor, r)) std::abort();
                continue;
            }
            // A physical epoch proves freshness, not player-route ownership. Keep an
            // unowned native binding until native mode1 releases it; its saved lifetime
            // then prevents charging a reused slot without retargeting native AI early.
            if (r.phase == reservation_phase::bound && same(r.target, target) && cleanup_owned(s, actor)) {
                detach_primary(s, a, actor, target);
                s.note_primary_detached(actor);
                if (!put_none(s, actor)) std::abort();
            }
        }
    }
}
void retire_actor(state &s, const access &a, actor_key actor) {
    primary_reservation r{};
    if (!s.read_reservation(actor, r)) return;
    if (r.phase == reservation_phase::pending) {
        if (!cancel_pending(s, a, actor) || !s.read_reservation(actor, r)) std::abort();
    }
    if (r.phase == reservation_phase::bound && r.execution_epoch) {
        subtract(a, r.target, r.delta_raw16);
        if (!a.clear_target(a.context, actor) || !a.write(a.context, actor, 0, 0)) std::abort();
    }
    if (!put_none(s, actor)) std::abort();
}
} // namespace mh::order_tasks::reservations
