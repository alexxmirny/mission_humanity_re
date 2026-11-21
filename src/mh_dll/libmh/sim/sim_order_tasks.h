// Destination: src/mh_dll/libmh/sim/sim_order_tasks.h
#pragma once
#include "sim/sim_state.h"
#include "include/mh_order_tasks_host.h"
#include "include/mh_order_tasks_path.h"
#include <cstdlib>
namespace mh::sim::order_tasks_adapter {
namespace ot = mh::order_tasks;
namespace hp = mh::order_tasks::host_protocol;
// Per-handler synchronous control latch. It carries ONLY a returned engine control value
// through the original handler return, never a guessed phase or a second queue authority.
class handler_budget_scope {
public:
    handler_budget_scope(uint32_t player, uint32_t slot);
    ~handler_budget_scope();
    bool hold                                                     = false;
    handler_budget_scope(const handler_budget_scope &)            = delete;
    handler_budget_scope &operator=(const handler_budget_scope &) = delete;

private:
    friend void           latch(ot::actor_key, ot::control);
    handler_budget_scope *previous_;
    uint32_t              player_, slot_;
};
void latch(ot::actor_key, ot::control);
void published();
struct scope {
    ot::ticket            owner{};
    ot::executor          family{};
    ot::action            action{};
    ot::phase             phase{};
    ot::tile              assigned{};
    ot::assignment_reason placement{};
    bool                  present = false, assignment_valid = false;
    uint32_t              path_slot         = 0;
    bool                  planner_succeeded = false;
    ot::tile              requested{};
    uint32_t              group = 0;
};
inline mh_order_tasks_result request(sim_store &own, hp::operation op, uint32_t p = 0, uint32_t slot = 0) {
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(op);
    q.player    = p;
    q.slot      = slot;
    own.order_tasks_request(q, r);
    return r;
}
struct ground_patrol_placement {
    bool     eligible = false;
    uint32_t group = 0, sequence = 0;
};
inline ground_patrol_placement placement_patrol(sim_store &own, uint32_t p, uint32_t slot) {
    const auto r = request(own, hp::operation::ground_patrol_placement, p, slot);
    return {r.values[0] != 0, r.values[1], r.values[2]};
}
inline void begin_world_replacement(sim_store &own, bool new_campaign = false) {
    if (!own.has_order_tasks_observer()) return;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(new_campaign ? hp::operation::new_campaign : hp::operation::begin_load);
    q.args[0]   = uint32_t(hp::begin_load_reason::preserve_input_ring);
    if (!own.order_tasks_request(q, r) ||
        (new_campaign ? !r.values[0] : r.values[0] == UINT32_MAX)) std::abort();
}
inline void publish_fresh_world(const sim_view &v, sim_store &own) {
    if (!own.has_order_tasks_observer()) return;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::fresh_world);
    q.args[0]   = uint32_t(*v.current_system);
    q.args[1]   = uint32_t(*v.planet_index);
    q.args[2]   = uint32_t(*v.map_width);
    q.args[3]   = uint32_t(*v.map_height);
    if (!own.order_tasks_request(q, r) || !r.values[0]) std::abort();
}
inline scope capture(sim_store &own, uint32_t p, uint32_t slot) {
    const auto r = request(own, hp::operation::ticket, p, slot);
    if (!r.values[0]) return {};
    return {hp::get_ticket(r.values + 1), ot::executor(r.values[10]), ot::action(r.values[13]), ot::phase(r.values[11]), {r.values[15], r.values[16]}, ot::assignment_reason(r.values[17]), true, r.values[14] != 0, r.values[18], r.values[20] != 0, {r.values[21], r.values[22]}, r.values[23]};
}
inline scope capture(const sim_view &v, sim_store &own) { return capture(own, *v.cur_player, *v.cur_index); }
inline bool  air_boundary(const sim_view &v, sim_store &own, scope &current) {
    const auto &before = own.cur_unit();
    if (!own.has_order_tasks_observer() || before.move_microstep != 31 || before.state != 17 || before.order != 16) return false;
    const auto type = v.cfg_units[before.unit_proto_id].type;
    if (type != 17 && type != 18) return false;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::air_boundary);
    q.player    = *v.cur_player;
    q.slot      = *v.cur_index;
    q.args[0]   = before.path_slot_id == 0xff ? UINT32_MAX : uint32_t(before.path_slot_id);
    q.args[1]   = uint32_t(before.path_cursor);
    if (!own.order_tasks_request(q, r)) return false;
    // An owned 64-step successor is allocation-free. Failed replacement at its
    // terminator is an invariant violation, never finite arrival or airborne hold.
    if (!r.values[0] && r.values[1] && q.args[1] == ot::AIR_CHUNK_STEPS) std::abort();
    if (!r.values[0]) return false;
    current = capture(own, q.player, q.slot); // real passage may change the logical leg
    return true;
}
inline bool                  travel(const scope &s) { return s.action == ot::action::move || s.action == ot::action::patrol || s.phase == ot::phase::returning_anchor; }
inline ot::assignment_reason assignment_for(const scope &s, ot::tile goal) {
    return goal.x == s.requested.x && goal.y == s.requested.y ? ot::assignment_reason::requested : s.group ? ot::assignment_reason::formation
                                                                                                           : ot::assignment_reason::valid_projection;
}
inline ot::control observe(sim_store &own, const scope &s, ot::outcome_kind kind,
                           const unit &u, ot::tile assigned = {},
                           ot::assignment_reason placement = ot::assignment_reason::none, uint32_t path_slot = UINT32_MAX,
                           uint32_t producer_line = 0) {
    if (!s.present) return ot::control::unchanged;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    if (path_slot == UINT32_MAX)
        path_slot = kind == ot::outcome_kind::reached_exact || kind == ot::outcome_kind::reached_near ? s.path_slot : u.path_slot_id;
    hp::put_outcome(q, {s.owner, kind, s.family, {u.x, u.y}, assigned, placement, path_slot});
    q.args[21] = producer_line;
    q.args[22] = 2; // Translated carrier; host-only event diagnostics.
    own.order_tasks_request(q, r);
    const auto result = ot::control(r.values[0]);
    latch(s.owner.actor, result);
    return result;
}
inline bool accepted_near(sim_store &own, scope s, const unit &u, bool on_goal) {
    if (!s.present || !travel(s) || s.phase == ot::phase::blocked) return false;
    if (!s.assignment_valid &&
        (!s.planner_succeeded || (on_goal && (u.goal_x != u.x || u.goal_y != u.y)))) return false;
    if (u.order == 16 && s.assignment_valid &&
        (!on_goal || s.assigned.x != u.x || s.assigned.y != u.y)) return false;
    ot::control accepted = ot::control::unchanged;
    if (s.family == ot::executor::ground_run) observe(own, s, ot::outcome_kind::preparation_finished, u);
    if (!s.assignment_valid) {
        observe(own, s, ot::outcome_kind::assigned, u, {u.x, u.y}, ot::assignment_reason::valid_projection, 255);
        s        = capture(own, s.owner.actor.player, s.owner.actor.slot);
        accepted = observe(own, s, ot::outcome_kind::reached_exact, u, {u.x, u.y}, ot::assignment_reason::valid_projection);
    } else if (on_goal && s.assigned.x == u.x && s.assigned.y == u.y) {
        accepted = observe(own, s, ot::outcome_kind::reached_exact, u, s.assigned, s.placement);
    } else {
        accepted = observe(own, s, ot::outcome_kind::reached_near, u, {u.x, u.y}, ot::assignment_reason::valid_projection);
    }
    return u.order != 16 || accepted == ot::control::hold;
}
inline void prepare_fresh_continuation(const sim_view &v, unit &u) {
    if (v.cfg_units[u.unit_proto_id].type <= 14 || u.move_heading >= 24 || u.move_microstep < 0 || u.move_microstep >= 32) return;
    const uint8_t facing = v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing;
    if (u.move_microstep == 31 && (u.state == 0x0b || u.state == 0x11 || u.state == 0x12 || u.state == 0x2c || u.state == 0x2d) && u.facing_target != facing) {
        u.facing_target  = facing;
        u.facing_current = facing; // measured PT-FIX3 native fresh-order boundary
    } else if (u.state == 0x2e || u.state == 0x2f) {
        if (u.facing_target != facing) u.state = 0x2f;
        else if (u.move_microstep < 31) u.state = 0x12;
    }
}
inline bool primary_access(const sim_view &v, sim_store &own) {
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::primary_access);
    q.player    = *v.cur_player;
    q.slot      = *v.cur_index;
    if (!own.order_tasks_request(q, r)) return true; // pure component fixture has no observer
    latch({q.player, q.slot, 0}, ot::control(r.values[1]));
    if (!r.values[0] && !r.values[2]) prepare_fresh_continuation(v, own.cur_unit());
    // No fake order_queued flag: hover requires a real queued record. Only a settled actor
    // waiting for a real facing/safe boundary spends this frame's remaining budget here.
    if (!r.values[0] && !r.values[2] && (own.cur_unit().move_microstep >= 31 || own.cur_unit().state == 0x2e || own.cur_unit().state == 0x2f))
        own.tick_budget() = 0.0;
    return r.values[0] != 0;
}
inline bool held(sim_store &own, uint32_t p, uint32_t slot) { return request(own, hp::operation::hold, p, slot).values[0] != 0; }
inline bool suppress_home(sim_store &own, uint32_t p, uint32_t slot) { return request(own, hp::operation::suppress_home, p, slot).values[0] != 0; }
inline void record_applied(sim_store &own, uint32_t p, uint32_t slot, uint16_t owner,
                           uint16_t code, int16_t param0, const int32_t (&args)[13], ot::tile anchor = {}) {
    if (code == 0x1d || code == 0x1e) return;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(uint32_t(args[8]) == hp::AUTONOMOUS_PRIMARY ? hp::operation::autonomous_applied : hp::operation::ordinary_applied);
    q.player    = p;
    q.slot      = slot;
    q.args[0]   = owner;
    q.args[1]   = code;
    q.args[2]   = uint16_t(param0);
    for (uint32_t i = 0; i < 13; ++i) q.args[3 + i] = uint32_t(args[i]);
    if (uint32_t(args[8]) == hp::AUTONOMOUS_PRIMARY) {
        q.args[16] = anchor.x;
        q.args[17] = anchor.y;
    }
    own.order_tasks_request(q, r);
}
inline void record_discarded(sim_store &own, uint32_t p, uint32_t slot, uint16_t owner,
                             uint16_t code, int16_t param0, const int32_t (&args)[13],
                             uint32_t lane = 0, uint32_t begin = 0, uint32_t end = 0) {
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::discarded);
    q.player    = p;
    q.slot      = slot;
    q.args[0]   = owner;
    q.args[1]   = code;
    q.args[2]   = uint16_t(param0);
    for (uint32_t i = 0; i < 13; ++i) q.args[i + 3] = uint32_t(args[i]);
    q.args[18] = lane;
    q.args[19] = begin;
    q.args[20] = end;
    own.order_tasks_request(q, r);
    latch({p, slot, 0}, ot::control(r.values[0]));
}
inline bool admit_primary(sim_store &own, const order &record, ot::tile *anchor = nullptr, bool inspect_only = false) {
    if ((record.owner_and_kind & 0xf0u) != 0x80u || record.order_code == 0x1d || record.order_code == 0x1e) return true;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::primary_admission);
    q.player    = record.owner_and_kind & 15;
    q.slot      = record.unit_index;
    q.args[0]   = record.owner_and_kind;
    q.args[1]   = record.order_code;
    q.args[2]   = uint16_t(record.param0);
    for (uint32_t i = 0; i < 13; ++i) q.args[i + 3] = uint32_t(record.args[i]);
    q.args[16]            = inspect_only ? 1 : 0; // retained-record inspection must never cancel ordinary ownership
    const int32_t handled = own.order_tasks_request(q, r);
    // A pure component fixture has no policy observer. Never accept tagged identity there.
    if (!handled) return !hp::is_primary_marker(uint32_t(record.args[8]));
    if (r.values[0] && anchor) *anchor = {r.values[2], r.values[3]};
    if (r.values[0] && anchor && (uint32_t(record.args[8]) == hp::AUTONOMOUS_PRIMARY || uint32_t(record.args[8]) == hp::PHYSICAL_PRIMARY)) {
        mh_order_tasks_request handoff{};
        mh_order_tasks_result  result{};
        handoff.operation = uint32_t(hp::operation::reservation);
        handoff.player    = q.player;
        handoff.slot      = q.slot;
        handoff.args[0]   = uint32_t(hp::reservation_operation::handoff);
        handoff.args[1]   = uint32_t((record.args[5] & 0x40) ? ot::object_kind::building : ot::object_kind::unit);
        handoff.args[2]   = uint32_t(record.args[5]) & 15;
        handoff.args[3]   = uint32_t(record.args[6]);
        handoff.args[4]   = uint32_t(record.args[11]);
        handoff.args[5]   = uint32_t(record.args[10]);
        return own.order_tasks_request(handoff, result) && result.values[0];
    }
    if (r.values[0] && anchor && uint32_t(record.args[8]) == hp::EXPLICIT_PRIMARY) {
        mh_order_tasks_request captured{};
        mh_order_tasks_result  result{};
        captured.operation = uint32_t(hp::operation::reservation);
        captured.player    = q.player;
        captured.slot      = q.slot;
        captured.args[0]   = uint32_t(hp::reservation_operation::captured_application);
        return own.order_tasks_request(captured, result) && result.values[0];
    }
    if (r.values[0] && anchor && !hp::is_primary_marker(uint32_t(record.args[8]))) {
        mh_order_tasks_request physical{};
        mh_order_tasks_result  result{};
        physical.operation = uint32_t(hp::operation::reservation);
        physical.player    = q.player;
        physical.slot      = q.slot;
        physical.args[0]   = uint32_t(hp::reservation_operation::physical_application);
        if (record.args[5] > 0 && record.args[6] > 0) {
            physical.args[1] = uint32_t((record.args[5] & 0x40) ? ot::object_kind::building : ot::object_kind::unit);
            physical.args[2] = uint32_t(record.args[5]) & 15;
            physical.args[3] = uint32_t(record.args[6]);
        }
        return own.order_tasks_request(physical, result) && result.values[0];
    }
    return r.values[0] != 0;
}
inline void refresh_queued(sim_store &own, uint32_t player, uint32_t slot, uint16_t owner, uint32_t kept, uint32_t current) {
    auto &u = own.unit_at(player, slot);
    if (!u.order_queued) return;
    u.order_queued = 0;
    for (int32_t i = 0; i < own.order_count(); ++i) {
        if (uint32_t(i) >= kept && uint32_t(i) <= current) continue;
        const auto &record = own.order_at(i);
        if (record.unit_index == slot && record.owner_and_kind == owner && admit_primary(own, record, nullptr, true)) {
            u.order_queued = 1;
            break;
        }
    }
}
inline bool  preparation(uint32_t state) { return state == 0x15 || state == 0x30 || state == 0x7c || state == 0x2d; }
inline scope begin_planning(sim_store &own, uint32_t p, uint32_t slot, const unit &u) {
    const auto before = capture(own, p, slot);
    observe(own, before, ot::outcome_kind::planning_started, u);
    return capture(own, p, slot);
}
inline void fail_wave(sim_store &own, uint32_t p, const sim_view &v, int32_t count, ot::outcome_kind kind) {
    for (int32_t i = 0; i < count; ++i) {
        const int32_t slot = v.group_move_scratch[i].unit_idx;
        if (slot <= 0 || uint32_t(slot) >= uint32_t(v.caps.units)) continue;
        const auto s = capture(own, p, uint32_t(slot));
        // Earlier independently committed members keep their successful assignment.
        if (s.present && !s.assignment_valid) observe(own, s, kind, v.units[p * v.caps.units + slot]);
    }
}
inline void succeeded_wave(sim_store &own, uint32_t p, const sim_view &v, int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        const int32_t slot = v.group_move_scratch[i].unit_idx;
        if (slot <= 0 || uint32_t(slot) >= uint32_t(v.caps.units)) continue;
        const auto s = capture(own, p, uint32_t(slot));
        if (s.present && s.phase != ot::phase::blocked)
            observe(own, s, ot::outcome_kind::planning_succeeded, v.units[p * v.caps.units + slot]);
    }
}
inline bool due_edit(sim_store &own, const order &record, uint32_t kept) {
    if (record.order_code != ot::COMMAND || record.param0 != ot::COMMAND || (record.owner_and_kind & 0xf0u) != 0) return false;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::due_edit);
    q.player    = record.owner_and_kind & 15;
    q.slot      = record.unit_index;
    q.args[0]   = record.owner_and_kind;
    q.args[1]   = record.order_code;
    q.args[2]   = uint16_t(record.param0);
    for (uint32_t i = 0; i < 13; ++i) q.args[3 + i] = uint32_t(record.args[i]);
    q.args[17] = kept + 1;
    own.order_tasks_request(q, r);
    return r.values[0] != 0;
}
inline void retire(sim_store &own, ot::object_kind kind, uint32_t p, uint32_t slot, bool lethal) {
    // Capture current canonical incarnation BEFORE lifecycle storage retires/reuses it.
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(hp::operation::retire_target);
    q.player    = p;
    q.slot      = slot;
    q.args[0]   = uint32_t(kind);
    q.args[2]   = lethal;
    own.order_tasks_request(q, r);
    if (kind == ot::object_kind::unit) request(own, hp::operation::retire_actor, p, slot);
}
// Settlement is a typed executor boundary, NOT an idle or notify65 observer. A valid
// assignment must already belong to this exact ticket. Plane final-leg behavior stays native.
inline void terminal(sim_store &own, const scope &s, const unit &u) {
    if (!s.present || !travel(s)) return;
    const bool reached = s.assignment_valid && u.x == s.assigned.x && u.y == s.assigned.y;
    observe(own, s, reached ? ot::outcome_kind::reached_exact : ot::outcome_kind::planning_failed, u, s.assigned, s.placement, UINT32_MAX, __LINE__);
}
inline ot::control cleanup(sim_store &own, const scope &s, const unit &u) {
    return observe(own, s, s.phase == ot::phase::excursion ? ot::outcome_kind::primary_excursion_ended : ot::outcome_kind::attack_cleanup_finished, u);
}
// Decode actual directions after formation relocation, including native special one-step routes.
// Exact goals need no inference. A differing plane endpoint requires the fresh complete solver
// proof at assignment below; neither a writer branch nor exhaustion alone proves successful travel.
inline bool direction_endpoint(const sim_view &v, uint32_t p, const unit &u, ot::tile &endpoint) {
    if (u.path_slot_id >= PATH_SLOTS_PER_PLAYER || u.path_cursor < 0 || u.path_cursor >= PATH_WAYPOINTS_PER_SLOT) return false;
    endpoint = {u.x, u.y};
    for (uint32_t i = uint32_t(u.path_cursor); i < PATH_WAYPOINTS_PER_SLOT; ++i) {
        const uint32_t heading = v.path_buffers[p * PATH_WAYPOINTS_PER_PLAYER + u.path_slot_id * PATH_WAYPOINTS_PER_SLOT + i].heading;
        if (heading == 255) return true;
        if (heading >= 24) return false;
        endpoint.x = (endpoint.x + uint32_t(v.dir_step_offsets[heading].dx)) & map_width_mask(v);
        endpoint.y = (endpoint.y + uint32_t(v.dir_step_offsets[heading].dy)) & map_height_mask(v);
    }
    return false;
}
inline void direction_ready(sim_store &own, const scope &s, const unit &u, ot::executor family, uint32_t state) {
    if (!s.present || s.family != family || u.state != state) return;
    const auto  live = capture(own, s.owner.actor.player, s.owner.actor.slot);
    const auto &a    = s.owner;
    const auto &b    = live.owner;
    if (!live.present || live.phase != ot::phase::preparation || !live.assignment_valid ||
        live.path_slot != u.path_slot_id || a.world.system != b.world.system || a.world.planet != b.world.planet ||
        a.world.incarnation != b.world.incarnation || a.actor.player != b.actor.player || a.actor.slot != b.actor.slot ||
        a.actor.incarnation != b.actor.incarnation || a.route != b.route || a.leg != b.leg || a.attempt != b.attempt) return;
    observe(own, s, ot::outcome_kind::preparation_finished, u);
}
inline void direction_assigned(const sim_view &v, sim_store &own, const scope &s, uint32_t p, const unit &u, const uint8_t *fresh_frontier) {
    // Actual attack/excursion paths also publish assignments. Only terminal()
    // restricts movement-arrival completion to travel tasks.
    if (!s.present) return;
    ot::tile   endpoint{};
    const bool encoded   = direction_endpoint(v, p, u, endpoint);
    const bool exact     = encoded && endpoint.x == u.goal_x && endpoint.y == u.goal_y;
    const bool projected = encoded && !exact &&
                           ((s.family == ot::executor::direction_plane && u.state == 11) ||
                            (s.family == ot::executor::direction_nonplane && (u.state == 18 || u.state == 55))) &&
                           u.path_cursor == 0 &&
                           ot::complete_direction_trace(fresh_frontier,
                                                        reinterpret_cast<const uint8_t *>(&v.path_buffers[p * PATH_WAYPOINTS_PER_PLAYER +
                                                                                                          u.path_slot_id * PATH_WAYPOINTS_PER_SLOT]),
                                                        PATH_WAYPOINTS_PER_SLOT);
    const bool valid = exact || projected;
    observe(own, s, valid ? ot::outcome_kind::assigned : ot::outcome_kind::planning_failed, u,
            valid ? endpoint : ot::tile{u.goal_x, u.goal_y},
            projected ? ot::assignment_reason::valid_projection : exact ? assignment_for(s, endpoint)
                                                                        : ot::assignment_reason::none,
            UINT32_MAX, __LINE__);
    if (valid) direction_ready(own, s, u, ot::executor::direction_nonplane, 18);
}
} // namespace mh::sim::order_tasks_adapter
