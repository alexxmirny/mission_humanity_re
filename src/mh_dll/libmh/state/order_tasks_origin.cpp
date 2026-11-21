// Destination: src/mh_dll/libmh/state/order_tasks_origin.cpp
#include "state/order_tasks_origin.h"
#include "include/libmh_host_api.gen.h"
#include "state/host_api.h"
namespace mh::order_tasks::origin {
namespace {
primary_scope *current = nullptr;
issue_guard   *issuing = nullptr;
} // namespace
primary_scope::primary_scope(uint32_t player, uint32_t slot) : previous_(current), player_(player), slot_(slot) {
    current = this; // proves only this synchronous producer, never owns canonical intent
}
primary_scope::~primary_scope() {
    if (current == this) current = previous_;
}
issue_guard::issue_guard(uint32_t player, uint32_t slot, uint32_t ref, uint32_t target_slot, bool physical_only)
    : previous_(issuing), physical_only_(physical_only) {
    if (physical_only && issuing && issuing->active_) return;
    if (!physical_only && (!current || current->player_ != player || current->slot_ != slot)) return;
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(host_protocol::operation::reservation);
    q.player    = player;
    q.slot      = slot;
    q.args[0]   = uint32_t(physical_only ? host_protocol::reservation_operation::generic_begin : host_protocol::reservation_operation::begin_issue);
    q.args[1]   = uint32_t((ref & 0x40) ? object_kind::building : object_kind::unit);
    q.args[2]   = ref & 15;
    q.args[3]   = target_slot;
    if (!mh::host().order_tasks(&q, &r)) {
        denied_ = true;
        return;
    }
    const auto decision  = reservations::issue_decision(r.values[0]);
    denied_              = decision == reservations::issue_decision::denied;
    active_              = decision == reservations::issue_decision::allowed || decision == reservations::issue_decision::physical;
    physical_only_       = physical_only_ || decision == reservations::issue_decision::physical;
    yielded_to_due_edit_ = !physical_only && denied_ && r.values[18] != 0;
    if (active_) {
        scope_     = host_protocol::get_issue_scope(r.values + 1, r.values + 15);
        issuing    = this;
        installed_ = true;
    }
}
issue_guard::~issue_guard() {
    if (active_ && issuing == this) {
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(host_protocol::operation::reservation);
        q.player    = scope_.actor.player;
        q.slot      = scope_.actor.slot;
        q.args[0]   = uint32_t(host_protocol::reservation_operation::finish_issue);
        host_protocol::put_issue_scope(q.args + 1, q.args + 18, scope_);
        q.args[15] = recorded_;
        q.args[16] = source_sink_executed_;
        q.args[17] = source_sink_suppressed_;
        mh::host().order_tasks(&q, &r);
    }
    if (installed_ && issuing == this) issuing = previous_;
}
void published() {
    current = nullptr;
    issuing = nullptr;
}
void note_source_sink(mh_order_tasks_callback observer, uint32_t player, uint32_t slot, bool suppressed) {
    if (issuing && issuing->active_ && issuing->scope_.actor.player == player && issuing->scope_.actor.slot == slot) {
        issuing->source_sink_executed_   = true;
        issuing->source_sink_suppressed_ = suppressed;
        return;
    }
    if (!observer) return; // pure component fixture, no producer broker
    mh_order_tasks_request q{};
    mh_order_tasks_result  r{};
    q.operation = uint32_t(host_protocol::operation::reservation);
    q.player    = player;
    q.slot      = slot;
    q.args[0]   = uint32_t(host_protocol::reservation_operation::sink_observed);
    q.args[1]   = suppressed ? 1 : 0;
    observer(&q, &r); // native producer -> promoted sink: observation only
}
void stamp(uint32_t player, uint32_t slot, uint16_t code, int16_t param, int32_t (&args)[13]) {
    if (!issuing || !issuing->active_ ||
        (!issuing->physical_only_ && (!current || current->player_ != player || current->slot_ != slot)) ||
        code == 0x1d || code == 0x1e ||
        !((param == 0x1a || param == 0x1b || param == 0x1c) || (param == 1 && (code == 0x1a || code == 0x1b || code == 0x1c))) ||
        args[5] <= 0 || args[6] <= 0) return;
    const auto &scope = issuing->scope_;
    if (scope.actor.player != player || scope.actor.slot != slot ||
        (uint32_t(args[5]) & 15) != scope.target.object.player || uint32_t(args[6]) != scope.target.object.slot ||
        bool(args[5] & 0x40) != (scope.target.kind == object_kind::building)) return;
    issuing->recorded_ = true;
    args[8]            = int32_t(issuing->physical_only_ ? host_protocol::PHYSICAL_PRIMARY : host_protocol::AUTONOMOUS_PRIMARY);
    args[9]            = int32_t(scope.actor.incarnation);
    args[10]           = int32_t(scope.epoch);
    args[11]           = int32_t(scope.target.object.incarnation);
}
} // namespace mh::order_tasks::origin
