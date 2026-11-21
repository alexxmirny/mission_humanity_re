// Destination: src/mh_dll/mh/seams/order_tasks_hosted.cpp
#include "seams/order_tasks_hosted.h"
#include "include/mh_order_tasks_dispatch.h"
#include "include/mh_air_patrol.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "caps/path_pool.h"
#include "seams/storage_policy.h"
#include "seams/storage_policy_save.h"
#include "state/host_bind.h"
#include "state/region_runtime.h"
#include "sim/sim_attack_range_footprint.h"
#include "seams/order_tasks_native.h"
#include "seams/strategic_input_native.h"
#include "../../libmh/orders/order_index_codec.h"
#include "seams/net_internal.h"
#include "mh_config_dir.h"
#include "mh_ini_gate.h"
#include <cstdio>
extern "C" int MH_OrderWideIndex_Enabled();
extern "C" int MH_LibmhModule_IsBound();
// Undefined C imports are derived into the existing required cached libmh contract.
extern "C" int32_t libmh_order_tasks_bind(void *, uint32_t, void *, uint32_t);
extern "C" void    libmh_order_tasks_published();
extern "C" int32_t libmh_order_tasks_begin_load();
extern "C" void    libmh_order_tasks_abort_load(int32_t);
#include "ui/target_line_order_events.h"
#include "ui/target_line_route_adapter.h"
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>
extern int g_arf_rt; // Existing native combat_proto.cpp feature state, not a second knob.

namespace mh::order_tasks::hosted {
namespace {
using U                = mh::game::mh_map_object_unit;
using B                = mh::game::mh_map_object_building;
using C                = mh::game::mh_cfg_final_struct_Unit;
using O                = mh::game::mh_llm_strat_order;
namespace R            = mh::state;
namespace P            = host_protocol;
namespace presentation = mh::ui::selected_targets::detail;
struct native_context {
    uint32_t capture_limit = UINT32_MAX, captured_index = UINT32_MAX;
    O        captured{};
    uint32_t hole_first[3]{}, hole_end[3]{};
    // Invocation-local diagnostic scratch. Never canonical and never consulted by gameplay.
    bool           diagnostic_active = false;
    const char    *diagnostic_stage  = "not_reached";
    actor_key      diagnostic_actor{};
    uint32_t       diagnostic_index = UINT32_MAX;
    active_capture diagnostic_capture{};
};
native_context context;
struct owned_state {
    std::unique_ptr<uint8_t[]> allocation;
    uint32_t                   capacity = 0;
    bool                       live = false, publication_pending = false;
    uint8_t                    campaign_bytes[campaign_state::BYTES]{};
    state                      routes;
    campaign_state             campaign;
    owned_state() { campaign.initialize(campaign_bytes, sizeof campaign_bytes); }
} g;
struct warning_key {
    uint32_t life = 0, route = 0, leg = 0, attempt = 0, reason = 0;
};
warning_key warned[500]{};
uint32_t    refusals[32]{}, failures[32]{};
struct due_player_edit {
    uint32_t  index_plus_one = 0;
    actor_key actor{};
    O         record{};
};
// Derivable pre-AI scratch only. This is not an origin tag, lease, order or authority.
due_player_edit due_player_edits[8][500]{};
world_key       due_player_world{};
bool            due_player_batch    = false;
uint32_t        due_player_receipts = 0;
uint32_t        admission_receipts  = 0;
uint32_t        local_player() {
    const auto *p = R::ptr<const uint16_t>(R::RID_PLAYERSIDE);
    return p ? *p : UINT32_MAX;
}
void clear_local() {
    context          = native_context{};
    due_player_batch = false;
    due_player_world = {};
    for (auto &player : due_player_edits)
        for (auto &entry : player) entry.index_plus_one = 0;
    due_player_receipts = 0;
    admission_receipts  = 0;
    native::before_world_publication();
    mh::seams::strategic_input_native::reset_pending_commands();
    std::memset(warned, 0, sizeof warned);
    std::memset(refusals, 0, sizeof refusals);
    std::memset(failures, 0, sizeof failures);
}
bool                                        import_legacy_primaries();
mh::storage_policy::savefile::order_context save_context() {
    mh::storage_policy::savefile::order_context c;
    c.owner         = &g.routes;
    c.campaign      = &g.campaign;
    c.current_world = [](void *, world_key &key) {
        const auto *system = R::ptr<const int32_t>(R::RID_CURRENTSYSTEM);
        const auto *planet = R::ptr<const int32_t>(R::RID_G_PLANET_INDEX);
        if (!system || !planet || *system < 0 || *planet < 0 || *planet >= 32) return false;
        const uint32_t incarnation = g.campaign.world_inc(uint32_t(*planet));
        if (!incarnation) return false;
        key = {uint32_t(*system), uint32_t(*planet), incarnation};
        return true;
    };
    c.prepare_loaded = [](void *, dimensions d, world_key key) {
        const auto have = g.routes.shape();
        return have.players == d.players && have.units == d.units && have.buildings == d.buildings &&
               setup(d.width, d.height, key);
    };
    c.clear_pending   = [](void *) { clear_local(); };
    c.building_exists = [](void *, uint32_t p, uint32_t s) {
        return mh::storage_policy::hosted::building_exists(p, s);
    };
    c.begin_load  = [](void *) { return begin_load(P::begin_load_reason::preserve_input_ring) != UINT32_MAX; };
    c.finish_load = [](void *, bool legacy_routes) {
        return (!legacy_routes || import_legacy_primaries()) && bind_published();
    };
    return c;
}
bool same_ticket(const ticket &, const ticket &);
bool failure_outcome(outcome_kind);
void trace_queue(const char *, uint32_t, uint32_t, uint32_t, const route_view &,
                 const route_view *, uint32_t, uint32_t, const mh_order_tasks_request *);
void report_one(uint32_t p, uint32_t s, const route_view *before = nullptr, uint32_t producer = 0, uint32_t carrier = 0) {
    if (p != local_player() || s >= 500) return;
    route_view r{};
    auto      &old = warned[s];
    if (!read_route(p, s, r) || !r.count || r.status != phase::blocked) {
        old = {};
        return;
    }
    const warning_key now{r.active.actor.incarnation, r.active.route, r.active.leg, r.active.attempt, uint32_t(r.blocked_reason)};
    if (old.life == now.life && old.route == now.route && old.leg == now.leg && old.attempt == now.attempt && old.reason == now.reason) return;
    old = now;
    if (now.reason >= 32) std::abort();
    ++failures[now.reason];
    const bool transition = before && before->status != phase::blocked && same_ticket(before->active, r.active);
    trace_queue("blocked", now.reason, p, s, r, transition ? before : nullptr,
                transition ? producer : 0u, transition ? carrier : 0u, nullptr);
}
void report_phase() {
    const auto     d = g.routes.shape();
    const uint32_t p = local_player();
    if (p < d.players)
        for (uint32_t s = 1; s < d.units; ++s) report_one(p, s);
    for (uint32_t i = 1; i < 32; ++i) {
        if (refusals[i]) mh::seams::strategic_input_native::refused(i, refusals[i]);
        if (failures[i]) mh::seams::strategic_input_native::blocked(i, failures[i]);
    }
    std::memset(refusals, 0, sizeof refusals);
    std::memset(failures, 0, sizeof failures);
}

template <class T>
const T *region(R::region_id id) { return R::ptr<const T>(id); }
// Raw binary64 positivity avoids loading the native x87 stack at observation sites.
bool positive(const double &v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return !(bits >> 63) && (bits & 0x7fffffffffffffffull) != 0 &&
           (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}
bool bound(mh::caps::arr array, uint32_t p, uint32_t s) {
    const auto &d = mh::caps::g(array);
    return d.base && p < d.players && s && s < d.limit;
}
U *unit(uint32_t p, uint32_t s) {
    return bound(mh::caps::UNITS, p, s) ? mh::caps::rec<U>(mh::caps::UNITS, p, s) : nullptr;
}
bool same_ticket(const ticket &a, const ticket &b) {
    return a.world.system == b.world.system && a.world.planet == b.world.planet &&
           a.world.incarnation == b.world.incarnation && a.actor.player == b.actor.player &&
           a.actor.slot == b.actor.slot && a.actor.incarnation == b.actor.incarnation &&
           a.route == b.route && a.leg == b.leg && a.attempt == b.attempt;
}
bool failure_outcome(outcome_kind kind) {
    return kind == outcome_kind::planning_failed || kind == outcome_kind::pool_exhausted ||
           kind == outcome_kind::give_up || kind == outcome_kind::rejected;
}
void trace_queue(const char *event, uint32_t reason, uint32_t p, uint32_t s,
                 const route_view &route, const route_view *before,
                 uint32_t producer, uint32_t carrier, const mh_order_tasks_request *request) {
    const U  *u = unit(p, s);
    node_view node{};
    if (route.count) read_entry(p, s, route.cursor, node);
    const auto &r     = before ? *before : route;
    const auto  world = g.routes.world();
    char        line[1536];
    std::snprintf(line, sizeof line,
                  "; [order_queue] event=%s reason=%u carrier=%u producer_line=%u world=%u/%u/%u "
                  "player=%u slot=%u actorlife=%u route=%u leg=%u attempt=%u action=%u request=%u,%u seq=%u group=%u "
                  "count=%u cursor=%u cycle=%u phase=%u proto=%u state=%u order=%u xy=%u,%u goal=%u,%u microstep=%d "
                  "pathcursor=%d nativepathslot=%u assignedvalid=%u assigned=%u,%u placement=%u qpathslot=%u "
                  "implicitanchor=%u,%u implicit=%u active=%u circuitstart=%u,%u,%u progress=%u "
                  "edit_payload=%u,%u,%u,%u\n",
                  event, reason, carrier, producer, world.system, world.planet, world.incarnation,
                  p, s, route.active.actor.incarnation, route.active.route, route.active.leg, route.active.attempt,
                  uint32_t(node.task.kind), node.task.requested.x, node.task.requested.y, node.task.command_sequence, node.task.group,
                  route.count, route.cursor, route.cycle_begin, uint32_t(r.status),
                  u ? uint32_t(u->unit_proto_id) : 0u, u ? uint32_t(u->state) : 0u, u ? uint32_t(u->order) : 0u,
                  u ? uint32_t(u->x) : 0u, u ? uint32_t(u->y) : 0u,
                  u ? uint32_t(u->goal_x) : 0u, u ? uint32_t(u->goal_y) : 0u,
                  u ? int(u->move_microstep) : 0, u ? int(u->path_cursor) : 0,
                  u ? uint32_t(mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u))) : UINT32_MAX,
                  uint32_t(r.assignment_valid), r.assigned.x, r.assigned.y, uint32_t(r.placement), r.path_slot,
                  r.implicit_anchor.x, r.implicit_anchor.y, uint32_t(r.has_implicit_anchor), uint32_t(r.implicit_leg_active),
                  r.circuit_start_cursor, r.circuit_start_position.x, r.circuit_start_position.y, uint32_t(r.circuit_made_progress),
                  request ? request->args[3] : 0u, request ? request->args[4] : 0u,
                  request ? request->args[8] : 0u, request ? request->args[13] : 0u);
    seam_log(line);
}
bool diagnostic_outcome(outcome_kind kind) {
    return (kind == outcome_kind::assigned || kind == outcome_kind::reached_exact ||
            kind == outcome_kind::reached_near) &&
           diagnostics_enabled();
}
void trace_observed(const route_view &before, outcome_kind kind, tile assigned,
                    uint32_t path, control result, uint32_t producer, uint32_t carrier) {
    static uint32_t receipts = 0;
    if (receipts >= 4096) return;
    route_view after{};
    const auto actor = before.active.actor;
    if (!read_route(actor.player, actor.slot, after) || !same_ticket(before.active, after.active)) return;
    const bool assignment = kind == outcome_kind::assigned && after.assignment_valid &&
                            after.assigned.x == assigned.x && after.assigned.y == assigned.y &&
                            after.path_slot == path;
    const bool passage = (kind == outcome_kind::reached_exact || kind == outcome_kind::reached_near) &&
                         result == control::hold;
    if (!assignment && !passage) return;
    ++receipts;
    trace_queue(assignment ? "assigned" : kind == outcome_kind::reached_exact ? "reached_exact"
                                                                              : "reached_near",
                uint32_t(kind), actor.player, actor.slot, after, assignment ? nullptr : &before,
                producer, carrier, nullptr);
}
const C *config(uint32_t proto) {
    const auto *v = region<C>(R::RID_UNIT);
    return v && proto && proto < R::live_size(R::RID_UNIT) / sizeof(C) ? v + proto : nullptr;
}
bool accounting_read(void *, actor_key a, reservations::actual_primary &out) {
    U *u = unit(a.player, a.slot);
    if (!u || !a.incarnation || identity(nullptr, object_kind::unit, a.player, a.slot).incarnation != a.incarnation) return false;
    const uint16_t ref = uint16_t(u->target_ref);
    out.kind           = !ref ? object_kind::none : (ref & 0x40) ? object_kind::building
                                                : (ref & 0xa0)   ? object_kind::unit
                                                                 : object_kind::none;
    if (ref && out.kind == object_kind::none) return false;
    out.player       = ref & 15;
    out.slot         = uint16_t(u->target_index);
    out.cached_raw16 = uint16_t(u->committed_weapon_damage_est);
    out.owned_bits   = uint32_t(u->engagement_flags & 1u) | (uint32_t(u->order_status_flags & 0x80u) << 8);
    return out.kind == object_kind::none || out.slot != 0;
}
bool accounting_write(void *, actor_key a, uint32_t cached, uint32_t bits) {
    U *u = unit(a.player, a.slot);
    if (!u || identity(nullptr, object_kind::unit, a.player, a.slot).incarnation != a.incarnation) return false;
    u->committed_weapon_damage_est = int16_t(uint16_t(cached));
    u->engagement_flags            = uint8_t((u->engagement_flags & 0xfeu) | (bits & 1u));
    u->order_status_flags          = uint8_t((u->order_status_flags & 0x7fu) | ((bits >> 8) & 0x80u));
    return true;
}
bool accounting_adjust(void *, target_key t, uint32_t delta, bool subtract) {
    if (!t.object.incarnation || identity(nullptr, t.kind, t.object.player, t.object.slot).incarnation != t.object.incarnation) return false;
    int16_t *value = nullptr;
    if (t.kind == object_kind::unit) {
        U *u = unit(t.object.player, t.object.slot);
        if (u) value = &u->incoming_threat_damage;
    } else if (t.kind == object_kind::building && bound(mh::caps::BUILDINGS, t.object.player, t.object.slot)) {
        value = &mh::caps::rec<B>(mh::caps::BUILDINGS, t.object.player, t.object.slot)->incoming_damage_tally;
    }
    if (!value) return false;
    const uint32_t old = uint16_t(*value);
    *value             = int16_t(uint16_t(subtract ? old - uint16_t(delta) : old + uint16_t(delta)));
    return true;
}
bool accounting_clear(void *, actor_key a) {
    U *u = unit(a.player, a.slot);
    if (!u || identity(nullptr, object_kind::unit, a.player, a.slot).incarnation != a.incarnation) return false;
    u->target_ref   = 0;
    u->target_index = 0;
    return true;
}
bool retained_application(void *, actor_key a, target_key target) {
    for (uint32_t lane = 0; lane < 3; ++lane) {
        const auto     w = mh::caps::orders::which(lane);
        const auto    *v = reinterpret_cast<const O *>(mh::caps::orders::base(w));
        const uint32_t n = mh::caps::orders::live_count(w);
        for (uint32_t i = 0; i < n; ++i) {
            if (i >= context.hole_first[lane] && i < context.hole_end[lane]) continue;
            O q = v[i];
            if (w == mh::caps::orders::STAGING) {
                q.unit_index = MH_OrderWideIndex_Enabled() ? mh::orders::decode_index(q.unit_index) : uint16_t(q.unit_index & 255);
                q.owner_and_kind &= 255;
                q.order_code &= 255;
                q.param0 = int16_t(uint16_t(q.param0) & 255);
            }
            if (q.unit_index != a.slot || q.owner_and_kind != (a.player | 0x80) ||
                q.order_code == 29 || q.order_code == 30 || q.param0 == 29 || q.param0 == 30) continue;
            if (P::is_primary_marker(uint32_t(q.args[8]))) continue;
            if (!((q.param0 >= 26 && q.param0 <= 28) || (q.param0 == 1 && (q.order_code == 26 || q.order_code == 28)))) continue;
            const uint32_t ref = uint16_t(q.args[5]);
            if ((ref & 15) == target.object.player && uint16_t(q.args[6]) == target.object.slot &&
                bool(ref & 0x40) == (target.kind == object_kind::building)) return true;
        }
    }
    return false;
}
bool retained_marker(void *, actor_key a, uint32_t magic, uint32_t w10, uint32_t w11, const entry &expected) {
    for (uint32_t lane = 0; lane < 3; ++lane) {
        const auto  w = mh::caps::orders::which(lane);
        const auto *v = reinterpret_cast<const O *>(mh::caps::orders::base(w));
        for (uint32_t i = 0; i < mh::caps::orders::live_count(w); ++i) {
            if (i >= context.hole_first[lane] && i < context.hole_end[lane]) continue;
            O q = v[i];
            if (w == mh::caps::orders::STAGING) {
                q.unit_index = MH_OrderWideIndex_Enabled() ? mh::orders::decode_index(q.unit_index) : uint16_t(q.unit_index & 255);
                q.owner_and_kind &= 255;
                q.order_code &= 255;
                q.param0 = int16_t(uint16_t(q.param0) & 255);
            }
            if (q.unit_index != a.slot || q.owner_and_kind != (a.player | 0x80) ||
                uint32_t(q.args[8]) != magic || uint32_t(q.args[9]) != a.incarnation ||
                uint32_t(q.args[10]) != w10 || uint32_t(q.args[11]) != w11 ||
                q.order_code == 29 || q.order_code == 30 || q.param0 == 29 || q.param0 == 30) continue;
            entry actual{};
            if (!decode_applied(nullptr, a, q.order_code, q.param0, q.args, actual) || actual.kind != expected.kind) continue;
            if (actual.target.kind != expected.target.kind || actual.target.object.player != expected.target.object.player ||
                actual.target.object.slot != expected.target.object.slot ||
                actual.target.object.incarnation != expected.target.object.incarnation) continue;
            if (magic == P::EXPLICIT_PRIMARY &&
                (actual.requested.x != expected.requested.x || actual.requested.y != expected.requested.y || actual.group != expected.group)) continue;
            return true;
        }
    }
    return false;
}
const reservations::access ACCOUNTING{&context, accounting_read, accounting_write, identity, accounting_adjust, accounting_clear, retained_application, retained_marker};
bool                       admission_failure(const char *stage, actor_key actor, uint32_t index = UINT32_MAX) {
    if (context.diagnostic_active && std::strcmp(context.diagnostic_stage, "not_reached") == 0) {
        context.diagnostic_stage = stage;
        context.diagnostic_actor = actor;
        context.diagnostic_index = index;
    }
    return false;
}
void trace_admission(const mh_order_tasks_request &q, uint32_t reason) {
    // Fixed event-only ceiling per world. No frame/tick polling or record-array dump.
    constexpr uint32_t LIMIT = 192;
    if (admission_receipts >= LIMIT) return;
    ++admission_receipts;
    const actor_key              requested{q.player, q.slot, q.args[4]};
    const actor_key              current = identity(nullptr, object_kind::unit, q.player, q.slot);
    const auto                   world   = g.routes.world();
    const U                     *u       = unit(q.player, q.slot);
    const C                     *c       = u ? config(u->unit_proto_id) : nullptr;
    primary_reservation          descriptor{};
    const bool                   descriptor_read = g.routes.read_reservation(requested, descriptor);
    reservations::actual_primary actual{};
    const bool                   actual_read = accounting_read(nullptr, requested, actual);
    route_view                   route{};
    const bool                   route_read     = g.routes.read_route(requested, route);
    const bool                   cleanup        = g.routes.primary_cleanup_pending(requested);
    const auto                   target_current = descriptor.target.kind == object_kind::none ? actor_key{} : identity(nullptr, descriptor.target.kind, descriptor.target.object.player, descriptor.target.object.slot);
    // Mirrors capture_binding's predicates, but is observational only. The actual failure
    // stage above comes from the original call; this mask explains its exact input state.
    const bool target_live = descriptor.target.kind != object_kind::none && descriptor.target.object.incarnation &&
                             target_current.player == descriptor.target.object.player && target_current.slot == descriptor.target.object.slot &&
                             target_current.incarnation == descriptor.target.object.incarnation;
    const bool     fields_target = actual.kind == descriptor.target.kind && actual.player == descriptor.target.object.player && actual.slot == descriptor.target.object.slot;
    const bool     has_prior     = descriptor.prior_target.kind != object_kind::none && descriptor.prior_target.object.incarnation;
    const bool     fields_prior  = actual.kind == descriptor.prior_target.kind && actual.player == descriptor.prior_target.object.player && actual.slot == descriptor.prior_target.object.slot;
    const uint32_t packed        = (actual.cached_raw16 & 65535u) | ((actual.owned_bits & 0x8001u) << 16);
    const uint32_t predicates    = uint32_t(actual_read) | (uint32_t(descriptor_read) << 1) |
                                (uint32_t(route_read && route.count) << 2) | (uint32_t(cleanup) << 3) |
                                (uint32_t(target_live) << 4) | (uint32_t(fields_target) << 5) |
                                (uint32_t(has_prior) << 6) | (uint32_t(fields_prior) << 7) |
                                (uint32_t(actual.cached_raw16 == descriptor.delta_raw16) << 8) |
                                (uint32_t(actual.owned_bits == 0x8001u) << 9) |
                                (uint32_t(descriptor.execution_epoch == g.routes.execution_epoch(requested)) << 10) |
                                (uint32_t(descriptor.prior_packed == packed) << 11);
    char line[2048];
    std::snprintf(line, sizeof line,
                  "; [order_admission] event=refused reason=%u stage=%s observed_actor=%u/%u/%u record=%u "
                  "requested=%u/%u/%u current=%u/%u/%u world=%u/%u/%u requested_world=%u/%u/%u "
                  "proto=%u type=%d state=%u order=%u energypositive=%u microstep=%d heading=%u "
                  "targetref=%u targetindex=%u cached_raw16=%u owned8001=%u queued=%u "
                  "descriptor_read=%u descriptor_phase=%u descriptor_target=%u/%u/%u/%u target_live=%u "
                  "descriptor_delta=%u prior=%u/%u/%u/%u prior_packed=%u descriptor_epoch=%u live_epoch=%u "
                  "binding_predicates=0x%x route_read=%u route_count=%u cleanup=%u capture_limit=%u queue_count=%u "
                  "capture_count=%u capture_phase=%u capture_provenance=%u capture_assignment=%u capture_replan=%u "
                  "capture_action=%u capture_target=%u/%u/%u/%u edit=%u point=%u,%u seq=%u receipt=%u/%u\n",
                  reason, context.diagnostic_stage, context.diagnostic_actor.player, context.diagnostic_actor.slot, context.diagnostic_actor.incarnation, context.diagnostic_index,
                  requested.player, requested.slot, requested.incarnation, current.player, current.slot, current.incarnation,
                  world.system, world.planet, world.incarnation, q.args[5], q.args[6], q.args[7],
                  u ? uint32_t(u->unit_proto_id) : 0u, c ? int(c->type) : -1, u ? uint32_t(u->state) : 0u, u ? uint32_t(u->order) : 0u,
                  uint32_t(u && positive(u->energy)), u ? int(u->move_microstep) : -1, u ? uint32_t(u->move_heading) : UINT32_MAX,
                  u ? uint32_t(uint16_t(u->target_ref)) : 0u, u ? uint32_t(uint16_t(u->target_index)) : 0u,
                  u ? uint32_t(uint16_t(u->committed_weapon_damage_est)) : 0u,
                  u ? uint32_t(u->engagement_flags & 1u) | (uint32_t(u->order_status_flags & 0x80u) << 8) : 0u, u ? uint32_t(u->order_queued) : 0u,
                  uint32_t(descriptor_read), uint32_t(descriptor.phase), uint32_t(descriptor.target.kind), descriptor.target.object.player, descriptor.target.object.slot, descriptor.target.object.incarnation, uint32_t(target_live),
                  descriptor.delta_raw16, uint32_t(descriptor.prior_target.kind), descriptor.prior_target.object.player, descriptor.prior_target.object.slot, descriptor.prior_target.object.incarnation,
                  descriptor.prior_packed, descriptor.execution_epoch, g.routes.execution_epoch(requested), predicates, uint32_t(route_read), route.count, uint32_t(cleanup),
                  context.capture_limit, mh::caps::orders::live_count(mh::caps::orders::QUEUE), context.diagnostic_capture.count, uint32_t(context.diagnostic_capture.status),
                  uint32_t(context.diagnostic_capture.provenance), uint32_t(context.diagnostic_capture.assignment_valid), uint32_t(context.diagnostic_capture.requires_replan),
                  uint32_t(context.diagnostic_capture.nodes[0].kind), uint32_t(context.diagnostic_capture.nodes[0].target.kind),
                  context.diagnostic_capture.nodes[0].target.object.player, context.diagnostic_capture.nodes[0].target.object.slot, context.diagnostic_capture.nodes[0].target.object.incarnation,
                  (q.args[3] >> 8) & 255u, q.args[8] & 65535u, q.args[8] >> 16, q.args[13], admission_receipts, LIMIT);
    seam_log(line);
    if (admission_receipts == LIMIT) seam_log("; [order_admission] event=receipt_limit limit=192 (further refusal receipts suppressed until world publication)\n");
}
bool import_legacy_primaries() { return reservations::import_legacy_bindings(g.routes, ACCOUNTING); }
void primary_ref(actor_key a, uint32_t mode) {
    const bool handled = mode == 1 ? reservations::release_before(g.routes, ACCOUNTING, a) : mode == 0 ? reservations::rebound_before(g.routes, ACCOUNTING, a)
                                                                                                       : false;
    if (handled) return;
    mh::call::llm_strat_target_release_ref(a.player, int32_t(a.slot), mode);
    if (mode == 1) reservations::release_after(g.routes, ACCOUNTING, a);
    else if (mode == 0 && !reservations::rebound_after(g.routes, ACCOUNTING, a)) std::abort();
}
void prepare_unsafe_hover(U &u, const C &c) {
    if (c.type <= 14 || u.move_microstep < 0 || u.move_microstep > 31 || u.move_heading >= 24) return;
    const auto *m = region<mh::game::mh_llm_strat_move_microstep>(R::RID_STRAT_MOVE_MICROSTEPS);
    if (!m) return;
    const uint8_t facing = m[u.move_heading * 32 + u.move_microstep].facing;
    // Existing fresh-order PT-FIX3 continuation at480B86: settle the old aircraft heading
    // before F rechecks its unchanged safe gate. No synthetic native queued record.
    if (u.move_microstep == 31 && (u.state == 11 || u.state == 17 || u.state == 18 || u.state == 44 || u.state == 45) && u.facing_target != facing) {
        u.facing_current = facing;
        u.facing_target  = facing;
    } else if (u.state == 46 || u.state == 47) {
        // Native480899..4808E2: 2F rotates toward the microstep table; 2E keeps aiming.
        if (facing != u.facing_target) u.state = 47;
        else if (u.move_microstep < 31) u.state = 18;
    }
}
bool family_of(const C &c, executor &e) {
    // Complete finite constructor domain, EN cfg_final_unit_Construct004564c2.
    if (c.type >= 1 && c.type <= 14 && c.move_op_code == 15 && c.move_op_arg == 10 && c.default_op_code == 1) {
        e = executor::ground_run;
        return true;
    }
    if ((c.type == 17 || c.type == 18) && c.move_op_code == 17 && c.move_op_arg == 11 && c.default_op_code == 19) {
        e = executor::direction_plane;
        return true;
    }
    if ((c.type == 15 || c.type == 16 || (c.type >= 19 && c.type <= 24)) &&
        c.move_op_code == 18 && c.move_op_arg == 11 && c.default_op_code == 19) {
        e = executor::direction_nonplane;
        return true;
    }
    return false;
}
bool compatible(const U &u) {
    // Positive ordinary executor/preparation set. Never admit storage, merge, conversion,
    // orbital transfer, corpse/removal, landing request or deployment by exclusion guessing.
    switch (u.state) {
        case 1:
        case 10:
        case 11:
        case 12:
        case 15:
        case 16:
        case 17:
        case 18:
        case 19:
        case 21:
        case 26:
        case 27:
        case 28:
        case 44:
        case 45:
        case 46:
        case 47:
        case 48:
        case 49:
        case 55:
        case 124: break;
        default: return false;
    }
    switch (u.order) {
        case 0:
        case 1:
        case 10:
        case 11:
        case 15:
        case 16:
        case 17:
        case 18:
        case 19:
        case 26:
        case 27:
        case 28:
        case 46:
        case 47: return true;
        default: return false;
    }
}
bool safe(const U &u, const C &c) {
    if (u.move_microstep < 0 || u.move_microstep > 31) return false;
    if (u.move_microstep < 31 && (u.state == 15 || u.state == 17 || u.state == 18 || u.state == 46 || u.state == 47)) return false;
    if (c.type <= 14) return true;
    const auto *m = region<mh::game::mh_llm_strat_move_microstep>(R::RID_STRAT_MOVE_MICROSTEPS);
    if (!m || u.move_heading >= 24 || m[u.move_heading * 32 + u.move_microstep].facing != u.facing_target) return false;
    return u.state != 55 && u.state != 21 && u.state != 22 && u.state != 49 && u.state != 48 && u.state != 23 && u.state != 124;
}
bool read_actor(void *, actor_key key, actor_view &out) {
    if (!g.live) return admission_failure("actor.world_not_live", key);
    U       *u = unit(key.player, key.slot);
    const C *c = u ? config(u->unit_proto_id) : nullptr;
    executor family{};
    if (!u) return admission_failure("actor.slot_unbound", key);
    if (!c) return admission_failure("actor.prototype_invalid", key);
    if (!key.incarnation) return admission_failure("actor.requested_life_zero", key);
    if (identity(nullptr, object_kind::unit, key.player, key.slot).incarnation != key.incarnation)
        return admission_failure("actor.life_mismatch", key);
    if (!positive(u->energy)) return admission_failure("actor.energy_nonpositive_or_nonfinite", key);
    if (!family_of(*c, family)) return admission_failure("actor.executor_unsupported", key);
    if (!compatible(*u)) return admission_failure("actor.state_or_order_incompatible", key);
    if (u->move_microstep < 0 || u->move_microstep > 31) return admission_failure("actor.microstep_invalid", key);
    if (c->type > 14 && u->move_heading >= 24) return admission_failure("actor.heading_invalid", key);
    route_view          route{};
    primary_reservation binding{};
    if ((!g.routes.read_route(key, route) || !route.count) && !g.routes.primary_cleanup_pending(key) &&
        !reservations::capture_binding(g.routes, ACCOUNTING, key, binding)) return admission_failure("actor.primary_capture", key);
    out = {key, u->unit_proto_id, family, {u->x, u->y}, u->state, u->order, uint32_t(u->move_microstep), safe(*u, *c), uint32_t(u->move_heading), mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u)), uint32_t(u->path_cursor)};
    return true;
}
uint32_t target_mask(target_key t) {
    if (t.kind == object_kind::building) return 1;
    U *v = unit(t.object.player, t.object.slot);
    return v && v->elevation != 0 ? 2u : 1u;
}
target_status read_target(void *, actor_key a, target_key t) {
    actor_view actor{};
    if (!read_actor(nullptr, a, actor) || t.kind == object_kind::none || !t.object.incarnation ||
        identity(nullptr, t.kind, t.object.player, t.object.slot).incarnation != t.object.incarnation)
        return target_status::unavailable;
    const auto *pd = region<mh::game::mh_game_player_data>(R::RID_PLAYER_DATA);
    if (!pd || t.object.player >= g.routes.shape().players || pd[a.player].ai_player_relation[t.object.player] >= 0)
        return target_status::unavailable;
    if (t.kind == object_kind::unit) {
        const U *u = unit(t.object.player, t.object.slot);
        if (!u || !positive(u->energy) || !config(u->unit_proto_id)) return target_status::unavailable;
    } else {
        if (!bound(mh::caps::BUILDINGS, t.object.player, t.object.slot)) return target_status::unavailable;
        const auto &b = *mh::caps::rec<const B>(mh::caps::BUILDINGS, t.object.player, t.object.slot);
        if (b.index <= 0 || b.state == 2 || !positive(b.energy)) return target_status::unavailable;
    }
    // The native selector returns4 when any enabled compatible slot exists,100 otherwise.
    // Do not reinterpret4 as a real weapons[4] index or invent a different native weapon choice.
    return mh::call::llm_strat_unit_select_weapon(uint16_t(a.player), int32_t(a.slot), target_mask(t)) != 100 ? target_status::alive_hostile : target_status::unavailable;
}
bool native_order16_is_patrol(actor_key a, executor family) {
    if (family == executor::ground_run) return true;
    route_view route{};
    node_view  node{};
    return g.routes.read_route(a, route) && route.externally_pending &&
           g.routes.read_entry(a, route.cursor, node) &&
           node.task.kind == action::patrol;
}
bool capture_active_impl(void *, actor_key a, active_capture &out) {
    actor_view view{};
    if (!read_actor(nullptr, a, view)) return false;
    primary_reservation binding{};
    if (!reservations::capture_binding(g.routes, ACCOUNTING, a, binding)) return admission_failure("capture.primary_binding", a);
    const U &u   = *unit(a.player, a.slot);
    out          = {};
    out.position = {u.x, u.y};
    // FE is admitted while ordinary earlier deferred records may still execute the old activity.
    // Only its retained prefix is authoritative here; future same-pass records are not activity.
    const uint32_t all = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    const uint32_t n   = context.capture_limit == UINT32_MAX ? all : context.capture_limit;
    if (n > all) return admission_failure("capture.prefix_out_of_range", a);
    bool        found = false;
    const auto *q     = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    for (uint32_t i = 0; i < n; ++i) {
        if (q[i].unit_index != a.slot || (q[i].owner_and_kind & 15) != a.player ||
            (q[i].owner_and_kind & 0xf0) != 0x80) continue;
        if (q[i].param0 == 29 || q[i].param0 == 30 || q[i].order_code == 29 || q[i].order_code == 30) continue;
        if (uint32_t(q[i].args[8]) == P::PHYSICAL_PRIMARY) {
            const uint32_t   ref  = uint16_t(q[i].args[5]);
            const auto       kind = ref & 0x40 ? object_kind::building : (ref & 0xa0) ? object_kind::unit
                                                                                      : object_kind::none;
            const target_key target{kind, {ref & 15u, uint16_t(q[i].args[6]), uint32_t(q[i].args[11])}};
            if (uint32_t(q[i].args[9]) != a.incarnation ||
                !reservations::physical_record_current(g.routes, ACCOUNTING, a, target, uint32_t(q[i].args[10]))) continue;
        }
        if (found) return admission_failure("capture.multiple_retained_primaries", a, i);
        if (uint32_t(q[i].args[8]) == P::AUTONOMOUS_PRIMARY) return admission_failure("capture.autonomous_retained", a, i);
        if (uint32_t(q[i].args[8]) == P::EXPLICIT_PRIMARY) return admission_failure("capture.explicit_owned_retained", a, i);
        if (!decode_applied(nullptr, a, q[i].order_code, q[i].param0, q[i].args, out.nodes[0])) return admission_failure("capture.retained_decode", a, i);
        if (binding.phase == reservation_phase::pending &&
            (out.nodes[0].target.kind != binding.target.kind ||
             out.nodes[0].target.object.player != binding.target.object.player ||
             out.nodes[0].target.object.slot != binding.target.object.slot ||
             out.nodes[0].target.object.incarnation != binding.target.object.incarnation)) return admission_failure("capture.pending_target_mismatch", a, i);
        out.nodes[0].command_sequence = 0;
        context.captured_index        = i;
        context.captured              = q[i];
        found                         = true;
    }
    if (found) {
        if (!u.order_queued) return admission_failure("capture.retained_without_queued_flag", a);
        out.count      = 1;
        out.status     = phase::awaiting_application;
        out.provenance = out.nodes[0].kind == action::patrol ? native_provenance::genuine_patrol : out.nodes[0].kind == action::move ? native_provenance::active_move
                                                                                                                                     : native_provenance::active_attack;
        if (out.nodes[0].kind == action::move) {
            out.logical_move       = out.nodes[0].requested;
            out.logical_move_valid = true;
        }
        // Pending patrol is one known explicit point; a suffix append provides the next.
        // Actual native F adoption does not invent its newly written home as a cycle node.
        return true;
    }
    if (u.order_queued) return admission_failure("capture.queued_without_retained", a);
    if (binding.phase == reservation_phase::pending) return admission_failure("capture.pending_without_retained", a);
    entry alternate{};
    if (reservations::capture_bound_alternate(g.routes, ACCOUNTING, a, u.state, u.order, alternate)) {
        out.nodes[0]   = alternate;
        out.count      = 1;
        out.status     = phase::executing;
        out.provenance = native_provenance::active_attack;
        return read_target(nullptr, a, alternate.target) == target_status::alive_hostile ||
               admission_failure("capture.captured_alternate_target_unavailable", a);
    }
    const C &c = *config(u.unit_proto_id);
    if ((u.state == 1 || u.state == 19) && !u.target_ref && (u.order == 0 || u.order == 1 || u.order == 19)) return true;
    if (view.family != executor::ground_run && !u.target_ref &&
        (u.order == c.default_op_code || u.order == c.move_op_code || u.order == 16) &&
        (u.state == c.default_op_code ||
         (u.state == 46 &&
          mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(&u)) ==
              mh::caps::path::NO_PATH))) {
        return true;
    }
    int32_t args[13];
    for (auto &v : args) v = -1;
    args[0]        = u.goal_x;
    args[1]        = u.goal_y;
    args[12]       = u.move_group_id;
    args[5]        = u.target_ref;
    args[6]        = uint16_t(u.target_index);
    uint16_t param = u.order;
    if (param == 1 && (u.state == 12 || u.state == 15)) param = c.move_op_code;
    const uint16_t code = (u.state == 26 || u.state == 28) ? u.state : c.move_op_arg;
    if (!decode_applied(nullptr, a, code, int16_t(param), args, out.nodes[0])) return admission_failure("capture.executing_decode", a);
    out.count                     = 1;
    out.nodes[0].command_sequence = 0; // Executing legacy activity has no reusable issue token.
    out.status                    = phase::executing;
    out.provenance                = out.nodes[0].kind == action::move ? native_provenance::active_move : native_provenance::active_attack;
    if (param == 16 && native_order16_is_patrol(a, view.family)) {
        out.count              = 2;
        out.nodes[0].kind      = action::patrol;
        out.nodes[1]           = out.nodes[0];
        out.nodes[1].requested = {u.home_x, u.home_y};
        out.provenance         = native_provenance::genuine_patrol;
    }
    if (out.provenance == native_provenance::active_move)
        out.logical_move_valid = g.routes.ordinary_move_witness(a, out.logical_move);
    // This append preserves the actual primary objective, not a guessed historical click.
    // A firing/approaching ordinary1a/1c attack needs no spatial assignment to remain active.
    if (out.nodes[0].kind == action::attack_unit || out.nodes[0].kind == action::attack_building) {
        const target_key known = binding.phase == reservation_phase::bound ? binding.target : binding.prior_target;
        if (known.kind != out.nodes[0].target.kind || known.object.player != out.nodes[0].target.object.player ||
            known.object.slot != out.nodes[0].target.object.slot) return admission_failure("capture.executing_target_binding_mismatch", a);
        out.nodes[0].target = known;
        return read_target(nullptr, a, known) == target_status::alive_hostile || admission_failure("capture.executing_target_unavailable", a);
    }
    tile       end{};
    const bool encoded   = u.state != 45 && path_endpoint(a.player, a.slot, view.family, end);
    out.assignment_valid = encoded && end.x == u.goal_x && end.y == u.goal_y;
    if (out.assignment_valid) {
        out.assigned  = {u.goal_x, u.goal_y};
        out.placement = assignment_reason::requested;
        out.path_slot = mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(&u));
    }
    if (!out.assignment_valid && encoded && out.nodes[0].kind == action::move &&
        ((view.family == executor::direction_plane && u.state == 17) ||
         (view.family == executor::direction_nonplane && (u.state == 18 || u.state == 55)))) {
        out.requires_replan = true;
        return true;
    }
    if (u.state == 45 || u.state == 55) out.status = phase::preparation;
    if (!out.assignment_valid) {
        if (u.state == 55 || u.state == 10 || u.state == 11 || u.state == 21 || u.state == 45 || u.state == 48 || u.state == 49 || u.state == 124)
            out.status = phase::preparation;
        else return admission_failure("capture.no_certified_assignment_or_preparation", a);
    }
    return true;
}
bool capture_active(void *opaque, actor_key a, active_capture &out) {
    const bool result = capture_active_impl(opaque, a, out);
    if (context.diagnostic_active) context.diagnostic_capture = out;
    return result;
}
apply_result apply(void *, const execution &x) {
    actor_view actor{};
    if (x.task.group == UINT32_MAX) return apply_result::rejected; // F sentinel, never a concrete native group
    if (!read_actor(nullptr, x.owner.actor, actor)) return apply_result::rejected;
    if (x.air) {
        const air_chunk &chunk = *x.air;
        if (actor.family != executor::direction_plane || x.task.kind != action::patrol ||
            x.task.target.kind != object_kind::none || chunk.count != AIR_CHUNK_STEPS || actor.heading >= 24)
            return apply_result::rejected;
        uint32_t previous = actor.heading;
        for (uint32_t i = 0; i != AIR_CHUNK_STEPS; ++i) {
            if (!air::legal(previous, chunk.headings[i])) return apply_result::rejected;
            previous = chunk.headings[i];
        }
    }
    if (!actor.safe_to_apply) {
        U &old = *unit(x.owner.actor.player, x.owner.actor.slot);
        prepare_unsafe_hover(old, *config(old.unit_proto_id));
        return apply_result::deferred;
    }
    const auto a = x.owner.actor;
    U         &u = *unit(a.player, a.slot);
    const C   &c = *config(u.unit_proto_id);
    if (x.air) {
        air_chunk &chunk = *x.air;
        // Never bypass native takeoff, climb or landing preparation.
        // These stores only replace a genuinely settled, cruise-height predecessor.
        if (u.move_microstep != 31 || u.elevation != c.elevation ||
            (u.state != 11 && u.state != 17 && u.state != 19 && u.state != 44 && u.state != 45) ||
            u.order == 41 || u.order == 49)
            return apply_result::deferred;
        route_view route{};
        if (!g.routes.read_route(a, route) || !route.count) return apply_result::rejected;
        const bool     refill = route.guided_flight;
        const uint32_t actual = mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(&u));
        const uint32_t limit = mh::caps::path::slot_limit(), row = mh::caps::path::flags_row();
        uint8_t *const flags = mh::caps::path::flags();
        if (!flags || !mh::caps::path::buffers() || !limit || limit > row || a.player >= 8)
            return apply_result::rejected;
        if (chunk.slot != actual) return apply_result::rejected;
        uint32_t chosen = actual;
        if (actual != mh::caps::path::NO_PATH) {
            if (actual >= limit || flags[a.player * row + actual] == 0) return apply_result::rejected;
        } else {
            if (refill) return apply_result::rejected;
            for (chosen = 0; chosen != limit; ++chosen)
                if (flags[a.player * row + chosen] == 0) break;
            // Absolutely no F/target/path mutation before a real free slot exists.
            if (chosen == limit) return apply_result::deferred;
        }
        if (refill && (u.state != 17 || u.order != 16 || u.target_ref || u.target_index ||
                       route.path_slot != actual ||
                       (route.status != phase::executing && route.status != phase::returning_anchor)))
            return apply_result::rejected;
        if (x.task.requested.x >= g.routes.shape().width || x.task.requested.y >= g.routes.shape().height)
            return apply_result::rejected;
        using waypoint = mh::game::mh_llm_strat_path_waypoint;
        static_assert(sizeof(waypoint) == 2);
        auto *const path = reinterpret_cast<waypoint *>(mh::caps::path::waypoint(a.player, chosen, 0));
        if (!refill) {
            // Exact first F target/reference bookkeeping. Refill never repeats it.
            if (u.target_ref) primary_ref(a, 1);
            u.target_ref   = 0;
            u.target_index = 0;
            if (u.state == c.move_op_arg || u.state == c.move_op_code)
                mh::call::llm_strat_unit_notify_status(a.player, int32_t(a.slot), 0x67);
            u.order                    = 16;
            u.state                    = 17;
            u.goal_x                   = uint8_t(x.task.requested.x);
            u.goal_y                   = uint8_t(x.task.requested.y);
            u.move_group_id            = int32_t(x.task.group);
            u.path_blocked_retry_count = 0;
            u.order_queued             = 0;
            u.home_x                   = u.x;
            u.home_y                   = u.y;
            mh::call::llm_strat_unit_notify_status(a.player, int32_t(a.slot), 0);
        }
        for (uint32_t i = 0; i != AIR_CHUNK_STEPS; ++i) path[i] = {chunk.headings[i], 0};
        path[AIR_CHUNK_STEPS] = {255, 0};
        // attach_slot decrements free count; an actor-owned busy slot MUST NOT attach twice.
        if (actual == mh::caps::path::NO_PATH)
            mh::call::llm_strat_path_attach_slot(int32_t(a.player), int32_t(a.slot), int32_t(chosen));
        u.path_cursor = 0;
        chunk.slot    = chosen;
        if (!refill) mh::call::llm_strat_unit_notify_status(a.player, int32_t(a.slot), 100);
        // Actual heading, microstep and both facings are untouched.
        return apply_result::applied;
    }
    tile       goal   = x.return_to_anchor ? x.anchor : x.task.requested;
    int32_t    fine_x = 0, fine_y = 0;
    uint16_t   ref = 0, index = 0;
    uint8_t    weapon = 100;
    uint16_t   order = (!x.return_to_anchor && x.task.kind == action::patrol) ? 16 : c.move_op_code, state = c.move_op_arg;
    const bool attack = !x.return_to_anchor && (x.task.kind == action::attack_unit || x.task.kind == action::attack_building);
    if (attack) {
        if (read_target(nullptr, a, x.task.target) != target_status::alive_hostile) return apply_result::rejected;
        const auto t = x.task.target;
        uint32_t   tx, ty;
        if (t.kind == object_kind::unit) {
            const auto &victim = *unit(t.object.player, t.object.slot);
            tx                 = victim.x;
            ty                 = victim.y;
        } else {
            const auto &victim = *mh::caps::rec<const B>(mh::caps::BUILDINGS, t.object.player, t.object.slot);
            tx                 = victim.x;
            ty                 = victim.y;
        }
        // Tile-major counts, not the stale player-major generated field declaration.
        // Match the source carrier: do not resolve a hidden target's live fine position.
        const auto *fog = region<uint8_t>(R::RID_FOG_OF_WAR);
        if (!fog || !fog[(((tx << 8) | ty) * 8u) + a.player]) return apply_result::deferred;
        if (t.kind == object_kind::unit) mh::call::llm_strat_unit_get_coords(uint16_t(t.object.player), int32_t(t.object.slot), &fine_x, &fine_y);
        else mh::call::llm_strat_bldg_get_coords(uint16_t(t.object.player), int32_t(t.object.slot), &fine_x, &fine_y);
        if (t.kind == object_kind::building && g_arf_rt) {
            const auto &b    = *mh::caps::rec<const B>(mh::caps::BUILDINGS, t.object.player, t.object.slot);
            const auto *cb   = region<mh::game::mh_cfg_final_struct_Building>(R::RID_BUILDING);
            const auto *cw   = region<mh::game::mh_cfg_final_struct_Weapon>(R::RID_WEAPON);
            const auto *geom = region<mh::game::mh_llm_strat_map_geom>(R::RID_GENERAL);
            if (cb && geom) mh::sim::footprint_aim_fine_of(u, a.player, b, cb, cw, *geom, &fine_x, &fine_y);
        }
        if (fine_x < 0 || fine_y < 0) return apply_result::rejected;
        goal   = {uint32_t(fine_x / 32), uint32_t(fine_y / 32)};
        ref    = uint16_t(t.object.player | (t.kind == object_kind::unit ? 0x80 : 0x40));
        index  = uint16_t(t.object.slot);
        weapon = mh::call::llm_strat_unit_select_weapon(uint16_t(a.player), int32_t(a.slot), target_mask(t));
        if (weapon == 100) return apply_result::rejected;
        const uint16_t attack_order = t.kind == object_kind::unit ? 26 : 28;
        const int32_t  cls          = mh::call::llm_strat_target_class(ref, index);
        if (mh::call::llm_strat_unit_in_weapon_range(int32_t(a.player), int32_t(a.slot), int32_t(goal.x), int32_t(goal.y), cls) != 0 &&
            actor.family == executor::ground_run) {
            order = 1;
            state = attack_order;
            if (t.kind == object_kind::building) {
                if (g_arf_rt) mh::call::llm_strat_bldg_get_coords(uint16_t(t.object.player), int32_t(t.object.slot), &fine_x, &fine_y);
                mh::call::llm_strat_bldg_footprint_random_offset(a.player, a.slot, t.object.player, int32_t(t.object.slot),
                                                                 reinterpret_cast<uint32_t *>(&fine_x), reinterpret_cast<uint32_t *>(&fine_y));
                goal = {uint32_t(fine_x / 32), uint32_t(fine_y / 32)};
            }
        } else order = attack_order;
    }
    if (goal.x >= g.routes.shape().width || goal.y >= g.routes.shape().height) return apply_result::rejected;
    if (u.target_ref) primary_ref(a, 1);
    u.target_ref   = 0;
    u.target_index = 0;
    if (u.state == c.move_op_arg || u.state == c.move_op_code)
        mh::call::llm_strat_unit_notify_status(a.player, int32_t(a.slot), 0x67);
    // Exact F stores; do not call the generic setters (explicit cancellation hooks).
    u.order  = order;
    u.state  = state;
    u.goal_x = uint8_t(goal.x);
    u.goal_y = uint8_t(goal.y);
    if (attack) {
        u.target_fine_x   = fine_x;
        u.target_fine_y   = fine_y;
        u.target_ref      = int16_t(ref);
        u.target_index    = int16_t(index);
        u.selected_weapon = weapon;
    }
    u.move_group_id            = int32_t(x.task.group);
    u.path_blocked_retry_count = 0;
    u.order_queued             = 0;
    u.home_x                   = u.x;
    u.home_y                   = u.y;
    if (u.state == c.move_op_arg) mh::call::llm_strat_unit_notify_status(a.player, int32_t(a.slot), 0);
    if (u.target_ref) {
        primary_ref(a, 0);
        if (actor.family != executor::direction_plane) u.activity_clock = double(a.slot % 50) / 100.0 + u.activity_clock;
    }
    return apply_result::applied;
}
bool cleanup(void *, const ticket &t) {
    actor_view actor{};
    if (!read_actor(nullptr, t.actor, actor)) return false;
    if (!actor.safe_to_apply) {
        U &old = *unit(t.actor.player, t.actor.slot);
        prepare_unsafe_hover(old, *config(old.unit_proto_id));
        return false;
    }
    U       &u = *unit(t.actor.player, t.actor.slot);
    const C &c = *config(u.unit_proto_id);
    if (u.target_ref) primary_ref(t.actor, 1);
    u.target_ref   = 0;
    u.target_index = 0;
    u.state        = c.default_op_code;
    u.order        = c.default_op_code;
    return true;
}
bool cancel_pending(void *, actor_key a) { return !g.live || reservations::cancel_pending(g.routes, ACCOUNTING, a); }
void detach_primary(void *, actor_key a, target_key t) {
    if (g.live) reservations::detach_primary(g.routes, ACCOUNTING, a, t);
}
actor_key retirement_identity(object_kind kind, uint32_t p, uint32_t s) {
    const auto &policy = mh::storage_policy::hosted::owner();
    if (!policy.valid() || !g.routes.valid()) std::abort();
    const auto  d       = g.routes.shape();
    const auto  pd      = policy.shape();
    const bool  is_unit = kind == object_kind::unit;
    const auto &cap     = mh::caps::g(is_unit ? mh::caps::UNITS : mh::caps::BUILDINGS);
    if (!cap.base || p >= d.players || !s || s >= (is_unit ? d.units : d.buildings) ||
        p >= pd.players || s >= (is_unit ? pd.units : pd.buildings) || p >= cap.players || s >= cap.limit ||
        d.players != pd.players || d.units != pd.units || d.buildings != pd.buildings) std::abort();
    return identity(nullptr, kind, p, s);
}
const backend LIVE{&context, read_actor, capture_active, read_target, apply, cleanup, cancel_pending, detach_primary};
bool          same_world(world_key a, world_key b) {
    return a.system == b.system && a.planet == b.planet && a.incarnation == b.incarnation;
}
void begin_due_player_batch() {
    due_player_batch = false;
    due_player_world = {};
    for (auto &player : due_player_edits)
        for (auto &entry : player) entry.index_plus_one = 0;
    if (!g.live) return;
    const auto d = g.routes.shape();
    if (d.players > 8 || d.units > 500) std::abort(); // same supported setup bound as native adapter
    const auto    *records = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    const uint32_t count   = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    if (count && !records) std::abort();
    due_player_world = g.routes.world();
    // One scan at the deterministic pre-AI boundary, never a queue scan per shooter.
    // QUEUE is already the dispatcher's due-now lane. Do not inspect exec_time:
    // direct enqueue deliberately leaves it stale (order_queue.cpp:263-264), and
    // due_edit runs before the router without a timestamp test (sim_order_dispatch.cpp:306).
    // Timed PENDING records reach QUEUE only through release_due's !(exec_time > now),
    // which deliberately releases unordered/NaN timestamps too (order_queue.cpp:521-529).
    // Semantic preview is read-only and is performed only here, before unsafe precommits.
    for (uint32_t i = 0; i < count; ++i) {
        const O &record = records[i];
        if (record.order_code != COMMAND || record.param0 != COMMAND || (record.owner_and_kind & 0xf0u)) continue;
        edit candidate{};
        if (!decode_edit(record.owner_and_kind, record.unit_index, record.args, candidate) ||
            !same_world(candidate.world, due_player_world) || candidate.actor.player >= d.players ||
            !candidate.actor.slot || candidate.actor.slot >= d.units) continue;
        const actor_key live = identity(nullptr, object_kind::unit, candidate.actor.player, candidate.actor.slot);
        if (live.incarnation != candidate.actor.incarnation) continue;
        route_view existing{};
        if (candidate.operation == edit_kind::cancel ||
            (g.routes.read_route(candidate.actor, existing) && existing.count)) continue;
        auto &slot = due_player_edits[candidate.actor.player][candidate.actor.slot];
        if (slot.index_plus_one) continue; // earliest currently admissible semantic edit wins
        const native_context saved = context;
        context.capture_limit      = i;
        context.captured_index     = UINT32_MAX;
        const refusal decision     = g.routes.preview_edit(LIVE, candidate);
        context                    = saved;
        if (decision != refusal::none) continue;
        slot.index_plus_one = i + 1;
        slot.actor          = candidate.actor;
        slot.record         = record;
    }
    due_player_batch = true;
}
void end_due_player_batch() { due_player_batch = false; }
bool due_player_preempts(actor_key actor, uint32_t producer, target_key automatic_target) {
    if (!due_player_batch || !g.live || actor.player >= 8 || actor.slot >= 500 ||
        !actor.incarnation || !same_world(due_player_world, g.routes.world())) return false;
    const auto &slot = due_player_edits[actor.player][actor.slot];
    if (!slot.index_plus_one || slot.actor.incarnation != actor.incarnation ||
        identity(nullptr, object_kind::unit, actor.player, actor.slot).incarnation != actor.incarnation) return false;
    route_view existing{};
    if (g.routes.read_route(actor, existing) && existing.count) return false;
    const uint32_t index   = slot.index_plus_one - 1;
    const auto    *records = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (!records || index >= mh::caps::orders::live_count(mh::caps::orders::QUEUE) ||
        std::memcmp(records + index, &slot.record, sizeof(O)) != 0) return false;
    // We are still inside the bounded pre-AI/post-AI interval. No route admission,
    // unit movement or user dispatch has run since preview; the byte identity check
    // detects container overwrite/overflow. Never infer origin from this queued record.
    if (due_player_receipts < 192) {
        ++due_player_receipts;
        char line[512];
        std::snprintf(line, sizeof line,
                      "; [order_admission] event=automatic_yield_to_due_player producer=%u actor=%u/%u/%u "
                      "world=%u/%u/%u due_index=%u edit=%u action=%u seq=%u automatic_target=%u/%u/%u/%u "
                      "accounting_mutated=0 receipt=%u/192\n",
                      producer, actor.player, actor.slot, actor.incarnation, due_player_world.system,
                      due_player_world.planet, due_player_world.incarnation, index,
                      (uint32_t(slot.record.args[0]) >> 8) & 255u, (uint32_t(slot.record.args[0]) >> 16) & 255u,
                      uint32_t(slot.record.args[10]), uint32_t(automatic_target.kind), automatic_target.object.player,
                      automatic_target.object.slot, automatic_target.object.incarnation, due_player_receipts);
        seam_log(line);
        if (due_player_receipts == 192) seam_log("; [order_admission] event=automatic_yield_receipt_limit limit=192\n");
    }
    return true;
}

presentation::route_read_result read_presented_route(unsigned player, unsigned slot, uint32_t life,
                                                     presentation::route_snapshot &out) {
    out             = {};
    out.cycle_begin = presentation::no_cycle;
    if (!g.live || g.publication_pending) return presentation::route_read_result::invalid;
    const auto actor = identity(nullptr, object_kind::unit, player, slot);
    if (!life || actor.incarnation != life) return presentation::route_read_result::invalid;
    const auto result = presentation::read_task_route(g.routes, player, slot, life, out);
    if (result != presentation::route_read_result::owned) return result;
    // The owner certifies target identity; painting resolves visibility and anchors.
    // Presentation building counters are a different lifetime domain.
    for (unsigned n = 0; n < out.count; ++n) {
        auto &node = out.nodes[n];
        if (!node.target_current) continue;
        const auto kind     = (node.target_ref & 0xf0u) == 0x40u ? object_kind::building : object_kind::unit;
        const auto target   = identity(nullptr, kind, node.target_ref & 15u, node.target_slot);
        node.target_current = node.target_life != 0 && target.incarnation == node.target_life;
    }
    return presentation::route_read_result::owned;
}
} // namespace

state                      &owner() { return g.routes; }
campaign_state             &campaign_owner() { return g.campaign; }
const backend              &live_backend() { return LIVE; }
const reservations::access &live_accounting() { return ACCOUNTING; }
bool                        automatic_yields_to_due_player(actor_key actor, target_key target) {
    // This entry is called only by the two proven passive native CALL sites, never
    // by generic physical begin or an untagged queued attack application.
    return due_player_preempts(actor, 1, target);
}
void retained_view(uint32_t lane, uint32_t first, uint32_t end) {
    if (lane > 2 || first > end) std::abort();
    context.hole_first[lane] = first;
    context.hole_end[lane]   = end;
}
actor_key identity(void *, object_kind kind, uint32_t p, uint32_t s) {
    const auto id = kind == object_kind::building ? mh::storage_policy::hosted::building_identity(p, s) : kind == object_kind::unit ? mh::storage_policy::hosted::unit_identity(p, s)
                                                                                                                                    : mh::storage_policy::identity{};
    return {p, s, id.incarnation};
}
bool     live() { return g.live; }
bool     checkpoint_ready() { return !g.publication_pending && g.routes.valid() && g.routes.checkpoint_ready(); }
uint32_t begin_load(P::begin_load_reason reason) {
    // Snapshot bytes already replaced the queue, even if later quarantine
    // cannot reserve its carriers. Ordinary load/world changes keep it.
    if (reason == P::begin_load_reason::snapshot_input_ring_replaced)
        mh::seams::strategic_input_native::key_ring_replaced();
    if (!reserve_after_caps()) return UINT32_MAX;
    const uint32_t prior  = (g.live ? 1u : 0u) | (g.publication_pending ? 4u : 0u);
    const int32_t  source = MH_LibmhModule_IsBound() ? libmh_order_tasks_begin_load() : 0;
    if (source < 0 || source > 1) std::abort();
    g.live                = false;
    g.publication_pending = true;
    // Incoming snapshot bytes may already have been atomically copied. Quarantine
    // never initializes/clears either canonical span or adjusts native accounting.
    clear_local();
    return prior | (uint32_t(source) << 1);
}
void abort_load(uint32_t prior) {
    if (prior > 7) std::abort();
    // Caller must have made no mutation or restored ALL pre-publication bytes.
    if (MH_LibmhModule_IsBound()) libmh_order_tasks_abort_load(int32_t((prior >> 1) & 1));
    g.live                = (prior & 1) != 0;
    g.publication_pending = (prior & 4) != 0;
    clear_local();
}
bool reserve_after_caps() {
    const auto &u = mh::caps::g(mh::caps::UNITS), &b = mh::caps::g(mh::caps::BUILDINGS);
    if (!u.players || u.players != b.players || u.players > 8 || mh::caps::RAISED_ROW[mh::caps::UNITS] > 500 ||
        u.row > mh::caps::RAISED_ROW[mh::caps::UNITS] || b.row > mh::caps::RAISED_ROW[mh::caps::BUILDINGS]) return false;
    dimensions     d{u.players, mh::caps::RAISED_ROW[mh::caps::UNITS], 0, 0, mh::caps::RAISED_ROW[mh::caps::BUILDINGS]};
    const uint64_t bytes = uint64_t(HEADER_BYTES) + uint64_t(d.players) * d.units * ROW_BYTES;
    if (bytes > UINT32_MAX) return false;
    if (g.allocation) return g.capacity == bytes;
    std::unique_ptr<uint8_t[]> mem(new (std::nothrow) uint8_t[uint32_t(bytes)]);
    if (!mem || !g.routes.initialize(mem.get(), uint32_t(bytes), d, {})) return false;
    g.capacity   = uint32_t(bytes);
    g.allocation = std::move(mem);
    return true;
}
bool diagnostics_enabled() {
    static const bool enabled = [] {
        char ini[MAX_PATH];
        mh::cfgdir::ini_path(ini);
        return mh_ini_get_int("harness", "enable", 0, ini) != 0 &&
               mh_ini_get_int("harness", "patrol_watch_to", 0, ini) > 0;
    }();
    return enabled;
}
bool bind_regions() {
    if (!reserve_after_caps()) return false;
    R::bind(R::RID_ORDER_TASKS, uint32_t(reinterpret_cast<uintptr_t>(region_data_mut())), region_bytes());
    R::bind(R::RID_ORDER_CAMPAIGN, uint32_t(reinterpret_cast<uintptr_t>(campaign_data_mut())), campaign_bytes());
    mh::storage_policy::save::bind_order_context(save_context());
    if (!bind_published()) return false;
    presentation::set_route_reader(read_presented_route);
    return true;
}
bool setup(uint32_t w, uint32_t h, world_key key) {
    if (!reserve_after_caps()) return false;
    const auto old = g.routes.shape();
    if (g.routes.valid() && old.width == w && old.height == h && g.routes.world().system == key.system &&
        g.routes.world().planet == key.planet && g.routes.world().incarnation == key.incarnation) {
        if (!g.publication_pending) g.live = key.incarnation != 0 && w != 0 && h != 0;
        return true;
    }
    const dimensions d{old.players, old.units, w, h, old.buildings};
    if (!g.routes.initialize(g.allocation.get(), g.capacity, d, key)) return false;
    if (!g.publication_pending) g.live = key.incarnation != 0 && w != 0 && h != 0;
    clear_local();
    return true;
}
void prepare_fresh_continuation(uint32_t p, uint32_t s) {
    U       *u = unit(p, s);
    const C *c = u ? config(u->unit_proto_id) : nullptr;
    if (c) prepare_unsafe_hover(*u, *c);
}
bool bind_published() {
    g.live                = false;
    g.publication_pending = true;
    if (!g.routes.bind(g.allocation.get(), g.capacity) || !g.campaign.bind(g.campaign_bytes, sizeof g.campaign_bytes)) return false;
    const auto key = g.routes.world();
    if (g.routes.shape().width && g.campaign.world_inc(key.planet) != key.incarnation) return false;
    if (MH_LibmhModule_IsBound() && libmh_order_tasks_bind(g.allocation.get(), g.capacity,
                                                           g.campaign_bytes, sizeof g.campaign_bytes) != 1) return false;
    clear_local();
    if (MH_LibmhModule_IsBound()) libmh_order_tasks_published();
    g.publication_pending = false;
    g.live                = key.incarnation != 0 && g.routes.shape().width != 0 && g.routes.shape().height != 0;
    return true;
}
bool new_campaign() {
    if (begin_load(P::begin_load_reason::preserve_input_ring) == UINT32_MAX) return false;
    if (!reserve_after_caps() || !g.campaign.initialize(g.campaign_bytes, sizeof g.campaign_bytes)) return false;
    const auto d = g.routes.shape();
    if (!g.routes.initialize(g.allocation.get(), g.capacity, {d.players, d.units, 0, 0, d.buildings}, {})) return false;
    clear_local();
    return true;
}
uint32_t ensure_known_planet(uint32_t planet) {
    uint32_t incarnation = 0;
    return g.campaign.ensure_known(planet, incarnation) ? incarnation : 0;
}
bool fresh_world(uint32_t system, uint32_t planet, uint32_t w, uint32_t h) {
    uint32_t inc = 0;
    if (!g.campaign.fresh_world(planet, inc)) return false;
    g.publication_pending = false;
    return setup(w, h, {system, planet, inc}) && bind_published();
}
bool reset_world() {
    const auto d = g.routes.shape();
    if (!g.routes.initialize(g.allocation.get(), g.capacity, d, g.routes.world())) return false;
    clear_local();
    return true;
}
const uint8_t *region_data() { return g.routes.data(); }
uint8_t       *region_data_mut() { return g.routes.data(); }
uint32_t       region_bytes() { return g.routes.size(); }
const uint8_t *campaign_data() { return g.campaign.data(); }
uint8_t       *campaign_data_mut() { return g.campaign.data(); }
uint32_t       campaign_bytes() { return campaign_state::BYTES; }
int32_t        invoke(const mh_order_tasks_request *q, mh_order_tasks_result *r) {
    if (!r) return 0;
    *r = {};
    if (!q || !g.routes.valid()) return 0;
    if (q->operation == uint32_t(P::operation::published)) {
        r->values[0] = bind_published() ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(P::operation::checkpoint_ready)) {
        r->values[0] = checkpoint_ready() ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(P::operation::begin_load)) {
        r->values[0] = begin_load(static_cast<P::begin_load_reason>(q->args[0]));
        return 1;
    }
    if (q->operation == uint32_t(P::operation::abort_load)) {
        abort_load(q->args[0]);
        r->values[0] = 1;
        return 1;
    }
    if (q->operation == uint32_t(P::operation::new_campaign)) {
        r->values[0] = new_campaign() ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(P::operation::fresh_world)) {
        r->values[0] = fresh_world(q->args[0], q->args[1], q->args[2], q->args[3]) ? 1u : 0u;
        return 1;
    }
    if (q->operation == uint32_t(P::operation::ensure_known)) {
        r->values[0] = ensure_known_planet(q->args[0]);
        return r->values[0] ? 1 : 0;
    }
    if (q->operation == uint32_t(P::operation::begin_due_player_batch)) {
        begin_due_player_batch();
        r->values[0] = 1;
        return 1;
    }
    if (q->operation == uint32_t(P::operation::end_due_player_batch)) {
        end_due_player_batch();
        r->values[0] = 1;
        return 1;
    }
    if (!g.live) {
        const auto op = P::operation(q->operation);
        if (op == P::operation::primary_access) r->values[0] = 1;
        else if (op == P::operation::primary_admission)
            r->values[0] = !P::is_primary_marker(q->args[11]);
        else if (op == P::operation::due_edit) {
            r->values[0] = 1;
            r->values[1] = uint32_t(refusal::stale_world);
        } else if (op == P::operation::reservation) {
            const auto sub = P::reservation_operation(q->args[0]);
            // Before callbacks report unmanaged; actual native/source arithmetic still runs.
            if (sub != P::reservation_operation::release_before && sub != P::reservation_operation::rebound_before &&
                sub != P::reservation_operation::begin_issue && sub != P::reservation_operation::generic_begin)
                r->values[0] = 1;
        }
        return 1;
    }
    if (q->operation == uint32_t(P::operation::reservation)) {
        if (q->args[0] == uint32_t(P::reservation_operation::generic_begin)) {
            const auto actor = identity(nullptr, object_kind::unit, q->player, q->slot);
            const auto kind  = object_kind(q->args[1]);
            target_key target{kind, {q->args[2], q->args[3], q->args[4]}};
            if (!target.object.incarnation && (kind == object_kind::unit || kind == object_kind::building))
                target.object = identity(nullptr, kind, target.object.player, target.object.slot);
            if (native::source_issue_is_active(actor, target)) {
                // The native outer scope keeps the original sink/suppression receipts.
                // No second epoch, cancellation, source TLS scope or borrowed finish.
                r->values[0] = uint32_t(reservations::issue_decision::unmanaged);
                return 1;
            }
        }
        if (q->args[0] == uint32_t(P::reservation_operation::begin_issue)) {
            // Source issue_guard reached this only under its proven primary_scope.
            // Denial occurs before original OR8001/estimate/cache/tally/enqueue. A
            // generic public commit, explicit user attack and secondary fire do not enter.
            const actor_key actor = identity(nullptr, object_kind::unit, q->player, q->slot);
            const auto      kind  = object_kind(q->args[1]);
            target_key      target{kind, {q->args[2], q->args[3], q->args[4]}};
            if (!target.object.incarnation && (kind == object_kind::unit || kind == object_kind::building))
                target.object = identity(nullptr, kind, target.object.player, target.object.slot);
            if (due_player_preempts(actor, 2, target)) {
                r->values[0]  = uint32_t(reservations::issue_decision::denied);
                r->values[18] = 1; // source selector follows native success continuation, with NO attack
                return 1;
            }
        }
        if (q->args[0] == uint32_t(P::reservation_operation::finish_issue)) {
            const auto scope = P::get_issue_scope(q->args + 1, q->args + 18);
            if (!native::finish_borrowed(scope, q->args[15] != 0, q->args[16] != 0, q->args[17] != 0)) return 0;
            r->values[0] = 1;
            return 1;
        }
        if (q->args[0] == uint32_t(P::reservation_operation::sink_observed)) {
            native::source_sink_observed(q->player, q->slot, q->args[1] != 0);
            r->values[0] = 1;
            return 1;
        }
    }
    const bool due       = q->operation == uint32_t(P::operation::due_edit);
    const bool ordinary  = q->operation == uint32_t(P::operation::ordinary_applied);
    const bool hole      = q->operation == uint32_t(P::operation::discarded) && q->args[18] != 0;
    uint32_t   old_limit = 0, old_index = 0, old_first = 0, old_end = 0, hole_lane = 0;
    O          old_record;
    if (due || ordinary) old_limit = context.capture_limit;
    if (due) {
        old_index  = context.captured_index;
        old_record = context.captured;
    }
    const bool      old_diagnostic_active = context.diagnostic_active;
    const char     *old_diagnostic_stage  = context.diagnostic_stage;
    const actor_key old_diagnostic_actor  = context.diagnostic_actor;
    const uint32_t  old_diagnostic_index  = context.diagnostic_index;
    active_capture  old_diagnostic_capture;
    if (due) {
        old_diagnostic_capture     = context.diagnostic_capture;
        context.diagnostic_active  = true;
        context.diagnostic_stage   = "not_reached";
        context.diagnostic_actor   = {};
        context.diagnostic_index   = UINT32_MAX;
        context.diagnostic_capture = {};
    }
    if (due) {
        context.capture_limit  = q->args[17] ? q->args[17] - 1 : UINT32_MAX;
        context.captured_index = UINT32_MAX;
    } else if (q->operation == uint32_t(P::operation::ordinary_applied)) context.capture_limit = 0;
    if (hole) {
        hole_lane = q->args[18] - 1;
        if (hole_lane > 2 || q->args[19] > q->args[20] ||
            q->args[20] > mh::caps::orders::cap(mh::caps::orders::which(hole_lane))) return 0;
        old_first                     = context.hole_first[hole_lane];
        old_end                       = context.hole_end[hole_lane];
        context.hole_first[hole_lane] = q->args[19];
        context.hole_end[hole_lane]   = q->args[20];
    }
    route_view before_failure{};
    const bool observing       = q->operation == uint32_t(P::operation::observe);
    const auto observed_kind   = observing ? outcome_kind(q->args[9]) : outcome_kind::deferred;
    const bool success_trace   = observing && diagnostic_outcome(observed_kind);
    const bool observed_before = observing && (failure_outcome(observed_kind) || success_trace) &&
                                 read_route(q->player, q->slot, before_failure) &&
                                 before_failure.status != phase::blocked &&
                                 same_ticket(before_failure.active, P::get_ticket(q->args));
    const bool          observed_failure = observed_before && failure_outcome(observed_kind);
    P::dispatch_context ctx{&g.routes, LIVE, &context, identity, decode_applied, ACCOUNTING};
    const int32_t       result = P::dispatch(ctx, *q, *r);
    if (result && observed_before && success_trace)
        trace_observed(before_failure, observed_kind, {q->args[13], q->args[14]},
                              q->args[16], control(r->values[0]), q->args[21], q->args[22]);
    if (q->operation == uint32_t(P::operation::reservation) &&
        (q->args[0] == uint32_t(P::reservation_operation::begin_issue) ||
         q->args[0] == uint32_t(P::reservation_operation::generic_begin)) &&
        result && (r->values[0] == uint32_t(reservations::issue_decision::allowed) || r->values[0] == uint32_t(reservations::issue_decision::physical)))
        native::begin_borrowed(P::get_issue_scope(r->values + 1, r->values + 15),
                                      q->args[0] == uint32_t(P::reservation_operation::begin_issue) &&
                                          r->values[0] == uint32_t(reservations::issue_decision::allowed));
    if (due && result && r->values[1] == uint32_t(refusal::none) && context.captured_index != UINT32_MAX) {
        // Synchronous validated capture+edit cannot interleave a container mutation.
        // Stamp only the uniquely captured earlier retained record, never ordinary admission.
        auto      *records = reinterpret_cast<O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
        route_view route{};
        if (!read_route(q->player, q->slot, route) || !route.externally_pending ||
            context.captured_index >= mh::caps::orders::live_count(mh::caps::orders::QUEUE) ||
            std::memcmp(records + context.captured_index, &context.captured, sizeof(O)) != 0) std::abort();
        auto      &record       = records[context.captured_index];
        const auto presentation = mh::ui::selected_targets::detail::begin_order_metadata_edit(mh::caps::orders::QUEUE, context.captured_index);
        record.args[8]          = int32_t(P::EXPLICIT_PRIMARY);
        record.args[9]          = int32_t(route.active.actor.incarnation);
        record.args[10]         = int32_t(route.active.route);
        record.args[11]         = int32_t(route.active.leg);
        mh::ui::selected_targets::detail::finish_order_metadata_edit(presentation);
    }
    if (due && result && r->values[1]) {
        if (r->values[1] >= 32) std::abort();
        if (q->player == local_player()) ++refusals[r->values[1]];
        route_view route{};
        read_route(q->player, q->slot, route);
        trace_queue("refused_due_edit", r->values[1], q->player, q->slot, route, nullptr, 0, 0, q);
        trace_admission(*q, r->values[1]);
    }
    if (q->operation == uint32_t(P::operation::pre_unit_tick)) report_phase();
    else report_one(q->player, q->slot, observed_failure ? &before_failure : nullptr, q->args[21], q->args[22]);
    if (due) {
        context.captured_index     = old_index;
        context.captured           = old_record;
        context.diagnostic_active  = old_diagnostic_active;
        context.diagnostic_stage   = old_diagnostic_stage;
        context.diagnostic_actor   = old_diagnostic_actor;
        context.diagnostic_index   = old_diagnostic_index;
        context.diagnostic_capture = old_diagnostic_capture;
    }
    if (due || ordinary) context.capture_limit = old_limit;
    if (hole) {
        context.hole_first[hole_lane] = old_first;
        context.hole_end[hole_lane]   = old_end;
    }
    return result;
}
bool read_route(uint32_t p, uint32_t s, route_view &r) { return g.live && g.routes.read_route(identity(nullptr, object_kind::unit, p, s), r); }
bool read_entry(uint32_t p, uint32_t s, uint32_t i, node_view &v) { return g.live && g.routes.read_entry(identity(nullptr, object_kind::unit, p, s), i, v); }
bool decode_applied(void *, actor_key a, uint16_t code, int16_t param0, const int32_t (&args)[13], entry &e) {
    U       *u = unit(a.player, a.slot);
    const C *c = u ? config(u->unit_proto_id) : nullptr;
    executor f{};
    if (!u || !c || !family_of(*c, f)) return false;
    e                       = {};
    e.group                 = args[12] == -1 ? uint32_t(u->move_group_id) : uint32_t(args[12]);
    const uint16_t p        = uint16_t(param0);
    const uint32_t marker   = uint32_t(args[8]);
    const bool     producer = marker == P::AUTONOMOUS_PRIMARY || marker == P::PHYSICAL_PRIMARY;
    target_key     captured_target{};
    const bool     captured = marker == P::EXPLICIT_PRIMARY && uint32_t(args[9]) == a.incarnation &&
                          reservations::captured_record_target(g.routes, a, uint32_t(args[10]), uint32_t(args[11]),
                                                               uint32_t(args[5]), uint32_t(args[6]), captured_target);
    const bool alternate = producer || captured;
    if (code == 29 || code == 30 || p == 29 || p == 30 || ((p == 27 || code == 27) && !alternate)) return false;
    if ((p == c->move_op_code || p == 16) && code == c->move_op_arg) {
        if (args[0] < 0 || args[1] < 0 || uint32_t(args[0]) >= g.routes.shape().width || uint32_t(args[1]) >= g.routes.shape().height) return false;
        e.kind      = p == 16 && native_order16_is_patrol(a, f) ? action::patrol : action::move;
        e.requested = {uint32_t(args[0]), uint32_t(args[1])};
        return true;
    }
    const bool primary_order = p == 26 || p == 28 || (alternate && p == 27);
    const bool primary_state = code == 26 || code == 28 || (alternate && code == 27);
    if (!(primary_order && (code == c->move_op_arg || code == 46)) && !(p == 1 && primary_state)) return false;
    if (args[5] < 0 || args[5] > 255 || args[6] <= 0 || args[6] > 65535) return false;
    const uint32_t ref  = uint32_t(args[5]);
    const auto     kind = (ref & 0x40) ? object_kind::building : (ref & 0xa0) ? object_kind::unit
                                                                              : object_kind::none;
    if (kind == object_kind::none) return false;
    e.kind   = kind == object_kind::building ? action::attack_building : action::attack_unit;
    e.target = captured ? captured_target : target_key{kind, identity(nullptr, kind, ref & 15u, uint32_t(args[6]))};
    if (producer) e.target.object.incarnation = uint32_t(args[11]);
    return read_target(nullptr, a, e.target) == target_status::alive_hostile;
}
void applied(uint32_t p, uint32_t s, uint16_t owner_kind, uint16_t code, int16_t param0, const int32_t (&args)[13], tile anchor) {
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(uint32_t(args[8]) == P::AUTONOMOUS_PRIMARY ? P::operation::autonomous_applied : P::operation::ordinary_applied);
    q.player    = p;
    q.slot      = s;
    q.args[0]   = owner_kind;
    q.args[1]   = code;
    q.args[2]   = uint16_t(param0);
    std::memcpy(q.args + 3, args, sizeof args);
    if (q.operation == uint32_t(P::operation::autonomous_applied)) {
        q.args[16] = anchor.x;
        q.args[17] = anchor.y;
    }
    invoke(&q, &r);
}
bool consume_command(uint16_t owner_kind, uint16_t code, uint16_t param0, uint32_t slot, const int32_t (&args)[13], uint32_t kept) {
    if (code != COMMAND || param0 != COMMAND || (owner_kind & 0xf0)) return false;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(P::operation::due_edit);
    q.player    = owner_kind & 15;
    q.slot      = slot;
    q.args[0]   = owner_kind;
    q.args[1]   = code;
    q.args[2]   = param0;
    q.args[17]  = kept + 1;
    std::memcpy(q.args + 3, args, sizeof args);
    invoke(&q, &r);
    return true;
}
void tick() {
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(P::operation::pre_unit_tick);
    invoke(&q, &r);
}
void retire_unit(uint32_t p, uint32_t s, bool dead) {
    if (!g.live) return;
    const auto id = retirement_identity(object_kind::unit, p, s);
    if (!id.incarnation) {
        const auto &u = *mh::caps::rec<const U>(mh::caps::UNITS, p, s);
        // Death/removal can visit loaded zero-HP objects before corpse state is
        // published; final-free also visits old corpses. Neither owned a lifetime.
        const bool already_dead = u.unit_proto_id && u.energy <= 0.0;
        const bool freed        = !dead && !u.unit_proto_id && !u.unit_above[0] && !u.unit_above[1];
        if (already_dead || freed) return;
        std::abort();
    }
    reservations::retire_target(g.routes, ACCOUNTING, {object_kind::unit, id});
    reservations::retire_actor(g.routes, ACCOUNTING, id);
    if (!g.routes.retire_target(LIVE, {object_kind::unit, id}, dead)) std::abort();
    g.routes.retire_actor(id);
}
void retire_building(uint32_t p, uint32_t s, bool dead) {
    if (!g.live) return;
    const target_key target{object_kind::building, retirement_identity(object_kind::building, p, s)};
    if (!target.object.incarnation) {
        const auto &b = *mh::caps::rec<const B>(mh::caps::BUILDINGS, p, s);
        // A loaded zero-HP record can reach lethal retirement before state2 is
        // written; rubble cleanup reaches the same adapter after the actual clears.
        const bool already_dead = b.index > 0 && b.building_id && b.energy <= 0.0;
        const bool freed        = !dead && b.index == 0 && b.building_id == 0;
        if (already_dead || freed) return;
        std::abort();
    }
    reservations::retire_target(g.routes, ACCOUNTING, target);
    if (!g.routes.retire_target(LIVE, target, dead)) std::abort();
}

void cancel(uint32_t p, uint32_t s) {
    if (g.live) g.routes.cancel_actor(LIVE, identity(nullptr, object_kind::unit, p, s));
}
bool snapshot(uint32_t p, uint32_t s, ticket &t, executor *f) {
    route_view r{};
    if (!read_route(p, s, r) || !r.count) return false;
    t = r.active;
    if (f) *f = r.family;
    return true;
}
control outcome_for(const ticket &t, outcome_kind kind, tile assigned, assignment_reason why, uint32_t path, uint32_t producer_line) {
    actor_view a{};
    if (!read_actor(nullptr, t.actor, a)) return control::unchanged;
    if (kind == outcome_kind::reached_exact || kind == outcome_kind::reached_near) {
        route_view route{};
        if (!g.routes.read_route(t.actor, route) || !route.assignment_valid) return control::unchanged;
        path = route.path_slot;
        if (why == assignment_reason::none) {
            assigned = route.assigned;
            why      = route.placement;
        }
    }
    route_view before{};
    const bool success_trace = diagnostic_outcome(kind);
    const bool observed      = (failure_outcome(kind) || success_trace) && g.routes.read_route(t.actor, before) &&
                          before.status != phase::blocked && same_ticket(before.active, t);
    const bool failed = observed && failure_outcome(kind);
    const auto result = g.routes.observe(LIVE, {t, kind, a.family, a.position, assigned, why, path});
    if (observed && success_trace) trace_observed(before, kind, assigned, path, result, producer_line, 1);
    report_one(t.actor.player, t.actor.slot, failed ? &before : nullptr, producer_line, 1);
    return result;
}
bool should_hold(uint32_t p, uint32_t s) {
    return g.live && g.routes.handler_control(LIVE, identity(nullptr, object_kind::unit, p, s)) == control::hold;
}
bool suppress_home(uint32_t p, uint32_t s) {
    route_view r{};
    node_view  n{};
    return read_route(p, s, r) && r.count && read_entry(p, s, r.cursor, n) &&
           (r.has_excursion || n.task.kind == action::attack_unit || n.task.kind == action::attack_building);
}
bool path_endpoint(uint32_t p, uint32_t s, executor f, tile &end) {
    const U *u = unit(p, s);
    if (!u || u->path_cursor < 0 || u->path_cursor >= 300) return false;
    const auto d = g.routes.shape();
    if (!d.width || !d.height) return false;
    const uint32_t slot = mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u));
    if (slot >= mh::caps::path::slot_limit() || !mh::caps::path::flags()[p * mh::caps::path::flags_row() + slot]) return false;
    end               = {u->x, u->y};
    const auto *steps = region<mh::game::mh_llm_vec2i>(R::RID_STRAT_DIR_STEP_OFFSET_TABLE);
    for (uint32_t i = uint32_t(u->path_cursor); i < 300; ++i) {
        const auto *w  = mh::caps::path::waypoint(p, slot, i);
        int32_t     dx = 0, dy = 0;
        if (f == executor::ground_run) {
            if (w[0] == 0) return true;
            if (w[0] > 24) return false;
            mh::call::llm_strat_facing24_to_delta(w[0], &dx, &dy);
            dx *= w[1];
            dy *= w[1];
        } else {
            if (w[0] == 255) return true;
            if (w[0] >= 24 || !steps) return false;
            dx = steps[w[0]].dx;
            dy = steps[w[0]].dy;
        }
        end.x = uint32_t((int32_t(end.x) + dx) % int32_t(d.width) + int32_t(d.width)) % d.width;
        end.y = uint32_t((int32_t(end.y) + dy) % int32_t(d.height) + int32_t(d.height)) % d.height;
    }
    return false;
}
} // namespace mh::order_tasks::hosted
