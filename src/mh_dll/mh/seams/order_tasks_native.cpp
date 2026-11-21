// Destination: src/mh_dll/mh/seams/order_tasks_native.cpp
#include "seams/order_tasks_native.h"
#include "seams/order_tasks_hosted.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "addr/mh_calls.gen.h"
#include "include/mh_order_tasks_path.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "caps/path_pool.h"
#include "en_guard.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "../../libmh/orders/order_index_codec.h"
extern "C" int MH_OrderWideIndex_Enabled();
#include "ui/target_line_order_events.h"
#include "../../libmh/orders/order_queue.h"
extern "C" int     MH_LibmhModule_IsBound(void);
extern "C" int32_t libmh_order_tasks_enqueue_suppressed(void);
#include <cstring>
#include <cstdlib>

namespace mh::order_tasks::hosted {
bool automatic_yields_to_due_player(actor_key, target_key);
}
namespace mh::order_tasks::native {
namespace {
namespace H = hosted;
namespace P = host_protocol;
namespace R = mh::state;
using U     = mh::game::mh_map_object_unit;
using O     = mh::game::mh_llm_strat_order;
// Noncanonical invocation scratch only. Every use validates the full canonical ticket.
// The supported maximum is measured by caps/caps.h; refuse larger setup, never truncate actors.
struct plan {
    ticket key{};
    bool   valid = false, prior_assignment = false, near = false, hold = false;
};
plan plans[8][500]{};
bool handler_latch = false;
struct wave {
    uint32_t player = 0, count = 0;
    ticket   members[100]{};
    bool     valid[100]{}, failed = false, completed = false;
} current_wave;
struct origin {
    uint32_t                  player = 0, slot = 0, counts[3]{};
    reservations::issue_scope scope{};
    world_key                 world{};
    bool                      managed = false, borrowed = false, source_accepted = false, source_sink = false, source_suppressed = false, immediate = true;
};
origin origins[4]{}, generic_origins[8]{};
struct ref_scope {
    actor_key actor{};
    uint32_t  mode = 0;
};
ref_scope      references[8]{};
uint32_t       origin_depth = 0, generic_depth = 0, reference_depth = 0;
uint32_t       writer_player = 0, writer_slot = 0;
bool           writer_zero = false, writer_plane = false;
const uint8_t *discarded_deferred = nullptr;
O              deferred_record{};
struct discard {
    const uint8_t *frame = nullptr;
    O              record{};
} dispatch_discards[4]{};
struct application_anchor {
    const uint8_t *frame = nullptr;
    tile           anchor{};
} anchors[4]{};
tile deferred_anchor{};
struct transfer_state {
    const uint8_t *frame = nullptr;
    uint32_t       lane = 0, index = 0;
    bool           kept = false;
};
transfer_state transfers[4]{};
struct overwritten {
    const uint8_t *frame = nullptr;
    O              record{};
};
overwritten overwrites[4]{};
uint32_t    native_row_bytes = 0;
uint8_t     F_EXPECT[7]      = {0x69, 0x55, 0xcc, 0x04, 0x5b, 0x00, 0x00};
uint32_t    word(const uint8_t *p, int32_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, 4);
    return v;
}
struct machine {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, flags, return_address, operation;
};
static_assert(offsetof(machine, return_address) == 36);
U *unit(uint32_t p, uint32_t s) {
    const auto &d = mh::caps::g(mh::caps::UNITS);
    return d.base && p < d.players && s && s < d.limit ? mh::caps::rec<U>(mh::caps::UNITS, p, s) : nullptr;
}
uint32_t player() {
    const auto *p = R::ptr<const uint16_t>(R::RID_STRAT_CUR_PLAYER);
    return p ? *p : UINT32_MAX;
}
uint32_t slot() {
    const auto *p = R::ptr<const uint16_t>(R::RID_STRAT_CUR_INDEX);
    return p ? *p : UINT32_MAX;
}
bool equal(const ticket &a, const ticket &b) {
    return a.world.system == b.world.system && a.world.planet == b.world.planet && a.world.incarnation == b.world.incarnation &&
           a.actor.player == b.actor.player && a.actor.slot == b.actor.slot && a.actor.incarnation == b.actor.incarnation &&
           a.route == b.route && a.leg == b.leg && a.attempt == b.attempt;
}
plan *saved(uint32_t p, uint32_t s) { return p < 8 && s < 500 ? &plans[p][s] : nullptr; }
void  begin(uint32_t p, uint32_t s, bool planning) {
    plan *v = saved(p, s);
    if (!v) return;
    *v = {};
    route_view route{};
    if (!H::read_route(p, s, route) || !route.count) return;
    v->prior_assignment = route.assignment_valid;
    if (planning) H::outcome_for(route.active, outcome_kind::planning_started);
    v->valid = H::snapshot(p, s, v->key);
}
plan *matching(uint32_t p, uint32_t s) {
    plan  *v = saved(p, s);
    ticket now{};
    return v && v->valid && H::snapshot(p, s, now) && equal(v->key, now) ? v : nullptr;
}
control emit(uint32_t p, uint32_t s, outcome_kind kind, tile assigned = {}, assignment_reason why = assignment_reason::none, uint32_t path = UINT32_MAX, uint32_t producer_line = 0) {
    auto *v = matching(p, s);
    if (!v) return control::unchanged;
    const control result = H::outcome_for(v->key, kind, assigned, why, path, producer_line);
    if (result == control::hold) {
        v->hold = true;
        if (p == player() && s == slot()) handler_latch = true;
    }
    return result;
}
void zero_budget() {
    auto *p = R::ptr<uint32_t>(R::RID_STRAT_TICK_BUDGET);
    if (p) p[0] = p[1] = 0;
}
bool primary_access(uint32_t p, uint32_t s, bool *safe = nullptr) {
    if (!H::live()) {
        if (safe) *safe = false;
        return true;
    }
    if (!H::identity(nullptr, object_kind::unit, p, s).incarnation) {
        if (safe) *safe = false;
        return true;
    }
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(P::operation::primary_access);
    q.player    = p;
    q.slot      = s;
    if (!H::invoke(&q, &r)) std::abort();
    if (safe) *safe = r.values[2] != 0;
    if (r.values[1] == uint32_t(control::hold) && p == player() && s == slot()) handler_latch = true;
    return r.values[0] != 0;
}
bool secondary(const O &q) { return q.order_code == 29 || q.order_code == 30 || q.param0 == 29 || q.param0 == 30; }
bool token_current(const O &q, uint32_t p, uint32_t s) {
    if (secondary(q)) return true;
    const uint32_t magic = uint32_t(q.args[8]);
    if (!P::is_primary_marker(magic)) return true;
    if (magic == P::PHYSICAL_PRIMARY) {
        const auto       actor = H::identity(nullptr, object_kind::unit, p, s);
        const uint32_t   ref   = uint16_t(q.args[5]);
        const auto       kind  = ref & 0x40 ? object_kind::building : (ref & 0xa0) ? object_kind::unit
                                                                                   : object_kind::none;
        const target_key target{kind, {ref & 15u, uint16_t(q.args[6]), uint32_t(q.args[11])}};
        return uint32_t(q.args[9]) == actor.incarnation &&
               reservations::physical_record_current(H::owner(), H::live_accounting(), actor, target, uint32_t(q.args[10]));
    }
    route_view r{};
    if (!H::read_route(p, s, r) || !r.count || uint32_t(q.args[9]) != r.active.actor.incarnation) return false;
    if (magic == P::EXPLICIT_PRIMARY) return uint32_t(q.args[10]) == r.active.route &&
                                             uint32_t(q.args[11]) == r.active.leg && r.externally_pending;
    entry active{};
    return uint32_t(q.args[10]) == r.execution_epoch &&
           H::decode_applied(nullptr, r.active.actor, q.order_code, q.param0, q.args, active) &&
           H::owner().validate_excursion_application(H::live_backend(), r.active.actor, r.execution_epoch, active.target) == refusal::none;
}
void reconcile_flag(uint32_t p, uint32_t s, uint32_t excluded, uint32_t kept = UINT32_MAX) {
    U *u = unit(p, s);
    if (!u || !u->order_queued) return;
    const auto    *q       = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    const uint32_t n       = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    bool           pending = false;
    for (uint32_t i = 0; i < n; ++i)
        if (i != excluded && !(kept != UINT32_MAX && i >= kept && i <= excluded) &&
            q[i].unit_index == s && (q[i].owner_and_kind & 15) == p &&
            (q[i].owner_and_kind & 0xf0) == 0x80 && !secondary(q[i]) && token_current(q[i], p, s)) {
            pending = true;
            break;
        }
    u->order_queued = uint8_t(pending);
}
bool guard(const O &record, uint32_t index, tile *anchor = nullptr, uint32_t kept = UINT32_MAX) {
    if ((record.owner_and_kind & 0xf0) != 0x80 || secondary(record)) return true;
    if (!H::live()) return !P::is_primary_marker(uint32_t(record.args[8]));
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(P::operation::primary_admission);
    q.player    = record.owner_and_kind & 15;
    q.slot      = record.unit_index;
    q.args[0]   = record.owner_and_kind;
    q.args[1]   = record.order_code;
    q.args[2]   = uint16_t(record.param0);
    std::memcpy(q.args + 3, record.args, sizeof record.args);
    bool allow = H::invoke(&q, &r) != 0 && r.values[0] != 0;
    if (allow && anchor) {
        *anchor             = {r.values[2], r.values[3]};
        const auto     a    = H::identity(nullptr, object_kind::unit, q.player, q.slot);
        const uint32_t ref  = uint16_t(record.args[5]);
        const auto     kind = ref & 0x40 ? object_kind::building : (ref & 0xa0) ? object_kind::unit
                                                                                : object_kind::none;
        target_key     target{};
        if (kind != object_kind::none && record.args[6] > 0) target = {kind, H::identity(nullptr, kind, ref & 15, uint16_t(record.args[6]))};
        if (uint32_t(record.args[8]) == P::AUTONOMOUS_PRIMARY || uint32_t(record.args[8]) == P::PHYSICAL_PRIMARY) {
            target.object.incarnation = uint32_t(record.args[11]);
            allow                     = reservations::handoff(H::owner(), H::live_accounting(), a, target, uint32_t(record.args[10]));
        } else if (uint32_t(record.args[8]) == P::EXPLICIT_PRIMARY) {
            allow = reservations::captured_application(H::owner(), H::live_accounting(), a);
        } else {
            allow = reservations::physical_application(H::owner(), H::live_accounting(), a, target);
        }
    }
    if (!allow) reconcile_flag(q.player, q.slot, index, kept);
    return allow;
}
bool discarded(const O &record) {
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(P::operation::discarded);
    q.player    = record.owner_and_kind & 15;
    q.slot      = record.unit_index;
    q.args[0]   = record.owner_and_kind;
    q.args[1]   = record.order_code;
    q.args[2]   = uint16_t(record.param0);
    std::memcpy(q.args + 3, record.args, sizeof record.args);
    H::invoke(&q, &r);
    if (r.values[0] == uint32_t(control::hold)) {
        if (auto *v = matching(q.player, q.slot)) v->hold = true;
        if (q.player == player() && q.slot == slot()) handler_latch = true;
    }
    return r.values[1] != 0;
}
void begin_wave(uint32_t p, uint32_t count) {
    current_wave        = {};
    current_wave.player = p;
    if (count > 100) std::abort();
    current_wave.count  = count;
    const auto *members = R::ptr<const mh::game::mh_llm_strat_group_scratch_member>(R::RID_STRAT_GROUP_MOVE_SCRATCH);
    if (!members && count) std::abort();
    for (uint32_t i = 0; i < count; ++i) {
        if (members[i].unit_idx <= 0) continue;
        const uint32_t s = uint32_t(members[i].unit_idx);
        begin(p, s, true);
        if (auto *v = matching(p, s)) {
            current_wave.members[i] = v->key;
            current_wave.valid[i]   = true;
        }
    }
}
void fail_wave(outcome_kind kind) {
    for (uint32_t i = 0; i < current_wave.count; ++i)
        if (current_wave.valid[i]) {
            const ticket &t = current_wave.members[i];
            route_view    r{};
            if (H::read_route(t.actor.player, t.actor.slot, r) && equal(t, r.active) && !r.assignment_valid)
                H::outcome_for(t, kind, {}, assignment_reason::none, UINT32_MAX, __LINE__);
        }
}
void settled(uint32_t p, uint32_t s, bool direction) {
    auto      *v = matching(p, s);
    route_view r{};
    node_view  n{};
    const U   *u = unit(p, s);
    if (!v || !u || u->move_microstep < 31 || !H::read_route(p, s, r) || !H::read_entry(p, s, r.cursor, n)) return;
    if (n.task.kind != action::move && n.task.kind != action::patrol && r.status != phase::returning_anchor) return;
    if (!r.assignment_valid) {
        if (direction) emit(p, s, outcome_kind::planning_failed, {}, assignment_reason::none, UINT32_MAX, __LINE__);
        return;
    }
    if (r.assigned.x == u->x && r.assigned.y == u->y) emit(p, s, outcome_kind::reached_exact);
    else if (direction) emit(p, s, outcome_kind::planning_failed, {}, assignment_reason::none, UINT32_MAX, __LINE__);
}
// Called only after a measured native direction-state handoff, never on assignment alone.
void direction_ready(uint32_t p, uint32_t s, executor family, uint16_t state, bool require_assignment = true) {
    const U   *u = unit(p, s);
    route_view r{};
    if (!matching(p, s) || !u || u->state != state ||
        mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u)) >= mh::caps::path::slot_limit() || !H::read_route(p, s, r) || r.family != family ||
        r.status != phase::preparation || (require_assignment && (!r.assignment_valid || r.path_slot != mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u))))) return;
    emit(p, s, outcome_kind::preparation_finished);
}
void assigned(uint32_t p, uint32_t s, bool direction, bool plane_writer) {
    const U   *u = unit(p, s);
    auto      *v = matching(p, s);
    route_view r{};
    if (!u || !v || !H::read_route(p, s, r)) return;
    if (!direction && current_wave.failed) {
        emit(p, s, outcome_kind::planning_failed, {}, assignment_reason::none, UINT32_MAX, __LINE__);
        return;
    }
    const uint32_t path = mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u));
    if (path >= mh::caps::path::slot_limit()) {
        emit(p, s, outcome_kind::pool_exhausted, {}, assignment_reason::none, UINT32_MAX, __LINE__);
        return;
    }
    tile goal{u->goal_x, u->goal_y};
    bool projected = false;
    if (direction) {
        tile endpoint{};
        if (!H::path_endpoint(p, s, r.family, endpoint)) {
            emit(p, s, outcome_kind::planning_failed, {}, assignment_reason::none, UINT32_MAX, __LINE__);
            return;
        }
        if (endpoint.x != u->goal_x || endpoint.y != u->goal_y) {
            const bool writer_matches = plane_writer
                                            ? r.family == executor::direction_plane && u->state == 11
                                            : r.family == executor::direction_nonplane && (u->state == 18 || u->state == 55);
            if (!writer_matches || u->path_cursor != 0 ||
                !complete_direction_trace(static_cast<const uint8_t *>(mh::call::llm_strat_pathtrace_dirs_get()),
                                          mh::caps::path::waypoint(p, path, 0), 300)) {
                emit(p, s, outcome_kind::planning_failed, {}, assignment_reason::none, UINT32_MAX, __LINE__);
                return;
            }
            goal      = endpoint;
            projected = true;
        }
    }
    node_view node{};
    H::read_entry(p, s, r.cursor, node);
    const auto placement = projected ? assignment_reason::valid_projection : goal.x == node.task.requested.x && goal.y == node.task.requested.y ? assignment_reason::requested
                                                                         : node.task.group                                                      ? assignment_reason::formation
                                                                                                                                                : assignment_reason::valid_projection;
    if (!direction && r.family == executor::ground_run && (u->state == 15 || u->state == 12))
        emit(p, s, outcome_kind::preparation_finished);
    emit(p, s, outcome_kind::assigned, goal, placement, path);
    if (direction) direction_ready(p, s, executor::direction_nonplane, 18);
}
O normalized(O q, bool staged) {
    if (staged) {
        q.unit_index = MH_OrderWideIndex_Enabled() ? mh::orders::decode_index(q.unit_index) : uint16_t(q.unit_index & 255);
        q.owner_and_kind &= 255;
        q.order_code &= 255;
        q.param0 = int16_t(uint16_t(q.param0) & 255);
    }
    return q;
}
bool sink_suppressed() {
    if (mh::hook::promoted_owner_of(0x00466094)) {
        if (!MH_LibmhModule_IsBound()) std::abort();
        return libmh_order_tasks_enqueue_suppressed() != 0; // required cached module export
    }
    const auto *p = reinterpret_cast<const uint8_t *>(0x00466094);
    return p[0] == 0x31 && p[1] == 0xc0 && p[2] == 0xc3;
}
void scope_snapshot(origin &v, const machine &m) {
    v        = {};
    v.player = m.eax & 15;
    v.slot   = m.edx;
    for (uint32_t i = 0; i < 3; ++i) v.counts[i] = mh::caps::orders::live_count(mh::caps::orders::which(i));
    v.world             = H::owner().world();
    const auto *mode    = R::ptr<const int32_t>(R::RID_GAME_SESSION_MODE);
    const auto *players = R::ptr<const uint8_t>(R::RID_PLAYERS);
    if (!mode || !players) std::abort();
    v.immediate = *mode != mh::orders::SESSION_MP_LOCKSTEP ||
                  !(players[v.player * mh::orders::PLAYERS_STRIDE + mh::orders::PLAYERS_FLAG_OFF] & mh::orders::NET_CONTROLLED);
}
bool scope_begin(origin &v, const machine &m, bool physical) {
    if (!H::live()) {
        v = {};
        return true;
    }
    scope_snapshot(v, m);
    const auto       a    = H::identity(nullptr, object_kind::unit, v.player, v.slot);
    const uint32_t   ref  = m.ebx;
    const auto       kind = ref & 0x40 ? object_kind::building : object_kind::unit;
    const target_key t{kind, H::identity(nullptr, kind, ref & 15, m.ecx)};
    if (!physical && H::automatic_yields_to_due_player(a, t)) return false;
    const auto decision = physical
                              ? reservations::begin_physical_issue(H::owner(), H::live_accounting(), a, t, v.scope)
                              : reservations::begin_issue(H::owner(), H::live_backend(), H::live_accounting(), a, t, v.scope);
    v.managed           = !physical && decision == reservations::issue_decision::allowed;
    return decision != reservations::issue_decision::denied;
}
void scope_finish(const origin &v) {
    if (!v.scope.active) return;
    const auto world = H::owner().world();
    if (world.system != v.world.system || world.planet != v.world.planet || world.incarnation != v.world.incarnation) return;
    O       *accepted      = nullptr;
    uint32_t accepted_lane = 0, accepted_index = 0;
    for (uint32_t lane = 0; lane < 3; ++lane) {
        const auto     w       = mh::caps::orders::which(lane);
        const uint32_t n       = mh::caps::orders::live_count(w);
        auto          *records = reinterpret_cast<O *>(mh::caps::orders::base(w));
        for (uint32_t i = v.counts[lane]; i < n; ++i) {
            const auto     q   = normalized(records[i], w == mh::caps::orders::STAGING);
            const uint32_t ref = uint16_t(q.args[5]);
            if (q.unit_index != v.slot || (q.owner_and_kind & 255) != (v.player | 0x80) || secondary(q) ||
                !((q.param0 >= 26 && q.param0 <= 28) || (q.order_code >= 26 && q.order_code <= 28)) ||
                (ref & 15) != v.scope.target.object.player || uint16_t(q.args[6]) != v.scope.target.object.slot ||
                bool(ref & 0x40) != (v.scope.target.kind == object_kind::building)) continue;
            if (accepted) std::abort();
            accepted       = records + i;
            accepted_lane  = lane;
            accepted_index = i;
        }
    }
    const bool suppressed = v.source_sink ? v.source_suppressed : (v.immediate && sink_suppressed());
    if (!reservations::finish_issue(H::owner(), H::live_accounting(), v.scope, accepted || v.source_accepted || suppressed)) std::abort();
    const uint32_t marker = v.managed ? P::AUTONOMOUS_PRIMARY : P::PHYSICAL_PRIMARY;
    if (accepted && (uint32_t(accepted->args[8]) != marker ||
                     uint32_t(accepted->args[9]) != v.scope.actor.incarnation ||
                     uint32_t(accepted->args[10]) != v.scope.epoch ||
                     uint32_t(accepted->args[11]) != v.scope.target.object.incarnation)) {
        const auto presentation = mh::ui::selected_targets::detail::begin_order_metadata_edit(accepted_lane, accepted_index);
        accepted->args[8]       = int32_t(marker);
        accepted->args[9]       = int32_t(v.scope.actor.incarnation);
        accepted->args[10]      = int32_t(v.scope.epoch);
        accepted->args[11]      = int32_t(v.scope.target.object.incarnation);
        mh::ui::selected_targets::detail::finish_order_metadata_edit(presentation);
    }
}
bool origin_begin(const machine &m) {
    if (origin_depth == 4) std::abort();
    auto &v = origins[origin_depth++];
    if (scope_begin(v, m, false)) return true;
    --origin_depth;
    return false;
}
void origin_end() {
    if (!origin_depth) std::abort();
    scope_finish(origins[--origin_depth]);
}
bool generic_begin(const machine &m) {
    if (generic_depth == 8) std::abort();
    auto &v = generic_origins[generic_depth++];
    if (origin_depth && origins[origin_depth - 1].scope.active) {
        scope_snapshot(v, m);
        return true;
    }
    if (scope_begin(v, m, true)) return true;
    --generic_depth;
    return false;
}
void generic_end() {
    if (!generic_depth) std::abort();
    scope_finish(generic_origins[--generic_depth]);
}
// All callback continuations have one operation dword still on the stack; discard it with LEA.
__declspec(naked) void skip_handler() { __asm lea esp, [esp + 4] __asm push 0047c8e3h __asm ret }
__declspec(naked) void skip_hover() { __asm lea esp, [esp + 4] __asm push 00480a4ah __asm ret }
__declspec(naked) void skip_deferred() { __asm lea esp, [esp + 4] __asm push 00469d9dh __asm ret }
__declspec(naked) void skip_source() { __asm push 004668c8h __asm ret }
__declspec(naked) void skip_commit() { __asm lea esp, [esp + 4] __asm push 004668c8h __asm ret }
__declspec(naked) void skip_home1() { __asm lea esp, [esp + 4] __asm push 0047cd2eh __asm ret }
__declspec(naked) void skip_home2() { __asm lea esp, [esp + 4] __asm push 00485130h __asm ret }
__declspec(naked) void skip_home3() { __asm lea esp, [esp + 4] __asm push 0048525eh __asm ret }
__declspec(naked) void skip_home4() { __asm lea esp, [esp + 4] __asm push 00482c72h __asm ret }
__declspec(naked) void skip_alt() { __asm lea esp, [esp + 4] __asm push 004ede61h __asm ret }
__declspec(naked) void skip_attack() { __asm lea esp, [esp + 4] __asm push 004ede7ah __asm ret }
__declspec(naked) void skip_entry() { __asm lea esp, [esp + 4] __asm ret }
__declspec(naked) void skip_walker() { __asm lea esp, [esp + 4] __asm push 0047d737h __asm ret }
__declspec(naked) void replan_walker() { __asm lea esp, [esp + 4] __asm push 0047cfa0h __asm ret }
__declspec(naked) void skip_chase() { __asm lea esp, [esp + 4] __asm push 00480c74h __asm ret }
__declspec(naked) void skip_path() { __asm lea esp, [esp + 4] __asm push 00481172h __asm ret }
void                   reselect_handler();
uintptr_t __cdecl observe(uint32_t op, machine *m) {
    const uint32_t p = player(), s = slot();
    switch (op) {
        case 1: { // BEFORE actual indirect handler CALL, after native guard/selection.
            handler_latch              = false;
            const U       *before      = unit(p, s);
            const uint16_t prior_state = before ? before->state : 0;
            const bool     may_access  = primary_access(p, s);
            if (!may_access) H::prepare_fresh_continuation(p, s);
            if (handler_latch || H::should_hold(p, s)) {
                zero_budget();
                return reinterpret_cast<uintptr_t>(&skip_handler);
            }
            const U *prepared = unit(p, s);
            if (!may_access && prepared && (prepared->state == 46 || prepared->state == 47)) {
                zero_budget();
                return reinterpret_cast<uintptr_t>(&skip_handler); // real rotation runs outside this budget
            }
            begin(p, s, false);
            route_view r{};
            U         *u = unit(p, s);
            if (u && H::read_route(p, s, r) && r.count &&
                (u->state == 21 || u->state == 45 || u->state == 48 || u->state == 49 || u->state == 124) && r.status != phase::preparation) {
                emit(p, s, outcome_kind::preparation_started);
                begin(p, s, false);
            }
            if (u && u->state != prior_state) return reinterpret_cast<uintptr_t>(&reselect_handler);
            break;
        }
        case 2: {
            const auto *v = matching(p, s);
            if (handler_latch || (v && v->hold) || H::should_hold(p, s)) zero_budget();
            break;
        }
        case 3: begin_wave(m->ebx, m->ecx); break; // marshal CALL41c86c, EBXowner ECXwave count
        case 4: begin_wave(m->eax, m->edx); break; // nonplane gathered scratch, EDXcount
        case 5: assigned(m->eax, m->edx, false, false); break;
        case 63:
        case 6: {
            writer_plane  = op == 63;
            writer_player = m->eax;
            writer_slot   = m->edx;
            writer_zero   = false;
            const U   *u  = unit(writer_player, writer_slot);
            route_view r{};
            node_view  n{};
            if (u && matching(writer_player, writer_slot) && !current_wave.failed &&
                H::read_route(writer_player, writer_slot, r) && H::read_entry(writer_player, writer_slot, r.cursor, n) &&
                (r.status == phase::returning_anchor || n.task.kind == action::move || n.task.kind == action::patrol))
                writer_zero = u->goal_x == u->x && u->goal_y == u->y;
            break;
        }
        case 7: {
            if (writer_zero) {
                const U       *u  = unit(writer_player, writer_slot);
                const uint32_t id = u ? mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u)) : UINT32_MAX;
                if (!u || id >= mh::caps::path::slot_limit() || u->goal_x != u->x || u->goal_y != u->y) std::abort();
                // Positive pre-writer zero assignment; remove the solver's fabricated first
                // heading, not a failed-plan fallback. Native attach/cursor/terminal run once.
                mh::caps::path::waypoint(writer_player, id, 0)[0] = 255;
            }
            assigned(writer_player, writer_slot, true, writer_plane);
            writer_zero = writer_plane = false;
            break;
        }
        case 8: {
            const auto *u = unit(p, s);
            if (u && u->order == 16 && (u->x != u->goal_x || u->y != u->goal_y)) {
                mh_order_tasks_request q{};
                mh_order_tasks_result  r{};
                q.operation = uint32_t(P::operation::ground_patrol_placement);
                q.player    = p;
                q.slot      = s;
                if (H::invoke(&q, &r) && r.values[0] && r.values[1] == uint32_t(u->move_group_id))
                    return reinterpret_cast<uintptr_t>(&replan_walker);
            }
            settled(p, s, false);
            break;
        }
        case 9: settled(p, s, true); break;
        case 10: current_wave.failed = true; break;
        case 11:
            if (m->eax) current_wave.failed = true;
            break;
        case 12:
            if (!m->eax) current_wave.failed = true;
            break;
        case 13: {
            const uint32_t owner = m->eax, member = m->edx;
            if (auto *v = matching(owner, member); v && !v->near) emit(owner, member, outcome_kind::give_up, {}, assignment_reason::none, UINT32_MAX, __LINE__);
            break;
        }
        case 14: emit(p, s, outcome_kind::pool_exhausted, {}, assignment_reason::none, UINT32_MAX, __LINE__); break;
        case 15: fail_wave(outcome_kind::pool_exhausted); break;
        case 16:
        case 37:
            if (!origin_begin(*m)) return reinterpret_cast<uintptr_t>(op == 16 ? &skip_alt : &skip_attack);
            break;
        case 17: origin_end(); break;
        case 18: H::retire_unit(p, s, true); break;
        case 19: H::retire_building(p, s, true); break;
        case 20: emit(p, s, outcome_kind::attack_cleanup_finished); break;
        case 21:
            emit(p, s, outcome_kind::preparation_finished);
            begin(p, s, false);
            break;
        case 22: {
            if (H::suppress_home(p, s)) {
                emit(p, s, outcome_kind::primary_excursion_ended);
                if (H::should_hold(p, s)) zero_budget();
                return reinterpret_cast<uintptr_t>(&skip_hover);
            }
            break;
        }
        case 23: {
            const uint32_t index = word(reinterpret_cast<const uint8_t *>(m->ebp), -0x1c);
            const auto    *q     = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
            if (index >= mh::caps::orders::live_count(mh::caps::orders::QUEUE)) std::abort();
            deferred_anchor = {};
            if (!guard(q[index], index, &deferred_anchor)) {
                discarded_deferred = reinterpret_cast<const uint8_t *>(m->ebp);
                deferred_record    = q[index];
                return reinterpret_cast<uintptr_t>(&skip_deferred);
            }
            break;
        }
        case 24: { // generic lifecycle notification; lethal already tagged, never erase satisfaction
            const uint32_t kind = m->eax & 0xf0, owner = m->eax & 15;
            if (kind == 0x40) H::retire_building(owner, m->edx, false);
            else if (kind == 0x80) H::retire_unit(owner, m->edx, false);
            break;
        }
        case 25: {
            if (discarded_deferred == reinterpret_cast<const uint8_t *>(m->ebp)) {
                discarded(deferred_record);
                discarded_deferred = nullptr;
            }
            break;
        }
        case 26: {
            const auto    *frame = reinterpret_cast<const uint8_t *>(m->ebp);
            const uint32_t index = word(frame, -0x24);
            const auto    *q     = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
            if (index >= mh::caps::orders::live_count(mh::caps::orders::QUEUE)) std::abort();
            tile anchor{};
            if (!guard(q[index], index, &anchor, word(frame, -0x20))) {
                bool saved = false;
                for (auto &d : dispatch_discards)
                    if (!d.frame) {
                        d.frame  = frame;
                        d.record = q[index];
                        saved    = true;
                        break;
                    }
                if (!saved) std::abort();
                // Same operation-dword cleanup as other event-controlled continuations.
                return reinterpret_cast<uintptr_t>(&skip_commit);
            }
            if (uint32_t(q[index].args[8]) == P::AUTONOMOUS_PRIMARY) {
                bool saved = false;
                for (auto &a : anchors)
                    if (!a.frame) {
                        a.frame  = frame;
                        a.anchor = anchor;
                        saved    = true;
                        break;
                    }
                if (!saved) std::abort();
            }
            break;
        }
        case 28: {
            if (current_wave.failed) {
                fail_wave(outcome_kind::planning_failed);
                break;
            }
            const uint32_t row = mh::caps::row_bytes(mh::caps::UNITS);
            if (p >= 8 || !row || m->eax < p * row || (m->eax - p * row) % sizeof(U)) std::abort();
            const uint32_t member = (m->eax - p * row) / sizeof(U);
            U             *u      = unit(p, member);
            if (u && u->goal_x == u->x && u->goal_y == u->y && accepted_near(p, member, true))
                u->path_blocked_retry_count = 4;
            break;
        }
        case 29:
            if (!primary_access(p, s)) return reinterpret_cast<uintptr_t>(&skip_walker);
            break;
        case 30: {
            bool safe = false;
            if (!primary_access(p, s, &safe)) {
                const U *u = unit(p, s);
                m->eax     = 0;
                if (u && u->move_microstep >= 31) {
                    if (handler_latch || !safe) zero_budget();
                    return reinterpret_cast<uintptr_t>(&skip_path);
                }
                return reinterpret_cast<uintptr_t>(&skip_chase);
            }
            break;
        }
        case 31: {
            const auto     a       = H::live() ? H::identity(nullptr, object_kind::unit, m->eax & 15, m->edx) : actor_key{};
            const uint32_t mode    = m->ebx & 15;
            const bool     handled = a.incarnation && (mode == 1 ? reservations::release_before(H::owner(), H::live_accounting(), a) : mode == 0 ? reservations::rebound_before(H::owner(), H::live_accounting(), a)
                                                                                                                                                 : false);
            if (handled) return reinterpret_cast<uintptr_t>(&skip_entry);
            if (reference_depth == 8) std::abort();
            references[reference_depth++] = {a, mode};
            break;
        }
        case 32: {
            if (!reference_depth) std::abort();
            const auto v = references[--reference_depth];
            if (v.actor.incarnation) {
                if (v.mode == 1) reservations::release_after(H::owner(), H::live_accounting(), v.actor);
                else if (v.mode == 0 && !reservations::rebound_after(H::owner(), H::live_accounting(), v.actor)) std::abort();
            }
            break;
        }
        case 33:
            if (!generic_begin(*m)) return reinterpret_cast<uintptr_t>(&skip_entry);
            break;
        case 34: generic_end(); break;
        case 40:
            current_wave = {};
            begin(p, s, true);
            break;
        case 42: {
            current_wave.completed = true;
            if (!current_wave.failed)
                for (uint32_t i = 0; i < current_wave.count; ++i)
                    if (current_wave.valid[i]) {
                        const auto &t = current_wave.members[i];
                        H::outcome_for(t, outcome_kind::planning_succeeded);
                    }
            break;
        }
        case 41: {
            const uint32_t member = word(reinterpret_cast<const uint8_t *>(m->ebp), -0x48);
            const auto    *free   = R::ptr<const int32_t>(R::RID_STRAT_PATH_FREE_SLOT_COUNT);
            emit(p, member, !current_wave.failed && free && p < 8 && free[p] == 0 ? outcome_kind::pool_exhausted : outcome_kind::planning_failed, {}, assignment_reason::none, UINT32_MAX, __LINE__);
            break;
        }
        case 35: {
            const auto *frame = reinterpret_cast<const uint8_t *>(m->ebp);
            if (word(frame, -0x34) == 1) {
                bool found = false;
                for (auto &v : overwrites)
                    if (v.frame == frame) {
                        found          = true;
                        const O record = v.record;
                        v              = {};
                        if (discarded(record)) reconcile_flag(record.owner_and_kind & 15, record.unit_index, UINT32_MAX);
                        break;
                    }
                if (!found) std::abort();
            }
            break;
        }
        case 38:
        case 39: {
            const auto    *frame = reinterpret_cast<const uint8_t *>(m->ebp);
            const uint32_t lane  = op == 38 ? uint32_t(mh::caps::orders::STAGING) : uint32_t(mh::caps::orders::PENDING);
            const uint32_t index = word(frame, -0x24);
            H::retained_view(lane, word(frame, -0x20), index + 1);
            bool kept = false;
            for (auto &v : transfers)
                if (v.frame == frame && v.lane == lane && v.index == index) {
                    kept = v.kept;
                    v    = {};
                    break;
                }
            if (!kept) {
                const auto w = mh::caps::orders::which(lane);
                if (index >= mh::caps::orders::cap(w)) std::abort();
                const O record = normalized(reinterpret_cast<const O *>(mh::caps::orders::base(w))[index], w == mh::caps::orders::STAGING);
                if ((record.owner_and_kind & 0xf0) == 0x80 && !secondary(record) && discarded(record))
                    reconcile_flag(record.owner_and_kind & 15, record.unit_index, UINT32_MAX);
            }
            break;
        }
        case 51:
        case 52:
        case 53:
        case 54: {
            if (H::suppress_home(m->eax, m->edx)) {
                emit(m->eax, m->edx, outcome_kind::primary_excursion_ended);
                const uintptr_t skip[] = {reinterpret_cast<uintptr_t>(&skip_home1), reinterpret_cast<uintptr_t>(&skip_home2),
                                          reinterpret_cast<uintptr_t>(&skip_home3), reinterpret_cast<uintptr_t>(&skip_home4)};
                return skip[op - 51];
            }
            break;
        }
        case 43:
            if (m->eax == 0 || m->eax == 2) {
                if (!H::new_campaign()) std::abort();
            } else if (H::begin_load(P::begin_load_reason::preserve_input_ring) == UINT32_MAX) std::abort();
            break;
        case 64:
            // All four measured status stores receive EAX = planet * sizeof(int32_t).
            if ((m->eax & 3u) || m->eax >= 32u * 4u || !H::ensure_known_planet(m->eax / 4u)) std::abort();
            break;
        case 60: direction_ready(p, s, executor::direction_plane, 17); break;
        case 61: direction_ready(p, s, executor::direction_nonplane, 18); break;
        case 62:
            // Plot-turn's finite turn path is preparation completion, NOT travel assignment/arrival.
            direction_ready(p, s, executor::direction_plane, 44, false);
            break;
        case 65: {
            U         *u = unit(p, s);
            route_view route{};
            if (!u || u->state != 17 || u->order != 16 || u->move_microstep != 31 ||
                !H::read_route(p, s, route) || !route.guided_flight) break;
            const uint32_t actual = mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u));
            if (actual >= mh::caps::path::slot_limit() || actual != route.path_slot) break;
            mh_order_tasks_request q{};
            mh_order_tasks_result  result{};
            q.operation = uint32_t(P::operation::air_boundary);
            q.player    = p;
            q.slot      = s;
            q.args[0]   = actual;
            q.args[1]   = uint32_t(u->path_cursor);
            if (!H::invoke(&q, &result)) std::abort();
            if (!result.values[0] && result.values[1] && q.args[1] >= AIR_CHUNK_STEPS) std::abort();
            if (result.values[0]) {
                // An actual waypoint visit can advance the logical ticket without a
                // finite replacement. Later native observations need the new ticket.
                begin(p, s, false);
            }
            u = unit(p, s);
            route_view after{};
            if (u && u->state == 17 && u->order == 16 && H::read_route(p, s, after) && after.guided_flight &&
                mh::caps::path::unit_slot(reinterpret_cast<const uint8_t *>(u)) == after.path_slot) {
                // Refill has no allocation and its complete legal chunk is prevalidated.
                // A broken refill contract must never masquerade as finite arrival.
                if (u->path_cursor < 0 || uint32_t(u->path_cursor) >= AIR_CHUNK_STEPS) std::abort();
            }
            break;
        }
        case 66: {
            mh_order_tasks_request q{};
            mh_order_tasks_result  result{};
            q.operation = uint32_t(P::operation::begin_due_player_batch);
            if (!H::invoke(&q, &result)) std::abort();
            const auto *enabled = R::ptr<const int32_t>(R::RID_STRAT_AI_ENABLED);
            if (!enabled) std::abort();
            if (!*enabled) {
                // Original JZ bypasses the AI CALL and its post callback entirely.
                q.operation = uint32_t(P::operation::end_due_player_batch);
                if (!H::invoke(&q, &result)) std::abort();
            }
            break;
        }
        case 67: {
            mh_order_tasks_request q{};
            mh_order_tasks_result  result{};
            q.operation = uint32_t(P::operation::end_due_player_batch);
            if (!H::invoke(&q, &result)) std::abort();
            break;
        }
        default: std::abort();
    }
    return 0;
}
// Reuses target_line_order_events' measured full FX envelope. Callback cannot consume a live x87
// operand, change precision/rounding, XMM/MXCSR or native EFLAGS. Displaced instructions run outside.
__declspec(naked) void gateway() {
    __asm {
        pushfd
        pushad
        mov esi,esp
        sub esp,527
        and esp,-16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push esi
        push dword ptr [esi+28h]
        call observe
        add esp,8
        test eax,eax
        jz keep_return
        mov dword ptr [esi+24h],eax
    keep_return:
        fxrstor [esp]
        mov esp,esi
        popad
        popfd
        ret
    }
}
#define EVENT(ID)  __asm push ID __asm call gateway __asm lea esp, [esp + 4]
#define RESUME(VA) __asm push VA __asm ret
// These are JMP-splices. CALL reproductions use data slots, never a temporary argument register.
uint32_t               fn_notify = 0x004dae0e, fn_writer = 0x0049581d, fn_commit = 0x0041c86c, fn_centroid = 0x0048d36f;
uint32_t               fn_setstate = 0x004866c9, fn_setstateof = 0x004869b0, fn_setpair = 0x00486657, fn_setpairof = 0x00486913;
uint32_t               fn_alt = 0x004d5708, fn_attack = 0x004d55ef, fn_home = 0x0046a56d, fn_remove = 0x004db551;
uint32_t               fn_valid = 0x004201e4, fn_nearest = 0x0041ed6d, fn_fallback = 0x0042039c, fn_order = 0x0048696f;
uint32_t               fn_due_ai             = 0x004db905; // Generated call wrapper has no corresponding mh::addr entry.
uint32_t               addr_due_ai_enabled   = mh::addr::_G_LLM_STRAT_AI_ENABLED;
uint32_t               addr_air_current_unit = mh::addr::_G_LLM_STRAT_CUR_UNIT;
__declspec(naked) void air_settled_boundary() {
    EVENT(65)
    __asm mov eax, dword ptr[addr_air_current_unit] __asm mov eax, dword ptr[eax] RESUME(00480cf6h)
}
__declspec(naked) void due_player_batch_begin() {
    EVENT(66)
    // Replay the displaced CMP after restoring native EFLAGS/FX state.
    __asm push eax __asm mov eax, dword ptr[addr_due_ai_enabled] __asm cmp dword ptr[eax], 0 __asm pop eax RESUME(0043f531h)
}
__declspec(naked) void due_player_batch_end() {
    // JMP splice: original two PUSHes still hold double dt on this stack.
    // The CALL pushes exactly one return address; original __watcall RET 8
    // consumes dt. No ADD ESP,8 here, and no wrapper return address to remove.
    __asm call dword ptr[fn_due_ai] EVENT(67)
        RESUME(0043f544h)
}
__declspec(naked) void handler() {
    EVENT(1)
    __asm call dword ptr[eax + 00e15a3ch] EVENT(2) RESUME(0047c8e3h)
}
// clang-format off
__declspec(naked) void reselect_handler() {
    __asm {
        lea esp, [esp + 4]
        mov eax, dword ptr ds:[00e162e0h]
        movzx eax, word ptr[eax + 6]
        shl eax, 2
        call dword ptr[eax + 00e15a3ch]
    }
    EVENT(2)
    RESUME(0047c8e3h)
}
// clang-format on
uint32_t fn_chase  = 0x004866fb,
         fn_attach = 0x00496a7b;
__declspec(naked) void walker_access() {
    EVENT(29)
    __asm cmp word ptr[eax + 4], 1ah RESUME(0047ca37h)
}
__declspec(naked) void path_access() {
    EVENT(30)
    __asm call dword ptr[fn_chase] RESUME(00480c74h)
}
__declspec(naked) void reference_return() {
    EVENT(32)
    __asm ret
}
__declspec(naked) void reference_entry() {
    EVENT(31)
    __asm push offset reference_return __asm push ebp __asm mov ebp, esp __asm push 18h RESUME(004dac4ch)
}
__declspec(naked) void generic_return() {
    EVENT(34)
    __asm ret
}
__declspec(naked) void generic_primary() {
    EVENT(33)
    __asm push offset generic_return __asm push ebp __asm mov ebp, esp __asm push 28h RESUME(004d55f7h)
}
__declspec(naked) void generic_alt() {
    EVENT(33)
    __asm push offset generic_return __asm push ebp __asm mov ebp, esp __asm push 28h RESUME(004d5710h)
}
__declspec(naked) void plane_start() {
    EVENT(40)
    __asm push ebp __asm mov ebp, esp __asm push 58h RESUME(00484b52h)
}
__declspec(naked) void overwrite_done() {
    EVENT(35)
    __asm mov dword ptr[ebp - 2ch], 1 RESUME(00466704h)
}
__declspec(naked) void schedule_end() {
    EVENT(38)
    __asm mov eax, dword ptr[ebp - 24h] __asm inc dword ptr[ebp - 24h] RESUME(00466390h)
}
__declspec(naked) void release_end() {
    EVENT(39)
    __asm mov eax, dword ptr[ebp - 24h] __asm inc dword ptr[ebp - 24h] RESUME(004665adh)
}
__declspec(naked) void ground_start() {
    EVENT(3)
    __asm call dword ptr[fn_commit] RESUME(00482897h)
}
__declspec(naked) void direction_start() {
    EVENT(4)
    __asm call dword ptr[fn_centroid] RESUME(004835aeh)
}
__declspec(naked) void ground_assigned() {
    EVENT(5)
    __asm call dword ptr[fn_notify] RESUME(00482ebbh)
}
__declspec(naked) void writer1() {
    EVENT(6)
    __asm call dword ptr[fn_writer] EVENT(7) RESUME(00483c0fh)
}
__declspec(naked) void writer2() {
    EVENT(6)
    __asm call dword ptr[fn_writer] EVENT(7) RESUME(00484332h)
}
__declspec(naked) void writer3() {
    EVENT(6)
    __asm call dword ptr[fn_writer] EVENT(7) RESUME(004849d3h)
}
__declspec(naked) void writerplane() {
    EVENT(63)
    __asm call dword ptr[fn_writer] EVENT(7) RESUME(00484dd3h)
}
__declspec(naked) void walker_terminal() {
    EVENT(8)
    __asm mov edx, 1 RESUME(0047cfcch)
}
__declspec(naked) void direction_terminal() {
    EVENT(9)
    __asm mov eax, dword ptr ds : [00e162e0h] RESUME(00480d3ah)
}
__declspec(naked) void abort_wave() {
    EVENT(10)
    RESUME(0041ca2eh)
}
__declspec(naked) void valid_tile() { __asm call dword ptr[fn_valid] EVENT(11) RESUME(0041ccfch) }
__declspec(naked) void nearest() { __asm call dword ptr[fn_nearest] EVENT(12) RESUME(0041d0dch) }
__declspec(naked) void fallback() { __asm call dword ptr[fn_fallback] EVENT(11) RESUME(0041d23bh) }
__declspec(naked) void giveup() {
    EVENT(13)
    __asm call dword ptr[fn_setpairof] RESUME(00482ca1h)
}
__declspec(naked) void poolplane() {
    EVENT(14)
    __asm call dword ptr[fn_setstate] RESUME(00484b8fh)
}
__declspec(naked) void pool1() {
    EVENT(15)
    __asm call dword ptr[fn_setstateof] RESUME(004838fah)
}
__declspec(naked) void pool2() {
    EVENT(15)
    __asm call dword ptr[fn_setstateof] RESUME(00483fcch)
}
__declspec(naked) void pool3() {
    EVENT(15)
    __asm call dword ptr[fn_setstateof] RESUME(004846beh)
}
__declspec(naked) void auto_alt() {
    EVENT(16)
    __asm call dword ptr[fn_alt] EVENT(17) RESUME(004ede61h)
}
__declspec(naked) void auto_attack() {
    EVENT(37)
    __asm call dword ptr[fn_attack] EVENT(17) RESUME(004ede7ah)
}
__declspec(naked) void death_unit() {
    EVENT(18)
    __asm mov eax, dword ptr ds : [00e162e0h] RESUME(0047e40ah)
}
__declspec(naked) void death_building() {
    EVENT(19)
    __asm mov eax, dword ptr ds : [00e162dch] RESUME(0047112ah)
}
__declspec(naked) void clean_unit() { __asm call dword ptr[fn_setpair] EVENT(20) RESUME(0048513fh) }
__declspec(naked) void clean_building() { __asm call dword ptr[fn_setpair] EVENT(20) RESUME(00484f09h) }
__declspec(naked) void clean_plane() { __asm call dword ptr[fn_setpair] EVENT(20) RESUME(00484cf0h) }
__declspec(naked) void clean_hover1() { __asm call dword ptr[fn_setpair] EVENT(20) RESUME(004868cfh) }
__declspec(naked) void clean_hover2() { __asm call dword ptr[fn_setpair] EVENT(20) RESUME(004868e0h) }
__declspec(naked) void plane_ready() { __asm call dword ptr[fn_setstate] EVENT(60) RESUME(00484deeh) }
__declspec(naked) void hover_ready() { __asm call dword ptr[fn_setstate] EVENT(61) RESUME(004809c4h) }
__declspec(naked) void turn_ready() { __asm call dword ptr[fn_attach] EVENT(62) RESUME(00485eaah) }
__declspec(naked) void climb_done() { __asm call dword ptr[fn_setstate] EVENT(21) RESUME(00481a61h) }
__declspec(naked) void descent_done() { __asm call dword ptr[fn_setstate] EVENT(21) RESUME(00482067h) }
__declspec(naked) void hover_return() {
    EVENT(22)
    __asm mov eax, dword ptr ds : [00e162e0h] RESUME(004809fah)
}
__declspec(naked) void deferred_guard() {
    EVENT(23)
    __asm call dword ptr[fn_order] RESUME(00469a6dh)
}
__declspec(naked) void commit_guard() {
    EVENT(26)
    __asm mov edx, dword ptr[ebp - 34h] __asm imul edx, dword ptr[native_row_bytes] RESUME(00466ce8h)
}
__declspec(naked) void home1() {
    EVENT(51)
    __asm call dword ptr[fn_home] RESUME(0047cd2eh)
}
__declspec(naked) void home2() {
    EVENT(52)
    __asm call dword ptr[fn_home] RESUME(00485130h)
}
__declspec(naked) void home3() {
    EVENT(53)
    __asm call dword ptr[fn_home] RESUME(0048525eh)
}
__declspec(naked) void home4() {
    EVENT(54)
    __asm call dword ptr[fn_home] RESUME(00482c72h)
}
__declspec(naked) void deferred_end() {
    EVENT(25)
    __asm lea esp, [ebp - 0ch] __asm pop edi __asm pop esi __asm pop ecx __asm pop ebp __asm ret
}
// Generic removal seams are disjoint from lethal witnesses and preserve their original call once.
__declspec(naked) void remove1() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(0047117bh)
}
__declspec(naked) void remove2() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(0047e53ch)
}
__declspec(naked) void remove3() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(00471a09h)
}
__declspec(naked) void remove4() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(004736d6h)
}
__declspec(naked) void remove5() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(0048595eh)
}
__declspec(naked) void remove6() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(004877b3h)
}
__declspec(naked) void remove7() {
    EVENT(24)
    __asm call dword ptr[fn_remove] RESUME(00488025h)
}
__declspec(naked) void session_reset() {
    EVENT(43)
    __asm push ebp __asm mov ebp, esp __asm push 28h RESUME(00453eech)
}
// Displace whole ten-byte stores. EVENT preserves EAX, EFLAGS and full FX state.
__declspec(naked) void known_conquered() {
    EVENT(64)
    __asm mov dword ptr[eax + 00e5836bh], 2 RESUME(0044041ah)
}
__declspec(naked) void known_invasion_here() {
    EVENT(64)
    __asm mov dword ptr[eax + 00e5836bh], 4 RESUME(004997c7h)
}
__declspec(naked) void known_invasion_remote() {
    EVENT(64)
    __asm mov dword ptr[eax + 00e5836bh], 4 RESUME(00499884h)
}
__declspec(naked) void known_tactical_return() {
    EVENT(64)
    __asm mov dword ptr[eax + 00e5836bh], 1 RESUME(0044d40eh)
}
#define SITE(VA, OWNER, FN, BYTES) {VA, OWNER, reinterpret_cast<const uint8_t *>(BYTES), uint32_t(sizeof(BYTES) - 1), reinterpret_cast<void *>(&FN)}
const site PATCHES[] = {
    SITE(0x43f52a, 0x43f512, due_player_batch_begin, "\x83\x3d\xa0\x01\x5d\x00\x00"),
    SITE(0x43f53f, 0x43f512, due_player_batch_end, "\xe8\xc1\xc3\x09\x00"),
    SITE(0x480cf1, 0x480b53, air_settled_boundary, "\xa1\xe0\x62\xe1\x00"),
    SITE(0x440410, 0x4402b0, known_conquered, "\xc7\x80\x6b\x83\xe5\x00\x02\x00\x00\x00"),
    SITE(0x4997bd, 0x4996c4, known_invasion_here, "\xc7\x80\x6b\x83\xe5\x00\x04\x00\x00\x00"),
    SITE(0x49987a, 0x4996c4, known_invasion_remote, "\xc7\x80\x6b\x83\xe5\x00\x04\x00\x00\x00"),
    SITE(0x44d404, 0x44d3a8, known_tactical_return, "\xc7\x80\x6b\x83\xe5\x00\x01\x00\x00\x00"),
    SITE(0x453ee4, 0x453ee4, session_reset, "\x55\x89\xe5\x68\x28\x00\x00\x00"),
    SITE(0x484b4a, 0x484b4a, plane_start, "\x55\x89\xe5\x68\x58\x00\x00\x00"),
    SITE(0x4dac44, 0x4dac44, reference_entry, "\x55\x89\xe5\x68\x18\x00\x00\x00"),
    SITE(0x4d55ef, 0x4d55ef, generic_primary, "\x55\x89\xe5\x68\x28\x00\x00\x00"),
    SITE(0x4d5708, 0x4d5708, generic_alt, "\x55\x89\xe5\x68\x28\x00\x00\x00"),
    SITE(0x47ca32, 0x47c903, walker_access, "\x66\x83\x78\x04\x1a"),
    SITE(0x480c6f, 0x480b53, path_access, "\xe8\x87\x5a\x00\x00"),
    SITE(0x4666fd, 0x46652e, overwrite_done, "\xc7\x45\xd4\x01\x00\x00\x00"),
    SITE(0x46638a, 0x466348, schedule_end, "\x8b\x45\xdc\xff\x45\xdc"),
    SITE(0x4665a7, 0x46652e, release_end, "\x8b\x45\xdc\xff\x45\xdc"),
    {0x466ce1, 0x466892, F_EXPECT, 7, reinterpret_cast<void *>(&commit_guard)},
    SITE(0x47c8dd, 0x47c73a, handler, "\xff\x90\x3c\x5a\xe1\x00"),
    SITE(0x482892, 0x482451, ground_start, "\xe8\xd5\x9f\xf9\xff"),
    SITE(0x4835a9, 0x483011, direction_start, "\xe8\xc1\x9d\x00\x00"),
    SITE(0x482eb6, 0x482451, ground_assigned, "\xe8\x53\x7f\x05\x00"),
    SITE(0x483c0a, 0x483011, writer1, "\xe8\x0e\x1c\x01\x00"),
    SITE(0x48432d, 0x483011, writer2, "\xe8\xeb\x14\x01\x00"),
    SITE(0x4849ce, 0x483011, writer3, "\xe8\x4a\x0e\x01\x00"),
    SITE(0x484dce, 0x484b4a, writerplane, "\xe8\x4a\x0a\x01\x00"),
    SITE(0x47cfc7, 0x47c903, walker_terminal, "\xba\x01\x00\x00\x00"),
    SITE(0x480d35, 0x480b53, direction_terminal, "\xa1\xe0\x62\xe1\x00"),
    SITE(0x41c9ac, 0x41c86c, abort_wave, "\xe9\x7d\x00\x00\x00"),
    SITE(0x41ccf7, 0x41ca37, valid_tile, "\xe8\xe8\x34\x00\x00"),
    SITE(0x41d0d7, 0x41ca37, nearest, "\xe8\x91\x1c\x00\x00"),
    SITE(0x41d236, 0x41ca37, fallback, "\xe8\x61\x31\x00\x00"),
    SITE(0x482c9c, 0x482451, giveup, "\xe8\x72\x3c\x00\x00"),
    SITE(0x484b8a, 0x484b4a, poolplane, "\xe8\x3a\x1b\x00\x00"),
    SITE(0x4838f5, 0x483011, pool1, "\xe8\xb6\x30\x00\x00"),
    SITE(0x483fc7, 0x483011, pool2, "\xe8\xe4\x29\x00\x00"),
    SITE(0x4846b9, 0x483011, pool3, "\xe8\xf2\x22\x00\x00"),
    SITE(0x4ede5c, 0x4edcca, auto_alt, "\xe8\xa7\x78\xfe\xff"),
    SITE(0x4ede75, 0x4edcca, auto_attack, "\xe8\x75\x77\xfe\xff"),
    SITE(0x47e405, 0x47e380, death_unit, "\xa1\xe0\x62\xe1\x00"),
    SITE(0x471125, 0x4710ba, death_building, "\xa1\xdc\x62\xe1\x00"),
    SITE(0x48513a, 0x485008, clean_unit, "\xe8\x18\x15\x00\x00"),
    SITE(0x484f04, 0x484e10, clean_building, "\xe8\x4e\x17\x00\x00"),
    SITE(0x484ceb, 0x484b4a, clean_plane, "\xe8\x67\x19\x00\x00"),
    SITE(0x4868ca, 0x4866fb, clean_hover1, "\xe8\x88\xfd\xff\xff"),
    SITE(0x4868db, 0x4866fb, clean_hover2, "\xe8\x77\xfd\xff\xff"),
    SITE(0x484de9, 0x484b4a, plane_ready, "\xe8\xdb\x18\x00\x00"),
    SITE(0x4809bf, 0x4807d9, hover_ready, "\xe8\x05\x5d\x00\x00"),
    SITE(0x485ea5, 0x485d43, turn_ready, "\xe8\xd1\x0b\x01\x00"),
    SITE(0x481a5c, 0x481960, climb_done, "\xe8\x68\x4c\x00\x00"),
    SITE(0x482062, 0x481f7c, descent_done, "\xe8\x62\x46\x00\x00"),
    SITE(0x4809f5, 0x4807d9, hover_return, "\xa1\xe0\x62\xe1\x00"),
    SITE(0x469a68, 0x469a37, deferred_guard, "\xe8\x02\xcf\x01\x00"),
    SITE(0x469dde, 0x469a37, deferred_end, "\x8d\x65\xf4\x5f\x5e\x59\x5d\xc3"),
    SITE(0x47cd29, 0x47c903, home1, "\xe8\x3f\xd8\xfe\xff"),
    SITE(0x48512b, 0x485008, home2, "\xe8\x3d\x54\xfe\xff"),
    SITE(0x485259, 0x485008, home3, "\xe8\x0f\x53\xfe\xff"),
    SITE(0x482c6d, 0x482451, home4, "\xe8\xfb\x78\xfe\xff"),
    SITE(0x471176, 0x4710ba, remove1, "\xe8\xd6\xa3\x06\x00"),
    SITE(0x47e537, 0x47e380, remove2, "\xe8\x15\xd0\x05\x00"),
    SITE(0x471a04, 0x471443, remove3, "\xe8\x48\x9b\x06\x00"),
    SITE(0x4736d1, 0x4736a2, remove4, "\xe8\x7b\x7e\x06\x00"),
    SITE(0x485959, 0x48592d, remove5, "\xe8\xf3\x5b\x05\x00"),
    SITE(0x4877ae, 0x48753e, remove6, "\xe8\x9e\x3d\x05\x00"),
    SITE(0x488020, 0x487ba5, remove7, "\xe8\x2c\x35\x05\x00")};
struct installation {
    const site *contract = nullptr;
    uint8_t     expected[16]{}, replacement[16]{};
    bool        enabled = false, applied = false;
};
installation installed_sites[sizeof PATCHES / sizeof PATCHES[0]]{};
bool         installed = false;
void         rollback() {
    for (size_t n = sizeof installed_sites / sizeof installed_sites[0]; n != 0;) {
        auto &p = installed_sites[--n];
        if (!p.applied) continue;
        if (!mh::hook::patch_bytes_guarded(p.contract->address, p.replacement, p.expected, int(p.contract->length))) std::abort();
        p.applied = false;
    }
}
#undef SITE
#undef EVENT
#undef RESUME
} // namespace
bool guard_dispatch(const uint8_t *frame) {
    const uint32_t i = word(frame, -0x24);
    if (i >= mh::caps::orders::live_count(mh::caps::orders::QUEUE)) return false;
    const auto *q = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (guard(q[i], i, nullptr, word(frame, -0x20))) return true;
    for (auto &d : dispatch_discards)
        if (!d.frame) {
            d.frame  = frame;
            d.record = q[i];
            return false;
        }
    std::abort();
}
void applied_dispatch(const uint8_t *frame) {
    const uint32_t i = word(frame, -0x24);
    if (i >= mh::caps::orders::live_count(mh::caps::orders::QUEUE)) std::abort();
    const auto &q = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE))[i];
    tile        anchor{};
    bool        found = false;
    for (auto &a : anchors)
        if (a.frame == frame) {
            anchor = a.anchor;
            a      = {};
            found  = true;
            break;
        }
    if (uint32_t(q.args[8]) == P::AUTONOMOUS_PRIMARY && !found) std::abort();
    if ((q.owner_and_kind & 0xf0) == 0x80 && !secondary(q)) H::applied(q.owner_and_kind & 15, q.unit_index, q.owner_and_kind, q.order_code, q.param0, q.args, anchor);
}
void finish_dispatch(const uint8_t *frame) {
    for (auto &d : dispatch_discards)
        if (d.frame == frame) {
            discarded(d.record);
            d = {};
            return;
        }
}
bool finish_deferred(const uint8_t *frame) {
    if (discarded_deferred == frame) return false;
    const uint32_t i = word(frame, -0x1c);
    if (i >= mh::caps::orders::live_count(mh::caps::orders::QUEUE)) std::abort();
    const auto &q = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE))[i];
    H::applied(q.owner_and_kind & 15, q.unit_index, q.owner_and_kind, q.order_code, q.param0, q.args, deferred_anchor);
    deferred_anchor = {};
    return true;
}
bool accepted_near(uint32_t p, uint32_t s, bool on_goal) {
    auto      *v = matching(p, s);
    const U   *u = unit(p, s);
    route_view r{};
    node_view  n{};
    if (!v || !u || current_wave.failed || !H::read_route(p, s, r) ||
        !H::read_entry(p, s, r.cursor, n) ||
        (n.task.kind != action::move && n.task.kind != action::patrol && r.status != phase::returning_anchor)) return false;
    if (!r.assignment_valid &&
        (!r.planner_succeeded || (on_goal && (u->goal_x != u->x || u->goal_y != u->y)))) return false;
    if (u->order == 16 && r.assignment_valid &&
        (!on_goal || r.assigned.x != u->x || r.assigned.y != u->y)) return false;
    control accepted = control::unchanged;
    if (r.family == executor::ground_run) emit(p, s, outcome_kind::preparation_finished);
    if (!r.assignment_valid) {
        // Positive post-plan zero-length goal, not a failure-restored current tile.
        emit(p, s, outcome_kind::assigned, {u->x, u->y}, assignment_reason::valid_projection, 255);
        accepted = emit(p, s, outcome_kind::reached_exact, {u->x, u->y}, assignment_reason::valid_projection, 255);
    } else if (on_goal && r.assigned.x == u->x && r.assigned.y == u->y) {
        accepted = emit(p, s, outcome_kind::reached_exact, r.assigned, r.placement, r.path_slot);
    } else {
        accepted = emit(p, s, outcome_kind::reached_near, {u->x, u->y}, assignment_reason::valid_projection, r.path_slot);
    }
    if (u->order == 16 && accepted != control::hold) return false;
    v->near = true;
    return true;
}
void projection_result(bool failure) {
    if (failure) current_wave.failed = true;
}
void retained_record(const uint8_t *frame, uint32_t lane, uint32_t index, bool copy) {
    H::retained_view(lane, word(frame, -0x20) + (copy ? 1u : 0u), index + 1);
    for (auto &v : transfers)
        if (v.frame == frame && v.lane == lane && v.index == index) {
            v.kept = true;
            return;
        }
    for (auto &v : transfers)
        if (!v.frame) {
            v = {frame, lane, index, true};
            return;
        }
    std::abort();
}
void before_queue_overwrite(const uint8_t *frame) {
    const uint32_t index = word(frame, -0x30);
    if (index >= mh::caps::orders::live_count(mh::caps::orders::QUEUE)) std::abort();
    for (auto &v : overwrites)
        if (!v.frame) {
            v = {frame, reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::QUEUE))[index]};
            return;
        }
    std::abort();
}
void lost_records(uint32_t lane, uint32_t count) {
    H::retained_view(lane, 0, 0);
    if (lane > 2 || count > mh::caps::orders::cap(mh::caps::orders::which(lane))) std::abort();
    const auto *q = reinterpret_cast<const O *>(mh::caps::orders::base(mh::caps::orders::which(lane)));
    for (uint32_t i = 0; i < count; ++i) {
        const O record = normalized(q[i], lane == uint32_t(mh::caps::orders::STAGING));
        if ((record.owner_and_kind & 0xf0) != 0x80 || secondary(record)) continue;
        if (!discarded(record)) continue;
        const auto a = H::identity(nullptr, object_kind::unit, record.owner_and_kind & 15, record.unit_index);
        if (a.incarnation) reconcile_flag(a.player, a.slot, UINT32_MAX);
    }
}
bool source_issue_is_active(actor_key actor, target_key target) {
    const auto world   = H::owner().world();
    auto       matches = [&](const origin &v) {
        const auto &scope = v.scope;
        return scope.active && !v.borrowed &&
               scope.actor.player == actor.player && scope.actor.slot == actor.slot && scope.actor.incarnation == actor.incarnation &&
               scope.target.kind == target.kind && scope.target.object.player == target.object.player &&
               scope.target.object.slot == target.object.slot && scope.target.object.incarnation == target.object.incarnation &&
               scope.world.system == world.system && scope.world.planet == world.planet && scope.world.incarnation == world.incarnation &&
               v.world.system == world.system && v.world.planet == world.planet && v.world.incarnation == world.incarnation;
    };
    // Match source_sink_observed's stack selection, not an arbitrary older scope.
    if (origin_depth && origins[origin_depth - 1].player == actor.player && origins[origin_depth - 1].slot == actor.slot)
        return matches(origins[origin_depth - 1]);
    return generic_depth && matches(generic_origins[generic_depth - 1]);
}
void begin_borrowed(const reservations::issue_scope &scope, bool managed) {
    if (!scope.active) return;
    if (origin_depth == 4) std::abort();
    machine m{};
    m.eax     = scope.actor.player;
    m.edx     = scope.actor.slot;
    origin &v = origins[origin_depth++];
    scope_snapshot(v, m);
    if (scope.world.system != v.world.system || scope.world.planet != v.world.planet ||
        scope.world.incarnation != v.world.incarnation) std::abort();
    v.scope    = scope;
    v.managed  = managed;
    v.borrowed = true;
}
bool finish_borrowed(const reservations::issue_scope &scope, bool recorded, bool source_entered, bool suppressed) {
    if (!origin_depth || !origins[origin_depth - 1].borrowed) return false;
    origin  &v = origins[origin_depth - 1];
    uint32_t a[17], b[17];
    P::put_issue_scope(a, a + 14, v.scope);
    P::put_issue_scope(b, b + 14, scope);
    if (std::memcmp(a, b, sizeof a)) return false;
    v.source_accepted = recorded;
    if (source_entered) {
        v.source_sink       = true;
        v.source_suppressed = suppressed;
    }
    scope_finish(v);
    --origin_depth;
    return true;
}
void source_sink_observed(uint32_t p, uint32_t s, bool suppressed) {
    origin *v = nullptr;
    if (origin_depth && origins[origin_depth - 1].player == p && origins[origin_depth - 1].slot == s) v = origins + origin_depth - 1;
    else if (generic_depth && generic_origins[generic_depth - 1].player == p && generic_origins[generic_depth - 1].slot == s) v = generic_origins + generic_depth - 1;
    if (v) {
        v->source_sink       = true;
        v->source_suppressed = suppressed;
    }
}
void before_world_publication() {
    for (uint32_t lane = 0; lane < 3; ++lane) H::retained_view(lane, 0, 0);
    std::memset(plans, 0, sizeof plans);
    current_wave       = {};
    origin_depth       = 0;
    discarded_deferred = nullptr;
    handler_latch      = false;
    for (auto &d : dispatch_discards) d = {};
    generic_depth = reference_depth = 0;
    writer_zero = writer_plane = false;
    for (auto &a : anchors) a = {};
    deferred_anchor = {};
    for (auto &v : transfers) v = {};
    for (auto &v : overwrites) v = {};
}
const site *sites(size_t &count) {
    native_row_bytes = mh::caps::row_bytes(mh::caps::UNITS);
    std::memcpy(F_EXPECT + 3, &native_row_bytes, 4);
    count = sizeof PATCHES / sizeof PATCHES[0];
    return PATCHES;
}
bool install() {
    if (installed) return true;
    if (!mh::en_build_ok() || !H::reserve_after_caps()) return false;
    size_t      count = 0;
    const site *list  = sites(count);
    for (size_t i = 0; i < count; ++i) {
        auto &p    = installed_sites[i];
        p          = {};
        p.contract = list + i;
        p.enabled  = mh::hook::promoted_owner_of(p.contract->owner) == nullptr;
        if (!p.enabled) continue;
        if (p.contract->length < 5 || p.contract->length > sizeof p.expected ||
            mh::hook::promoted_owner_of(p.contract->address)) return false;
        std::memcpy(p.expected, p.contract->expected, p.contract->length);
        if (std::memcmp(reinterpret_cast<const void *>(p.contract->address), p.expected, p.contract->length) != 0) return false;
        std::memset(p.replacement, 0x90, sizeof p.replacement);
        p.replacement[0]       = 0xe9;
        const int32_t relative = int32_t(reinterpret_cast<uintptr_t>(p.contract->destination) - p.contract->address - 5);
        std::memcpy(p.replacement + 1, &relative, 4);
    }
    for (auto &p : installed_sites)
        if (p.enabled) {
            if (!mh::hook::patch_bytes_guarded_caps(p.contract->address, p.expected, p.replacement, int(p.contract->length))) {
                rollback();
                return false;
            }
            p.applied = true;
        }
    installed = true;
    return true;
}
void preparation_finished() {
    emit(player(), slot(), outcome_kind::preparation_finished);
    begin(player(), slot(), false);
}
extern "C" __declspec(naked) void order_tasks_projection_gateway() {
    __asm push 11 __asm call gateway __asm lea esp, [esp + 4] __asm ret
}
extern "C" __declspec(naked) void order_tasks_retry_gateway() {
    __asm push 28 __asm call gateway __asm lea esp, [esp + 4] __asm ret
}
extern "C" __declspec(naked) void order_tasks_planner_return_gateway() {
    __asm push 42 __asm call gateway __asm lea esp, [esp + 4] __asm ret
}
extern "C" __declspec(naked) void order_tasks_no_slot_gateway() {
    __asm push 41 __asm call gateway __asm lea esp, [esp + 4] __asm ret
}
extern "C" uintptr_t __cdecl order_tasks_guard_frame(const uint8_t *frame) {
    return guard_dispatch(frame) ? 0 : reinterpret_cast<uintptr_t>(&skip_source);
}
extern "C" __declspec(naked) void order_tasks_dispatch_guard_gateway() {
    __asm {
        pushfd
        pushad
        mov esi,esp
        sub esp,527
        and esp,-16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push ebp
        call order_tasks_guard_frame
        add esp,4
        test eax,eax
        jz source_valid
        mov dword ptr [esi+24h],eax
    source_valid:
        fxrstor [esp]
        mov esp,esi
        popad
        popfd
        ret
    }
}
} // namespace mh::order_tasks::native
