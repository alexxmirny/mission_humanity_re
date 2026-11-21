#include "sim/sim_storage_bulk_rally.h"

#include "sim/sim_storage_cancel_pending_docked.h"
#include "state/region_runtime.h"

namespace mh::sim::detail {
namespace {
namespace policy = mh::storage_policy;

bool bounded_unit(const sim_view &v, uint32_t player, int32_t index) {
    return v.units && player < MAX_PLAYERS && index > 0 && index < v.caps.units;
}
bool bounded_building(const sim_view &v, uint32_t player, int32_t index) {
    return v.buildings && player < MAX_PLAYERS && index > 0 &&
           index < v.caps.buildings;
}
bool manual_bulk_family(uint8_t type) {
    // A/H barracks, garage, airfield, helipad: HUD groups 8/9, not ports/mothers/shuttles.
    return (type >= 0x07 && type <= 0x0a) || (type >= 0x1b && type <= 0x1e);
}
struct departure {
    uint32_t incarnation = 0, x = 0, y = 0;
    uint16_t prototype = 0, building_prototype = 0, default_order = 0, move_state = 0;
    uint8_t  storage_slot = 0, native_x = 0, native_y = 0, queued = 0;
    bool     receiver_valid = false, rally = false;
};

bool receiver_matches(const sim_view &v, uint32_t player, int32_t building_index,
                      int32_t unit_index, const departure &d) {
    if (!bounded_unit(v, player, unit_index) || !bounded_building(v, player, building_index) ||
        !v.storage || !d.storage_slot || d.storage_slot >= v.caps.storage) return false;
    const unit         &u = unit_of(v, player, unit_index);
    const building     &b = building_of(v, player, building_index);
    const unit_storage &s = storage_of(v, player, d.storage_slot);
    return u.unit_proto_id == d.prototype && u.home_storage_slot == d.storage_slot &&
           b.index == building_index && b.building_id == d.building_prototype &&
           b.sub_id == d.storage_slot && s.b_index == building_index;
}

departure snapshot(const sim_view &v, sim_store &own, uint32_t player,
                   int32_t building_index, int32_t unit_index) {
    departure d;
    if (!bounded_unit(v, player, unit_index) || !bounded_building(v, player, building_index) ||
        !v.cfg_units || !v.cfg_buildings) return d;
    const unit     &u = unit_of(v, player, unit_index);
    const building &b = building_of(v, player, building_index);
    // These are CFG table extents, not roster bounds. The per-player rosters use v.caps above.
    const uint32_t unit_types     = mh::state::live_size(mh::state::RID_UNIT) / sizeof(cfg_unit);
    const uint32_t building_types = mh::state::live_size(mh::state::RID_BUILDING) / sizeof(cfg_building);
    if (u.state != 0x1f || !(u.energy > 0) || !u.unit_proto_id || u.unit_proto_id >= unit_types ||
        !b.building_id || b.building_id >= building_types || b.state == 2 || !(b.energy > 0) ||
        !manual_bulk_family(v.cfg_buildings[b.building_id].type)) return d;
    d.prototype          = u.unit_proto_id;
    d.building_prototype = b.building_id;
    d.storage_slot       = u.home_storage_slot;
    if (!receiver_matches(v, player, building_index, unit_index, d)) return d;
    const auto r = own.storage_policy(policy::operation::read_rally, player, uint32_t(building_index));
    if (!r.values[0]) return d;
    d.incarnation    = r.values[0];
    const auto rally = own.storage_policy(policy::operation::manual_rally, player, uint32_t(unit_index));
    d.rally          = rally.values[0] && r.values[3] && rally.values[1] == r.values[1] &&
              rally.values[2] == r.values[2];
    d.x                   = rally.values[1];
    d.y                   = rally.values[2];
    const unit_storage &s = storage_of(v, player, d.storage_slot);
    d.native_x            = static_cast<uint8_t>(s.exit_tile_x);
    d.native_y            = static_cast<uint8_t>(s.exit_tile_y);
    d.queued              = u.order_queued;
    d.default_order       = v.cfg_units[u.unit_proto_id].default_op_code;
    d.move_state          = v.cfg_units[u.unit_proto_id].move_op_arg;
    d.receiver_valid      = true;
    return d;
}
} // namespace

bool storage_bulk_force_disembark(const sim_view &v, sim_store &own,
                                  const storage_cancel_pending_docked_calls &c,
                                  uint32_t player, int32_t building_index, int32_t unit_index) {
    player            = static_cast<uint16_t>(player);
    const bool parked = bounded_unit(v, player, unit_index) &&
                        unit_of(v, player, unit_index).state == 0x1f;
    const departure d = snapshot(v, own, player, building_index, unit_index);
    if (d.receiver_valid) own.storage_policy(policy::operation::cancel, player, uint32_t(unit_index));

    // The native/promoted force body has no outward calls and writes ONLY state/order/goal_x/y.
    // Consequently there is no unit retirement/reallocation or later-order application between
    // this snapshot and commit. The receiver incarnation is checked through the same policy owner.
    c.unit_force_disembark(player, unit_index);
    const bool accepted = parked && bounded_unit(v, player, unit_index) &&
                          unit_of(v, player, unit_index).state == 0x20;
    if (!accepted || !d.rally || !receiver_matches(v, player, building_index, unit_index, d)) return accepted;
    const auto current = own.storage_policy(policy::operation::read_rally, player, uint32_t(building_index));
    if (current.values[0] != d.incarnation) return accepted;
    unit &u = own.unit_at(player, unit_index);
    // Do not clear order_queued or touch any explicit-order record. The exact native output
    // guards prevent decorating a replacement action, even with an adversarial test callback.
    if (u.order != d.default_order || u.goal_x != d.native_x || u.goal_y != d.native_y ||
        u.order_queued != d.queued || d.x > 0xff || d.y > 0xff ||
        !policy::is_rally_departure(d.move_state, d.move_state, d.default_order)) return accepted;
    u.order         = d.move_state;
    u.goal_x        = static_cast<uint8_t>(d.x);
    u.goal_y        = static_cast<uint8_t>(d.y);
    u.move_group_id = 0;
    return accepted;
}
} // namespace mh::sim::detail
