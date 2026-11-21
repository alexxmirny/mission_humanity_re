#include "seams/storage_bulk_rally.h"

#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "seams/storage_policy.h"

namespace mh::storage_policy::bulk_rally {
namespace {
using unit         = mh::game::mh_map_object_unit;
using building     = mh::game::mh_map_object_building;
using storage      = mh::game::mh_map_object_unit_storage;
using cfg_unit     = mh::game::mh_cfg_final_struct_Unit;
using cfg_building = mh::game::mh_cfg_final_struct_Building;

bool bounded(mh::caps::arr array, uint32_t player, int32_t index) {
    const auto &g = mh::caps::g(array);
    return player < g.players && index > 0 && static_cast<uint32_t>(index) < g.limit;
}
bool manual_bulk_family(uint8_t type) {
    // cfg_enum_E_BUILDING_TYPE: A/H barracks, garage, airfield, helipad (HUD groups 8/9).
    // Ports/mothers (group 10) and shuttles (group 11) retain their exact cargo/orbit semantics.
    return (type >= 0x07 && type <= 0x0a) || (type >= 0x1b && type <= 0x1e);
}
bool same_identity(identity a, identity b) {
    return a.incarnation != 0 && a.player == b.player && a.slot == b.slot &&
           a.incarnation == b.incarnation;
}
struct departure {
    identity unit_id{}, building_id{};
    uint16_t prototype = 0, building_prototype = 0;
    uint8_t  storage_slot = 0, native_x = 0, native_y = 0, queued = 0;
    uint16_t default_order = 0, move_state = 0;
    uint32_t x = 0, y = 0;
    bool     receiver_valid = false, rally = false;
};

bool receiver_matches(uint32_t player, int32_t unit_index, int32_t building_index,
                      const departure &d) {
    if (!bounded(mh::caps::UNITS, player, unit_index) ||
        !bounded(mh::caps::BUILDINGS, player, building_index) ||
        !bounded(mh::caps::UNIT_STORAGE, player, d.storage_slot)) return false;
    const auto &u = *mh::caps::rec<const unit>(mh::caps::UNITS, player, uint32_t(unit_index));
    const auto &b = *mh::caps::rec<const building>(mh::caps::BUILDINGS, player, uint32_t(building_index));
    const auto &s = *mh::caps::rec<const storage>(mh::caps::UNIT_STORAGE, player, d.storage_slot);
    return u.unit_proto_id == d.prototype && u.home_storage_slot == d.storage_slot &&
           b.index == building_index && b.building_id == d.building_prototype &&
           b.sub_id == d.storage_slot && s.b_index == building_index &&
           same_identity(d.unit_id, hosted::unit_identity(player, uint32_t(unit_index))) &&
           same_identity(d.building_id, hosted::building_identity(player, uint32_t(building_index)));
}

departure snapshot(uint32_t player, int32_t unit_index, int32_t building_index) {
    departure d;
    if (!bounded(mh::caps::UNITS, player, unit_index) ||
        !bounded(mh::caps::BUILDINGS, player, building_index)) return d;
    const auto &u  = *mh::caps::rec<const unit>(mh::caps::UNITS, player, uint32_t(unit_index));
    const auto &b  = *mh::caps::rec<const building>(mh::caps::BUILDINGS, player, uint32_t(building_index));
    const auto *cu = mh::state::ptr<const cfg_unit>(mh::state::RID_UNIT);
    const auto *cb = mh::state::ptr<const cfg_building>(mh::state::RID_BUILDING);
    if (u.state != 0x1f || !(u.energy > 0) || !cu || !cb || !u.unit_proto_id ||
        u.unit_proto_id >= mh::state::live_size(mh::state::RID_UNIT) / sizeof(cfg_unit) ||
        !b.building_id ||
        b.building_id >= mh::state::live_size(mh::state::RID_BUILDING) / sizeof(cfg_building) ||
        !manual_bulk_family(cb[b.building_id].type) ||
        !hosted::eligible_building(player, uint32_t(building_index))) return d;
    d.unit_id            = hosted::unit_identity(player, uint32_t(unit_index));
    d.building_id        = hosted::building_identity(player, uint32_t(building_index));
    d.prototype          = u.unit_proto_id;
    d.building_prototype = b.building_id;
    d.storage_slot       = u.home_storage_slot;
    if (!receiver_matches(player, unit_index, building_index, d)) return d;
    const auto &s    = *mh::caps::rec<const storage>(mh::caps::UNIT_STORAGE, player, d.storage_slot);
    d.native_x       = static_cast<uint8_t>(s.exit_tile_x);
    d.native_y       = static_cast<uint8_t>(s.exit_tile_y);
    d.queued         = u.order_queued;
    d.default_order  = cu[u.unit_proto_id].default_op_code;
    d.move_state     = cu[u.unit_proto_id].move_op_arg;
    d.receiver_valid = true;
    // The existing manual policy deliberately has no Auto/full-HP/queued-order gate.
    d.rally = hosted::manual_rally(player, uint32_t(unit_index), d.x, d.y);
    return d;
}
} // namespace

bool force_disembark(uint32_t player, int32_t unit_index, int32_t building_index) {
    // The native call site already MOVZXs player; preserve that contract for direct callers too.
    player            = static_cast<uint16_t>(player);
    const bool parked = bounded(mh::caps::UNITS, player, unit_index) &&
                        mh::caps::rec<const unit>(mh::caps::UNITS, player, uint32_t(unit_index))->state == 0x1f;
    const departure d = snapshot(player, unit_index, building_index);
    // Match explicit manual-order admission: revoke only uncommitted automatic eligibility.
    // This touches the policy record, NOT order_queued or the native explicit-order containers.
    if (d.receiver_valid) hosted::cancel_pending(player, uint32_t(unit_index));

    // Keep every native guard, default action and refusal. Never call the auto launch backend.
    mh::call::llm_unit_force_disembark(player, unit_index);
    const bool accepted = parked && bounded(mh::caps::UNITS, player, unit_index) &&
                          mh::caps::rec<const unit>(mh::caps::UNITS, player, uint32_t(unit_index))->state == 0x20;
    if (!accepted || !d.rally || !receiver_matches(player, unit_index, building_index, d)) return accepted;
    auto &u = *mh::caps::rec<unit>(mh::caps::UNITS, player, uint32_t(unit_index));
    // Decorate only the exact native commit. A later explicit/queued order must win, and no
    // deferred work here may resample a building rally after an accepted blocked-door departure.
    if (u.order != d.default_order || u.goal_x != d.native_x || u.goal_y != d.native_y ||
        u.order_queued != d.queued || d.x > 0xff || d.y > 0xff ||
        !is_rally_departure(d.move_state, d.move_state, d.default_order)) return accepted;
    u.order         = d.move_state;
    u.goal_x        = static_cast<uint8_t>(d.x);
    u.goal_y        = static_cast<uint8_t>(d.y);
    u.move_group_id = 0;
    return accepted;
}

__declspec(naked) void native_force_disembark_thunk() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push dword ptr [ebp - 20h]
        push edx
        push eax
        call force_disembark
        add esp, 12
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    }
}
} // namespace mh::storage_policy::bulk_rally
