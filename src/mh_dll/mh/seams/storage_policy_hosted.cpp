#include "seams/storage_policy.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include <memory>
#include <new>
#include <cstring>
#include <utility>
#include "ui/target_line_intents.h"

namespace mh::storage_policy::hosted {
namespace {
using building_record = mh::game::mh_map_object_building;
using unit_record     = mh::game::mh_map_object_unit;
using storage_record  = mh::game::mh_map_object_unit_storage;
using cfg_building    = mh::game::mh_cfg_final_struct_Building;
using cfg_unit        = mh::game::mh_cfg_final_struct_Unit;
struct storage_owner {
    uint8_t                    empty[HEADER_BYTES]{};
    std::unique_ptr<uint8_t[]> allocation;
    state                      policy;
    storage_owner() { policy.initialize(empty, sizeof(empty), {}); }
};
storage_owner g;

bool bbound(uint32_t p, uint32_t s) {
    const auto &b = mh::caps::g(mh::caps::BUILDINGS);
    return p < b.players && s > 0 && s < b.limit;
}
bool ubound(uint32_t p, uint32_t s) {
    const auto &u = mh::caps::g(mh::caps::UNITS);
    return p < u.players && s > 0 && s < u.limit;
}
const cfg_building *building_config(uint32_t id) {
    const uint32_t size = mh::state::live_size(mh::state::RID_BUILDING);
    const auto    *base = mh::state::ptr<const cfg_building>(mh::state::RID_BUILDING);
    return base && id && id < size / sizeof(cfg_building) ? base + id : nullptr;
}
const cfg_building *live_building_config(uint32_t p, uint32_t slot) {
    if (!bbound(p, slot)) return nullptr;
    const auto &b = *mh::caps::rec<const building_record>(mh::caps::BUILDINGS, p, slot);
    return b.index > 0 && b.state != 2 && b.energy > 0 ? building_config(b.building_id) : nullptr;
}
const cfg_unit *unit_config(uint32_t id) {
    const uint32_t size = mh::state::live_size(mh::state::RID_UNIT);
    const auto    *base = mh::state::ptr<const cfg_unit>(mh::state::RID_UNIT);
    return base && id && id < size / sizeof(cfg_unit) ? base + id : nullptr;
}
bool family(uint32_t type) {
    // cfg_enum_E_BUILDING_TYPE; shuttles 0x0d/0x21 deliberately excluded.
    return (type >= 0x07 && type <= 0x0a) || type == 0x0c ||
           (type >= 0x1b && type <= 0x1e) || type == 0x20;
}
const storage_record *receiver(uint32_t p, uint32_t slot, uint32_t &bslot) {
    if (!ubound(p, slot)) return nullptr;
    const auto &u  = *mh::caps::rec<const unit_record>(mh::caps::UNITS, p, slot);
    const auto &sg = mh::caps::g(mh::caps::UNIT_STORAGE);
    if (p >= sg.players || u.home_storage_slot >= sg.limit) return nullptr;
    const auto *s = mh::caps::rec<const storage_record>(mh::caps::UNIT_STORAGE, p, u.home_storage_slot);
    if (s->b_index <= 0 || !eligible_building(p, uint32_t(s->b_index))) return nullptr;
    const auto &b = *mh::caps::rec<const building_record>(mh::caps::BUILDINGS, p, uint32_t(s->b_index));
    if (b.sub_id != u.home_storage_slot) return nullptr;
    bslot = uint32_t(s->b_index);
    return s;
}
bool healthy(uint32_t p, uint32_t slot) {
    if (!ubound(p, slot)) return false;
    const auto     &u = *mh::caps::rec<const unit_record>(mh::caps::UNITS, p, slot);
    const cfg_unit *c = unit_config(u.unit_proto_id);
    return c && u.energy > 0 && u.energy >= c->energy && !(u.pending_damage > 0);
}
bool unit_exists(void *, uint32_t p, uint32_t slot) {
    if (!ubound(p, slot)) return false;
    const auto &u = *mh::caps::rec<const unit_record>(mh::caps::UNITS, p, slot);
    return u.energy > 0 && unit_config(u.unit_proto_id);
}
bool eligible(void *, uint32_t p, uint32_t slot) { return eligible_building(p, slot); }
bool building_exists(void *, uint32_t p, uint32_t slot) { return hosted::building_exists(p, slot); }
bool resolve_receiver(void *, uint32_t p, uint32_t slot, receiver_reason reason,
                      double before, uint32_t &bslot) {
    if (!unit_exists(nullptr, p, slot)) return false;
    const auto &u = *mh::caps::rec<const unit_record>(mh::caps::UNITS, p, slot);
    if (u.state != 0x1f || !receiver(p, slot, bslot)) return false;
    if (reason == receiver_reason::manual) return true;
    if (u.order_queued || !healthy(p, slot)) return false;
    const cfg_unit &c = *unit_config(u.unit_proto_id);
    return reason == receiver_reason::production || (before > 0 && before < c.energy);
}
launch_result launch(void *, identity uid, identity bid, rally_view rally) {
    if (!healthy(uid.player, uid.slot)) return launch_result::invalid;
    auto &u = *mh::caps::rec<unit_record>(mh::caps::UNITS, uid.player, uid.slot);
    if (u.state != 0x1f || u.order_queued) return launch_result::invalid;
    uint32_t              bslot;
    const storage_record *s = receiver(uid.player, uid.slot, bslot);
    if (!s || bslot != bid.slot || building_identity(uid.player, bslot).incarnation != bid.incarnation)
        return launch_result::invalid;
    const auto &b = *mh::caps::rec<const building_record>(mh::caps::BUILDINGS, uid.player, bslot);
    if (b.built_flags != 3 || !b.online_state) return launch_result::deferred;
    const cfg_unit  &c = *unit_config(u.unit_proto_id);
    const dimensions d = g.policy.shape();
    const uint32_t   x = rally.enabled ? rally.x : (uint32_t(s->exit_tile_x) + 2u) % d.width;
    const uint32_t   y = rally.enabled ? rally.y : (uint32_t(s->exit_tile_y) + 2u) % d.height;

    // Native args0/1-only F launch (EN00466ce1..0046705d), with the planner continuation
    // retained only for rally departures. Boarding's turn-defer arm still reaches F;
    // no retained queue copy is manufactured by this simulation-derived launch.
    if (u.target_ref) {
        mh::call::llm_strat_target_release_ref(uid.player, int32_t(uid.slot), 1);
        u.target_ref   = 0;
        u.target_index = 0;
    }
    if (u.state == c.move_op_arg || u.state == c.move_op_code)
        mh::call::llm_strat_unit_notify_status(uid.player, int32_t(uid.slot), 0x67);
    // The two native setters at 48696f/4869b0 are single field stores; doing them here avoids
    // routing a sim-derived operation through an explicit-order cancellation hook.
    u.order                    = rally.enabled ? c.move_op_arg : c.default_op_code;
    u.state                    = 0x20;
    u.goal_x                   = uint8_t(x);
    u.goal_y                   = uint8_t(y);
    u.path_blocked_retry_count = 0;
    u.order_queued             = 0;
    u.home_x                   = u.x;
    u.home_y                   = u.y;
    if (rally.enabled) u.move_group_id = 0;
    if (u.state == c.move_op_arg) mh::call::llm_strat_unit_notify_status(uid.player, int32_t(uid.slot), 0);
    return launch_result::accepted;
}
const backend             LIVE{nullptr, eligible, unit_exists, resolve_receiver, launch, building_exists};
mh_storage_policy_request request(operation op, uint32_t p = 0, uint32_t slot = 0) {
    mh_storage_policy_request q{};
    q.operation = static_cast<uint32_t>(op);
    q.player    = p;
    q.slot      = slot;
    return q;
}
} // namespace

state  &owner() { return g.policy; }
int32_t invoke(const mh_storage_policy_request *q, mh_storage_policy_result *result) {
    if (!result) return 0;
    *result = {};
    if (!q) return 0;
    // Shared hosted post-dispatch boundary, before automatic launches and unit ticks.
    if (q->operation == static_cast<uint32_t>(operation::retry))
        mh::ui::selected_targets::detail::observe_after_dispatch();
    const int32_t rc = dispatch(g.policy, LIVE, *q, *result);
    if (rc) {
        using namespace mh::ui::selected_targets::detail;
        switch (static_cast<operation>(q->operation)) {
            case operation::birth_building: observe_building_birth(q->player, q->slot); break;
            case operation::retire_building: observe_building_retirement(q->player, q->slot); break;
            case operation::reset_loaded_roster: reset_intents(); break;
            default: break;
        }
    }
    return rc;
}
const uint8_t *region_data() { return g.policy.data(); }
uint8_t       *region_data_mut() { return g.policy.data(); }
uint32_t       region_bytes() { return g.policy.size(); }
bool           reserve_after_caps() { return setup(0, 0); }
bool           setup(uint32_t width, uint32_t height) {
    const auto &b = mh::caps::g(mh::caps::BUILDINGS), &u = mh::caps::g(mh::caps::UNITS);
    // The native carrier relocates even its stock-limit rows; promoted code keeps stock strides.
    // Reserve the same supported slot domain in both images, not their different memory pitches.
    // All live accesses still pass the backend's actual limit checks.
    const uint32_t building_rows = mh::caps::RAISED_ROW[mh::caps::BUILDINGS];
    const uint32_t unit_rows     = mh::caps::RAISED_ROW[mh::caps::UNITS];
    if (b.players != u.players || b.row > building_rows || u.row > unit_rows) return false;
    const dimensions d{b.players, building_rows, unit_rows, width, height};
    const uint32_t   bytes = state::required_bytes(d);
    if (!bytes) return false;
    if (!g.allocation) {
        std::unique_ptr<uint8_t[]> memory(new (std::nothrow) uint8_t[bytes]);
        if (!memory) return false;
        if (!g.policy.initialize(memory.get(), bytes, d)) return false;
        g.allocation = std::move(memory);
        mh::ui::selected_targets::detail::reset_intents();
        return true;
    }
    const dimensions old = g.policy.shape();
    if (old.players != d.players || old.buildings != d.buildings || old.units != d.units) return false;
    if (old.width != width || old.height != height) {
        if (!g.policy.initialize(g.allocation.get(), bytes, d)) return false;
        mh::ui::selected_targets::detail::reset_intents();
    }
    return true;
}
bool reset_world() {
    const auto               q = request(operation::reset_loaded_roster);
    mh_storage_policy_result result{};
    return invoke(&q, &result) != 0;
}
bool building_exists(uint32_t p, uint32_t slot) { return live_building_config(p, slot) != nullptr; }
bool eligible_building(uint32_t p, uint32_t slot) {
    const cfg_building *c = live_building_config(p, slot);
    return c && family(c->type);
}
identity   building_identity(uint32_t p, uint32_t s) { return g.policy.building_identity(p, s); }
identity   unit_identity(uint32_t p, uint32_t s) { return g.policy.unit_identity(p, s); }
rally_view read_rally(uint32_t p, uint32_t s) { return eligible_building(p, s) ? g.policy.read_rally(p, s) : rally_view{}; }
bool       birth_building(uint32_t p, uint32_t s) {
    mh::ui::selected_targets::detail::observe_building_birth(p, s);
    if (!building_exists(p, s)) return false;
    identity id{};
    return g.policy.birth_building(p, s, id);
}
bool birth_unit(uint32_t p, uint32_t s) {
    if (!ubound(p, s)) return false;
    identity id{};
    return g.policy.birth_unit(p, s, id);
}
void retire_building(uint32_t p, uint32_t s) {
    mh::ui::selected_targets::detail::observe_building_retirement(p, s);
    g.policy.retire_building(building_identity(p, s));
}
void retire_unit(uint32_t p, uint32_t s) { g.policy.retire_unit(unit_identity(p, s)); }
void cancel_pending(uint32_t p, uint32_t s) { g.policy.cancel_pending(unit_identity(p, s)); }
bool apply_command(uint32_t actor, const command &cmd) {
    return eligible_building(cmd.building.player, cmd.building.slot) && g.policy.apply_command(actor, cmd);
}
bool consume_command(uint16_t owner_and_kind, uint16_t code, uint32_t slot, const int32_t (&args)[13]) {
    if ((owner_and_kind & 0xf0u) || (code != RALLY_COMMAND && code != AUTO_COMMAND)) return false;
    command        c{};
    const uint32_t actor = owner_and_kind & 15u;
    if (decode_command(actor, slot, args, c) &&
        ((code == AUTO_COMMAND) == (c.kind == command_kind::set_auto))) apply_command(actor, c);
    return true;
}
void production_inserted(uint32_t p, uint32_t s) {
    const auto               q = request(operation::production, p, s);
    mh_storage_policy_result result{};
    invoke(&q, &result);
}
void repair_paid(uint32_t p, uint32_t s, double before) {
    auto     q = request(operation::paid_repair, p, s);
    uint64_t bits;
    std::memcpy(&bits, &before, sizeof(bits));
    q.args[0] = uint32_t(bits);
    q.args[1] = uint32_t(bits >> 32);
    mh_storage_policy_result result{};
    invoke(&q, &result);
}
void retry() {
    const auto               q = request(operation::retry);
    mh_storage_policy_result result{};
    invoke(&q, &result);
}
bool manual_rally(uint32_t p, uint32_t s, uint32_t &x, uint32_t &y) {
    const auto               q = request(operation::manual_rally, p, s);
    mh_storage_policy_result result{};
    if (!invoke(&q, &result) || !result.values[0]) return false;
    x = result.values[1];
    y = result.values[2];
    return true;
}
} // namespace mh::storage_policy::hosted
