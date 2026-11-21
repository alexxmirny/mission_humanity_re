// Destination: src/mh_dll/libmh_test/order_tasks_selftest.cpp
// Consumer-edge tests: fixed fake executors expose actual typed assignment/settled events;
// no engine policy is reimplemented here. Wire/codec negatives use public byte grammar.
#include "include/mh_order_tasks.h"
#include "include/mh_air_patrol.h"
#include "include/mh_order_tasks_dispatch.h"
#include "include/mh_order_tasks_path.h"
#include "orders/order_queue.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace mh::order_tasks;
struct checks {
    uint32_t count = 0, failed = 0;
    void     operator()(const char *name, bool ok) {
        ++count;
        if (!ok) {
            ++failed;
            std::printf("  FAIL: %s\n", name);
        }
    }
};
struct fixture {
    dimensions           d{2, 2, 32, 32, 4};
    world_key            w{7, 5, 100};
    std::vector<uint8_t> raw = std::vector<uint8_t>(state::required_bytes(d));
    state                s;
    actor_view           actors[4]{};
    active_capture       captures[4]{};
    bool                 capture_ok[4]{true, true, true, true};
    uint32_t             building_life[8]{};
    target_status        building_status[8]{};
    execution            calls[128]{};
    uint32_t             issued = 0, cleanup_attempts = 0, cleaned = 0;
    bool                 cleanup_ready = true, cancel_allowed = true;
    uint32_t             target_reads = 0, detached = 0, cancelled_pending = 0;
    int32_t              reserved_tally = 0;
    fixture() {
        s.initialize(raw.data(), uint32_t(raw.size()), d, w);
        for (uint32_t i = 0; i != 4; ++i) {
            actors[i]          = {{i / 2, i % 2, 1}, 10 + i, executor::ground_run, {1, 1}, 0, 0, 0, true};
            captures[i].status = phase::empty;
        }
        for (uint32_t i = 0; i != 8; ++i) {
            building_life[i]   = 1;
            building_status[i] = target_status::alive_hostile;
        }
    }
    actor_key       a(uint32_t i = 0) const { return actors[i].key; }
    static uint32_t index(actor_key a) { return a.player * 2 + a.slot; }
    static bool     actor(void *p, actor_key a, actor_view &out) {
        auto &f = *static_cast<fixture *>(p);
        if (a.player >= 2 || a.slot >= 2 || !a.incarnation) return false;
        out = f.actors[index(a)];
        return out.key.incarnation == a.incarnation;
    }
    static bool capture(void *p, actor_key a, active_capture &out) {
        auto &f = *static_cast<fixture *>(p);
        if (!f.capture_ok[index(a)]) return false;
        out = f.captures[index(a)];
        return true;
    }
    static target_status target(void *p, actor_key, target_key t) {
        auto &f = *static_cast<fixture *>(p);
        ++f.target_reads;
        if (t.kind == object_kind::building) {
            const uint32_t i = t.object.player * 4 + t.object.slot;
            return f.building_life[i] == t.object.incarnation ? f.building_status[i] : target_status::unavailable;
        }
        actor_view view{};
        return actor(p, t.object, view) ? target_status::alive_hostile : target_status::unavailable;
    }
    static apply_result apply(void *p, const execution &e) {
        auto &f = *static_cast<fixture *>(p);
        if (f.issued < 128) f.calls[f.issued] = e;
        ++f.issued;
        return apply_result::applied;
    }
    static bool cleanup(void *p, const ticket &) {
        auto &f = *static_cast<fixture *>(p);
        ++f.cleanup_attempts;
        if (!f.cleanup_ready) return false;
        ++f.cleaned;
        return true;
    }
    static bool lifetime(void *p, object_kind kind, actor_key a) {
        auto &f = *static_cast<fixture *>(p);
        if (kind == object_kind::unit) {
            actor_view v{};
            return actor(p, a, v);
        }
        return kind == object_kind::building && a.player < 2 && a.slot < 4 &&
               f.building_life[a.player * 4 + a.slot] == a.incarnation;
    }
    static bool cancel_pending(void *p, actor_key a) {
        auto               &f = *static_cast<fixture *>(p);
        primary_reservation v{};
        if (!f.s.read_reservation(a, v) || v.phase != reservation_phase::pending) return true;
        if (!f.cancel_allowed) return false;
        primary_reservation prior{};
        if (v.prior_target.kind != object_kind::none && lifetime(p, v.prior_target.kind, v.prior_target.object))
            prior = {reservation_phase::bound, v.prior_target, v.prior_packed & 65535u, {}, v.prior_packed, v.execution_epoch};
        if (!f.s.write_reservation(a, prior)) return false;
        f.reserved_tally -= int32_t(v.delta_raw16);
        ++f.cancelled_pending;
        return true;
    }
    static void detach(void *p, actor_key a, target_key target) {
        auto               &f = *static_cast<fixture *>(p);
        primary_reservation v{};
        if (f.s.read_reservation(a, v) && v.phase == reservation_phase::bound &&
            v.target.kind == target.kind && v.target.object.player == target.object.player &&
            v.target.object.slot == target.object.slot && v.target.object.incarnation == target.object.incarnation &&
            lifetime(p, target.kind, target.object)) f.reserved_tally -= int32_t(v.delta_raw16);
        ++f.detached;
    }
    backend live() { return {this, actor, capture, target, apply, cleanup, cancel_pending, detach}; }
    entry   move(uint32_t x, uint32_t y, uint32_t seq, action kind = action::move, uint32_t group = 0) {
        return {kind, {x, y}, {object_kind::none, {}}, group, seq};
    }
    entry attack(uint32_t slot, uint32_t seq) {
        return {action::attack_building, {0, 0}, {object_kind::building, {1, slot, building_life[4 + slot]}}, 40, seq};
    }
    refusal    edit_task(uint32_t i, edit_kind op, const entry &e) { return s.apply_edit(live(), {w, a(i), op, e}); }
    route_view view(uint32_t i = 0) {
        route_view r{};
        s.read_route(a(i), r);
        return r;
    }
    outcome event(uint32_t i, outcome_kind kind, tile position = {}, tile assigned = {},
                  assignment_reason placement = assignment_reason::none, uint32_t path = 0) {
        return {view(i).active, kind, actors[i].family, position, assigned, placement, path, {}};
    }
    void assign(uint32_t i, tile at, assignment_reason reason = assignment_reason::requested, uint32_t path = 9) {
        s.observe(live(), event(i, outcome_kind::assigned, actors[i].position, at, reason, path));
    }
    control reach(uint32_t i, tile at, outcome_kind kind = outcome_kind::reached_exact, uint32_t path = 9) {
        actors[i].position = at;
        return s.observe(live(), event(i, kind, at,
                                       kind == outcome_kind::reached_near ? at : tile{},
                                       kind == outcome_kind::reached_near ? assignment_reason::valid_projection : assignment_reason::none, path));
    }
    control arrive(uint32_t i, tile at, assignment_reason reason = assignment_reason::requested, uint32_t path = 9) {
        assign(i, at, reason, path);
        return reach(i, at, reason == assignment_reason::requested ? outcome_kind::reached_exact : outcome_kind::reached_near, path);
    }
};
struct legacy_booking_fixture : fixture {
    reservations::actual_primary actual{object_kind::building, 1, 1, 4, 0x8001};
    uint32_t                     tally = 23, adjustments = 0, native_writes = 0, target_clears = 0;
    static bool                  read(void *v, actor_key key, reservations::actual_primary &out) {
        auto &f = *static_cast<legacy_booking_fixture *>(v);
        if (key.player != 0 || key.slot != 1 || key.incarnation != f.a(1).incarnation) return false;
        out = f.actual;
        return true;
    }
    static bool write(void *v, actor_key, uint32_t cached, uint32_t bits) {
        auto &f = *static_cast<legacy_booking_fixture *>(v);
        ++f.native_writes;
        f.actual.cached_raw16 = cached;
        f.actual.owned_bits   = bits;
        return true;
    }
    static actor_key identity(void *v, object_kind kind, uint32_t p, uint32_t slot) {
        auto &f = *static_cast<legacy_booking_fixture *>(v);
        if (kind == object_kind::unit && p < 2 && slot < 2) return f.actors[p * 2 + slot].key;
        if (kind == object_kind::building && p < 2 && slot < 4) return {p, slot, f.building_life[p * 4 + slot]};
        return {p, slot, 0};
    }
    static bool adjust(void *v, target_key target, uint32_t delta, bool subtract) {
        auto &f = *static_cast<legacy_booking_fixture *>(v);
        if (target.kind != object_kind::building || target.object.player != 1 || target.object.slot != 1 || !subtract) return false;
        ++f.adjustments;
        f.tally -= delta;
        return true;
    }
    static bool clear_target(void *v, actor_key) {
        auto &f = *static_cast<legacy_booking_fixture *>(v);
        ++f.target_clears;
        f.actual.kind = object_kind::none;
        return true;
    }
    reservations::access accounting() { return {this, read, write, identity, adjust, clear_target, nullptr, nullptr}; }
    static bool          booked_actor(void *v, actor_key key, actor_view &out) {
        auto               &f = *static_cast<legacy_booking_fixture *>(v);
        primary_reservation binding{};
        return fixture::actor(static_cast<fixture *>(&f), key, out) &&
               reservations::capture_binding(f.s, f.accounting(), key, binding);
    }
    static bool booked_capture(void *v, actor_key key, active_capture &out) {
        auto               &f = *static_cast<legacy_booking_fixture *>(v);
        primary_reservation binding{};
        return reservations::capture_binding(f.s, f.accounting(), key, binding) &&
               fixture::capture(static_cast<fixture *>(&f), key, out);
    }
    backend booked_live() {
        auto b           = live();
        b.context        = this;
        b.read_actor     = booked_actor;
        b.capture_active = booked_capture;
        return b;
    }
    refusal append() {
        const edit edit{w, a(1), edit_kind::append, move(7, 9, 1)};
        int32_t    words[13]{};
        if (!encode_edit(edit, words)) return refusal::malformed;
        mh_order_tasks_request request{};
        mh_order_tasks_result  result{};
        request.operation = uint32_t(host_protocol::operation::due_edit);
        request.player    = edit.actor.player;
        request.slot      = edit.actor.slot;
        request.args[1] = request.args[2] = COMMAND;
        for (uint32_t i = 0; i < 13; ++i) request.args[i + 3] = uint32_t(words[i]);
        host_protocol::dispatch_context context{&s, booked_live(), this, identity, nullptr, accounting()};
        return host_protocol::dispatch(context, request, result) ? refusal(result.values[1]) : refusal::malformed;
    }
};
void legacy_primary_booking_import(checks &check) {
    for (const uint32_t bits : {0x8000u, 0x8001u}) {
        legacy_booking_fixture f;
        f.actual.owned_bits      = bits; // Both exact real legacy witnesses must remain distinct.
        f.captures[1].count      = 1;
        f.captures[1].nodes[0]   = f.move(2, 20, 0);
        f.captures[1].status     = phase::awaiting_application;
        f.captures[1].position   = f.actors[1].position;
        f.captures[1].provenance = native_provenance::active_move;
        check("legacy actual booking without metadata refuses before import", f.append() == refusal::stale_actor);
        const auto          accounting = f.accounting();
        primary_reservation imported{};
        check("legacy publication imports exact nonowning current-life booking without native writes",
              reservations::import_legacy_bindings(f.s, accounting) && f.s.read_reservation(f.a(1), imported) &&
                  imported.phase == reservation_phase::bound && imported.execution_epoch == 0 &&
                  imported.target.object.incarnation == 1 && imported.delta_raw16 == 4 &&
                  imported.prior_packed == (4u | (bits << 16)) &&
                  f.actual.cached_raw16 == 4 && f.actual.owned_bits == bits &&
                  f.tally == 23 && f.adjustments == 0 && f.native_writes == 0 && f.target_clears == 0);
        state validated;
        check("legacy imported booking passes complete canonical validation before publication",
              validated.bind(f.raw.data(), uint32_t(f.raw.size())) &&
                  validated.validate_lifetimes(static_cast<fixture *>(&f), fixture::lifetime));
        check("real due-edit acquisition preserves pending ordinary head and appends suffix",
              f.append() == refusal::none && f.view(1).count == 2 && f.view(1).externally_pending);
        primary_reservation acquired{};
        check("first append adopts existing booking at exact acquired epoch without double addition",
              f.s.read_reservation(f.a(1), acquired) && acquired.execution_epoch == f.s.execution_epoch(f.a(1)) &&
                  acquired.execution_epoch != 0 && f.tally == 23 && f.adjustments == 0);
        const auto modern = f.raw;
        check("legacy-only helper refuses modern metadata atomically", !reservations::import_legacy_bindings(f.s, accounting) && f.raw == modern);
        check("subsequent managed cleanup subtracts already-counted native booking exactly once",
              reservations::release_before(f.s, accounting, f.a(1)) &&
                  f.tally == 19 && f.adjustments == 1 && f.native_writes == 1 &&
                  f.actual.cached_raw16 == 0 && f.actual.owned_bits == 0);
        reservations::release_after(f.s, accounting, f.a(1));
        check("repeated cleanup cannot subtract legacy booking twice",
              !reservations::release_before(f.s, accounting, f.a(1)) && f.tally == 19 && f.adjustments == 1);
    }
    legacy_booking_fixture stale;
    stale.building_life[5] = 0;
    primary_reservation absent{};
    check("unresolvable legacy target stays unowned without rejecting world or inventing a life",
          reservations::import_legacy_bindings(stale.s, stale.accounting()) &&
              !stale.s.read_reservation(stale.a(1), absent) && stale.append() == refusal::stale_actor &&
              stale.tally == 23 && stale.adjustments == 0 && stale.native_writes == 0);
}
void put32(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i != 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
uint32_t test_crc(const uint8_t *p, uint32_t n) {
    uint32_t c = ~0u;
    for (uint32_t i = 0; i != n; ++i) {
        c ^= p[i];
        for (uint32_t j = 0; j != 8; ++j) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
void repair_crc(std::vector<uint8_t> &encoded) {
    put32(encoded.data() + 12, test_crc(encoded.data() + ENVELOPE_BYTES, uint32_t(encoded.size()) - ENVELOPE_BYTES));
}
void independent_progress_and_attempts(checks &check) {
    fixture f;
    f.actors[1].family = executor::direction_plane;
    for (uint32_t i = 0; i != 2; ++i) {
        check("both actors accept same issued group, distinct per-unit ownership",
              f.edit_task(i, edit_kind::replace, f.move(4, 4, 1, action::move, 77)) == refusal::none &&
                  f.edit_task(i, edit_kind::append, f.move(8, 8, 2, action::move, 78)) == refusal::none);
    }
    f.s.tick(f.live());
    check("one release per actor, group and sequence survive", f.issued == 2 &&
                                                                   f.calls[0].task.group == 77 && f.calls[0].task.command_sequence == 1);
    f.assign(0, {3, 4}, assignment_reason::formation);
    const auto old_ground = f.event(0, outcome_kind::reached_near, {3, 4}, {3, 4}, assignment_reason::valid_projection, 9);
    f.s.observe(f.live(), f.event(0, outcome_kind::planning_started));
    const auto renewed = f.view(0).active;
    f.assign(0, {3, 4}, assignment_reason::formation);
    f.actors[0].position = {3, 4}; // All positive-arrival facts match except the old attempt.
    f.s.observe(f.live(), old_ground);
    check("same-leg old planner ticket cannot finish replacement assignment",
          f.view(0).status == phase::executing && renewed.attempt != old_ground.owner.attempt);
    const auto old_plane = f.event(1, outcome_kind::assigned, {}, {4, 4}, assignment_reason::requested, 9);
    f.s.observe(f.live(), f.event(1, outcome_kind::preparation_started));
    f.s.observe(f.live(), old_plane);
    check("air preparation is not assignment or arrival", f.view(1).status == phase::preparation && !f.view(1).assignment_valid);
    check("ground formation endpoint completes its own head", f.reach(0, {3, 4}, outcome_kind::reached_near) == control::hold);
    f.s.tick(f.live());
    check("no group barrier or idle-as-success", f.issued == 3 && f.view(0).active.leg == 2 && f.view(1).active.leg == 1 &&
                                                     f.view(1).status == phase::preparation);
    f.s.observe(f.live(), f.event(1, outcome_kind::preparation_finished));
    check("settled projected air endpoint visits finish without motionless condition",
          f.arrive(1, {4, 5}, assignment_reason::valid_projection) == control::hold);
    f.s.tick(f.live());
    check("air next leg releases once at next pass", f.issued == 4 && f.view(1).active.leg == 2);
    f.s.observe(f.live(), old_ground);
    check("old leg cannot pop new head", f.view(0).count == 1 && f.view(0).active.leg == 2);
    check("last finite arrival has no extra handler hold", f.arrive(0, {8, 8}) == control::unchanged &&
                                                               f.arrive(1, {8, 8}) == control::unchanged && f.s.handler_control(f.live(), f.a()) == control::unchanged);
    route_view none{};
    f.s.tick(f.live());
    f.s.tick(f.live());
    check("finite final heads are gone exactly once", !f.s.read_route(f.a(), none) && !f.s.read_route(f.a(1), none) && f.issued == 4);
    fixture zero;
    zero.edit_task(0, edit_kind::replace, zero.move(1, 1, 1));
    zero.s.tick(zero.live());
    check("validated zero-length spatial assignment completes once without idle inference",
          zero.arrive(0, {1, 1}, assignment_reason::requested, 0xff) == control::unchanged);
    zero.s.tick(zero.live());
    check("zero-length task is not reissued", zero.issued == 1 && !zero.s.read_route(zero.a(), none));
    fixture near;
    near.edit_task(0, edit_kind::replace, near.move(8, 8, 1));
    near.edit_task(0, edit_kind::append, near.move(12, 12, 2));
    near.s.tick(near.live());
    near.actors[0].position = {7, 8};
    auto accepted           = near.event(0, outcome_kind::reached_near, {7, 8}, {7, 8}, assignment_reason::valid_projection, 9);
    check("positive near witness cannot invent a missing successful assignment",
          near.s.observe(near.live(), accepted) == control::unchanged && near.view().status == phase::executing &&
              !near.view().assignment_valid && near.view().count == 2);
    near.assign(0, {8, 8});
    const auto assigned_owner = near.view();
    auto       rejected       = accepted;
    rejected.path_slot        = 10;
    near.s.observe(near.live(), rejected);
    rejected           = accepted;
    rejected.placement = assignment_reason::none;
    near.s.observe(near.live(), rejected);
    rejected          = accepted;
    rejected.assigned = {6, 8};
    near.s.observe(near.live(), rejected);
    check("near completion needs saved path ownership and positive consistent placement",
          near.view().status == phase::executing && near.view().count == 2 && near.view().assigned.x == 8);
    check("actual accepted near tile finishes the same requested-placement attempt",
          near.s.observe(near.live(), accepted) == control::hold && near.view().status == phase::ready_next &&
              near.view().active.attempt == assigned_owner.active.attempt &&
              near.view().execution_epoch == assigned_owner.execution_epoch);
    near.s.tick(near.live());
    near.assign(0, {12, 12});
    near.s.observe(near.live(), accepted); // New head is assigned: only the stale ticket rejects.
    check("accepted near completion releases its successor once and rejects stale duplicate",
          near.issued == 2 && near.view().count == 1 && near.view().active.leg == 2);
    fixture receipt;
    receipt.edit_task(0, edit_kind::replace, receipt.move(8, 8, 1));
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::planning_succeeded));
    check("planner receipt cannot precede actual application", !receipt.view().planner_succeeded);
    receipt.s.tick(receipt.live());
    const auto solved = receipt.event(0, outcome_kind::planning_succeeded);
    check("positive planner receipt alone is not assignment, completion or another activation",
          receipt.s.observe(receipt.live(), solved) == control::unchanged &&
              receipt.view().planner_succeeded && !receipt.view().assignment_valid &&
              receipt.view().status == phase::executing && receipt.view().count == 1 && receipt.issued == 1);
    receipt.edit_task(0, edit_kind::append, receipt.move(12, 12, 2));
    check("tail append preserves the active planner receipt and execution identity",
          receipt.view().planner_succeeded && receipt.view().count == 2 &&
              receipt.view().active.attempt == solved.owner.attempt && receipt.view().active.route == solved.owner.route &&
              receipt.view().execution_epoch == 1 && receipt.issued == 1);
    std::vector<uint8_t> receipt_save(receipt.s.encoded_bytes());
    fixture              restored_receipt;
    check("successful planner receipt persists without inventing an endpoint",
          receipt.s.encode(receipt_save.data(), uint32_t(receipt_save.size())) &&
              restored_receipt.s.import(receipt_save.data(), uint32_t(receipt_save.size())) &&
              restored_receipt.view().planner_succeeded && !restored_receipt.view().assignment_valid);
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::planning_started));
    receipt.s.observe(receipt.live(), solved);
    check("new attempt clears receipt and rejects prior attempt success",
          !receipt.view().planner_succeeded && receipt.view().active.attempt != solved.owner.attempt);
    const auto replanned = receipt.event(0, outcome_kind::planning_succeeded);
    receipt.s.observe(receipt.live(), replanned);
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::preparation_started));
    receipt.s.observe(receipt.live(), replanned);
    check("preparation invalidates the previous planner receipt", receipt.view().status == phase::preparation && !receipt.view().planner_succeeded);
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::planning_succeeded));
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::preparation_finished));
    check("physical success during preparation does not finish its task",
          receipt.view().planner_succeeded && receipt.view().count == 2 && receipt.issued == 1);
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::planning_started));
    receipt.assign(0, {8, 8});
    check("actual successful assignment also records planner success",
          receipt.view().assignment_valid && receipt.view().planner_succeeded);
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::pool_exhausted));
    receipt.s.observe(receipt.live(), receipt.event(0, outcome_kind::planning_succeeded));
    check("negative planner outcome clears receipt and cannot heal from a late success",
          receipt.view().status == phase::blocked && !receipt.view().planner_succeeded && receipt.view().count == 2);
}
void captures_capacity_and_replacement(checks &check) {
    fixture    f;
    const auto pending = f.move(4, 4, 0, action::move, 123);
    route_view unowned{};
    check("ordinary native Move stays unmanaged without a native issue",
          f.s.adopt_applied(f.live(), f.a(), pending) == refusal::none &&
              !f.s.read_route(f.a(), unowned) && f.issued == 0);
    auto &c      = f.captures[0];
    c.count      = 1;
    c.nodes[0]   = pending;
    c.status     = phase::awaiting_application;
    c.provenance = native_provenance::active_move;
    c.position   = {1, 1};
    check("first append captures earlier retained primary", f.edit_task(0, edit_kind::append, f.move(8, 8, 10)) == refusal::none &&
                                                                f.view().count == 2 && f.view().externally_pending);
    const uint32_t generation     = f.view().active.route;
    const ticket   captured_owner = f.view().active;
    auto           stale_owner    = captured_owner;
    ++stale_owner.route;
    check("external pending permits native predecessor to reach actual application",
          f.s.handler_control(f.live(), f.a()) == control::unchanged);
    const auto before_stale = f.raw;
    check("stale QOEX owner refuses without cancelling current tail",
          f.s.adopt_applied(f.live(), f.a(), pending, &stale_owner) == refusal::stale_ticket &&
              f.raw == before_stale && !f.s.owns_ticket(stale_owner) && f.s.owns_ticket(captured_owner));
    f.s.tick(f.live());
    f.s.tick(f.live());
    check("retained native primary is never issued twice", f.issued == 0);
    c.status           = phase::executing;
    c.assigned         = {3, 4};
    c.assignment_valid = true;
    c.placement        = assignment_reason::formation;
    c.path_slot        = 9;
    check("actual QOEX F adoption retains seq0 tail and generation", f.s.adopt_applied(f.live(), f.a(), pending, &captured_owner) == refusal::none &&
                                                                         !f.view().externally_pending && f.view().count == 2 && f.view().active.route == generation);
    f.reach(0, {3, 4}, outcome_kind::reached_near);
    f.s.tick(f.live());
    check("captured head completion releases suffix once", f.issued == 1 && f.calls[0].task.requested.x == 8);
    check("ordinary replacement cancels ownership without acquiring native activity",
          f.s.adopt_applied(f.live(), f.a(), f.move(12, 12, 11)) == refusal::none);
    route_view out{};
    f.s.tick(f.live());
    check("ordinary replacement does not become an engine task", !f.s.read_route(f.a(), out) && f.issued == 1);

    fixture legacy;
    auto   &lc          = legacy.captures[0];
    lc.count            = 2;
    lc.nodes[0]         = legacy.move(6, 6, 0, action::patrol);
    lc.nodes[1]         = legacy.move(1, 1, 0, action::patrol);
    lc.status           = phase::executing;
    lc.provenance       = native_provenance::genuine_patrol;
    lc.position         = {2, 2};
    lc.assigned         = {6, 6};
    lc.assignment_valid = true;
    lc.placement        = assignment_reason::requested;
    lc.path_slot        = 9;
    check("legacy patrol keeps both actual endpoints", legacy.edit_task(0, edit_kind::append, legacy.move(9, 9, 1, action::patrol)) == refusal::none &&
                                                           legacy.view().count == 3 && legacy.view().cycle_begin == 0 && !legacy.view().has_implicit_anchor);
    const auto before_tail = legacy.raw;
    check("finite after infinite tail is atomic refusal", legacy.edit_task(0, edit_kind::append, legacy.move(12, 12, 2)) == refusal::after_patrol && legacy.raw == before_tail);
    fixture pending_patrol;
    auto   &pc    = pending_patrol.captures[0];
    pc.count      = 1;
    pc.nodes[0]   = pending_patrol.move(4, 4, 0, action::patrol);
    pc.position   = {1, 1};
    pc.status     = phase::awaiting_application;
    pc.provenance = native_provenance::genuine_patrol;
    check("pending new patrol does not invent pre-F home", pending_patrol.edit_task(0, edit_kind::append,
                                                                                    pending_patrol.move(7, 7, 8, action::patrol)) == refusal::none);
    const ticket pending_patrol_owner = pending_patrol.view().active;
    pc.count                          = 2;
    pc.nodes[1]                       = pending_patrol.move(1, 1, 0, action::patrol);
    pc.status                         = phase::executing;
    pc.assigned                       = {4, 4};
    pc.assignment_valid               = true;
    pc.placement                      = assignment_reason::requested;
    pc.path_slot                      = 9;
    node_view second{};
    check("pending patrol adoption never inserts new native home", pending_patrol.s.adopt_applied(pending_patrol.live(), pending_patrol.a(), pc.nodes[0], &pending_patrol_owner) == refusal::none &&
                                                                       pending_patrol.s.read_entry(pending_patrol.a(), 1, second) && second.task.requested.x == 7 && !pending_patrol.view().has_implicit_anchor);

    fixture cap;
    cap.capture_ok[0]   = false;
    const auto pristine = cap.raw;
    check("ambiguous capture leaves native/route untouched", cap.edit_task(0, edit_kind::append, cap.move(3, 3, 1)) == refusal::unsupported && cap.raw == pristine && cap.issued == 0);
    check("replacement does not need ambiguous native capture", cap.edit_task(0, edit_kind::replace, cap.move(3, 3, 1)) == refusal::none);
    bool all = true;
    for (uint32_t i = 2; i <= CAPACITY; ++i) all &= cap.edit_task(0, edit_kind::append, cap.move(i % 31, 2, i)) == refusal::none;
    const auto full = cap.raw;
    check("active head included in32-node capacity", all && cap.view().count == CAPACITY &&
                                                         cap.edit_task(0, edit_kind::append, cap.move(7, 7, 33)) == refusal::full && cap.raw == full);
    cap.s.tick(cap.live());
    cap.assign(0, {3, 3});
    const auto stale = cap.event(0, outcome_kind::reached_exact, {3, 3}, {}, assignment_reason::none, 9);
    cap.s.observe(cap.live(), cap.event(0, outcome_kind::pool_exhausted));
    cap.s.tick(cap.live());
    cap.s.tick(cap.live());
    cap.s.observe(cap.live(), stale);
    check("blocked head and all31 tail entries retained, no retry loop", cap.view().status == phase::blocked &&
                                                                             cap.view().blocked_reason == failure::path_pool && cap.view().count == 32 && cap.issued == 1);
    cap.edit_task(0, edit_kind::replace, cap.move(5, 5, 34));
    cap.s.tick(cap.live());
    cap.s.observe(cap.live(), stale);
    check("replace generation rejects stale reached ticket", cap.view().status == phase::executing && cap.view().count == 1 && cap.view().active.route != stale.owner.route);
    fixture deferred;
    deferred.actors[0].safe_to_apply = false;
    deferred.edit_task(0, edit_kind::replace, deferred.move(7, 7, 1));
    deferred.s.tick(deferred.live());
    check("unsafe deferred replacement lets old native state reach a safe boundary",
          deferred.issued == 0 && deferred.s.handler_control(deferred.live(), deferred.a()) == control::unchanged);
    deferred.actors[0].position      = {2, 1};
    deferred.actors[0].safe_to_apply = true;
    check("first safe handler return holds before old executor can start another leg",
          deferred.s.handler_control(deferred.live(), deferred.a()) == control::hold);
    deferred.s.tick(deferred.live());
    check("next pre-unit pass commits deferred replacement once then permits execution",
          deferred.issued == 1 && deferred.view().status == phase::executing &&
              deferred.s.handler_control(deferred.live(), deferred.a()) == control::unchanged);
}
void attack_retirement_and_cleanup(checks &check) {
    fixture    f;
    const auto attack = f.attack(3, 2); // Real building target outside any storage eligibility.
    f.edit_task(0, edit_kind::replace, f.move(3, 3, 1));
    f.edit_task(0, edit_kind::append, attack);
    f.edit_task(0, edit_kind::append, f.move(8, 8, 3));
    f.s.tick(f.live());
    f.s.retire_target(f.live(), attack.target, true);
    ++f.building_life[7];
    node_view target{};
    f.s.read_entry(f.a(), 1, target);
    check("target retirement tagged before reuse, no tombstone needed", target.target == objective::destroyed_confirmed &&
                                                                            f.s.validate_lifetimes(&f, fixture::lifetime));
    f.arrive(0, {3, 3});
    f.s.tick(f.live());
    check("future already-destroyed attack is consumed without hitting replacement", f.issued == 1 && f.view().status == phase::ready_next);
    f.s.tick(f.live());
    check("already-satisfied objective releases following move exactly once", f.issued == 2 && f.calls[1].task.kind == action::move && f.view().count == 1);
    fixture live;
    live.cleanup_ready          = false;
    const auto objective_attack = live.attack(2, 1);
    live.edit_task(0, edit_kind::replace, objective_attack);
    live.edit_task(0, edit_kind::append, live.move(9, 9, 2));
    live.s.tick(live.live());
    live.s.retire_target(live.live(), objective_attack.target, true);
    live.s.tick(live.live());
    check("destroyed explicit attack waits native target cleanup", live.view().count == 2 && live.issued == 1 && live.cleaned == 0);
    live.cleanup_ready = true;
    live.s.tick(live.live());
    live.s.tick(live.live());
    live.s.tick(live.live());
    check("cleanup commits once and no hidden home leg", live.cleaned == 1 && live.issued == 2 &&
                                                             live.calls[1].task.kind == action::move && !live.calls[1].return_to_anchor && live.calls[1].task.requested.x == 9);
    fixture    unavailable;
    const auto invalid = unavailable.attack(1, 1);
    unavailable.edit_task(0, edit_kind::replace, invalid);
    unavailable.edit_task(0, edit_kind::append, unavailable.move(8, 8, 2));
    unavailable.s.retire_target(unavailable.live(), invalid.target, false);
    ++unavailable.building_life[5];
    unavailable.s.tick(unavailable.live());
    unavailable.s.read_entry(unavailable.a(), 0, target);
    check("unconfirmed retirement parks attack and tail, never succeeds", target.target == objective::unavailable &&
                                                                              unavailable.view().status == phase::blocked && unavailable.view().count == 2 && unavailable.issued == 0);
    fixture    ended_before_apply;
    const auto ended = ended_before_apply.attack(0, 1);
    ended_before_apply.edit_task(0, edit_kind::replace, ended);
    ended_before_apply.s.retire_target(ended_before_apply.live(), ended.target, true);
    ended_before_apply.actors[0].safe_to_apply = false;
    ended_before_apply.cleanup_ready           = true;
    ended_before_apply.s.tick(ended_before_apply.live());
    check("satisfied replacement cannot forget its unsafe old native predecessor",
          ended_before_apply.view().count == 1 && ended_before_apply.cleaned == 0 && ended_before_apply.issued == 0);
    ended_before_apply.actors[0].safe_to_apply = true;
    ended_before_apply.s.tick(ended_before_apply.live());
    route_view none{};
    check("satisfied replacement settles old predecessor without dead-target F or home",
          ended_before_apply.cleaned == 1 && ended_before_apply.issued == 0 &&
              !ended_before_apply.s.read_route(ended_before_apply.a(), none));
    fixture pending;
    auto   &pc    = pending.captures[0];
    pc.count      = 1;
    pc.nodes[0]   = pending.attack(3, 0);
    pc.status     = phase::awaiting_application;
    pc.provenance = native_provenance::active_attack;
    pc.position   = {1, 1};
    pending.edit_task(0, edit_kind::append, pending.move(9, 9, 1));
    pending.s.retire_target(pending.live(), pc.nodes[0].target, true);
    pending.s.tick(pending.live());
    check("dead pending target alone cannot bypass a retained native command",
          pending.view().externally_pending && pending.view().count == 2 && pending.issued == 0);
    check("pending destroyed target fails shared pre-F objective guard",
          !pending.s.pending_application_allowed(pending.live(), pending.view().active) && pending.issued == 0);
    pending.s.observe(pending.live(), pending.event(0, outcome_kind::rejected));
    pending.s.tick(pending.live());
    check("actual native discard releases satisfied pending head exactly once",
          pending.issued == 1 && pending.view().count == 1 && pending.calls[0].task.requested.x == 9);
    fixture observed;
    auto   &active    = observed.captures[0];
    active.count      = 1;
    active.nodes[0]   = observed.attack(2, 0);
    active.status     = phase::executing;
    active.provenance = native_provenance::active_attack;
    active.position   = {1, 1};
    check("first append adopts actual compatible firing activity without guessing original issuer",
          observed.edit_task(0, edit_kind::append, observed.move(9, 9, 1)) == refusal::none &&
              observed.view().count == 2 && observed.view().status == phase::executing);
    observed.s.tick(observed.live());
    check("captured firing head is not applied again", observed.issued == 0);
    fixture                   predecessor;
    const target_key          old_primary{object_kind::building, {1, 2, 1}};
    const primary_reservation old_bound{reservation_phase::bound, old_primary, 7, {}, 0x80010007u, 0};
    predecessor.reserved_tally = 7;
    predecessor.s.write_reservation(predecessor.a(), old_bound);
    predecessor.actors[0].safe_to_apply = false;
    predecessor.edit_task(0, edit_kind::replace, predecessor.move(7, 7, 1));
    primary_reservation retained{};
    check("replacement retains known old primary even though new head is movement",
          predecessor.s.read_reservation(predecessor.a(), retained) && retained.target.object.incarnation == 1);
    check("old managed predecessor detaches before target slot reuse",
          predecessor.s.retire_target(predecessor.live(), old_primary, true) &&
              predecessor.detached == 1 && predecessor.reserved_tally == 0 && predecessor.s.primary_cleanup_pending(predecessor.a()));
    ++predecessor.building_life[6];
    predecessor.reserved_tally = 100;
    bool may_access            = true;
    check("unsafe detached predecessor skips target reads but does not freeze interpolation",
          predecessor.s.guard_primary(predecessor.live(), predecessor.a(), may_access) == control::unchanged && !may_access &&
              predecessor.s.handler_control(predecessor.live(), predecessor.a()) == control::unchanged);
    predecessor.actors[0].position      = {2, 1};
    predecessor.actors[0].safe_to_apply = true;
    check("first safe boundary cleans once and holds without charging reused victim",
          predecessor.s.guard_primary(predecessor.live(), predecessor.a(), may_access) == control::hold &&
              !predecessor.s.primary_cleanup_pending(predecessor.a()) && predecessor.cleaned == 1 &&
              predecessor.reserved_tally == 100 && predecessor.actors[0].position.x == 2);
    predecessor.s.tick(predecessor.live());
    check("new movement commits once after predecessor cleanup", predecessor.issued == 1 && predecessor.view().count == 1);
}
void patrol_excursion_and_zero_progress(checks &check) {
    fixture f;
    f.actors[0].heading = 3;
    const auto pristine = f.raw;
    check("one coincident patrol point refuses before mutation", f.edit_task(0, edit_kind::replace, f.move(1, 1, 1, action::patrol)) == refusal::degenerate_cycle && f.raw == pristine);
    f.edit_task(0, edit_kind::replace, f.move(4, 4, 1, action::patrol));
    f.s.tick(f.live());
    const uint32_t   leg = f.view().active.leg;
    const target_key observed{object_kind::building, {1, 3, 1}};
    const uint32_t   active_epoch = f.view().execution_epoch;
    check("QOPA shared guard accepts actual compatible patrol target and current epoch",
          active_epoch && f.s.validate_excursion_application(f.live(), f.a(), active_epoch, observed) == refusal::none);
    ++f.building_life[7];
    check("self-contained QOPA target lifetime refuses reused target slot before F",
          f.s.validate_excursion_application(f.live(), f.a(), active_epoch, observed) == refusal::unavailable_target);
    check("one-point patrol saves activation anchor outside capacity", f.view().count == 1 && f.view().has_implicit_anchor && f.view().implicit_anchor.x == 1);
    f.arrive(0, {4, 4});
    f.s.tick(f.live());
    check("second half retains explicit leg identity", f.view().implicit_leg_active && f.view().active.leg == leg && f.calls[1].task.requested.x == 1);
    f.actors[0].position            = {2, 2};
    const uint32_t before_excursion = f.view().execution_epoch;
    auto           pursuit          = f.event(0, outcome_kind::primary_excursion_started, {2, 2});
    pursuit.target                  = {object_kind::building, {1, 3, f.building_life[7]}};
    f.s.observe(f.live(), pursuit);
    const auto interrupted = f.view();
    f.s.tick(f.live());
    check("primary pursuit suspends and invalidates prior execution epoch without consuming patrol",
          interrupted.status == phase::excursion && interrupted.excursion_anchor.x == 2 && f.issued == 2 &&
              interrupted.execution_epoch == before_excursion + 1 &&
              f.s.validate_excursion_application(f.live(), f.a(), before_excursion, observed) == refusal::stale_ticket);
    f.s.observe(f.live(), f.event(0, outcome_kind::primary_excursion_ended));
    check("canonical barrier holds before safe anchor-return application", f.s.handler_control(f.live(), f.a()) == control::hold);
    f.s.tick(f.live());
    check("applied anchor-return keeps native travel handler running", f.s.handler_control(f.live(), f.a()) == control::unchanged);
    check("pursuit termination releases fixed anchor return", f.calls[2].return_to_anchor && f.calls[2].anchor.x == 2 && f.view().active.leg == leg);
    f.arrive(0, {2, 2});
    f.s.tick(f.live());
    check("anchor reached resumes same unconsumed patrol half", f.issued == 4 && !f.calls[3].return_to_anchor &&
                                                                    f.calls[3].task.requested.x == 1 && f.view().implicit_anchor.x == 1 && f.view().active.leg == leg);
    state returned_view;
    check("finite patrol after a nonzero-heading excursion remains saveable",
          returned_view.bind(f.raw.data(), uint32_t(f.raw.size())));
    f.arrive(0, {1, 1});
    f.s.tick(f.live());
    check("completed real circuit repeats without draining", f.view().count == 1 && !f.view().implicit_leg_active && f.issued == 5);
    const auto     current       = f.view().active;
    const uint32_t before_append = f.view().execution_epoch;
    check("second explicit patrol removes implicit circuit point without reversing current leg",
          f.edit_task(0, edit_kind::append, f.move(7, 7, 2, action::patrol)) == refusal::none && f.view().count == 2 &&
              !f.view().has_implicit_anchor && f.view().active.attempt == current.attempt && f.view().execution_epoch == before_append);
    fixture collapsed;
    collapsed.edit_task(0, edit_kind::replace, collapsed.move(4, 4, 1, action::patrol));
    collapsed.edit_task(0, edit_kind::append, collapsed.move(7, 7, 2, action::patrol));
    collapsed.s.tick(collapsed.live());
    collapsed.arrive(0, {1, 1}, assignment_reason::valid_projection);
    collapsed.s.tick(collapsed.live());
    collapsed.arrive(0, {1, 1}, assignment_reason::valid_projection);
    collapsed.s.tick(collapsed.live());
    collapsed.s.tick(collapsed.live());
    check("distinct requests collapsing to one assigned point block after one bounded circuit",
          collapsed.view().status == phase::blocked && collapsed.view().blocked_reason == failure::zero_progress_cycle &&
              collapsed.view().count == 2 && collapsed.issued == 2);
    fixture ended;
    ended.edit_task(0, edit_kind::replace, ended.move(4, 4, 1, action::patrol));
    ended.s.tick(ended.live());
    auto started   = ended.event(0, outcome_kind::primary_excursion_started, {1, 1});
    started.target = {object_kind::building, {1, 3, 1}};
    ended.s.observe(ended.live(), started);
    ended.actors[0].safe_to_apply = false;
    check("excursion lifetime retirement is canonical before reuse",
          ended.s.retire_target(ended.live(), started.target, true));
    ++ended.building_life[7];
    bool access = true;
    ended.s.guard_primary(ended.live(), ended.a(), access);
    check("retired autonomous victim cannot become reused-slot pursuit",
          !access && ended.s.excursion_target_status(ended.live(), ended.a()) == target_status::destroyed_confirmed &&
              ended.s.validate_lifetimes(&ended, fixture::lifetime));
    ended.actors[0].safe_to_apply = true;
    ended.s.guard_primary(ended.live(), ended.a(), access);
    check("retired pursuit settles to saved anchor without consuming patrol",
          ended.view().status == phase::returning_anchor && ended.view().count == 1 && ended.view().excursion_anchor.x == 1);
}
void codecs_wire_and_campaign(checks &check) {
    fixture f;
    f.edit_task(0, edit_kind::replace, f.move(4, 4, 99));
    f.s.tick(f.live());
    f.assign(0, {3, 4}, assignment_reason::formation);
    std::vector<uint8_t> encoded(f.s.encoded_bytes());
    check("live assigned state encodes", f.s.encode(encoded.data(), uint32_t(encoded.size())));
    fixture restored;
    check("envelope restores exact canonical bytes and highwater", restored.s.import(encoded.data(), uint32_t(encoded.size())) &&
                                                                       restored.raw == f.raw && restored.s.command_high_water(0) == 99);
    const auto original = restored.raw;
    encoded.back() ^= 128;
    check("CRC corruption rejects without publishing one byte", !restored.s.import(encoded.data(), uint32_t(encoded.size())) && restored.raw == original);
    encoded.back() ^= 128;
    auto malformed = encoded;
    put32(malformed.data() + ENVELOPE_BYTES + HEADER_BYTES + 24, 33);
    repair_crc(malformed);
    check("valid CRC cannot bless out-of-range ring metadata", !restored.s.import(malformed.data(), uint32_t(malformed.size())) && restored.raw == original);
    malformed                                                                = encoded;
    malformed[ENVELOPE_BYTES + HEADER_BYTES + ACTOR_BYTES + NODE_BYTES + 44] = 1;
    repair_crc(malformed);
    check("nonzero unused-node reserved byte is semantic refusal", !restored.s.import(malformed.data(), uint32_t(malformed.size())) && restored.raw == original);
    malformed = encoded;
    put32(malformed.data() + ENVELOPE_BYTES + HEADER_BYTES + 64, 16);
    repair_crc(malformed);
    check("valid CRC cannot import successful assignment without its planner receipt",
          !restored.s.import(malformed.data(), uint32_t(malformed.size())) && restored.raw == original);
    check("wrong world, shape and truncation refuse atomically", !state::validate(encoded.data(), uint32_t(encoded.size()), f.d, {7, 5, 101}) &&
                                                                     !state::validate(encoded.data(), uint32_t(encoded.size()), {2, 2, 32, 32, 5}, f.w) &&
                                                                     !restored.s.import(encoded.data(), uint32_t(encoded.size() - 1)) && restored.raw == original);
    constexpr uint32_t   old_header = 96, old_actor = 192, old_row = old_actor + CAPACITY * NODE_BYTES;
    const uint32_t       old_bytes = old_header + f.d.players * f.d.units * old_row;
    std::vector<uint8_t> legacy(ENVELOPE_BYTES + old_bytes);
    std::memcpy(legacy.data(), encoded.data(), ENVELOPE_BYTES);
    auto *prior_region = legacy.data() + ENVELOPE_BYTES;
    std::memcpy(prior_region, f.raw.data(), 76);
    put32(legacy.data() + 4, LEGACY_SCHEMA);
    put32(legacy.data() + 8, old_bytes);
    put32(prior_region + 4, LEGACY_SCHEMA);
    put32(prior_region + 36, old_bytes);
    for (uint32_t i = 0; i < f.d.players * f.d.units; ++i) {
        const auto *current = f.raw.data() + HEADER_BYTES + i * ROW_BYTES;
        auto       *prior   = prior_region + old_header + i * old_row;
        std::memcpy(prior, current, 176);
        std::memcpy(prior + old_actor, current + ACTOR_BYTES, CAPACITY * NODE_BYTES);
    }
    repair_crc(legacy);
    check("schema1 assigned save restores queue identity and accounting without epoch changes",
          restored.s.import(legacy.data(), uint32_t(legacy.size())) && restored.raw == original);
    legacy[ENVELOPE_BYTES + old_header + 176] = 1;
    repair_crc(legacy);
    check("old reserved bytes cannot become invented schema2 history",
          !restored.s.import(legacy.data(), uint32_t(legacy.size())) && restored.raw == original);
    fixture patrol, resumed;
    patrol.edit_task(0, edit_kind::replace, patrol.move(4, 4, 1, action::patrol));
    patrol.s.tick(patrol.live());
    std::vector<uint8_t> patrol_encoded(patrol.s.encoded_bytes());
    check("active one-point patrol serializes its fixed implicit anchor",
          patrol.s.encode(patrol_encoded.data(), uint32_t(patrol_encoded.size())) &&
              resumed.s.import(patrol_encoded.data(), uint32_t(patrol_encoded.size())));
    const auto resumed_before = resumed.raw;
    auto       anchorless     = patrol_encoded;
    auto      *bad_row        = anchorless.data() + ENVELOPE_BYTES + HEADER_BYTES;
    put32(bad_row + 64, 16); // APPLIED but neither HAS_IMPLICIT nor TRACKING.
    put32(bad_row + 56, 0);
    put32(bad_row + 60, 0);
    put32(bad_row + 76, 0);
    put32(bad_row + 80, 0);
    put32(bad_row + 84, 0);
    put32(bad_row + 88, 0);
    put32(bad_row + 100, 0);
    repair_crc(anchorless);
    check("valid CRC cannot import activated patrol without canonical anchor/circuit",
          !resumed.s.import(anchorless.data(), uint32_t(anchorless.size())) && resumed.raw == resumed_before);
    resumed.arrive(0, {4, 4});
    resumed.s.tick(resumed.live());
    check("accepted saved patrol continues to saved anchor, never fabricated origin",
          resumed.issued == 1 && resumed.view().implicit_leg_active &&
              resumed.calls[0].task.requested.x == 1 && resumed.calls[0].task.requested.y == 1);
    fixture exhausted, cancelled;
    exhausted.edit_task(0, edit_kind::replace, exhausted.move(4, 4, 1));
    exhausted.s.tick(exhausted.live());
    put32(exhausted.raw.data() + HEADER_BYTES + 104, UINT32_MAX);
    check("epoch exhaustion fixture binds valid canonical active row",
          exhausted.s.bind(exhausted.raw.data(), uint32_t(exhausted.raw.size())));
    const auto exhausted_before = exhausted.raw;
    check("same-life replacement refuses exhausted epoch before any mutation",
          exhausted.edit_task(0, edit_kind::replace, exhausted.move(7, 7, 2)) == refusal::exhausted_identity &&
              exhausted.raw == exhausted_before);
    exhausted.s.observe(exhausted.live(), exhausted.event(0, outcome_kind::planning_started));
    check("runtime attempt cannot wrap execution epoch",
          exhausted.view().status == phase::blocked && exhausted.view().blocked_reason == failure::exhausted_identity &&
              exhausted.view().execution_epoch == UINT32_MAX && exhausted.view().active.attempt == 1);
    exhausted.s.cancel_actor(exhausted.live(), exhausted.a());
    std::vector<uint8_t> cancelled_encoded(exhausted.s.encoded_bytes());
    check("cancel at max epoch succeeds and inactive highwater roundtrips",
          exhausted.s.encode(cancelled_encoded.data(), uint32_t(cancelled_encoded.size())) &&
              cancelled.s.import(cancelled_encoded.data(), uint32_t(cancelled_encoded.size())) && cancelled.raw == exhausted.raw);
    const auto cancelled_before = cancelled.raw;
    check("cancelled same-life row never reuses a spent execution epoch",
          cancelled.edit_task(0, edit_kind::replace, cancelled.move(8, 8, 3)) == refusal::exhausted_identity &&
              cancelled.raw == cancelled_before);
    cancelled.s.retire_actor(cancelled.a());
    ++cancelled.actors[0].key.incarnation;
    check("only actual actor retirement permits a fresh lifetime epoch",
          cancelled.edit_task(0, edit_kind::replace, cancelled.move(8, 8, 3)) == refusal::none &&
              cancelled.view().execution_epoch == 1);
    fixture                   metadata;
    const target_key          ordinary_target{object_kind::building, {1, 2, 1}};
    const primary_reservation ordinary_bound{reservation_phase::bound, ordinary_target, 7, {}, 0x80010007u, 0};
    check("actual ordinary binding records metadata without route or epoch acquisition",
          metadata.s.write_reservation(metadata.a(), ordinary_bound) && metadata.s.execution_epoch(metadata.a()) == 0);
    route_view absent{};
    bool       ordinary_access = false;
    const auto ordinary_before = metadata.raw;
    metadata.s.tick(metadata.live());
    metadata.s.guard_primary(metadata.live(), metadata.a(), ordinary_access);
    metadata.s.retire_target(metadata.live(), ordinary_target, true);
    ++metadata.building_life[6];
    check("metadata alone never polls, cleans, holds or changes ordinary native timing",
          !metadata.s.read_route(metadata.a(), absent) && ordinary_access && metadata.raw == ordinary_before &&
              metadata.issued == 0 && metadata.cleaned == 0 && metadata.detached == 0 && metadata.target_reads == 0 &&
              metadata.s.validate_lifetimes(&metadata, fixture::lifetime));
    std::vector<uint8_t> ordinary_encoded(metadata.s.encoded_bytes());
    check("historical nonowning binding remains serializable until real native clear",
          metadata.s.encode(ordinary_encoded.data(), uint32_t(ordinary_encoded.size())) &&
              state::validate(ordinary_encoded.data(), uint32_t(ordinary_encoded.size()), metadata.d, metadata.w));
    fixture reservation;
    reservation.edit_task(0, edit_kind::replace, reservation.move(4, 4, 1, action::patrol));
    reservation.s.tick(reservation.live());
    const uint32_t      issue_epoch = reservation.s.execution_epoch(reservation.a());
    primary_reservation pending{reservation_phase::pending, {object_kind::building, {1, 2, 1}}, 7, {}, 0, issue_epoch};
    check("pending native issue accounting stores exact target and epoch",
          reservation.s.write_reservation(reservation.a(), pending));
    reservation.reserved_tally = 7;
    reservation.cancel_allowed = false;
    const auto pending_before  = reservation.raw;
    check("failed pending rollback refuses whole replacement without native or canonical change",
          reservation.edit_task(0, edit_kind::replace, reservation.move(7, 7, 2)) == refusal::unsupported &&
              reservation.raw == pending_before && reservation.reserved_tally == 7);
    reservation.cancel_allowed = true;
    check("accepted replacement rolls pending reservation once before changing active epoch",
          reservation.edit_task(0, edit_kind::replace, reservation.move(7, 7, 2)) == refusal::none &&
              reservation.cancelled_pending == 1 && reservation.reserved_tally == 0 &&
              reservation.s.execution_epoch(reservation.a()) == issue_epoch + 1);
    fixture bridge;
    bridge.edit_task(0, edit_kind::replace, bridge.move(4, 4, 1, action::patrol));
    bridge.s.tick(bridge.live());
    pending.execution_epoch = bridge.s.execution_epoch(bridge.a());
    pending.phase           = reservation_phase::handoff;
    check("synchronous native handoff is representable but never a checkpoint",
          bridge.s.write_reservation(bridge.a(), pending) && !bridge.s.checkpoint_ready());
    std::vector<uint8_t> handoff_out(bridge.s.encoded_bytes(), 0x5a);
    const auto           handoff_before = handoff_out;
    state                handoff_reader;
    check("handoff cannot leak through envelope or raw snapshot binding",
          !bridge.s.encode(handoff_out.data(), uint32_t(handoff_out.size())) && handoff_out == handoff_before &&
              !handoff_reader.bind(bridge.raw.data(), uint32_t(bridge.raw.size())));
    const primary_reservation bound{reservation_phase::bound, pending.target, 7, {}, 0x80010007u, pending.execution_epoch};
    check("actual rebound becomes publishable without another tally addition",
          bridge.s.write_reservation(bridge.a(), bound) && bridge.s.checkpoint_ready() &&
              bridge.s.encode(handoff_out.data(), uint32_t(handoff_out.size())) &&
              state::validate(handoff_out.data(), uint32_t(handoff_out.size()), bridge.d, bridge.w));
    const auto dead = f.a();
    const auto old  = f.event(0, outcome_kind::reached_near, {3, 4}, {}, assignment_reason::none, 9);
    ++f.actors[0].key.incarnation;
    check("import-boundary actor lifetime rejects reused live row", !f.s.validate_lifetimes(&f, fixture::lifetime));
    f.s.retire_actor(dead);
    f.edit_task(0, edit_kind::replace, f.move(7, 7, 100));
    f.s.tick(f.live());
    f.s.observe(f.live(), old);
    check("actor lifetime reuse cannot inherit completion", f.view().active.actor.incarnation != old.owner.actor.incarnation && f.view().count == 1);
    edit    wire{f.w, f.a(), edit_kind::append, f.move(255, 254, 101, action::patrol, 777)}, decoded{};
    int32_t args[13]{};
    check("fe13 words preserve explicit group, sequence, owner and coordinate", encode_edit(wire, args) &&
                                                                                    decode_edit(uint16_t(wire.actor.player), wire.actor.slot, args, decoded) && decoded.task.group == 777 &&
                                                                                    decoded.task.command_sequence == 101 && decoded.task.requested.x == 255 && args[11] == 0 && args[12] == 0);
    args[12]             = 1;
    const edit unchanged = decoded;
    check("wire hidden payload rejects without touching output", !decode_edit(0, 0, args, decoded) &&
                                                                     decoded.task.command_sequence == unchanged.task.command_sequence);
    args[12]              = 0;
    const auto first_word = args[0];
    wire.task.requested.x = 256;
    check("wire coordinate overflow never masks", !encode_edit(wire, args) && args[0] == first_word);
    check("queue command independent of ordinary supersession in both orders", independent(0, COMMAND, 0x10, 1) && independent(0x10, 1, 0, COMMAND) && !independent(0x10, 1, 0x20, 2));
    uint8_t        raw[campaign_state::BYTES]{};
    campaign_state campaign;
    uint32_t       current = 0;
    check("legacy known worlds seeded deterministically ascending", campaign.initialize(raw, sizeof raw) &&
                                                                        campaign.seed_legacy((1u << 3) | (1u << 7), 5, current) && campaign.world_inc(3) == 1 && current == 2 &&
                                                                        campaign.world_inc(7) == 3 && campaign.last_world_inc() == 3);
    uint8_t saved[campaign_state::BYTES];
    std::memcpy(saved, raw, sizeof raw);
    uint32_t fresh = 0;
    check("fresh world uses global clock, not seed or planet-local count", campaign.fresh_world(3, fresh) && fresh == 4 && campaign.world_inc(3) == 4);
    check("full load restores exact older clock without max", campaign.import(saved, sizeof saved) && campaign.last_world_inc() == 3 && campaign.world_inc(3) == 1);
    put32(raw + 8, UINT32_MAX);
    const auto before_exhaustion = std::vector<uint8_t>(raw, raw + sizeof raw);
    fresh                        = 123;
    check("checked exhaustion fixture rebinds valid canonical bytes", campaign.bind(raw, sizeof raw));
    check("campaign exhaustion refuses without wrapping or mutation", !campaign.fresh_world(4, fresh) && fresh == 123 &&
                                                                          std::memcmp(raw, before_exhaustion.data(), sizeof raw) == 0);
    uint32_t known = 123;
    check("existing known world retains ID even at exhausted clock", campaign.ensure_known(3, known) && known == 1 &&
                                                                         std::memcmp(raw, before_exhaustion.data(), sizeof raw) == 0);
    known = 123;
    check("new known world exhaustion is atomic including output", !campaign.ensure_known(4, known) && known == 123 &&
                                                                       std::memcmp(raw, before_exhaustion.data(), sizeof raw) == 0);
    check("invalid known planet is atomic", !campaign.ensure_known(32, known) && known == 123 &&
                                                std::memcmp(raw, before_exhaustion.data(), sizeof raw) == 0);
    saved[144] = 1;
    check("campaign malformed import is atomic", !campaign.import(saved, sizeof saved) && std::memcmp(raw, before_exhaustion.data(), sizeof raw) == 0);
}
void known_planet_lifecycle(checks &check) {
    uint8_t        bytes[campaign_state::BYTES]{};
    campaign_state campaign;
    uint32_t       incarnation = 0;
    check("skirmish fresh world31 publishes first", campaign.initialize(bytes, sizeof bytes) &&
                                                        campaign.fresh_world(31, incarnation) && incarnation == 1);
    bool grants = true;
    for (uint32_t planet = 1; planet <= 25; ++planet)
        grants = campaign.ensure_known(planet, incarnation) && incarnation == planet + 1 && grants;
    check("bulk off-system progress grants allocate every real known planet", grants && campaign.last_world_inc() == 26 &&
                                                                                  campaign.world_inc(31) == 1);
    const auto saved = std::vector<uint8_t>(bytes, bytes + sizeof bytes);
    for (uint32_t planet = 1; planet <= 25; ++planet)
        grants = campaign.ensure_known(planet, incarnation) && incarnation == planet + 1 && grants;
    check("repeated progress invasion visit retains all IDs and clock", grants &&
                                                                            std::memcmp(bytes, saved.data(), sizeof bytes) == 0);
    check("materializing a known world still creates a fresh epoch", campaign.fresh_world(7, incarnation) &&
                                                                         incarnation == 27 && campaign.world_inc(7) == 27 && campaign.world_inc(31) == 1);
}
void direction_reacquisition_and_complete_trace(checks &check) {
    fixture wrong_family;
    auto    inherited         = wrong_family.captures[0];
    inherited.count           = 1;
    inherited.nodes[0]        = wrong_family.move(6, 6, 0);
    inherited.status          = phase::executing;
    inherited.provenance      = native_provenance::active_move;
    inherited.position        = wrong_family.actors[0].position;
    inherited.requires_replan = true;
    for (const auto family : {executor::direction_plane, executor::direction_nonplane}) {
        fixture f;
        f.actors[0].family        = family;
        f.actors[0].safe_to_apply = false;
        f.captures[0]             = inherited;
        check("unproved legacy aircraft intent is retained without claiming a pending native F",
              f.edit_task(0, edit_kind::append, f.move(8, 8, 1)) == refusal::none &&
                  f.view().count == 2 && f.view().status == phase::awaiting_application &&
                  !f.view().externally_pending && !f.view().assignment_valid);
        const auto head = f.view().active;
        f.s.tick(f.live());
        check("in-flight aircraft keeps its unassigned head until a safe application boundary",
              f.view().active.leg == head.leg && f.view().status == phase::awaiting_application &&
                  f.view().count == 2 && !f.view().assignment_valid);
        f.actors[0].safe_to_apply = true;
        f.s.tick(f.live());
        f.assign(0, {6, 6});
        f.s.observe(f.live(), f.event(0, outcome_kind::preparation_finished));
        f.reach(0, {6, 6});
        check("re-acquired head requires a real assigned settled visit before advancing",
              f.view().count == 2 && f.view().status == phase::ready_next);
        f.s.tick(f.live());
        check("only the completed re-acquired head is consumed", f.view().count == 1 && f.view().active.leg != head.leg);
    }

    wrong_family.captures[0] = inherited;
    const auto before        = wrong_family.raw;
    check("re-acquisition proof cannot bypass a ground mover",
          wrong_family.edit_task(0, edit_kind::append, wrong_family.move(8, 8, 1)) == refusal::unsupported &&
              wrong_family.raw == before);

    uint8_t solver[301]{3, 7, 3, 255};
    uint8_t encoded[600]{7, 0, 3, 0, 255, 0};
    check("complete native trace preserves the initial-direction skip", complete_direction_trace(solver, encoded, 300));
    solver[3] = 254;
    check("failed solver cannot be laundered by an encoded success terminator", !complete_direction_trace(solver, encoded, 300));
    solver[3]  = 255;
    encoded[2] = 255;
    check("writer remap truncation is not a complete projection", !complete_direction_trace(solver, encoded, 300));
    std::memset(solver, 0, sizeof solver);
    std::memset(encoded, 0, sizeof encoded);
    check("unterminated native trace is bounded and rejected", !complete_direction_trace(solver, encoded, 300));
}

void adjacent_native_air_arrival(checks &check) {
    // Native 0045fac0/0048103b physical offsets, not the greedy hint deltas.
    // Consume a real planned turn across the seam as the movement executor does.
    constexpr int8_t dx[24] = {0, 1, 0, -1, 1, 1, -1, -1, 0, 1, 0, -1, -1, 1, 1, -1, 0, 1, 0, -1, 1, 1, -1, -1};
    constexpr int8_t dy[24] = {-1, 0, 1, 0, -1, 1, 1, -1, -1, 0, 1, 0, -1, -1, 1, 1, -1, 0, 1, 0, -1, 1, 1, -1};
    const dimensions d{1, 100, 128, 128, 100};
    air::pose        actual{{127, 127}, 22};
    air::path        path{};
    bool             valid = air::plan(d, actual, {{0, 0}, 3}, true, path);
    for (uint32_t i = 0; valid && i < path.count; ++i) {
        const uint32_t h = path.headings[i];
        if (h >= 24) {
            valid = false;
            break;
        }
        actual.position.x = (actual.position.x + 128 + dx[h]) % 128;
        actual.position.y = (actual.position.y + 128 + dy[h]) % 128;
        actual.heading    = h;
    }
    check("adjacent seam route reaches requested tile and heading under native movement",
          valid && actual.position.x == 0 && actual.position.y == 0 && actual.heading == 3);
}

void move_prefixed_patrol_origin(checks &check) {
    {
        fixture f;
        f.edit_task(0, edit_kind::replace, f.move(4, 4, 1));
        f.edit_task(0, edit_kind::append, f.move(8, 8, 2));
        f.edit_task(0, edit_kind::append, f.move(20, 20, 3, action::patrol));
        f.s.tick(f.live());
        f.arrive(0, {5, 4}, assignment_reason::valid_projection);
        f.s.tick(f.live());
        f.arrive(0, {9, 8}, assignment_reason::valid_projection);
        // Save before the last finite head is retired, not just after the origin is used.
        std::vector<uint8_t> saved(f.s.encoded_bytes());
        fixture              restored;
        restored.actors[0].position = f.actors[0].position;
        check("move-prefix route restores before physical placement retirement",
              f.s.encode(saved.data(), uint32_t(saved.size())) &&
                  restored.s.import(saved.data(), uint32_t(saved.size())));
        restored.s.tick(restored.live());
        restored.arrive(0, {20, 20});
        restored.s.tick(restored.live());
        check("last logical Move is the patrol return, not a prior move or formation offset",
              restored.issued == 2 && restored.calls[0].task.requested.x == 20 &&
                  restored.calls[1].task.requested.x == 8 && restored.calls[1].task.requested.y == 8);
        // Appending during that return preserves the current half, then uses explicit-only cycling.
        restored.edit_task(0, edit_kind::append, restored.move(25, 25, 4, action::patrol));
        restored.arrive(0, {8, 8});
        restored.s.tick(restored.live());
        check("second explicit patrol point follows an in-flight implicit return",
              restored.issued == 3 && restored.calls[2].task.requested.x == 25);
        restored.arrive(0, {25, 25});
        restored.s.tick(restored.live());
        check("multiple explicit patrol points do not insert the move origin into the cycle",
              restored.issued == 4 && restored.calls[3].task.requested.x == 20);
    }
    {
        fixture f;
        f.s.adopt_applied(f.live(), f.a(), f.move(4, 4, 0));
        std::vector<uint8_t> saved(f.s.encoded_bytes());
        check("unmanaged applied Move history survives normal serialization",
              f.s.encode(saved.data(), uint32_t(saved.size())) &&
                  f.s.import(saved.data(), uint32_t(saved.size())));
        auto &c              = f.captures[0];
        c.count              = 1;
        c.nodes[0]           = f.move(5, 4, 0); // Certified native physical head, not the click.
        c.provenance         = native_provenance::active_move;
        c.status             = phase::executing;
        c.position           = f.actors[0].position;
        c.assignment_valid   = true;
        c.assigned           = {5, 4};
        c.placement          = assignment_reason::requested;
        c.path_slot          = 9;
        c.logical_move_valid = f.s.ordinary_move_witness(f.a(), c.logical_move);
        f.edit_task(0, edit_kind::append, f.move(20, 20, 1, action::patrol));
        f.reach(0, {5, 4});
        f.s.tick(f.live());
        f.arrive(0, {20, 20});
        f.s.tick(f.live());
        check("captured active native Move returns to the actual click after physical head completion",
              f.issued == 2 && f.calls[1].task.requested.x == 4 && f.calls[1].task.requested.y == 4);
    }
    {
        fixture idle;
        idle.s.adopt_applied(idle.live(), idle.a(), idle.move(4, 4, 0));
        idle.actors[0].position = {13, 14};
        // The positive capture says idle; old Move history must never override it.
        idle.edit_task(0, edit_kind::append, idle.move(20, 20, 1, action::patrol));
        idle.s.tick(idle.live());
        idle.arrive(0, {20, 20});
        idle.s.tick(idle.live());
        check("completed Move history does not replace an idle patrol origin",
              idle.issued == 2 && idle.calls[1].task.requested.x == 13 &&
                  idle.calls[1].task.requested.y == 14);
    }
    {
        fixture mixed;
        mixed.actors[0].position = {31, 10};
        mixed.actors[1].position = {1, 14};
        mixed.edit_task(1, edit_kind::replace, mixed.move(5, 5, 1));
        mixed.edit_task(1, edit_kind::append, mixed.move(7, 7, 2, action::patrol));
        mixed.edit_task(0, edit_kind::replace, mixed.move(7, 7, 2, action::patrol));
        mixed.s.tick(mixed.live());
        state validated;
        check("one click may retain distinct fixed and idle origins through full region validation",
              validated.bind(mixed.raw.data(), uint32_t(mixed.raw.size())) &&
                  mixed.view().cohort_origin.x == 31 && mixed.view(1).cohort_origin.x == 5);
        mixed.arrive(0, {7, 7});
        mixed.arrive(1, {6, 5}, assignment_reason::valid_projection);
        mixed.s.tick(mixed.live());
        mixed.arrive(1, {7, 7});
        mixed.s.tick(mixed.live());
        check("fixed and idle members advance independently to their own truthful return points",
              mixed.issued == 5 && mixed.calls[2].task.requested.x == 31 &&
                  mixed.calls[2].task.requested.y == 10 &&
                  mixed.calls[4].task.requested.x == 5 && mixed.calls[4].task.requested.y == 5);
    }
    {
        fixture mixed;
        mixed.edit_task(1, edit_kind::replace, mixed.move(5, 5, 1));
        mixed.edit_task(1, edit_kind::append, mixed.move(7, 7, 2, action::patrol));
        mixed.edit_task(0, edit_kind::replace, mixed.move(7, 7, 2, action::patrol));
        mixed.s.tick(mixed.live());
        mixed.s.retire_actor(mixed.a(0));
        ++mixed.actors[0].key.incarnation;
        mixed.actors[0].position = {13, 14};
        const auto before        = mixed.raw;
        check("a surviving fixed origin cannot replace a retired idle centroid",
              mixed.edit_task(0, edit_kind::replace, mixed.move(7, 7, 2, action::patrol)) == refusal::stale_ticket &&
                  mixed.raw == before);
    }
    {
        fixture f;
        f.edit_task(0, edit_kind::replace, f.move(4, 4, 1));
        f.s.tick(f.live());
        f.assign(0, {5, 4}, assignment_reason::valid_projection);
        const auto before = f.raw;
        check("coincident logical ground patrol refuses without blocking its projected Move",
              f.edit_task(0, edit_kind::append, f.move(4, 4, 2, action::patrol)) == refusal::degenerate_cycle &&
                  f.raw == before && f.view().status == phase::executing);
    }
    {
        fixture f;
        f.s.adopt_applied(f.live(), f.a(), f.move(4, 4, 0));
        std::vector<uint8_t> bad(f.s.encoded_bytes());
        f.s.encode(bad.data(), uint32_t(bad.size()));
        put32(bad.data() + ENVELOPE_BYTES + HEADER_BYTES + 184, 6); // Owning origin without a route.
        repair_crc(bad);
        const auto before = f.raw;
        check("valid CRC cannot turn unmanaged Move history into an anchor without a route",
              !f.s.import(bad.data(), uint32_t(bad.size())) && f.raw == before);
    }
}

void frozen_cohort_origin(checks &check) {
    fixture f;
    for (uint32_t i = 0; i < 2; ++i) {
        f.actors[i].family = executor::direction_plane;
        f.actors[i].state = f.actors[i].order = 19;
        f.actors[i].microstep                 = 31;
    }
    f.actors[0].position      = {31, 10};
    f.actors[1].position      = {1, 14};
    f.actors[1].safe_to_apply = false;
    // An old active native prefix has no historical click witness; keep centroid
    // fallback rather than treating its per-member physical goal as logical intent.
    auto &capture      = f.captures[1];
    capture.count      = 1;
    capture.nodes[0]   = f.move(5, 5, 0);
    capture.provenance = native_provenance::active_move;
    capture.status     = phase::preparation;
    capture.position   = f.actors[1].position;
    f.edit_task(1, edit_kind::append, f.move(7, 7, 2, action::patrol));
    f.edit_task(0, edit_kind::replace, f.move(7, 7, 2, action::patrol));
    auto live  = f.live();
    live.apply = [](void *p, const execution &e) {
        // Keep physical attachment separate; this case proves canonical cohort activation.
        return e.air ? apply_result::deferred : fixture::apply(p, e);
    };
    f.s.tick(live);
    auto first = f.view(), prefix = f.view(1);
    check("activation centroid includes a surviving unsafe finite-prefix member across the seam",
          first.origin_resolved && prefix.origin_resolved &&
              first.cohort_sequence == 2 && prefix.cohort_sequence == 2 &&
              first.cohort_origin.x == 0 && first.cohort_origin.y == 12 &&
              prefix.cohort_origin.x == 0 && prefix.cohort_origin.y == 12);
    f.actors[0].position      = {16, 16};
    f.actors[1].safe_to_apply = true;
    f.s.tick(live);
    f.s.observe(live, f.event(1, outcome_kind::preparation_finished));
    f.arrive(1, {5, 5});
    f.s.tick(live);
    prefix = f.view(1);
    check("later prefix completion inherits the frozen origin without recentering",
          prefix.count == 1 && prefix.cycle_begin == 0 &&
              prefix.cohort_origin.x == 0 && prefix.cohort_origin.y == 12);
    state rebound;
    check("shared origin survives complete cross-row codec validation",
          rebound.bind(f.raw.data(), uint32_t(f.raw.size())));
    f.s.retire_actor(f.a());
    f.s.retire_actor(f.a(1));
    ++f.actors[0].key.incarnation;
    const auto before = f.raw;
    check("retired donors cannot create a new centroid for an old root",
          f.edit_task(0, edit_kind::replace, f.move(7, 7, 2, action::patrol)) == refusal::stale_ticket &&
              f.raw == before);
}


// Physical-lease cases share the existing order task suite and real container dequeue.
// The fixture implements only native field access and the original single booking event;
// all ownership, cancellation, admission, handoff, retirement and codecs are production code.
struct physical_booking_fixture : fixture {
    using native_order                                   = mh::orders::order;
    using native_unit                                    = mh::game::mh_map_object_unit;
    inline static physical_booking_fixture *current      = nullptr;
    physical_booking_fixture               *previous     = current;
    std::vector<native_order>               queue        = std::vector<native_order>(mh::orders::QUEUE_CAP + 8);
    std::vector<native_order>               pending      = std::vector<native_order>(mh::orders::PENDING_CAP + 8);
    std::vector<native_order>               staging      = std::vector<native_order>(mh::orders::STAGING_CAP + 8);
    std::vector<native_unit>                native_units = std::vector<native_unit>(mh::orders::MAX_PLAYERS * 2);
    std::vector<uint8_t>                    unit_types   = std::vector<uint8_t>(100 * mh::orders::UNIT_TYPE_STRIDE);
    int32_t                                 queue_count = 0, pending_count = 0, staging_count = 0, scratch[13]{};
    uint16_t                                tally[8]{};
    uint32_t                                native_writes = 0, adjustments = 0, original_issues = 0;
    mh::orders::container_state             orders{};
    mh::orders::game_calls                  calls{};
    physical_booking_fixture() {
        current                     = this;
        actors[1].safe_to_apply     = false;
        u().unit_proto_id           = 10;
        u().energy                  = 100;
        orders.queue                = queue.data();
        orders.queue_count          = &queue_count;
        orders.pending              = pending.data();
        orders.pending_count        = &pending_count;
        orders.staging              = staging.data();
        orders.staging_count        = &staging_count;
        orders.scratch              = scratch;
        orders.units                = native_units.data();
        orders.unit_types           = unit_types.data();
        orders.caps.units           = 2;
        orders.order_tasks_observer = observe;
        calls.unit_set_order_param  = [](int32_t, int32_t, int16_t p) { current->u().order = uint16_t(p); };
        calls.target_release_ref    = [](uint32_t, int32_t, uint32_t mode) {
            auto &f = *current;
            if (mode == 1) {
                if (!reservations::release_before(f.s, f.accounting(), f.a(1)))
                    reservations::release_after(f.s, f.accounting(), f.a(1));
            } else if (!reservations::rebound_before(f.s, f.accounting(), f.a(1))) {
                reservations::rebound_after(f.s, f.accounting(), f.a(1));
            }
        };
        calls.unit_notify_status = [](uint32_t, int32_t, uint32_t) {};
    }
    ~physical_booking_fixture() { current = previous; }
    native_unit     &u() { return native_units[1]; }
    static actor_key identity(void *p, object_kind kind, uint32_t player, uint32_t slot) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        if (kind == object_kind::unit && player < 2 && slot < 2) return f.actors[player * 2 + slot].key;
        if (kind == object_kind::building && player < 2 && slot < 4) return {player, slot, f.building_life[player * 4 + slot]};
        return {player, slot, 0};
    }
    static bool read(void *p, actor_key key, reservations::actual_primary &out) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        if (key.player != 0 || key.slot != 1 || key.incarnation != f.a(1).incarnation) return false;
        const auto    &u   = f.u();
        const uint32_t ref = uint16_t(u.target_ref);
        out                = {ref == 0 ? object_kind::none : (ref & 0x40) ? object_kind::building
                                                                          : object_kind::unit,
               ref & 15u, uint16_t(u.target_index), uint16_t(u.committed_weapon_damage_est),
               uint32_t(u.engagement_flags & 1u) | (uint32_t(u.order_status_flags & 0x80u) << 8)};
        return true;
    }
    static bool write(void *p, actor_key, uint32_t cache, uint32_t bits) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        ++f.native_writes;
        f.u().committed_weapon_damage_est = int16_t(uint16_t(cache));
        f.u().engagement_flags            = uint8_t((f.u().engagement_flags & ~1u) | (bits & 1u));
        f.u().order_status_flags          = uint8_t((f.u().order_status_flags & ~0x80u) | ((bits >> 8) & 0x80u));
        return true;
    }
    static bool clear(void *p, actor_key) {
        auto &u      = static_cast<physical_booking_fixture *>(p)->u();
        u.target_ref = u.target_index = 0;
        return true;
    }
    static bool adjust(void *p, target_key t, uint32_t delta, bool subtract) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        if (t.kind != object_kind::building || t.object.player >= 2 || t.object.slot >= 4) return false;
        auto &value = f.tally[t.object.player * 4 + t.object.slot];
        value       = uint16_t(subtract ? value - uint16_t(delta) : value + uint16_t(delta));
        ++f.adjustments;
        return true;
    }
    static bool retained(void *, actor_key, target_key) { return false; }
    static bool marker(void *p, actor_key a, uint32_t magic, uint32_t epoch, uint32_t life, const entry &expected) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        for (int32_t i = 0; i < f.queue_count; ++i) {
            const auto &q = f.queue[i];
            if (q.unit_index == a.slot && q.owner_and_kind == (0x80u | a.player) &&
                uint32_t(q.args[8]) == magic && uint32_t(q.args[9]) == a.incarnation &&
                uint32_t(q.args[10]) == epoch && uint32_t(q.args[11]) == life &&
                (uint32_t(q.args[5]) & 15u) == expected.target.object.player &&
                uint32_t(q.args[6]) == expected.target.object.slot) return true;
        }
        return false;
    }
    reservations::access accounting() { return {this, read, write, identity, adjust, clear, retained, marker}; }
    static bool          actor(void *p, actor_key a, actor_view &out) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        if (!fixture::actor(&f, a, out)) return false;
        route_view route{};
        if (f.s.read_route(a, route) && route.count) return true;
        primary_reservation r{};
        return reservations::capture_binding(f.s, f.accounting(), a, r);
    }
    static bool capture(void *p, actor_key a, active_capture &out) {
        auto               &f = *static_cast<physical_booking_fixture *>(p);
        primary_reservation r{};
        if (!reservations::capture_binding(f.s, f.accounting(), a, r)) return false;
        entry alternate{};
        if (reservations::capture_bound_alternate(f.s, f.accounting(), a, f.u().state, f.u().order, alternate)) {
            out            = {};
            out.position   = f.actors[1].position;
            out.count      = 1;
            out.nodes[0]   = alternate;
            out.status     = phase::executing;
            out.provenance = native_provenance::active_attack;
            return true;
        }
        if (!fixture::capture(&f, a, out)) return false;
        if (out.status != phase::awaiting_application) return true;
        return r.phase == reservation_phase::pending &&
               reservations::physical_record_current(f.s, f.accounting(), a, r.target, r.execution_epoch);
    }
    static bool cancel(void *p, actor_key a) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        return reservations::cancel_pending(f.s, f.accounting(), a);
    }
    static void detach(void *p, actor_key a, target_key t) {
        auto &f = *static_cast<physical_booking_fixture *>(p);
        reservations::detach_primary(f.s, f.accounting(), a, t);
    }
    backend     live() { return {this, actor, capture, fixture::target, fixture::apply, fixture::cleanup, cancel, detach}; }
    static bool decode(void *p, actor_key actor, uint16_t code, int16_t param, const int32_t (&args)[13], entry &out) {
        auto          &f    = *static_cast<physical_booking_fixture *>(p);
        const uint32_t ref  = uint16_t(args[5]);
        const auto     kind = ref & 0x40 ? object_kind::building : object_kind::unit;
        target_key     captured_target{};
        const bool     captured = uint32_t(args[8]) == host_protocol::EXPLICIT_PRIMARY && uint32_t(args[9]) == actor.incarnation &&
                              reservations::captured_record_target(f.s, actor, uint32_t(args[10]), uint32_t(args[11]), ref, uint16_t(args[6]), captured_target);
        const bool producer = uint32_t(args[8]) == host_protocol::PHYSICAL_PRIMARY || uint32_t(args[8]) == host_protocol::AUTONOMOUS_PRIMARY;
        if ((param == 27 || code == 27) && !producer && !captured) return false;
        actor_key victim = captured ? captured_target.object : identity(&f, kind, ref & 15u, uint16_t(args[6]));
        if (uint32_t(args[8]) == host_protocol::PHYSICAL_PRIMARY || uint32_t(args[8]) == host_protocol::AUTONOMOUS_PRIMARY)
            victim.incarnation = uint32_t(args[11]);
        out = {kind == object_kind::building ? action::attack_building : action::attack_unit,
               {},
               {kind, victim},
               uint32_t(args[12]),
               0};
        return victim.incarnation != 0;
    }
    int32_t dispatch(const mh_order_tasks_request &q, mh_order_tasks_result &r) {
        host_protocol::dispatch_context d{&s, live(), this, identity, decode, accounting()};
        return host_protocol::dispatch(d, q, r);
    }
    static int32_t            observe(const mh_order_tasks_request *q, mh_order_tasks_result *r) { return current->dispatch(*q, *r); }
    reservations::issue_scope begin(target_key target, uint32_t &decision, bool generic = false) {
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(host_protocol::operation::reservation);
        q.player    = 0;
        q.slot      = 1;
        q.args[0]   = uint32_t(generic ? host_protocol::reservation_operation::generic_begin : host_protocol::reservation_operation::begin_issue);
        q.args[1]   = uint32_t(target.kind);
        q.args[2]   = target.object.player;
        q.args[3]   = target.object.slot;
        q.args[4]   = target.object.incarnation;
        dispatch(q, r);
        decision = r.values[0];
        return host_protocol::get_issue_scope(r.values + 1, r.values + 15);
    }
    void original_issue(const reservations::issue_scope &scope, uint32_t delta) {
        ++original_issues;
        u().committed_weapon_damage_est = int16_t(uint16_t(delta));
        u().engagement_flags |= 1u;
        u().order_status_flags |= 0x80u;
        u().order_queued = 1;
        tally[scope.target.object.player * 4 + scope.target.object.slot] += uint16_t(delta);
    }
    bool finish(const reservations::issue_scope &scope, bool accepted, bool entered = false, bool suppressed = false) {
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(host_protocol::operation::reservation);
        q.player    = 0;
        q.slot      = 1;
        q.args[0]   = uint32_t(host_protocol::reservation_operation::finish_issue);
        host_protocol::put_issue_scope(q.args + 1, q.args + 18, scope);
        q.args[15] = accepted;
        q.args[16] = entered;
        q.args[17] = suppressed;
        return dispatch(q, r) && r.values[0];
    }
    void enqueue(const reservations::issue_scope &scope, int16_t param = 28, uint16_t code = 11) {
        for (auto &v : scratch) v = -1;
        scratch[0] = scratch[1] = 0;
        scratch[5]              = int32_t(0x40u | scope.target.object.player);
        scratch[6]              = int32_t(scope.target.object.slot);
        scratch[7]              = 4;
        scratch[8]              = int32_t(host_protocol::PHYSICAL_PRIMARY);
        scratch[9]              = int32_t(scope.actor.incarnation);
        scratch[10]             = int32_t(scope.epoch);
        scratch[11]             = int32_t(scope.target.object.incarnation);
        scratch[12]             = 40;
        mh::orders::detail::enqueue(orders, 1, 0x80, param, code);
    }
    mh_order_tasks_request admission(const native_order &record, bool inspect = false) {
        mh_order_tasks_request q{};
        q.operation = uint32_t(host_protocol::operation::primary_admission);
        q.player    = 0;
        q.slot      = record.unit_index;
        q.args[0]   = record.owner_and_kind;
        q.args[1]   = record.order_code;
        q.args[2]   = uint16_t(record.param0);
        for (uint32_t i = 0; i < 13; ++i) q.args[3 + i] = uint32_t(record.args[i]);
        q.args[16] = inspect;
        return q;
    }
    refusal edit(edit_kind kind, uint32_t sequence) {
        const mh::order_tasks::edit value{w, a(1), kind, move(7, 9, sequence)};
        int32_t                     words[13];
        encode_edit(value, words);
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(host_protocol::operation::due_edit);
        q.player    = 0;
        q.slot      = 1;
        q.args[1] = q.args[2] = COMMAND;
        for (uint32_t i = 0; i < 13; ++i) q.args[3 + i] = uint32_t(words[i]);
        dispatch(q, r);
        if (!r.values[1] && kind == edit_kind::append && queue_count) {
            const auto route  = view(1);
            queue[0].args[8]  = int32_t(host_protocol::EXPLICIT_PRIMARY);
            queue[0].args[9]  = int32_t(a(1).incarnation);
            queue[0].args[10] = int32_t(route.active.route);
            queue[0].args[11] = int32_t(route.active.leg);
        }
        return refusal(r.values[1]);
    }
    bool retire(target_key target) {
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(host_protocol::operation::retire_target);
        q.player    = target.object.player;
        q.slot      = target.object.slot;
        q.args[0]   = uint32_t(target.kind);
        q.args[1]   = target.object.incarnation;
        q.args[2]   = 1;
        return dispatch(q, r) && r.values[0];
    }
    void retire_actor() {
        mh_order_tasks_request q{};
        mh_order_tasks_result  r{};
        q.operation = uint32_t(host_protocol::operation::retire_actor);
        q.player    = 0;
        q.slot      = 1;
        q.args[0]   = a(1).incarnation;
        dispatch(q, r);
    }
    void prior(uint32_t slot, uint32_t delta) {
        u().target_ref                  = 0x41;
        u().target_index                = int16_t(slot);
        u().committed_weapon_damage_est = int16_t(delta);
        u().engagement_flags            = 5;
        u().order_status_flags          = 0xa0;
        tally[4 + slot]                 = uint16_t(delta);
        reservations::rebound_after(s, accounting(), a(1));
    }
};

void physical_pending_accounting(checks &check) {
    const target_key target{object_kind::building, {1, 2, 1}};
    {
        physical_booking_fixture f;
        f.u().engagement_flags   = 4;
        f.u().order_status_flags = 0x20;
        f.tally[6]               = 101;
        uint32_t   decision      = 0;
        const auto scope         = f.begin(target, decision);
        route_view route{};
        check("unowned issue captures a physical scope before any native write or route", scope.active &&
                                                                                              decision == uint32_t(reservations::issue_decision::physical) && scope.epoch &&
                                                                                              !f.s.read_route(f.a(1), route) && f.native_writes == 0 && f.adjustments == 0 && f.tally[6] == 101);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        check("physical finish preserves the original one booking", f.finish(scope, true) && f.original_issues == 1 &&
                                                                        f.tally[6] == 149 && f.u().committed_weapon_damage_est == 48);
        state bound;
        check("route-zero spent physical epoch passes full owner grammar", bound.bind(f.raw.data(), uint32_t(f.raw.size())));
        const auto stale = f.queue[0];
        check("prior-batch pending replacement rolls back exact delta", f.edit(edit_kind::replace, 1) == refusal::none &&
                                                                            f.tally[6] == 101 && f.adjustments == 1 && f.u().committed_weapon_damage_est == 0 &&
                                                                            f.u().engagement_flags == 4 && f.u().order_status_flags == 0x20);
        const auto            before = f.raw;
        mh_order_tasks_result result{};
        f.dispatch(f.admission(stale), result);
        check("stale physical record is denied without canceling the replacement", !result.values[0] && f.raw == before && f.view(1).count == 1);
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("real stale dequeue cannot apply or debit twice", f.queue_count == 0 && !f.u().order_queued &&
                                                                    f.tally[6] == 101 && f.adjustments == 1 && f.view(1).count == 1);
    }
    for (const bool same_target : {false, true}) {
        physical_booking_fixture f;
        const uint32_t           prior_slot = same_target ? 2 : 1;
        f.prior(prior_slot, 7);
        uint32_t   decision = 0;
        const auto scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        f.captures[1]            = {};
        f.captures[1].count      = 1;
        f.captures[1].nodes[0]   = f.attack(2, 0);
        f.captures[1].status     = phase::awaiting_application;
        f.captures[1].provenance = native_provenance::active_attack;
        f.captures[1].position   = f.actors[1].position;
        const uint32_t writes    = f.native_writes;
        check("append transfers pending issue without a native write or new tally", f.edit(edit_kind::append, 1) == refusal::none &&
                                                                                        f.view(1).externally_pending && f.view(1).count == 2 && f.adjustments == 0 && f.native_writes == writes &&
                                                                                        f.tally[6] == (same_target ? 55 : 48));
        primary_reservation carried{};
        f.s.read_reservation(f.a(1), carried);
        check("captured lease keeps the preissue baseline at its new route epoch", carried.prior_packed == (7u | (0x8001u << 16)) &&
                                                                                       carried.delta_raw16 == 48 && carried.execution_epoch == f.s.execution_epoch(f.a(1)) && carried.execution_epoch > scope.epoch);
        f.actors[1].safe_to_apply = true;
        f.captures[1].status      = phase::executing;
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("real QOEX dequeue releases only prior tally and preserves original pending delta", f.queue_count == 0 &&
                                                                                                      f.tally[6] == 48 && (same_target || f.tally[5] == 0) && f.adjustments == 1 &&
                                                                                                      f.u().committed_weapon_damage_est == 48 && f.view(1).count == 2 && !f.view(1).externally_pending);
    }
    {
        physical_booking_fixture f;
        f.prior(1, 7);
        uint32_t   decision = 0;
        const auto scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        f.retire({object_kind::building, {1, 1, 1}});
        ++f.building_life[5];
        f.tally[5] = 300;
        primary_reservation pending{};
        f.s.read_reservation(f.a(1), pending);
        check("prior target retirement preserves new pending cache and tally", pending.phase == reservation_phase::pending &&
                                                                                   pending.prior_target.kind == object_kind::none && pending.prior_packed == 0 && f.tally[6] == 48 &&
                                                                                   f.u().target_ref == 0 && f.u().committed_weapon_damage_est == 48 && (f.u().engagement_flags & 1));
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("physical F after prior reuse never debits replacement target", f.queue_count == 0 && f.tally[5] == 300 &&
                                                                                  f.tally[6] == 48 && f.u().target_index == 2 && f.u().committed_weapon_damage_est == 48);
        f.retire(target);
        ++f.building_life[6];
        f.tally[6] = 400;
        check("unowned target retirement preserves native cleanup timing", f.u().target_ref == 0x41 &&
                                                                               f.u().target_index == 2 && f.u().committed_weapon_damage_est == 48 &&
                                                                               f.tally[6] == 400 && !f.view(1).count);
        check("deferred native release never debits the reused target lifetime",
              reservations::release_before(f.s, f.accounting(), f.a(1)) &&
                  f.u().committed_weapon_damage_est == 0 && f.tally[6] == 400);
    }
    {
        physical_booking_fixture f;
        uint32_t                 decision = 0;
        const auto               first    = f.begin(target, decision);
        f.original_issue(first, 48);
        f.enqueue(first);
        f.finish(first, true);
        reservations::cancel_pending(f.s, f.accounting(), f.a(1));
        const auto second = f.begin(target, decision);
        f.original_issue(second, 12);
        f.enqueue(second);
        f.finish(second, true);
        mh_order_tasks_result old_result{}, new_result{};
        const auto            bytes = f.raw;
        f.dispatch(f.admission(f.queue[0], true), old_result);
        f.dispatch(f.admission(f.queue[1], true), new_result);
        check("same-target later issue has a unique epoch and pure survivor inspection", second.epoch > first.epoch &&
                                                                                             !old_result.values[0] && new_result.values[0] && bytes == f.raw && f.tally[6] == 12);
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("dropping stale old record keeps newer queued flag and booking", f.queue_count == 1 && f.u().order_queued && f.tally[6] == 12);
        f.retire_actor();
        ++f.actors[1].key.incarnation;
        f.tally[6] = 700;
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("actor reuse rejects old physical record without touching new target tally", !f.queue_count &&
                                                                                               f.tally[6] == 700 && !f.u().target_ref);
    }
    {
        physical_booking_fixture f;
        uint32_t                 decision = 0;
        const auto               scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        std::vector<uint8_t> encoded(f.s.encoded_bytes()), restored(f.raw.size());
        state                imported;
        check("pending no-route booking survives public owner encode/import", f.s.encode(encoded.data(), uint32_t(encoded.size())) &&
                                                                                  imported.initialize(restored.data(), uint32_t(restored.size()), f.d, f.w) &&
                                                                                  imported.import(encoded.data(), uint32_t(encoded.size())) && restored == f.raw &&
                                                                                  imported.validate_lifetimes(&f, fixture::lifetime));
        reservations::cancel_pending(f.s, f.accounting(), f.a(1));
        check("released no-route spent epoch also roundtrips", f.s.encode(encoded.data(), uint32_t(encoded.size())) &&
                                                                   imported.import(encoded.data(), uint32_t(encoded.size())) && restored == f.raw &&
                                                                   imported.execution_epoch(f.a(1)) == scope.epoch);
        const auto next = f.begin(target, decision);
        f.original_issue(next, 19);
        check("rejected original sink rolls back exactly one booking", f.finish(next, false) && f.tally[6] == 0 &&
                                                                           f.u().committed_weapon_damage_est == 0);
        const auto suppressed = f.begin(target, decision);
        f.original_issue(suppressed, 23);
        check("explicit suppressed sink receipt retains real accounting", f.finish(suppressed, false, true, true) && f.tally[6] == 23);
        const auto old_world = f.w;
        f.s.initialize(f.raw.data(), uint32_t(f.raw.size()), f.d, {old_world.system, old_world.planet, old_world.incarnation + 1});
        f.tally[6]             = 900;
        const auto after_world = f.raw;
        check("old-world finish cannot debit the replacement world", !f.finish(suppressed, false) && f.tally[6] == 900 && f.raw == after_world);
    }
    {
        physical_booking_fixture f;
        uint32_t                 decision = 0;
        const auto               scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        f.queue_count = mh::orders::QUEUE_CAP;
        for (int32_t i = 1; i < f.queue_count; ++i) f.queue[i] = {};
        mh::orders::detail::enqueue(f.orders, 1, 0x80, 28, 11);
        check("real queue overflow cancels exact physical booking once", !f.queue_count && f.tally[6] == 0 &&
                                                                             f.adjustments == 1 && !f.u().order_queued);
    }
    {
        physical_booking_fixture f;
        f.prior(1, 7);
        uint32_t   decision = 0;
        const auto scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        check("cancel restores exact preissue prior cache and not the overwritten new estimate",
              reservations::cancel_pending(f.s, f.accounting(), f.a(1)) && f.tally[5] == 7 && f.tally[6] == 0 &&
                  f.u().target_ref == 0x41 && f.u().target_index == 1 && f.u().committed_weapon_damage_est == 7 &&
                  f.u().engagement_flags == 5 && f.u().order_status_flags == 0xa0);
    }
    {
        physical_booking_fixture f;
        f.prior(1, 7);
        uint32_t   decision = 0;
        const auto scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        f.retire(target);
        ++f.building_life[6];
        f.tally[6] = 777;
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("pending target reuse preserves prior booking and replacement target tally", !f.queue_count &&
                                                                                               f.tally[6] == 777 && f.tally[5] == 7 && f.u().target_index == 1 && f.u().committed_weapon_damage_est == 7);
    }
    {
        physical_booking_fixture f;
        uint32_t                 decision = 0;
        const auto               scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        const auto bytes  = f.raw;
        auto       forged = f.queue[0];
        forged.args[8]    = int32_t(host_protocol::AUTONOMOUS_PRIMARY);
        mh_order_tasks_result forged_result{}, physical_result{};
        f.dispatch(f.admission(forged, true), forged_result);
        f.dispatch(f.admission(f.queue[0], true), physical_result);
        check("a physical scope cannot acquire managed QOPA authority", !forged_result.values[0] && physical_result.values[0] &&
                                                                            f.raw == bytes && !f.view(1).count);
        auto lost      = f.admission(f.queue[0]);
        lost.operation = uint32_t(host_protocol::operation::discarded);
        f.queue[1]     = f.queue[0];
        f.queue_count  = 2;
        f.queue[0]     = f.queue[1];
        f.queue_count  = 1;
        mh_order_tasks_result result{};
        f.dispatch(lost, result);
        check("an exact retained duplicate prevents premature booking rollback", f.tally[6] == 48 && f.adjustments == 0);
        f.queue_count = 0;
        f.dispatch(lost, result);
        f.dispatch(lost, result);
        check("last duplicate loss rolls back once despite duplicate callbacks", f.tally[6] == 0 && f.adjustments == 1);
    }

    for (const bool state_form : {false, true}) {
        physical_booking_fixture f;
        f.building_life[6] = 9;
        const target_key alternate_target{object_kind::building, {1, 2, 9}};
        uint32_t         decision = 0;
        const auto       scope    = f.begin(alternate_target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope, state_form ? 1 : 27, state_form ? 27 : 11);
        f.finish(scope, true);
        f.captures[1].count      = 1;
        f.captures[1].nodes[0]   = f.attack(2, 0);
        f.captures[1].status     = phase::awaiting_application;
        f.captures[1].provenance = native_provenance::active_attack;
        f.edit(edit_kind::append, 1);
        mh_order_tasks_result result{};
        f.dispatch(f.admission(f.queue[0], true), result);
        check("captured alternate QOEX uses canonical target life rather than its leg word", result.values[0] &&
                                                                                                 uint32_t(f.queue[0].args[11]) != 9 && f.view(1).externally_pending);
        f.actors[1].safe_to_apply = true;
        if (state_form) f.u().state = 27;
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        primary_reservation bound{};
        f.s.read_reservation(f.a(1), bound);
        check("alternate actual F preserves captured head and suffix after QOEX conversion", !f.queue_count &&
                                                                                                 f.view(1).count == 2 && !f.view(1).externally_pending && bound.target.object.incarnation == 9 &&
                                                                                                 bound.phase == reservation_phase::bound && f.tally[6] == 48);
    }
    {
        physical_booking_fixture f;
        f.prior(1, 7);
        uint32_t   decision = 0;
        const auto scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        f.captures[1].count      = 1;
        f.captures[1].nodes[0]   = f.attack(2, 0);
        f.captures[1].status     = phase::awaiting_application;
        f.captures[1].provenance = native_provenance::active_attack;
        f.edit(edit_kind::append, 1);
        const uint32_t epoch = f.s.execution_epoch(f.a(1));
        check("common prior retirement leaves the transferred external booking intact", f.retire({object_kind::building, {1, 1, 1}}) &&
                                                                                            f.s.primary_cleanup_pending(f.a(1)) && f.tally[5] == 0 && f.tally[6] == 48);
        ++f.building_life[5];
        f.tally[5]                = 333;
        f.actors[1].safe_to_apply = true;
        f.s.tick(f.live());
        primary_reservation pending{};
        f.s.read_reservation(f.a(1), pending);
        check("safe detached cleanup preserves exact pending QOEX authority and epoch", pending.phase == reservation_phase::pending &&
                                                                                            pending.execution_epoch == epoch && f.s.execution_epoch(f.a(1)) == epoch && !f.s.primary_cleanup_pending(f.a(1)) &&
                                                                                            f.view(1).externally_pending && f.u().committed_weapon_damage_est == 48 && f.tally[6] == 48);
        f.captures[1].status = phase::executing;
        mh::orders::detail::queue_apply_and_dequeue(f.orders, f.calls, 0, 1, 0);
        check("QOEX after prior cleanup reuses its booking without re-estimation", f.tally[5] == 333 && f.tally[6] == 48 &&
                                                                                       f.view(1).count == 2 && !f.view(1).externally_pending && f.u().committed_weapon_damage_est == 48);
    }
    {
        physical_booking_fixture f;
        f.s.write_reservation(f.a(1), {});
        const uint32_t offset = HEADER_BYTES + ROW_BYTES + 104;
        put32(f.raw.data() + offset, UINT32_MAX - 1);
        check("exhaustion boundary fixture has a structurally valid spent no-route epoch", f.s.bind(f.raw.data(), uint32_t(f.raw.size())));
        uint32_t   decision = 0;
        const auto scope    = f.begin(target, decision);
        f.original_issue(scope, 48);
        f.enqueue(scope);
        f.finish(scope, true);
        const auto before        = f.raw;
        const auto native_before = f.u();
        f.begin(target, decision, true);
        check("exhausted generic begin refuses before canceling the valid previous booking", decision == uint32_t(reservations::issue_decision::denied) &&
                                                                                                 f.raw == before && !std::memcmp(&native_before, &f.u(), sizeof native_before) && f.tally[6] == 48 && f.adjustments == 0);
    }
}

} // namespace
int run_ordertaskstest() {
    checks check;
    independent_progress_and_attempts(check);
    captures_capacity_and_replacement(check);
    attack_retirement_and_cleanup(check);
    patrol_excursion_and_zero_progress(check);
    codecs_wire_and_campaign(check);
    known_planet_lifecycle(check);
    legacy_primary_booking_import(check);
    physical_pending_accounting(check);
    direction_reacquisition_and_complete_trace(check);
    frozen_cohort_origin(check);
    move_prefixed_patrol_origin(check);
    adjacent_native_air_arrival(check);
    std::printf("=== ordertaskstest: %u checks, %u failures ===\n", check.count, check.failed);
    return check.failed ? 1 : 0;
}
