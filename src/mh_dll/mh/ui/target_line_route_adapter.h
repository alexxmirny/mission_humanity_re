// Destination: src/mh_dll/mh/ui/target_line_route_adapter.h
// Include only in the canonical task-owner adapter, not in the native-only renderer.
#pragma once
#include <cstdint>
#include "mh_order_tasks.h"
#include "ui/target_line_routes.h"
namespace mh::ui::selected_targets::detail {
inline route_read_result read_task_route(const mh::order_tasks::state &owner,
                                         unsigned player, unsigned slot, uint32_t life,
                                         route_snapshot &out) {
    namespace tasks = mh::order_tasks;
    const tasks::actor_key actor{player, slot, life};
    tasks::route_view      route{};
    if (!owner.read_route(actor, route)) return route_read_result::unowned;
    out             = {};
    out.cycle_begin = no_cycle;
    out.actor_life  = life;
    out.generation  = route.active.route;
    if (route.count > explicit_route_capacity || route.active.actor.player != player ||
        route.active.actor.slot != slot || route.active.actor.incarnation != life)
        return route_read_result::invalid;
    out.count = route.count;
    // The final engine contract uses UINT32_MAX; do not guess count as sentinel.
    out.cycle_begin = route.cycle_begin == UINT32_MAX ? no_cycle : route.cycle_begin;
    // While a finite prefix is active, its cursor0 is not a circuit cursor.
    out.cycle_cursor        = out.cycle_begin != no_cycle && route.cursor < out.cycle_begin
                                  ? out.cycle_begin
                                  : route.cursor;
    out.has_implicit_anchor = route.has_implicit_anchor;
    out.implicit_leg_active = route.implicit_leg_active;
    if (route.implicit_anchor.x > UINT16_MAX || route.implicit_anchor.y > UINT16_MAX)
        return route_read_result::invalid;
    out.implicit_x = static_cast<uint16_t>(route.implicit_anchor.x);
    out.implicit_y = static_cast<uint16_t>(route.implicit_anchor.y);
    for (unsigned n = 0; n < out.count; ++n) {
        tasks::node_view node{};
        if (!owner.read_entry(actor, n, node) || node.task.requested.x > UINT16_MAX || node.task.requested.y > UINT16_MAX)
            return route_read_result::invalid;
        auto &dst   = out.nodes[n];
        dst.x       = static_cast<uint16_t>(node.task.requested.x);
        dst.y       = static_cast<uint16_t>(node.task.requested.y);
        dst.command = {route.active.world.system, route.active.world.planet,
                       route.active.world.incarnation, node.task.command_sequence, 0, true};
        switch (node.task.kind) {
            case tasks::action::move: dst.role = line_role::movement; break;
            case tasks::action::patrol: dst.role = line_role::patrol; break;
            case tasks::action::attack_unit:
            case tasks::action::attack_building: {
                dst.role = line_role::attack;
                // Destroyed/unavailable objectives remain intentional geometry
                // gaps until the owner consumes/replaces them. No tombstone XY.
                if (node.target != tasks::objective::unresolved) break;
                const auto &target = node.task.target;
                if (target.object.player >= 8 || target.object.slot > UINT16_MAX)
                    return route_read_result::invalid;
                const bool building = target.kind == tasks::object_kind::building;
                if (!building && target.kind != tasks::object_kind::unit)
                    return route_read_result::invalid;
                dst.target_ref  = static_cast<uint16_t>((building ? 0x40 : 0x80) | target.object.player);
                dst.target_slot = static_cast<uint16_t>(target.object.slot);
                dst.target_life = target.object.incarnation;
                // The owner-side reader must certify this canonical life against
                // its current roster. read_entry() alone does not establish that;
                // disposable presentation building counters cannot certify it.
                dst.target_current = true;
                break;
            }
            default: return route_read_result::invalid;
        }
    }
    return route_read_result::owned;
}
} // namespace mh::ui::selected_targets::detail
