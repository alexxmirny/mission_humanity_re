// Destination: src/mh_dll/libmh/state/order_tasks_runtime.cpp
#include "state/order_tasks_runtime.h"
#include "state/storage_policy_runtime.h"
#include "state/region_runtime.h"
#include "sim/sim_order_tasks.h"
#include "sim/sim_unit_is_boarding.h"
#include "sim/sim_unit_select_weapon.h"
#include "sim/sim_unit_in_weapon_range.h"
#include "sim/sim_target_class.h"
#include "sim/sim_target_release_ref.h"
#include "sim/sim_unit_fine_pos.h"
#include "sim/sim_bldg_get_coords.h"
#include "sim/sim_unit_notify.h"
#include "sim/sim_unit_facing24_delta.h"
#include "sim/sim_attack_range_footprint.h"
#include "sim/sim_bldg_footprint_random_offset.h"
#include "sim/sim_path_solver.h"
#include "sim/sim_path_attach_slot.h"
#include "include/mh_air_patrol.h"
#include "orders/order_queue.h"
#include "orders/order_index_codec.h"
#include "include/mh_order_witness.h"
#include "state/host_events.h"
#include "state/order_tasks_origin.h"
#include "state/host_api.h"
#include <limits.h>
#include <cstring>
namespace {
using namespace mh::order_tasks;
namespace sim = mh::sim;
namespace sp  = mh::storage_policy;
namespace hp  = mh::order_tasks::host_protocol;
state          routes;
campaign_state campaign;
bool           runtime_live = false;
struct due_player_edit {
    uint32_t   index_plus_one = 0;
    actor_key  actor{};
    sim::order record{};
};
due_player_edit due_player_edits[8][500]{};
world_key       due_player_world{};
bool            due_player_batch = false;
bool            local_checkpoint_ready() { return runtime_live && routes.valid() && campaign.valid() && routes.checkpoint_ready(); }
struct world {
    sim::sim_state sim;
    uint32_t       capture_limit;
    int32_t        captured_slot = -1;
    sim::order     captured_record{};
    uint32_t       excluded_lane = 0, hole_begin = 0, hole_end = 0;
};
bool      decode(void *, actor_key, uint16_t, int16_t, const int32_t (&)[13], entry &);
bool      same(actor_key a, actor_key b) { return a.player == b.player && a.slot == b.slot && a.incarnation == b.incarnation; }
actor_key identity(void *, object_kind kind, uint32_t p, uint32_t slot) {
    const auto &lives = sp::runtime::bound_state();
    const auto  id    = kind == object_kind::unit ? lives.unit_identity(p, slot) : lives.building_identity(p, slot);
    return {id.player, id.slot, id.incarnation};
}
bool account_read(void *context, actor_key id, reservations::actual_primary &out) {
    auto &w = *static_cast<world *>(context);
    if (id.player >= routes.shape().players || !id.slot || id.slot >= uint32_t(w.sim.read.caps.units) ||
        !same(id, identity(nullptr, object_kind::unit, id.player, id.slot))) return false;
    const auto &u = w.sim.read.units[id.player * w.sim.read.caps.units + id.slot];
    out           = {};
    if (u.target_ref) {
        const uint32_t ref = uint16_t(u.target_ref);
        out.kind           = (ref & 0x40) ? object_kind::building : object_kind::unit;
        out.player         = ref & 15;
        out.slot           = uint16_t(u.target_index);
    }
    out.cached_raw16 = uint16_t(u.committed_weapon_damage_est);
    out.owned_bits   = (uint32_t(u.engagement_flags) | (uint32_t(u.order_status_flags) << 8)) & 0x8001u;
    return true;
}
bool account_write(void *context, actor_key id, uint32_t cached, uint32_t bits) {
    auto                        &w = *static_cast<world *>(context);
    reservations::actual_primary ignored{};
    if (!account_read(context, id, ignored)) return false;
    auto &u                       = w.sim.own.unit_at(id.player, id.slot);
    u.committed_weapon_damage_est = int16_t(uint16_t(cached));
    u.engagement_flags            = uint8_t((u.engagement_flags & 0xfeu) | (bits & 1));
    u.order_status_flags          = uint8_t((u.order_status_flags & 0x7fu) | ((bits >> 8) & 0x80u));
    return true;
}
bool account_adjust(void *context, target_key key, uint32_t raw, bool subtract) {
    auto &w = *static_cast<world *>(context);
    if (!same(key.object, identity(nullptr, key.kind, key.object.player, key.object.slot))) return false;
    if (key.object.player >= routes.shape().players || !key.object.slot) return false;
    if (key.kind == object_kind::unit) {
        if (key.object.slot >= uint32_t(w.sim.read.caps.units)) return false;
        auto          &value = w.sim.own.unit_at(key.object.player, key.object.slot).incoming_threat_damage;
        const uint32_t old   = uint16_t(value);
        value                = int16_t(uint16_t(subtract ? old - raw : old + raw));
        return true;
    }
    if (key.kind == object_kind::building) {
        if (key.object.slot >= uint32_t(w.sim.read.caps.buildings)) return false;
        auto          &value = w.sim.own.building_at(key.object.player, key.object.slot).incoming_damage_tally;
        const uint32_t old   = uint16_t(value);
        value                = int16_t(uint16_t(subtract ? old - raw : old + raw));
        return true;
    }
    return false;
}
bool account_clear(void *context, actor_key id) {
    auto                        &w = *static_cast<world *>(context);
    reservations::actual_primary ignored{};
    if (!account_read(context, id, ignored)) return false;
    auto &u        = w.sim.own.unit_at(id.player, id.slot);
    u.target_ref   = 0;
    u.target_index = 0;
    return true;
}
bool retained_index(void *context, uint32_t lane, uint32_t index) {
    const auto &w = *static_cast<world *>(context);
    return w.excluded_lane != lane || index < w.hole_begin || index >= w.hole_end;
}
bool retained_application(void *context, actor_key actor, target_key target) {
    const auto &orders  = mh::orders::state();
    auto        matches = [&](const mh::orders::order &q, bool encoded) {
        const uint32_t slot = encoded ? (mh::orders::order_wide_index() ? mh::orders::decode_index(q.unit_index) : (q.unit_index & 255)) : q.unit_index;
        const uint32_t code = q.order_code & 255, param = uint16_t(q.param0) & 255;
        if (hp::is_primary_marker(uint32_t(q.args[8]))) return false;
        if (slot != actor.slot || (q.owner_and_kind & 255) != (0x80u | actor.player) ||
            code == 0x1d || code == 0x1e || !((param >= 0x1a && param <= 0x1c) || (param == 1 && (code == 0x1a || code == 0x1c))) || q.args[5] <= 0 || q.args[6] <= 0) return false;
        return (uint32_t(q.args[5]) & 15) == target.object.player && uint32_t(q.args[6]) == target.object.slot &&
               bool(q.args[5] & 0x40) == (target.kind == object_kind::building);
    };
    for (int32_t i = 0; i < *orders.queue_count; ++i)
        if (retained_index(context, 1, uint32_t(i)) && matches(orders.queue[i], false)) return true;
    for (int32_t i = 0; i < *orders.pending_count; ++i)
        if (retained_index(context, 2, uint32_t(i)) && matches(orders.pending[i], false)) return true;
    for (int32_t i = 0; i < *orders.staging_count; ++i)
        if (retained_index(context, 3, uint32_t(i)) && matches(orders.staging[i], true)) return true;
    return false;
}
bool retained_marker(void *context, actor_key actor, uint32_t magic, uint32_t word10, uint32_t word11, const entry &expected) {
    const auto &orders  = mh::orders::state();
    auto        matches = [&](const mh::orders::order &q, bool encoded) {
        const uint32_t slot = encoded ? (mh::orders::order_wide_index() ? mh::orders::decode_index(q.unit_index) : (q.unit_index & 255)) : q.unit_index;
        if (slot != actor.slot || (q.owner_and_kind & 255) != (0x80u | actor.player) ||
            uint32_t(q.args[8]) != magic || uint32_t(q.args[9]) != actor.incarnation ||
            uint32_t(q.args[10]) != word10 || uint32_t(q.args[11]) != word11) return false;
        entry actual{};
        if (!decode(context, actor, uint16_t(q.order_code & 255), int16_t(uint16_t(q.param0) & 255), q.args, actual)) return false;
        if (magic == hp::EXPLICIT_PRIMARY) return hp::equivalent_native_task(actual, expected);
        return actual.kind == expected.kind && actual.target.kind == expected.target.kind &&
               same(actual.target.object, expected.target.object);
    };
    for (int32_t i = 0; i < *orders.queue_count; ++i)
        if (retained_index(context, 1, uint32_t(i)) && matches(orders.queue[i], false)) return true;
    for (int32_t i = 0; i < *orders.pending_count; ++i)
        if (retained_index(context, 2, uint32_t(i)) && matches(orders.pending[i], false)) return true;
    for (int32_t i = 0; i < *orders.staging_count; ++i)
        if (retained_index(context, 3, uint32_t(i)) && matches(orders.staging[i], true)) return true;
    return false;
}
reservations::access accounting(world &w) {
    return {&w, account_read, account_write, identity, account_adjust, account_clear, retained_application, retained_marker};
}
bool cancel_pending(void *context, actor_key id) {
    auto               &w = *static_cast<world *>(context);
    primary_reservation before{};
    if (!routes.read_reservation(id, before) || !reservations::cancel_pending(routes, accounting(w), id)) return false;
    if (before.phase == reservation_phase::pending) {
        // Invalidate, never compact the source walk's borrowed q/slot/kept indices.
        for (int32_t i = 0; i < w.sim.own.order_count(); ++i) {
            auto &q = w.sim.own.order_at(i);
            if (q.unit_index == id.slot && (q.owner_and_kind & 15) == id.player &&
                uint32_t(q.args[8]) == hp::AUTONOMOUS_PRIMARY && uint32_t(q.args[9]) == id.incarnation &&
                uint32_t(q.args[10]) == before.execution_epoch) q.args[11] = 0;
        }
    }
    return true;
}
void detach_primary(void *context, actor_key id, target_key target) {
    auto &w = *static_cast<world *>(context);
    reservations::detach_primary(routes, accounting(w), id, target);
}
bool family(const sim::cfg_unit &cfg, executor &out) {
    // EN constructor finite type domain plus measured tuple. Not a broad >=air threshold.
    if (cfg.type < 1 || cfg.type > 24) return false;
    if (cfg.type <= 0x0e && cfg.move_op_code == 0x0f && cfg.move_op_arg == 0x0a && cfg.default_op_code == 1) {
        out = executor::ground_run;
        return true;
    }
    if (cfg.move_op_code == 0x11 && cfg.move_op_arg == 0x0b && cfg.default_op_code == 0x13 && (cfg.type == 0x11 || cfg.type == 0x12)) {
        out = executor::direction_plane;
        return true;
    }
    if ((cfg.type == 0x0f || cfg.type == 0x10 || (cfg.type >= 0x13 && cfg.type <= 0x18)) &&
        cfg.move_op_code == 0x12 && cfg.move_op_arg == 0x0b && cfg.default_op_code == 0x13) {
        out = executor::direction_nonplane;
        return true;
    }
    return false;
}
bool actor(world &w, actor_key id, sim::unit const *&u, executor &f) {
    const auto &v = w.sim.read;
    if (id.player >= routes.shape().players || !id.slot || id.slot >= uint32_t(v.caps.units) ||
        !same(id, identity(nullptr, object_kind::unit, id.player, id.slot))) return false;
    u = &v.units[id.player * v.caps.units + id.slot];
    return u->energy > 0 && u->unit_proto_id > 0 && u->unit_proto_id < 100 && family(v.cfg_units[u->unit_proto_id], f);
}
bool compatible(const sim::unit &u) {
    switch (u.state) {
        case 1:
        case 0xa:
        case 0xb:
        case 0xc:
        case 0xf:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x15:
        case 0x30:
        case 0x7c:
        case 0x1a:
        case 0x1b:
        case 0x1c:
        case 0x2c:
        case 0x2d:
        case 0x2e:
        case 0x2f:
        case 0x37: return true;
        default: return false;
    }
}
bool safe(const sim::sim_view &v, const sim::unit &u, executor f) {
    if (!compatible(u) || sim::unit_state_is_boarding(u.state)) return false;
    if (u.state == 0x37 || u.state == 0x15 || u.state == 0x30 || u.state == 0x7c) return false;
    if (u.move_microstep < 31 && (u.state == 0xf || u.state == 0x11 || u.state == 0x12 || u.state == 0x2e || u.state == 0x2f)) return false;
    if (f != executor::ground_run) {
        if (u.move_heading >= 24 || u.move_microstep < 0 || u.move_microstep >= 32) return false;
        if (u.facing_target != v.move_microsteps[u.move_heading * sim::MICROSTEPS_PER_HEADING + u.move_microstep].facing) return false;
    }
    return true;
}
bool read_actor(void *context, actor_key id, actor_view &out) {
    auto            &w = *static_cast<world *>(context);
    const sim::unit *u;
    executor         f;
    if (!actor(w, id, u, f) || !compatible(*u)) return false;
    route_view          route{};
    primary_reservation binding{};
    if (!routes.read_route(id, route) || !route.count) {
        if (!reservations::capture_binding(routes, accounting(w), id, binding)) return false;
    }
    out = {id, u->unit_proto_id, f, {u->x, u->y}, u->state, u->order, uint32_t(u->move_microstep), safe(w.sim.read, *u, f), uint32_t(u->move_heading), u->path_slot_id == 0xff ? UINT32_MAX : uint32_t(u->path_slot_id), uint32_t(u->path_cursor)};
    return true;
}
bool target(world &w, actor_key attacker, target_key id, uint32_t &mask, uint8_t *weapon = nullptr) {
    const auto &v = w.sim.read;
    if (id.object.player >= routes.shape().players || id.object.player == attacker.player || !id.object.slot ||
        !same(id.object, identity(nullptr, id.kind, id.object.player, id.object.slot)) ||
        v.players[attacker.player].ai_player_relation[id.object.player] >= 0) return false;
    if (id.kind == object_kind::unit) {
        if (id.object.slot >= uint32_t(v.caps.units)) return false;
        const auto &u = v.units[id.object.player * v.caps.units + id.object.slot];
        if (!(u.energy > 0) || !u.unit_proto_id) return false;
        mask = u.elevation ? 2u : 1u;
    } else if (id.kind == object_kind::building) {
        if (id.object.slot >= uint32_t(v.caps.buildings)) return false;
        const auto &b = v.buildings[id.object.player * v.caps.buildings + id.object.slot];
        if (!(b.energy > 0) || !b.building_id || b.index <= 0) return false;
        mask = 1;
    } else return false;
    const uint8_t selected = sim::detail::unit_select_weapon(v, uint16_t(attacker.player), int32_t(attacker.slot), mask);
    if (weapon) *weapon = selected;
    return selected != sim::UNIT_SELECT_WEAPON_NOT_FOUND;
}
target_status read_target(void *context, actor_key attacker, target_key id) {
    uint32_t mask = 0;
    return target(*static_cast<world *>(context), attacker, id, mask) ? target_status::alive_hostile : target_status::unavailable;
}
bool ground_endpoint(const sim::sim_view &v, uint32_t p, const sim::unit &u) {
    if (u.path_slot_id >= sim::PATH_SLOTS_PER_PLAYER || u.path_cursor < 0 || u.path_cursor >= sim::PATH_WAYPOINTS_PER_SLOT) return false;
    uint32_t x = u.x, y = u.y;
    for (uint32_t i = uint32_t(u.path_cursor); i < sim::PATH_WAYPOINTS_PER_SLOT; ++i) {
        const auto &wp = v.path_buffers[p * sim::PATH_WAYPOINTS_PER_PLAYER + u.path_slot_id * sim::PATH_WAYPOINTS_PER_SLOT + i];
        if (!wp.heading) return x == u.goal_x && y == u.goal_y;
        if (wp.heading > 22 || (wp.heading - 1) % 3) return false;
        int32_t dx = 0, dy = 0;
        sim::detail::facing24_to_delta(wp.heading, &dx, &dy);
        x = (x + uint32_t(dx) * wp.run_length) & sim::map_width_mask(v);
        y = (y + uint32_t(dy) * wp.run_length) & sim::map_height_mask(v);
    }
    return false;
}
bool native_order16_is_patrol(actor_key a, executor family) {
    if (family == executor::ground_run) return true;
    route_view route{};
    node_view  node{};
    return routes.read_route(a, route) && route.externally_pending &&
           routes.read_entry(a, route.cursor, node) &&
           node.task.kind == action::patrol;
}
bool decode(void *context, actor_key id, uint16_t code, int16_t param, const int32_t (&args)[13], entry &out) {
    auto            &w = *static_cast<world *>(context);
    const sim::unit *u;
    executor         f;
    if (!actor(w, id, u, f)) return false;
    const auto &cfg         = w.sim.read.cfg_units[u->unit_proto_id];
    out                     = {};
    const uint32_t marker   = uint32_t(args[8]);
    const bool     producer = marker == hp::AUTONOMOUS_PRIMARY || marker == hp::PHYSICAL_PRIMARY;
    target_key     captured_target{};
    const bool     captured = marker == hp::EXPLICIT_PRIMARY && uint32_t(args[9]) == id.incarnation &&
                          reservations::captured_record_target(routes, id, uint32_t(args[10]), uint32_t(args[11]),
                                                               uint32_t(args[5]), uint32_t(args[6]), captured_target);
    const bool alternate = producer || captured;
    if ((param == cfg.move_op_code || param == 0x10) &&
        code == cfg.move_op_arg && args[0] >= 0 && args[1] >= 0 &&
        uint32_t(args[0]) < routes.shape().width && uint32_t(args[1]) < routes.shape().height) {
        out.kind      = param == 0x10 && native_order16_is_patrol(id, f) ? action::patrol : action::move;
        out.requested = {uint32_t(args[0]), uint32_t(args[1])};
    } else {
        const bool attack = param == 0x1a || param == 0x1c ||
                            (param == 0x1b && alternate) ||
                            (param == 1 && (code == 0x1a || code == 0x1c || (alternate && code == 0x1b)));
        if (!attack || args[5] <= 0 || args[6] <= 0) return false;
        const auto kind   = (args[5] & 0x40) ? object_kind::building : object_kind::unit;
        actor_key  victim = captured ? captured_target.object : identity(nullptr, kind, uint32_t(args[5]) & 15, uint32_t(args[6]));
        if (producer) victim.incarnation = uint32_t(args[11]);
        if (!victim.incarnation) return false;
        out.kind   = kind == object_kind::building ? action::attack_building : action::attack_unit;
        out.target = {kind, victim};
        // Fixed intention only; future attack target positions are not canonical waypoints.
        out.requested = {args[0] >= 0 ? uint32_t(args[0]) : u->goal_x, args[1] >= 0 ? uint32_t(args[1]) : u->goal_y};
    }
    out.group = args[12] == -1 ? uint32_t(u->move_group_id) : uint32_t(args[12]);
    return true;
}
bool capture_active(void *context, actor_key id, active_capture &out) {
    auto            &w = *static_cast<world *>(context);
    const sim::unit *u;
    executor         f;
    if (!actor(w, id, u, f) || !compatible(*u)) return false;
    primary_reservation binding{};
    if (!reservations::capture_binding(routes, accounting(w), id, binding)) return false;
    out             = {};
    out.position    = {u->x, u->y};
    const auto &v   = w.sim.read;
    const auto &cfg = v.cfg_units[u->unit_proto_id];
    // Retained earlier primary replacement outranks the still-executing predecessor.
    const uint32_t count = uint32_t(w.sim.own.order_count());
    const uint32_t limit = w.capture_limit == UINT32_MAX ? count : w.capture_limit;
    if (limit > count) return false;
    bool pending = false;
    for (uint32_t i = 0; i < limit; ++i) {
        const auto &q = w.sim.own.order_at(int32_t(i));
        if (q.unit_index != id.slot || (q.owner_and_kind & 15) != id.player || (q.owner_and_kind & 0xf0) != 0x80 || q.order_code == 0x1d || q.order_code == 0x1e) continue;
        if (uint32_t(q.args[8]) == hp::PHYSICAL_PRIMARY) {
            const uint32_t   ref  = uint16_t(q.args[5]);
            const auto       kind = ref & 0x40 ? object_kind::building : (ref & 0xa0) ? object_kind::unit
                                                                                      : object_kind::none;
            const target_key target{kind, {ref & 15u, uint16_t(q.args[6]), uint32_t(q.args[11])}};
            if (uint32_t(q.args[9]) != id.incarnation ||
                !reservations::physical_record_current(routes, accounting(w), id, target, uint32_t(q.args[10]))) continue;
        }
        if (pending || uint32_t(q.args[8]) == hp::AUTONOMOUS_PRIMARY || uint32_t(q.args[8]) == hp::EXPLICIT_PRIMARY ||
            !decode(context, id, q.order_code, q.param0, q.args, out.nodes[0])) return false;
        pending           = true;
        w.captured_slot   = int32_t(i);
        w.captured_record = q;
    }
    if (pending) {
        if ((out.nodes[0].kind == action::attack_unit || out.nodes[0].kind == action::attack_building) &&
            binding.phase == reservation_phase::pending) {
            const auto victim = out.nodes[0].target;
            if (binding.target.kind != victim.kind ||
                !same(binding.target.object, victim.object)) return false;
        }
        out.count  = 1;
        out.status = phase::awaiting_application;
        out.provenance =
            out.nodes[0].kind == action::patrol ? native_provenance::genuine_patrol : out.nodes[0].kind == action::move ? native_provenance::active_move
                                                                                                                        : native_provenance::active_attack;
        if (out.nodes[0].kind == action::move) {
            out.logical_move       = out.nodes[0].requested;
            out.logical_move_valid = true;
        }
        return true;
    }
    if (binding.phase == reservation_phase::pending) return false; // no matching earlier retained head
    if (u->order_queued) return false;
    entry alternate{};
    if (reservations::capture_bound_alternate(routes, accounting(w), id, u->state, u->order, alternate)) {
        uint32_t mask = 0;
        if (!target(w, id, alternate.target, mask)) return false;
        out.nodes[0]   = alternate;
        out.count      = 1;
        out.status     = phase::executing;
        out.provenance = native_provenance::active_attack;
        return true;
    }
    if (u->state == cfg.default_op_code && u->order == cfg.default_op_code && !u->target_ref) {
        out.provenance = native_provenance::none;
        return true;
    }
    if (f != executor::ground_run && !u->target_ref &&
        (u->order == cfg.default_op_code || u->order == cfg.move_op_code ||
         u->order == 0x10) &&
        (u->state == cfg.default_op_code ||
         (u->state == 0x2e && u->path_slot_id == 0xff))) {
        return true;
    }
    if (u->target_ref && u->order != 0x1b && u->state != 0x1b &&
        (u->order == 0x1a || u->order == 0x1c || u->state == 0x1a || u->state == 0x1c)) {
        const object_kind kind   = (u->target_ref & 0x40) ? object_kind::building : object_kind::unit;
        const target_key  victim = binding.target;
        uint32_t          mask   = 0;
        if (!victim.object.incarnation || !target(w, id, victim, mask)) return false;
        out.count      = 1;
        out.status     = phase::executing;
        out.provenance = native_provenance::active_attack;
        out.nodes[0]   = {kind == object_kind::unit ? action::attack_unit : action::attack_building,
                        {u->goal_x, u->goal_y},
                        victim,
                        uint32_t(u->move_group_id),
                        0};
        // Explicit append adopts this ACTUAL task; it does not infer the historical issuer.
        return true;
    }
    if (u->order == 0x10 && native_order16_is_patrol(id, f)) {
        out.count      = 2;
        out.provenance = native_provenance::genuine_patrol;
        out.nodes[0]   = {action::patrol, {u->goal_x, u->goal_y}, {}, uint32_t(u->move_group_id), 0};
        out.nodes[1]   = {action::patrol, {u->home_x, u->home_y}, {}, uint32_t(u->move_group_id), 0};
    } else {
        // Match hosted capture's positive ordinary-move proof. A compatible state alone
        // cannot turn an attack-return leftover or an unowned turn path into a move.
        int32_t args[13];
        for (auto &value : args) value = -1;
        args[0]        = u->goal_x;
        args[1]        = u->goal_y;
        args[5]        = u->target_ref;
        args[6]        = uint16_t(u->target_index);
        args[12]       = u->move_group_id;
        uint16_t param = u->order;
        if (param == 1 && (u->state == 12 || u->state == 15)) param = cfg.move_op_code;
        const uint16_t code = (u->state == 26 || u->state == 28) ? u->state : cfg.move_op_arg;
        entry          actual{};
        if (u->target_ref || !decode(context, id, code, int16_t(param), args, actual) || actual.kind != action::move)
            return false;
        out.count              = 1;
        out.provenance         = native_provenance::active_move;
        out.nodes[0]           = actual; // requested goal stays the native explicit goal, not a turn-path endpoint.
        out.logical_move_valid = routes.ordinary_move_witness(id, out.logical_move);
    }
    out.status = (u->state == 0x2d || u->state == 0x37) ? phase::preparation : phase::executing;
    if (u->state == 0x2d) out.assignment_valid = false; // finite turn path is not travel assignment
    else if (f == executor::ground_run) out.assignment_valid = ground_endpoint(v, id.player, *u);
    else {
        tile       end{};
        const bool encoded   = sim::order_tasks_adapter::direction_endpoint(v, id.player, *u, end);
        out.assignment_valid = encoded && end.x == u->goal_x && end.y == u->goal_y;
        out.requires_replan  = encoded && !out.assignment_valid && out.nodes[0].kind == action::move &&
                              ((f == executor::direction_plane && u->state == 17) ||
                               (f == executor::direction_nonplane && (u->state == 18 || u->state == 55)));
        if (out.requires_replan) out.status = phase::executing;
    }
    if (out.assignment_valid) {
        out.assigned  = {u->goal_x, u->goal_y};
        out.placement = assignment_reason::requested;
        out.path_slot = u->path_slot_id;
    }
    if (!out.assignment_valid && !out.requires_replan) {
        if (u->state != 10 && u->state != 11 && u->state != 21 && u->state != 45 &&
            u->state != 48 && u->state != 49 && u->state != 55 && u->state != 124) return false;
        out.status = phase::preparation;
    }
    return true;
}
apply_result apply(void *context, const execution &e) {
    auto            &w = *static_cast<world *>(context);
    const sim::unit *read;
    executor         f;
    if (!actor(w, e.owner.actor, read, f) || !compatible(*read)) return apply_result::rejected;
    if (e.air) {
        if (f != executor::direction_plane || e.task.kind != action::patrol ||
            e.task.target.kind != object_kind::none || e.air->count != AIR_CHUNK_STEPS ||
            read->move_heading >= 24) return apply_result::rejected;
        // Validate the WHOLE successor before even an unsafe-facing adjustment.
        uint32_t previous = read->move_heading;
        for (uint32_t i = 0; i != AIR_CHUNK_STEPS; ++i) {
            const uint32_t next = e.air->headings[i];
            if (!air::legal(previous, next)) return apply_result::rejected;
            previous = next;
        }
    }
    if (!safe(w.sim.read, *read, f)) {
        sim::order_tasks_adapter::prepare_fresh_continuation(w.sim.read, w.sim.own.unit_at(e.owner.actor.player, e.owner.actor.slot));
        return apply_result::deferred; // commit only in the next pre-unit pass, never this handler
    }
    if (e.task.group == UINT32_MAX) return apply_result::rejected; // native F sentinel is not a formation identity
    auto          &own = w.sim.own;
    const auto    &v   = w.sim.read;
    const uint32_t p = e.owner.actor.player, slot = e.owner.actor.slot;
    auto          &u   = own.unit_at(p, slot);
    const auto    &cfg = v.cfg_units[u.unit_proto_id];
    if (e.air) {
        // Native field creation starts at configured cruise; real taxi/takeoff owns
        // every altitude transition. Configured cruise may itself be zero.
        if (u.move_microstep != 31 || u.elevation != cfg.elevation ||
            (u.state != 11 && u.state != 17 && u.state != 19 && u.state != 44 && u.state != 45) ||
            u.order == 0x29 || u.order == 0x31) return apply_result::deferred;
        route_view route{};
        if (!routes.read_route(e.owner.actor, route)) return apply_result::rejected;
        const bool refill    = route.guided_flight;
        const bool attached  = u.path_slot_id != 0xff;
        int32_t    path_slot = -1;
        if (attached) {
            if (u.path_slot_id >= sim::PATH_SLOTS_PER_PLAYER ||
                e.air->slot != uint32_t(u.path_slot_id) ||
                !own.path_slot_flag_at(int32_t(p), int32_t(u.path_slot_id))) return apply_result::deferred;
            path_slot = u.path_slot_id;
        } else {
            if (refill || e.air->slot != UINT32_MAX) return apply_result::deferred;
            path_slot = sim::detail::path_find_free_slot(own, int32_t(p));
            if (path_slot < 0) return apply_result::deferred;
        }
        if (refill && (u.state != 17 || u.order != 16 || u.target_ref || u.target_index ||
                       route.path_slot != uint32_t(path_slot))) return apply_result::deferred;
        if (!refill) {
            // The ordinary F replacement effects occur once, after allocation has
            // succeeded. A refill owns only the direction buffer and its cursor.
            if (u.target_ref) {
                sim::target_release_ref(p, int32_t(slot), 1);
                u.target_ref   = 0;
                u.target_index = 0;
            }
            if (u.state == cfg.move_op_arg || u.state == cfg.move_op_code)
                sim::detail::notify_status(v, own, p, int32_t(slot), 0x67);
            u.order                    = 16;
            u.state                    = 17;
            u.goal_x                   = uint8_t(e.task.requested.x);
            u.goal_y                   = uint8_t(e.task.requested.y);
            u.move_group_id            = int32_t(e.task.group);
            u.path_blocked_retry_count = 0;
            u.order_queued             = 0;
            u.home_x                   = u.x;
            u.home_y                   = u.y;
            sim::detail::notify_status(v, own, p, int32_t(slot), 0);
        }
        if (!attached) sim::detail::path_attach_slot(own, int32_t(p), int32_t(slot), path_slot);
        for (uint32_t i = 0; i != AIR_CHUNK_STEPS; ++i) {
            auto &waypoint      = own.path_buffer_at(p, path_slot, int32_t(i));
            waypoint.heading    = e.air->headings[i];
            waypoint.run_length = 0;
        }
        auto &terminal      = own.path_buffer_at(p, path_slot, int32_t(AIR_CHUNK_STEPS));
        terminal.heading    = 255;
        terminal.run_length = 0;
        u.path_cursor       = 0;
        e.air->slot         = uint32_t(path_slot);
        if (!refill) sim::detail::notify_status(v, own, p, int32_t(slot), 100);
        // Heading, microstep, both facings and activity clock are deliberately intact.
        return apply_result::applied;
    }
    uint16_t next_order = cfg.move_op_code, next_state = cfg.move_op_arg;
    if (!e.return_to_anchor && e.task.kind == action::patrol) next_order = 0x10;
    tile     goal       = e.return_to_anchor ? e.anchor : e.task.requested;
    uint16_t target_ref = 0, target_slot = 0;
    int32_t  fine_x = 0, fine_y = 0;
    uint8_t  weapon = 100;
    if (!e.return_to_anchor && (e.task.kind == action::attack_unit || e.task.kind == action::attack_building)) {
        uint32_t mask = 0;
        if (!target(w, e.owner.actor, e.task.target, mask, &weapon)) return apply_result::rejected;
        const auto &t = e.task.target;
        target_ref    = uint16_t(t.object.player) | (t.kind == object_kind::building ? 0x40 : 0x80);
        target_slot   = uint16_t(t.object.slot);
        // Visibility gate BEFORE querying coordinates. No hidden live coordinate enters canonical state.
        uint32_t tx, ty;
        if (t.kind == object_kind::unit) {
            const auto &victim = v.units[t.object.player * v.caps.units + t.object.slot];
            tx                 = victim.x;
            ty                 = victim.y;
        } else {
            const auto &victim = v.buildings[t.object.player * v.caps.buildings + t.object.slot];
            tx                 = victim.x;
            ty                 = victim.y;
        }
        if (!own.fog_visible_by_count_at(int32_t(tx), int32_t(ty), p)) return apply_result::deferred;
        if (t.kind == object_kind::unit) sim::detail::get_coords(v, uint16_t(t.object.player), int32_t(t.object.slot), &fine_x, &fine_y);
        else {
            sim::detail::bldg_get_coords(v, uint16_t(t.object.player), int32_t(t.object.slot), &fine_x, &fine_y);
            if (sim::attack_range_footprint())
                sim::footprint_aim_of_view(v, u, p, t.object.player, int32_t(t.object.slot), &fine_x, &fine_y);
        }
        goal       = {uint32_t(fine_x / 32), uint32_t(fine_y / 32)};
        next_order = t.kind == object_kind::unit ? 0x1a : 0x1c;
        if (f == executor::ground_run && sim::unit_in_weapon_range(int32_t(p), int32_t(slot), int32_t(goal.x), int32_t(goal.y), int32_t(mask))) {
            if (t.kind == object_kind::building) {
                if (sim::attack_range_footprint())
                    sim::detail::bldg_get_coords(v, uint16_t(t.object.player), int32_t(t.object.slot), &fine_x, &fine_y);
                sim::bldg_footprint_random_offset(p, slot, int32_t(t.object.player), int32_t(t.object.slot),
                                                  reinterpret_cast<uint32_t *>(&fine_x), reinterpret_cast<uint32_t *>(&fine_y));
                goal = {uint32_t(fine_x / 32), uint32_t(fine_y / 32)};
            }
            next_state = next_order;
            next_order = 1;
        }
    }
    if (u.target_ref) {
        sim::target_release_ref(p, int32_t(slot), 1);
        u.target_ref   = 0;
        u.target_index = 0;
    }
    if (u.state == cfg.move_op_arg || u.state == cfg.move_op_code) sim::detail::notify_status(v, own, p, int32_t(slot), 0x67);
    u.order                    = next_order;
    u.state                    = next_state;
    u.goal_x                   = uint8_t(goal.x);
    u.goal_y                   = uint8_t(goal.y);
    u.move_group_id            = int32_t(e.task.group);
    u.path_blocked_retry_count = 0;
    u.order_queued             = 0;
    u.home_x                   = u.x;
    u.home_y                   = u.y;
    if (target_ref) {
        u.target_fine_x   = fine_x;
        u.target_fine_y   = fine_y;
        u.target_ref      = int16_t(target_ref);
        u.target_index    = int16_t(target_slot);
        u.selected_weapon = weapon;
    }
    if (u.state == cfg.move_op_arg) sim::detail::notify_status(v, own, p, int32_t(slot), 0);
    if (u.target_ref) {
        sim::target_release_ref(p, int32_t(slot), 0);
        if (f != executor::direction_plane) u.activity_clock = double(slot % 50) / 100.0 + u.activity_clock;
    }
    return apply_result::applied;
}
bool cleanup_attack(void *context, const ticket &t) {
    auto            &w = *static_cast<world *>(context);
    const sim::unit *read;
    executor         f;
    if (!actor(w, t.actor, read, f) || !safe(w.sim.read, *read, f)) return false;
    auto &u = w.sim.own.unit_at(t.actor.player, t.actor.slot);
    if (u.target_ref) {
        sim::target_release_ref(t.actor.player, int32_t(t.actor.slot), 1);
        u.target_ref   = 0;
        u.target_index = 0;
    }
    const auto &cfg = w.sim.read.cfg_units[u.unit_proto_id];
    u.order         = cfg.default_op_code;
    u.state         = cfg.default_op_code;
    return true;
}
bool same_world(world_key a, world_key b) {
    return a.system == b.system && a.planet == b.planet && a.incarnation == b.incarnation;
}
void begin_due_player_batch() {
    due_player_batch = false;
    due_player_world = {};
    for (auto &player : due_player_edits)
        for (auto &entry : player) entry.index_plus_one = 0;
    if (!runtime_live || !routes.valid() || !campaign.valid()) return;
    const auto d = routes.shape();
    if (d.players > 8 || d.units > 500) std::abort();
    world         w{sim::state(), 0};
    const int32_t count = w.sim.own.order_count();
    if (count < 0) std::abort();
    const backend live{&w, read_actor, capture_active, read_target, apply, cleanup_attack, cancel_pending, detach_primary};
    due_player_world = routes.world();
    // Match actual semantic dispatch: QUEUE is the due-now lane; due_edit at
    // sim_order_dispatch.cpp:306 has no timestamp test. Immediate exec_time is
    // deliberately stale (order_queue.cpp:263-264), not an execution deadline.
    // PENDING is not indexed: release_due:521-529 alone gates its transfer with
    // !(exec_time > now), including unordered/NaN timestamps, before this boundary.
    for (int32_t i = 0; i < count; ++i) {
        const auto &record = w.sim.own.order_at(i);
        if (record.order_code != COMMAND || record.param0 != COMMAND || (record.owner_and_kind & 0xf0u)) continue;
        edit candidate{};
        if (!decode_edit(record.owner_and_kind, record.unit_index, record.args, candidate) ||
            !same_world(candidate.world, due_player_world) || candidate.actor.player >= d.players ||
            !candidate.actor.slot || candidate.actor.slot >= d.units) continue;
        if (!same(identity(nullptr, object_kind::unit, candidate.actor.player, candidate.actor.slot), candidate.actor)) continue;
        route_view existing{};
        if (candidate.operation == edit_kind::cancel ||
            (routes.read_route(candidate.actor, existing) && existing.count)) continue;
        auto &slot = due_player_edits[candidate.actor.player][candidate.actor.slot];
        if (slot.index_plus_one) continue;
        w.capture_limit = uint32_t(i);
        w.captured_slot = -1;
        if (routes.preview_edit(live, candidate) != refusal::none) continue;
        slot.index_plus_one = uint32_t(i) + 1;
        slot.actor          = candidate.actor;
        slot.record         = record;
    }
    due_player_batch = true;
}
void end_due_player_batch() { due_player_batch = false; }
bool due_player_preempts(actor_key actor) {
    if (!due_player_batch || !runtime_live || actor.player >= 8 || actor.slot >= 500 ||
        !actor.incarnation || !same_world(due_player_world, routes.world())) return false;
    const auto &slot = due_player_edits[actor.player][actor.slot];
    if (!slot.index_plus_one || !same(slot.actor, actor) ||
        !same(identity(nullptr, object_kind::unit, actor.player, actor.slot), actor)) return false;
    route_view existing{};
    if (routes.read_route(actor, existing) && existing.count) return false;
    const uint32_t index  = slot.index_plus_one - 1;
    const auto    &orders = mh::orders::state();
    return orders.queue && orders.queue_count && *orders.queue_count >= 0 &&
           index < uint32_t(*orders.queue_count) && std::memcmp(orders.queue + index, &slot.record, sizeof(sim::order)) == 0;
}
} // namespace
int32_t mh::order_tasks::runtime::request_host(const mh_order_tasks_request *request,
                                               mh_order_tasks_result        *result) {
    return mh::host().order_tasks(request, result);
}
extern "C" int32_t libmh_order_tasks_checkpoint_ready() {
    if (const auto *host = mh::state::host_api_or_null()) {
        if (!host->order_tasks) return 0;
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(hp::operation::checkpoint_ready);
        return host->order_tasks(&q, &r) != 0 && r.values[0] != 0;
    }
    return local_checkpoint_ready() ? 1 : 0;
}
extern "C" int32_t libmh_order_tasks_begin_load() {
    const bool prior = runtime_live;
    runtime_live     = false;
    end_due_player_batch();
    mh::order_tasks::origin::published();
    sim::order_tasks_adapter::published();
    return prior ? 1 : 0;
}
extern "C" void libmh_order_tasks_abort_load(int32_t prior) {
    runtime_live = prior != 0 && routes.valid() && campaign.valid();
}
extern "C" void libmh_order_tasks_published() {
    end_due_player_batch();
    mh::order_tasks::origin::published();
    sim::order_tasks_adapter::published();
    runtime_live = routes.valid() && campaign.valid();
}
extern "C" int32_t libmh_order_tasks_enqueue_suppressed() {
    return mh::orders::suppress_enqueue() ? 1 : 0;
}
extern "C" int32_t libmh_order_tasks_bind(void *region, uint32_t bytes, void *global, uint32_t global_bytes) {
    // Validate BOTH borrowed spans before changing either binding.
    state          candidate;
    campaign_state campaign_candidate;
    if (!candidate.bind(region, bytes) || !campaign_candidate.bind(global, global_bytes)) return 0;
    const auto key = candidate.world();
    if (candidate.shape().width != 0 && campaign_candidate.world_inc(key.planet) != key.incarnation) return 0;
    routes   = candidate;
    campaign = campaign_candidate;
    return 1;
}
extern "C" int32_t libmh_order_tasks_invoke(const mh_order_tasks_request *q, mh_order_tasks_result *out) {
    if (!out) return 0;
    *out = {};
    if (!q) return 0;
    if (q->operation == uint32_t(hp::operation::checkpoint_ready)) {
        out->values[0] = local_checkpoint_ready() ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::begin_load)) {
        out->values[0] = uint32_t(libmh_order_tasks_begin_load());
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::abort_load)) {
        libmh_order_tasks_abort_load(int32_t(q->args[0]));
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::published)) {
        const auto local = mh::state::order_tasks_region(), global = mh::state::order_campaign_region();
        if (!libmh_order_tasks_bind(local.data, local.bytes, global.data, global.bytes)) return 0;
        libmh_order_tasks_published();
        out->values[0] = runtime_live;
        return runtime_live ? 1 : 0;
    }
    if (q->operation == uint32_t(hp::operation::new_campaign)) {
        libmh_order_tasks_begin_load();
        if (!routes.valid() || !campaign.initialize(campaign.data(), campaign.size())) return 0;
        const auto d   = routes.shape();
        out->values[0] = routes.initialize(routes.data(), routes.size(), {d.players, d.units, 0, 0, d.buildings}, {}) ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::fresh_world)) {
        if (!routes.valid() || !campaign.valid()) return 0;
        uint32_t incarnation = 0;
        if (!campaign.fresh_world(q->args[1], incarnation)) return 0;
        const auto d = routes.shape();
        if (!routes.initialize(routes.data(), routes.size(), {d.players, d.units, q->args[2], q->args[3], d.buildings},
                               {q->args[0], q->args[1], incarnation})) return 0;
        libmh_order_tasks_published();
        out->values[0] = runtime_live ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::ensure_known)) {
        uint32_t incarnation = 0;
        if (!campaign.ensure_known(q->args[0], incarnation)) return 0;
        out->values[0] = incarnation;
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::begin_due_player_batch)) {
        begin_due_player_batch();
        out->values[0] = 1;
        return 1;
    }
    if (q->operation == uint32_t(hp::operation::end_due_player_batch)) {
        end_due_player_batch();
        out->values[0] = 1;
        return 1;
    }
    if (!runtime_live) {
        if (q->operation == uint32_t(hp::operation::primary_access)) out->values[0] = 1;
        else if (q->operation == uint32_t(hp::operation::primary_admission))
            out->values[0] = !hp::is_primary_marker(q->args[11]);
        else if (q->operation == uint32_t(hp::operation::due_edit)) {
            out->values[0] = 1;
            out->values[1] = uint32_t(refusal::stale_world);
        } else if (q->operation == uint32_t(hp::operation::reservation)) {
            const auto part = hp::reservation_operation(q->args[0]);
            out->values[0]  = part == hp::reservation_operation::physical_application ||
                             part == hp::reservation_operation::rebound_after || part == hp::reservation_operation::release_after;
        }
        return 1; // imported bytes are not authoritative until complete publication
    }
    if (!routes.valid() || !campaign.valid()) return 0;
    if (q->operation == uint32_t(hp::operation::reservation) &&
        q->args[0] == uint32_t(hp::reservation_operation::begin_issue) &&
        due_player_preempts(identity(nullptr, object_kind::unit, q->player, q->slot))) {
        out->values[0]  = uint32_t(reservations::issue_decision::denied);
        out->values[18] = 1;
        return 1;
    }
    const uint32_t capture_limit = q->operation == uint32_t(hp::operation::ordinary_applied) ? 0 : q->operation == uint32_t(hp::operation::due_edit) && q->args[17] ? q->args[17] - 1
                                                                                                                                                                    : UINT32_MAX;
    world          w{sim::state(), capture_limit};
    if (q->operation == uint32_t(hp::operation::discarded)) {
        w.excluded_lane = q->args[18];
        w.hole_begin    = q->args[19];
        w.hole_end      = q->args[20];
    }
    const backend        live{&w, read_actor, capture_active, read_target, apply, cleanup_attack, cancel_pending, detach_primary};
    hp::dispatch_context context{&routes, live, &w, identity, decode, accounting(w)};
    const int32_t        handled = hp::dispatch(context, *q, *out);
    // capture_active was read-only. A successful NEW acquisition alone may stamp its
    // prevalidated retained primary record, after dispatch-entry recording/replay injection.
    if (handled && q->operation == uint32_t(hp::operation::due_edit) && out->values[0] &&
        out->values[1] == uint32_t(refusal::none) && w.captured_slot >= 0) {
        const actor_key id = identity(nullptr, object_kind::unit, q->player, q->slot);
        route_view      r{};
        if (routes.read_route(id, r) && r.externally_pending) {
            auto      &record    = w.sim.own.order_at(w.captured_slot);
            auto      &witnesses = mh::state::order_witnesses();
            const auto binding   = mh::order_witness::metadata_at(witnesses, mh::order_witness::container::queue, uint32_t(w.captured_slot));
            const auto before    = mh::order_witness::canonical(record);
            record.args[8]       = int32_t(hp::EXPLICIT_PRIMARY);
            record.args[9]       = int32_t(id.incarnation);
            record.args[10]      = int32_t(r.active.route);
            record.args[11]      = int32_t(r.active.leg);
            (void)mh::order_witness::amend_metadata(witnesses, mh::order_witness::container::queue, uint32_t(w.captured_slot),
                                                    binding, before, mh::order_witness::canonical(record));
        }
    }
    return handled;
}
namespace mh::order_tasks::runtime {
state          &bound_state() { return routes; }
campaign_state &bound_campaign() { return campaign; }
bool            import_legacy_bindings() {
    world w{sim::state(), UINT32_MAX};
    return reservations::import_legacy_bindings(routes, accounting(w));
}
} // namespace mh::order_tasks::runtime
