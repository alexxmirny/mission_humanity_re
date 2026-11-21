#include "state/storage_policy_runtime.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/region_runtime.h"
#include "state/roster_caps.h"
#include "sim/sim_order_dispatch.h"
#include "state/host_events.h"

namespace {
using namespace mh::storage_policy;
using unit_record     = mh::game::mh_map_object_unit;
using building_record = mh::game::mh_map_object_building;
using storage_record  = mh::game::mh_map_object_unit_storage;
// A borrowed span wrapper only. No allocation, duplicate canonical bytes, or policy singleton:
// libref_host owns the region in its world arena. Hosted DLLs never call this adapter.
state policy;
using world = mh::state::storage_policy_world;
unit_record *unit_at(world &v, uint32_t p, uint32_t s) {
    return p < 8 && s && s < v.unit_rows && v.units ? v.units + p * v.unit_rows + s : nullptr;
}
building_record *building_at(world &v, uint32_t p, uint32_t s) {
    return p < 8 && s && s < v.building_rows && v.buildings ? v.buildings + p * v.building_rows + s : nullptr;
}
bool building_exists(void *context, uint32_t p, uint32_t slot) {
    auto       &v = *static_cast<world *>(context);
    const auto *b = building_at(v, p, slot);
    return b && b->index > 0 && b->state != 2 && b->energy > 0 && b->building_id &&
           b->building_id < v.building_types_count && v.building_types;
}
bool eligible(void *context, uint32_t p, uint32_t slot) {
    if (!building_exists(context, p, slot)) return false;
    auto          &v    = *static_cast<world *>(context);
    const auto    *b    = building_at(v, p, slot);
    const uint32_t type = v.building_types[b->building_id].type;
    return (type >= 0x07 && type <= 0x0a) || type == 0x0c ||
           (type >= 0x1b && type <= 0x1e) || type == 0x20;
}
bool unit_exists(void *context, uint32_t p, uint32_t slot) {
    auto       &v = *static_cast<world *>(context);
    const auto *u = unit_at(v, p, slot);
    return u && u->energy > 0 && u->unit_proto_id && u->unit_proto_id < v.unit_types_count && v.unit_types;
}
const storage_record *receiver(world &v, uint32_t p, uint32_t slot, uint32_t &bslot) {
    const auto *u = unit_at(v, p, slot);
    if (!u || !v.storages || u->home_storage_slot >= v.storage_rows) return nullptr;
    const auto *s = v.storages + p * v.storage_rows + u->home_storage_slot;
    if (s->b_index <= 0 || !eligible(&v, p, uint32_t(s->b_index))) return nullptr;
    const auto *b = building_at(v, p, uint32_t(s->b_index));
    if (b->sub_id != u->home_storage_slot) return nullptr;
    bslot = uint32_t(s->b_index);
    return s;
}
bool healthy(world &v, uint32_t p, uint32_t slot) {
    if (!unit_exists(&v, p, slot)) return false;
    const auto &u = *unit_at(v, p, slot);
    return u.energy >= v.unit_types[u.unit_proto_id].energy && !(u.pending_damage > 0);
}
bool resolve(void *context, uint32_t p, uint32_t slot, receiver_reason reason, double before, uint32_t &bslot) {
    auto &v = *static_cast<world *>(context);
    if (!unit_exists(&v, p, slot)) return false;
    const auto &u = *unit_at(v, p, slot);
    if (u.state != 0x1f || !receiver(v, p, slot, bslot)) return false;
    if (reason == receiver_reason::manual) return true;
    if (u.order_queued || !healthy(v, p, slot)) return false;
    return reason == receiver_reason::production || (before > 0 && before < v.unit_types[u.unit_proto_id].energy);
}
launch_result launch(void *context, identity uid, identity bid, rally_view r) {
    auto &v = *static_cast<world *>(context);
    if (!healthy(v, uid.player, uid.slot)) return launch_result::invalid;
    auto &u = *unit_at(v, uid.player, uid.slot);
    if (u.state != 0x1f || u.order_queued) return launch_result::invalid;
    uint32_t    bslot;
    const auto *storage = receiver(v, uid.player, uid.slot, bslot);
    if (!storage || bslot != bid.slot || policy.building_identity(uid.player, bslot).incarnation != bid.incarnation)
        return launch_result::invalid;
    const auto &b = *building_at(v, uid.player, bslot);
    if (b.built_flags != 3 || !b.online_state) return launch_result::deferred;
    const auto      &c     = v.unit_types[u.unit_proto_id];
    const dimensions d     = policy.shape();
    const uint32_t   x     = r.enabled ? r.x : (uint32_t(storage->exit_tile_x) + 2u) % d.width;
    const uint32_t   y     = r.enabled ? r.y : (uint32_t(storage->exit_tile_y) + 2u) % d.height;
    const auto      &calls = mh::sim::live_dispatch_calls();
    // Same F launch/planner continuation as the hosted adapter. These calls resolve to
    // translated callbacks in standalone; no legacy marshalling shim.
    if (u.target_ref) {
        calls.llm_strat_target_release_ref(uid.player, int32_t(uid.slot), 1);
        u.target_ref   = 0;
        u.target_index = 0;
    }
    if (u.state == c.move_op_arg || u.state == c.move_op_code)
        calls.llm_strat_unit_notify_status(uid.player, int32_t(uid.slot), 0x67);
    u.order                    = r.enabled ? c.move_op_arg : c.default_op_code;
    u.state                    = 0x20;
    u.goal_x                   = uint8_t(x);
    u.goal_y                   = uint8_t(y);
    u.path_blocked_retry_count = 0;
    u.order_queued             = 0;
    u.home_x                   = u.x;
    u.home_y                   = u.y;
    if (r.enabled) u.move_group_id = 0;
    if (u.state == c.move_op_arg) calls.llm_strat_unit_notify_status(uid.player, int32_t(uid.slot), 0);
    return launch_result::accepted;
}
} // namespace
extern "C" int32_t libmh_storage_policy_bind(void *region, uint32_t bytes) {
    return policy.bind(region, bytes) ? 1 : 0;
}
extern "C" int32_t libmh_storage_policy_invoke(const mh_storage_policy_request *q, mh_storage_policy_result *out) {
    if (!out) return 0;
    *out = {};
    if (!q || !policy.data()) return 0;
    world         v = mh::state::storage_policy_tables(mh::state::ROSTER_PLAYERS);
    const backend live{&v, eligible, unit_exists, resolve, launch, building_exists};
    const int32_t accepted = dispatch(policy, live, *q, *out);
    if (accepted == 1 &&
        q->operation == static_cast<uint32_t>(mh::storage_policy::operation::reset_loaded_roster))
        mh::order_witness::reset_world(mh::state::order_witnesses());
    return accepted;
}
namespace mh::storage_policy::runtime {
state     &bound_state() { return policy; }
dimensions roster_dimensions() {
    using namespace mh::state;
    uint32_t   width = 0, height = 0;
    const bool mapped = read_region_u32(RID_WIDTH, &width) && read_region_u32(RID_HEIGHT, &height) && width > 0 && width <= 256 && height > 0 && height <= 256;
    return {uint32_t(ROSTER_PLAYERS),
            uint32_t(per_player_capacity(RID_BUILDINGS, sizeof(building_record), ROSTER_PLAYERS)),
            uint32_t(per_player_capacity(RID_UNITS, sizeof(unit_record), ROSTER_PLAYERS)),
            mapped ? width : 0u, mapped ? height : 0u};
}
} // namespace mh::storage_policy::runtime
