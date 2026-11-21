// Destination: src/mh_dll/mh_common/mh_order_tasks_dispatch.cpp
#include "include/mh_order_tasks_dispatch.h"
namespace mh::order_tasks::host_protocol {
int32_t dispatch(dispatch_context &d, const mh_order_tasks_request &q, mh_order_tasks_result &out) {
    out = {};
    if (!d.routes || !d.routes->valid() || !d.identity) return 0;
    state          &s  = *d.routes;
    const operation op = operation(q.operation);
    if (op == operation::pre_unit_tick) {
        s.tick(d.live);
        return 1;
    }
    if (op == operation::command_sequence) {
        out.values[0] = s.observe_command_sequence(q.player, q.args[0]);
        return 1;
    }
    if (op == operation::due_edit) {
        if (q.args[1] != COMMAND || q.args[2] != COMMAND || (q.args[0] & 0xf0u) != 0) return 0;
        int32_t words[13];
        for (uint32_t i = 0; i < 13; ++i) words[i] = int32_t(q.args[i + 3]);
        edit e{};
        out.values[0] = 1;
        if (!decode_edit(uint16_t(q.args[0]), q.slot, words, e)) {
            out.values[1] = uint32_t(refusal::malformed);
            return 1;
        }
        // Observe even refused selection admission, once before actor expansion at the caller.
        // Re-observation of the same sequence is harmless; lower values never become fresh IDs.
        const auto world = s.world();
        if (e.world.system == world.system && e.world.planet == world.planet && e.world.incarnation == world.incarnation)
            s.observe_command_sequence(e.actor.player, e.task.command_sequence);
        route_view prior{};
        const bool acquiring = !s.read_route(e.actor, prior) || !prior.count;
        out.values[1]        = uint32_t(s.apply_edit(d.live, e));
        if (out.values[1] == uint32_t(refusal::none) && acquiring && e.operation != edit_kind::cancel)
            reservations::adopt_binding(s, d.accounting, e.actor);
        return 1;
    }
    const object_kind kind = op == operation::retire_target ? object_kind(q.args[0]) : object_kind::unit;
    if (kind != object_kind::unit && kind != object_kind::building) return 0;
    actor_key      actor    = d.identity(d.context, kind, q.player, q.slot);
    const uint32_t supplied = op == operation::retire_target ? q.args[1] : (op == operation::ticket || op == operation::hold || op == operation::suppress_home || op == operation::retire_actor || op == operation::cancel_actor ? q.args[0] : 0);
    if (supplied) actor.incarnation = supplied;
    if (!actor.incarnation) {
        if (op == operation::primary_access ||
            (op == operation::primary_admission && !is_primary_marker(q.args[11])))
            out.values[0] = 1; // no canonical owner: ordinary native behavior is untouched
        return 1;
    }
    if (op == operation::air_boundary) {
        bool owned_at_entry = false;
        out.values[0]       = s.aircraft_boundary(d.live, actor, q.args[0], q.args[1], &owned_at_entry);
        out.values[1]       = owned_at_entry;
        return 1;
    }
    if (op == operation::retire_target) {
        if (q.args[2] > 1) return 0;
        reservations::retire_target(s, d.accounting, {kind, actor});
        out.values[0] = s.retire_target(d.live, {kind, actor}, q.args[2] != 0);
        return 1;
    }
    if (op == operation::retire_actor) {
        reservations::retire_actor(s, d.accounting, actor);
        s.retire_actor(actor);
        return 1;
    }
    if (op == operation::cancel_actor) {
        out.values[0] = uint32_t(s.cancel_actor(d.live, actor));
        return 1;
    }
    if (op == operation::reservation) {
        switch (reservation_operation(q.args[0])) {
            case reservation_operation::begin_issue: {
                target_key target{object_kind(q.args[1]), {q.args[2], q.args[3], q.args[4]}};
                if (!target.object.incarnation)
                    target.object = d.identity(d.context, target.kind, target.object.player, target.object.slot);
                reservations::issue_scope scope{};
                out.values[0] = uint32_t(reservations::begin_issue(s, d.live, d.accounting, actor, target, scope));
                put_issue_scope(out.values + 1, out.values + 15, scope);
                return 1;
            }
            case reservation_operation::finish_issue:
                out.values[0] = reservations::finish_issue(s, d.accounting, get_issue_scope(q.args + 1, q.args + 18),
                                                           q.args[15] != 0 || (q.args[16] != 0 && q.args[17] != 0));
                return 1;
            case reservation_operation::sink_observed:
                return 0; // native host broker consumes this; standalone producer uses its local scope
            case reservation_operation::cancel_pending:
                out.values[0] = reservations::cancel_pending(s, d.accounting, actor);
                return 1;
            case reservation_operation::handoff:
                out.values[0] = reservations::handoff(s, d.accounting, actor,
                                                      {object_kind(q.args[1]), {q.args[2], q.args[3], q.args[4]}}, q.args[5]);
                return 1;
            case reservation_operation::release_before:
                out.values[0] = reservations::release_before(s, d.accounting, actor);
                return 1;
            case reservation_operation::rebound_before:
                out.values[0] = reservations::rebound_before(s, d.accounting, actor);
                return 1;
            case reservation_operation::rebound_after:
                out.values[0] = reservations::rebound_after(s, d.accounting, actor);
                return 1;
            case reservation_operation::release_after:
                reservations::release_after(s, d.accounting, actor);
                out.values[0] = 1;
                return 1;
            case reservation_operation::adopt_binding:
                out.values[0] = reservations::adopt_binding(s, d.accounting, actor);
                return 1;
            case reservation_operation::invalidate_nonowning_binding:
                reservations::invalidate_nonowning_binding(s, actor);
                out.values[0] = 1;
                return 1;
            case reservation_operation::generic_begin: {
                target_key target{object_kind(q.args[1]), {q.args[2], q.args[3], q.args[4]}};
                if (!target.object.incarnation) target.object = d.identity(d.context, target.kind, target.object.player, target.object.slot);
                reservations::issue_scope scope{};
                out.values[0] = uint32_t(reservations::begin_physical_issue(s, d.accounting, actor, target, scope));
                put_issue_scope(out.values + 1, out.values + 15, scope);
                return 1;
            }
            case reservation_operation::captured_application:
                out.values[0] = reservations::captured_application(s, d.accounting, actor);
                return 1;
            case reservation_operation::physical_application: {
                target_key target{object_kind(q.args[1]), {q.args[2], q.args[3], q.args[4]}};
                if (target.kind != object_kind::none && !target.object.incarnation)
                    target.object = d.identity(d.context, target.kind, target.object.player, target.object.slot);
                out.values[0] = reservations::physical_application(s, d.accounting, actor, target);
                return 1;
            }
        }
        return 0;
    }
    if (op == operation::primary_access) {
        bool may_access = true;
        out.values[1]   = uint32_t(s.guard_primary(d.live, actor, may_access));
        out.values[0]   = may_access;
        actor_view a{};
        if (!may_access) out.values[2] = d.live.read_actor(d.live.context, actor, a) && a.safe_to_apply;
        return 1;
    }
    if (op == operation::observe) {
        const outcome event = get_outcome(q);
        if (event.owner.actor.player != q.player || event.owner.actor.slot != q.slot ||
            q.args[9] > uint32_t(outcome_kind::planning_succeeded) || q.args[10] > uint32_t(executor::direction_plane) ||
            q.args[15] > uint32_t(assignment_reason::valid_projection) || q.args[17] > uint32_t(object_kind::building)) return 0;
        out.values[0] = uint32_t(s.observe(d.live, event));
        return 1;
    }
    route_view r{};
    const bool owned = s.read_route(actor, r) && r.count;
    if (op == operation::ground_patrol_placement) {
        node_view n{};
        if (owned && r.family == executor::ground_run &&
            (r.status == phase::executing || r.status == phase::preparation) &&
            !r.externally_pending && !r.has_excursion &&
            s.read_entry(actor, r.cursor, n) && n.task.kind == action::patrol && n.task.group) {
            out.values[0] = 1;
            out.values[1] = n.task.group;
            out.values[2] = n.task.command_sequence;
        }
        return 1;
    }
    if (op == operation::ticket || op == operation::hold || op == operation::suppress_home) {
        if (!owned) return 1;
        if (op == operation::hold) {
            out.values[0] = uint32_t(s.handler_control(d.live, actor));
            return 1;
        }
        node_view  node{};
        const bool have_node = s.read_entry(actor, r.cursor, node);
        const bool attack    = have_node && (node.task.kind == action::attack_unit || node.task.kind == action::attack_building);
        if (op == operation::suppress_home) {
            out.values[0] = r.has_excursion || attack;
            return 1;
        }
        out.values[0] = 1;
        put_ticket(out.values + 1, r.active);
        out.values[10] = uint32_t(r.family);
        out.values[11] = uint32_t(r.status);
        out.values[12] = uint32_t(s.handler_control(d.live, actor));
        out.values[13] = have_node ? uint32_t(node.task.kind) : 0;
        out.values[14] = r.assignment_valid;
        out.values[15] = r.assigned.x;
        out.values[16] = r.assigned.y;
        out.values[17] = uint32_t(r.placement);
        out.values[18] = r.path_slot;
        out.values[19] = r.execution_epoch;
        out.values[20] = r.planner_succeeded;
        if (have_node) {
            out.values[21] = node.task.requested.x;
            out.values[22] = node.task.requested.y;
            out.values[23] = node.task.group;
        }
        return 1;
    }
    if (op != operation::ordinary_applied && op != operation::autonomous_applied &&
        op != operation::primary_admission && op != operation::discarded) return 0;
    // Secondary-only orders do not alter the primary route or accept primary provenance.
    if (q.args[1] == 0x1d || q.args[1] == 0x1e) return 1;
    int32_t words[13];
    for (uint32_t i = 0; i < 13; ++i) words[i] = int32_t(q.args[i + 3]);
    actor = d.identity(d.context, object_kind::unit, q.player, q.slot);
    if (!actor.incarnation || (q.args[0] & 0xfu) != q.player || (q.args[0] & 0xf0u) != 0x80u) return 0;
    auto native_target = [&](uint32_t life) -> target_key {
        const uint32_t ref = uint32_t(words[5]), slot = uint32_t(words[6]), tag = ref & 0xf0u;
        if (words[5] <= 0 || words[6] <= 0 || (tag != 0x40 && tag != 0x80 && tag != 0x20)) return {};
        return {tag == 0x40 ? object_kind::building : object_kind::unit, {ref & 15, slot, life}};
    };
    const bool primary_attack = q.args[2] == 0x1a || q.args[2] == 0x1b || q.args[2] == 0x1c ||
                                (q.args[2] == 1 && (q.args[1] == 0x1a || q.args[1] == 0x1c ||
                                                    (q.args[1] == 0x1b && is_primary_marker(uint32_t(words[8])))));
    if (op == operation::discarded) {
        out.values[1] = uint32_t(words[9]) == actor.incarnation;
        if ((uint32_t(words[8]) == AUTONOMOUS_PRIMARY || uint32_t(words[8]) == PHYSICAL_PRIMARY) &&
            uint32_t(words[9]) == actor.incarnation) {
            primary_reservation pending{};
            const auto          target = native_target(uint32_t(words[11]));
            if (s.read_reservation(actor, pending) && pending.phase == reservation_phase::pending &&
                pending.execution_epoch == uint32_t(words[10]) &&
                pending.target.kind == target.kind && pending.target.object.player == target.object.player &&
                pending.target.object.slot == target.object.slot && pending.target.object.incarnation == target.object.incarnation) {
                entry expected{};
                expected.kind   = target.kind == object_kind::building ? action::attack_building : action::attack_unit;
                expected.target = target;
                if (d.accounting.retained_marker(d.accounting.context, actor, uint32_t(words[8]),
                                                 uint32_t(words[10]), uint32_t(words[11]), expected)) return 1;
                d.live.cancel_pending_reservation(d.live.context, actor);
            }
        }
        if (!owned && primary_attack && !is_primary_marker(uint32_t(words[8]))) {
            primary_reservation pending{};
            if (s.read_reservation(actor, pending) && pending.phase == reservation_phase::pending) {
                const auto target = native_target(pending.target.object.incarnation);
                if (target.kind == pending.target.kind && target.object.player == pending.target.object.player &&
                    target.object.slot == pending.target.object.slot) {
                    out.values[1] = 1;
                    if (!reservations::reconcile_lost_physical(s, d.accounting, actor, target)) return 0;
                }
            }
        }
        // This notification is emitted ONLY after the carrier has actually dropped/compacted
        // the retained record. A stale record never releases a different route's pending head.
        if (owned && r.externally_pending && uint32_t(words[8]) == EXPLICIT_PRIMARY &&
            uint32_t(words[9]) == actor.incarnation && uint32_t(words[10]) == r.active.route &&
            uint32_t(words[11]) == r.active.leg) {
            node_view expected{};
            if (s.read_entry(actor, r.cursor, expected) &&
                d.accounting.retained_marker(d.accounting.context, actor, EXPLICIT_PRIMARY,
                                             uint32_t(words[10]), uint32_t(words[11]), expected.task)) return 1;
            actor_view a{};
            if (d.live.read_actor(d.live.context, actor, a))
                out.values[0] = uint32_t(s.observe(d.live, {r.active, outcome_kind::rejected, a.family, a.position, {}, assignment_reason::none, 0}));
        }
        return 1;
    }
    if (op == operation::primary_admission) {
        if (uint32_t(words[8]) == PHYSICAL_PRIMARY) {
            if (!primary_attack || uint32_t(words[9]) != actor.incarnation ||
                !reservations::physical_record_current(s, d.accounting, actor,
                                                       native_target(uint32_t(words[11])), uint32_t(words[10]))) return 1;
        } else if (uint32_t(words[8]) == AUTONOMOUS_PRIMARY) {
            if (!owned || !primary_attack || uint32_t(words[9]) != actor.incarnation ||
                s.validate_excursion_application(d.live, actor, uint32_t(words[10]), native_target(uint32_t(words[11]))) != refusal::none) return 1;
            primary_reservation          pending{};
            reservations::actual_primary actual{};
            const auto                   target = native_target(uint32_t(words[11]));
            if (!s.read_reservation(actor, pending) || pending.phase != reservation_phase::pending ||
                pending.execution_epoch != uint32_t(words[10]) || pending.target.kind != target.kind ||
                pending.target.object.player != target.object.player || pending.target.object.slot != target.object.slot ||
                pending.target.object.incarnation != target.object.incarnation ||
                !d.accounting.read(d.accounting.context, actor, actual) ||
                actual.cached_raw16 != pending.delta_raw16 || actual.owned_bits != 0x8001u) return 1;
        } else if (uint32_t(words[8]) == EXPLICIT_PRIMARY) {
            if (!owned || uint32_t(words[9]) != actor.incarnation || uint32_t(words[10]) != r.active.route ||
                uint32_t(words[11]) != r.active.leg || !s.pending_application_allowed(d.live, r.active)) return 1;
            entry     actual{};
            node_view expected{};
            if (!d.decode_applied || !d.decode_applied(d.context, actor, uint16_t(q.args[1]), int16_t(q.args[2]), words, actual) ||
                !s.read_entry(actor, r.cursor, expected) || !equivalent_native_task(actual, expected.task)) return 1;
        } else {
            if (!q.args[16] && owned && s.cancel_actor(d.live, actor) != refusal::none) return 1;
            out.values[0] = 1;
            return 1;
        }
        actor_view a{};
        if (!d.live.read_actor(d.live.context, actor, a)) return 1;
        out.values[0] = 1;
        out.values[2] = a.position.x;
        out.values[3] = a.position.y;
        return 1;
    }
    if (op == operation::autonomous_applied) {
        if (!owned || !primary_attack || uint32_t(words[8]) != AUTONOMOUS_PRIMARY ||
            uint32_t(words[9]) != actor.incarnation ||
            s.validate_excursion_application(d.live, actor, uint32_t(words[10]), native_target(uint32_t(words[11]))) != refusal::none) return 1;
        // The successful pre-F guard supplied this exact caller-local settled anchor.
        out.values[0] = uint32_t(s.observe(d.live, {r.active, outcome_kind::primary_excursion_started, r.family, {q.args[16], q.args[17]}, {}, assignment_reason::none, 0, native_target(uint32_t(words[11]))}));
        return 1;
    }
    // Ordinary one-head activity is NOT managed. Only a first-append capture can make an
    // externally-pending native command a managed head. Plain F retains native timing/RNG/home.
    if (uint32_t(words[8]) != EXPLICIT_PRIMARY && uint32_t(words[8]) != AUTONOMOUS_PRIMARY) {
        entry ordinary{};
        // Only the original successful F record proves the click. The decoder may
        // otherwise fall back to a per-member goal, so require explicit bounded XY.
        const dimensions shape = s.shape();
        if (words[0] >= 0 && uint32_t(words[0]) < shape.width &&
            words[1] >= 0 && uint32_t(words[1]) < shape.height && d.decode_applied) {
            entry decoded{};
            if (d.decode_applied(d.context, actor, uint16_t(q.args[1]), int16_t(q.args[2]), words, decoded) &&
                decoded.kind == action::move) {
                ordinary           = decoded;
                ordinary.requested = {uint32_t(words[0]), uint32_t(words[1])};
            }
        }
        out.values[0] = s.adopt_applied(d.live, actor, ordinary) == refusal::none;
        return 1; // No route, task ownership, native timing or assignment changes.
    }
    if (!owned || !r.externally_pending) return 1;
    if (!d.decode_applied) return 0;
    entry e{};
    if (!d.decode_applied(d.context, actor, uint16_t(q.args[1]), int16_t(q.args[2]), words, e)) {
        s.adopt_applied(d.live, actor, entry{});
        return 1;
    }
    if (uint32_t(words[8]) != EXPLICIT_PRIMARY || uint32_t(words[9]) != actor.incarnation ||
        uint32_t(words[10]) != r.active.route || uint32_t(words[11]) != r.active.leg) return 1;
    e.command_sequence = 0; // captured legacy shared click identity is deliberately unknown
    out.values[0]      = s.adopt_applied(d.live, actor, e, &r.active) == refusal::none;
    return 1;
}
} // namespace mh::order_tasks::host_protocol
