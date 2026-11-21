#include "strategic_input_native.h"
#include "order_tasks_hosted.h"
#include "ui_storage_controls.h"
#include "storage_policy.h"
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "state/region_runtime.h"
#include "ui/target_line_intents.h"
#include "ui/target_line_attack_provenance.h"
#include "mh_order_witness.h"
#include <cstring>
#include "net_internal.h"

namespace mh::seams::strategic_input_native {
namespace b = mh::strategic_bindings;
namespace q = mh::order_tasks;
namespace {
using namespace mh::game;
using namespace mh::state;
template <class T>
T read(uintptr_t p) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof v);
    return v;
}
template <class T>
void                      store(uintptr_t p, T v) { std::memcpy(reinterpret_cast<void *>(p), &v, sizeof v); }
uint32_t                  pending_sequence[8]{};
q::world_key              pending_world{};
mh::order_witness::store *witnesses = nullptr;
struct physical_edges {
    bool left_down, right_down, left_up, right_up;
};
physical_edges current_edges{};
bool           command_pressed      = false;
bool           edit_press_available = false;
// A semantic command click owns these clocks, independent of the physical left/right choice.
double last_command = 0, command_delta = 0;
bool   have_command = false;
void   record_command_press() {
    const double now = *ptr<const double>(RID_CURRENT_GAME_TIME);
    command_delta    = have_command ? now - last_command : 1e30;
    last_command     = now;
    have_command     = true;
}
struct hit {
    b::target type  = b::target::terrain;
    uint32_t  owner = 0, slot = 0, selector = 0, sub = 0;
};
uint32_t                        player() { return *ptr<const uint16_t>(RID_PLAYERSIDE); }
mh_llm_strat_ctrl_group        *selected() { return ptr<mh_llm_strat_ctrl_group>(RID_STRAT_CTRL_GROUPS); }
const mh_cfg_final_struct_Unit *unit_cfg(uint32_t side, uint32_t slot) {
    const auto &cap = mh::caps::g(mh::caps::UNITS);
    if (side >= cap.players || !slot || slot >= cap.limit) return nullptr;
    const auto &u   = *mh::caps::rec<const mh_map_object_unit>(mh::caps::UNITS, side, slot);
    const auto *cfg = ptr<const mh_cfg_final_struct_Unit>(RID_UNIT);
    if (!cfg || !u.unit_proto_id || u.unit_proto_id >= live_size(RID_UNIT) / sizeof(*cfg)) return nullptr;
    return &cfg[u.unit_proto_id];
}
uint32_t building_type(uint32_t side, uint32_t slot) {
    const auto &cap = mh::caps::g(mh::caps::BUILDINGS);
    if (side >= cap.players || !slot || slot >= cap.limit) return 0;
    const auto &building = *mh::caps::rec<const mh_map_object_building>(mh::caps::BUILDINGS, side, slot);
    const auto *cfg      = ptr<const mh_cfg_final_struct_Building>(RID_BUILDING);
    return cfg && building.building_id < live_size(RID_BUILDING) / sizeof(*cfg) ? cfg[building.building_id].type : 0;
}
bool find_enemy(b::surface surface, uint32_t x, uint32_t y, uint32_t selector, uint32_t &slot, uint16_t (&owner)[4]) {
    if (surface == b::surface::world) return mh::call::llm_hit_list_find_enemy_of_type(&slot, owner, uint16_t(selector)) != 0;
    return mh::call::llm_map_tile_find_enemy_of_type(x, y, &slot, owner, int16_t(selector)) != 0;
}
bool find_own(b::surface surface, uint32_t x, uint32_t y, uint32_t selector, uint32_t &slot) {
    if (surface == b::surface::world) {
        int32_t    result = 0;
        const bool found  = mh::call::llm_hit_list_find_own_of_type(&result, uint16_t(selector)) != 0;
        slot              = uint32_t(result);
        return found;
    }
    return mh::call::llm_map_tile_find_own_of_type(x, y, &slot, int16_t(selector)) != 0;
}
hit classify(b::surface surface, uint32_t x, uint32_t y) {
    hit out;
    // Fixed future terrain points are legal in fog; hidden object identities are not sampled.
    const auto *tiles = ptr<const uint8_t>(RID_TILE_OBJECTS);
    if (surface != b::surface::world && (!tiles || !tiles[((x << 8) | y) * 8 + 7])) return out;
    uint16_t       owner[4]{};
    uint32_t       slot        = 0;
    const uint32_t selectors[] = {0x20, 0x80, 0x40};
    for (uint32_t selector : selectors)
        if (find_enemy(surface, x, y, selector, slot, owner)) {
            out.owner    = owner[0];
            out.slot     = slot;
            out.selector = selector;
            if (selector == 0x40) {
                const uint32_t type = building_type(out.owner, slot);
                out.type            = (type == 0x0e || type == 0x22) ? b::target::enemy_main_base : b::target::enemy_building;
            } else out.type = b::target::enemy_unit;
            return out;
        }
    // Match native own-unit priority for the contextual merge/storage branches.
    if (find_own(surface, x, y, 0x80, slot)) {
        out = {b::target::own_unit, player(), slot, 0x80, 0};
        return out;
    }
    if (find_own(surface, x, y, 0x40, slot)) {
        out     = {b::target::own_building, player(), slot, 0x40, 0};
        out.sub = mh::call::llm_strat_get_vehicle_building_sub_id(player(), int32_t(slot));
        if (out.sub) out.type = b::target::own_storage;
        return out;
    }
    if (find_own(surface, x, y, 0x20, slot)) out = {b::target::own_unit, player(), slot, 0x20, 0};
    return out;
}
bool next_sequence(uint32_t side, uint32_t &out) {
    auto &state = q::hosted::owner();
    if (!state.valid() || side >= 8) return false;
    const auto world = state.world();
    if (world.system != pending_world.system || world.planet != pending_world.planet || world.incarnation != pending_world.incarnation) {
        std::memset(pending_sequence, 0, sizeof pending_sequence);
        pending_world = world;
    }
    const uint32_t canonical = state.command_high_water(side);
    const uint32_t last      = pending_sequence[side] > canonical ? pending_sequence[side] : canonical;
    if (last == UINT32_MAX) return false;
    out                    = last + 1;
    pending_sequence[side] = out;
    return true;
}
void stamp_native_sequence(uint32_t side) {
    auto *seq = mh::state::ptr<uint8_t>(mh::state::RID_STRAT_ORDER_SEQ_ID_BY_PLAYER);
    if (++seq[side] == 0) ++seq[side];
}
bool move_plain(uint32_t x, uint32_t y) {
    auto *group = selected();
    if (!group || group->count < 0 || group->count > 200) return false;
    if (!witnesses) return false;
    mh::order_witness::move_group command_scope(*witnesses);
    const uint32_t                side     = player();
    const uint32_t                modifier = (modifiers() & b::ctrl) ? 0 : 0x8000u | mh::state::ptr<const uint8_t>(mh::state::RID_STRAT_ORDER_SEQ_ID_BY_PLAYER)[side];
    bool                          issued   = false;
    // No call to the native deferred GROUP wrapper: it secretly samples physical Alt.
    // Per-unit native entries retain exactly the ordinary scheduling/status/MP-echo behavior.
    for (int32_t i = 0; i < group->count; ++i) {
        const uint32_t slot = group->unit_ids[i];
        const auto    *cfg  = unit_cfg(side, slot);
        if (!cfg || cfg->type == 0x17 || cfg->type == 0x18) continue;
        mh::call::llm_strat_unit_order_move(side, int32_t(slot), x, y, modifier);
        issued = true;
    }
    if (issued) mh::call::llm_strat_group_order_ack_voice();
    if (modifier) stamp_native_sequence(side);
    return issued;
}
bool attack_plain(const hit &target, bool forced) {
    auto *group = selected();
    if (!group || group->count < 0 || group->count > 200) return false;
    const uint32_t side   = player();
    bool           issued = false;
    mh::ui::selected_targets::detail::begin_attack_group(target.slot, target.owner, uint16_t(target.selector));
    for (int32_t i = 0; i < group->count; ++i) {
        const uint32_t slot = group->unit_ids[i];
        const auto    *cfg  = unit_cfg(side, slot);
        if (!cfg || cfg->type == 0x17 || cfg->type == 0x18 || (target.selector != 0x40 && target.owner == side && target.slot == slot)) continue;
        const uint8_t weapon = mh::call::llm_strat_unit_select_weapon(uint16_t(side), int32_t(slot), target.selector == 0x20 ? 2 : 1);
        if (weapon == 0x64) continue;
        if (target.selector == 0x40) mh::call::llm_strat_unit_order_attack_building_reposition(side, int32_t(slot), target.owner, int32_t(target.slot), weapon);
        else if (target.selector == 0x20) mh::call::llm_strat_unit_order_attack_target(side, int32_t(slot), target.owner, int32_t(target.slot), weapon);
        else if (!forced && held(b::shift_modifier) && !held(b::confirm)) mh::call::llm_strat_unit_order_attack_unit(side, int32_t(slot), target.owner, int32_t(target.slot), weapon);
        else if (held(b::patrol)) mh::call::llm_strat_unit_order_attack_target_alt(side, int32_t(slot), target.owner, int32_t(target.slot), 4);
        else mh::call::llm_strat_unit_order_attack_target(side, int32_t(slot), target.owner, int32_t(target.slot), 4);
        issued = true;
    }
    if (issued) mh::call::llm_strat_race_alert_sound_emit();
    mh::ui::selected_targets::detail::end_attack_group();
    return issued;
}
bool issue_edit(b::gesture gesture, const hit &target, uint32_t x, uint32_t y) {
    auto          &state = q::hosted::owner();
    auto          *group = selected();
    const uint32_t side  = player();
    if (!group || group->count <= 0 || group->count > 200) return false;
    uint32_t sequence = 0;
    if (!next_sequence(side, sequence)) {
        refused(unsigned(q::refusal::exhausted_identity), unsigned(group->count));
        return false;
    }
    const bool    patrol = gesture == b::gesture::patrol_replace || gesture == b::gesture::patrol_append;
    const bool    append = gesture == b::gesture::append || gesture == b::gesture::patrol_append;
    const auto    kind   = patrol ? q::action::patrol : target.type == b::target::enemy_unit                                                  ? q::action::attack_unit
                                                    : (target.type == b::target::enemy_building || target.type == b::target::enemy_main_base) ? q::action::attack_building
                                                                                                                                              : q::action::move;
    q::target_key objective{};
    if (kind == q::action::attack_unit || kind == q::action::attack_building) {
        objective.kind   = kind == q::action::attack_unit ? q::object_kind::unit : q::object_kind::building;
        objective.object = q::hosted::identity(nullptr, objective.kind, target.owner, target.slot);
        if (!objective.object.incarnation) {
            refused(unsigned(q::refusal::unavailable_target), unsigned(group->count));
            return false;
        }
    }
    const uint32_t formation = (modifiers() & b::ctrl) ? 0 : 0x8000u | mh::state::ptr<const uint8_t>(mh::state::RID_STRAT_ORDER_SEQ_ID_BY_PLAYER)[side];
    bool           admitted  = false;
    unsigned       missing   = 0;
    // Group zero is LOCAL capture only. Each resulting wire command has one explicit lifetime key.
    // Every member gets this SAME semantic gesture's canonical-highwater sequence.
    for (int32_t i = 0; i < group->count; ++i) {
        const uint32_t     slot  = group->unit_ids[i];
        const q::actor_key actor = q::hosted::identity(nullptr, q::object_kind::unit, side, slot);
        if (!actor.incarnation) {
            ++missing;
            continue;
        }
        const q::edit edit = {state.world(), actor, append ? q::edit_kind::append : q::edit_kind::replace, {kind, {x, y}, objective, formation, sequence}};
        int32_t       args[13]{};
        if (!q::encode_edit(edit, args)) {
            ++missing;
            continue;
        }
        mh::call::llm_strat_order_scratch_reset();
        for (int index = 0; index < 13; ++index) mh::call::llm_strat_order_scratch_set_field(index, args[index]);
        admitted = mh::call::llm_strat_order_dispatch(uint16_t(slot), side, q::COMMAND, q::COMMAND) != 0 || admitted;
    }
    if (formation) stamp_native_sequence(side);
    if (admitted) mh::call::llm_strat_group_order_ack_voice();
    if (missing) refused(unsigned(q::refusal::stale_actor), missing);
    return admitted;
}
void command(b::surface surface, uint32_t x, uint32_t y, int pointer_x = 0, int pointer_y = 0) {
    // A new semantic edit belongs to the physical press, not native sticky plain-command replay.
    const bool new_edit_gesture = edit_press_available;
    edit_press_available        = false;
    const auto &shape           = q::hosted::owner();
    if (x > 255 || y > 255 || !shape.valid()) return;
    const auto dims = shape.shape();
    if (x >= dims.width || y >= dims.height) return;
    const uint32_t side = player();
    if (side >= 8) return;
    bool   trace_double = false;
    double trace_window = surface == b::surface::bigmap ? *ptr<const double>(RID_GAME_SPEED) * read<double>(0x00500c5fu) : 0;
    // Restore the existing expanded-map submission log at the actual shared branch.
    // This records a native issuer call / successful queue admission, not executor completion.
    auto log_order = [&](const char *what, int count) {
        if (surface != b::surface::bigmap) return;
        char      line[320];
        const int dt = command_delta < 2147483.0 ? int(command_delta * 1000.0) : 2147483647;
        wsprintfA(line, "; BIGMAP: order at (%d,%d) -> tile (%u,%u): %s [group %d, double click %d, shift %d, ctrl %d, dt %d window %d]\n",
                  pointer_x, pointer_y, x, y, what, count, trace_double ? 1 : 0, (modifiers() & b::shift) ? 1 : 0,
                  (modifiers() & b::ctrl) ? 1 : 0, dt, int(trace_window * 1000.0));
        seam_log(line);
    };
    // Command-click placement cancellation wins. It does not fall through into an order.
    if (*ptr<const int32_t>(RID_BUILD_PLACEMENT_ID) != 0) {
        *ptr<int32_t>(RID_BUILD_PLACEMENT_ID) = 0;
        log_order("cancel placement, no order", 0);
        return;
    }
    const hit target = classify(surface, x, y);
    auto     *group  = selected();
    if (!group) {
        log_order("no order (nothing selected)", 0);
        return;
    }
    const uint32_t building = *ptr<const uint16_t>(RID_STRAT_UI_SELECTED_BLDG_INDEX);
    if (group->count == 0 && mh::storage_policy::hosted::eligible_building(side, building)) {
        uint32_t own_building = 0;
        if (find_own(surface, x, y, 0x40, own_building) && own_building == building) {
            mh::ui::storage_controls::clear_rally(side, building);
            log_order("clear rally", 0);
        } else {
            mh::ui::storage_controls::set_rally(side, building, x, y);
            log_order("set rally", 0);
        }
        return;
    }
    if (group->count == 0) {
        const uint32_t type = building_type(side, building);
        if ((type == 5 || type == 25) && target.owner != side && target.selector) {
            if (target.selector == 0x40) mh::call::llm_strat_turret_order_target_building(uint16_t(side), uint16_t(building), uint16_t(target.owner), target.slot);
            else mh::call::llm_strat_turret_order_target_unit(uint16_t(side), uint16_t(building), uint16_t(target.owner), target.slot);
            log_order(target.selector == 0x40 ? "turret target building" : "turret target unit", 0);
        } else log_order("no order (nothing selected)", 0);
        return;
    }
    // Native interaction windows differ by target/surface. Select by measured family, not one universal timeout.
    uintptr_t window = surface == b::surface::world ? 0x00500c47u : 0x00500c5fu;
    if (target.type == b::target::own_unit) window = surface == b::surface::world ? 0x00500c37u : 0x00500c4fu;
    else if (target.type == b::target::own_storage || target.type == b::target::own_building) window = surface == b::surface::world ? 0x00500c3fu : 0x00500c57u;
    else if (target.type == b::target::enemy_main_base) window = 0x00500c2fu;
    const bool       special = target.type == b::target::terrain || (target.type == b::target::own_unit && target.selector == 0x80) || target.sub || target.type == b::target::enemy_main_base;
    b::click_context click;
    click.where            = surface;
    click.hit              = target.type;
    click.physical         = b::button(button_mask(b::command_button));
    click.modifiers        = modifiers();
    click.special_eligible = special;
    // Native surfaces have already updated the physical button's timer pair.
    // Expanded-map ingress precedes that block and owns its semantic history.
    const bool   native_surface = surface != b::surface::bigmap;
    const bool   command_left   = button_mask(b::command_button) == 1;
    const double delta          = native_surface
                                      ? read<double>(command_left ? 0x005d0b9cu : 0x005d0bacu) -
                                   read<double>(command_left ? 0x005d0ba4u : 0x005d0bb4u)
                                      : command_delta;
    click.native_double         = delta < *ptr<const double>(RID_GAME_SPEED) * read<double>(window);
    click.alternate_attack      = held(b::patrol) && target.type == b::target::enemy_unit;
    const auto gesture          = strategic_input::classify_click(click);
    trace_double                = click.native_double;
    if (surface == b::surface::bigmap) trace_window = *ptr<const double>(RID_GAME_SPEED) * read<double>(window);
    if (gesture == b::gesture::reject_queued_variant) {
        if (new_edit_gesture) refused(unsigned(q::refusal::unsupported), unsigned(group->count));
        log_order("no order (unsupported queued variant)", group->count);
        return;
    }
    if (gesture == b::gesture::append || gesture == b::gesture::patrol_append || gesture == b::gesture::patrol_replace) {
        const bool admitted = new_edit_gesture && issue_edit(gesture, target, x, y);
        log_order(!admitted ? "no order (queue edit refused)" : gesture == b::gesture::append      ? "queue append"
                                                            : gesture == b::gesture::patrol_append ? "patrol append"
                                                                                                   : "patrol replace",
                  group->count);
        return;
    }
    if (gesture == b::gesture::special_confirm) {
        if (target.type == b::target::enemy_main_base) {
            mh::call::llm_strat_try_enter_tactical_mission(side, target.owner, target.slot);
            log_order("enter tactical mission", group->count);
        } else if (target.sub) {
            mh::call::llm_strat_group_issue_enter_building_order(target.sub, x, y);
            log_order("enter building", group->count);
        } else if (target.type == b::target::own_unit) {
            mh::call::llm_strat_group_issue_move_order(x, y);
            log_order("move (own unit tile, confirmed)", group->count);
        } else {
            mh::call::llm_strat_group_issue_move_order_confirmed(x, y);
            log_order("move (confirmed)", group->count);
        }
        return;
    }
    if (gesture == b::gesture::force_attack) {
        // Native friendly-fire intentionally visits all own hit classes in this order.
        bool issued = false;
        for (uint32_t selector : {0x20u, 0x80u, 0x40u}) {
            uint32_t slot = 0;
            if (find_own(surface, x, y, selector, slot)) issued = attack_plain({selector == 0x40 ? b::target::own_building : b::target::own_unit, side, slot, selector, 0}, true) || issued;
        }
        log_order(issued ? "force attack" : "no order (force attack unavailable)", group->count);
        return;
    }
    if (target.type == b::target::enemy_unit || target.type == b::target::enemy_building || target.type == b::target::enemy_main_base) {
        if (attack_plain(target, false)) {
            if (surface != b::surface::bigmap) return;
            char what[80];
            wsprintfA(what, "attack 0x%x target %u side %u", target.selector, target.slot, target.owner);
            log_order(what, group->count);
        } else log_order("no order (attack unavailable)", group->count);
    } else {
        const bool issued = move_plain(x, y);
        if (surface != b::surface::bigmap) return;
        const auto *tiles   = ptr<const uint8_t>(RID_TILE_OBJECTS);
        const bool  visible = tiles && tiles[((x << 8) | y) * 8 + 7];
        log_order(issued ? (visible ? "move (deferred)" : "move (deferred, tile not visible)") : "no order (move unavailable)", group->count);
    }
}
} // namespace
bool prepare_mouse() {
    witnesses = mh::ui::selected_targets::detail::selected_targets_order_witness_store();
    return witnesses != nullptr;
}
void cancel_pointer_gesture() {
    have_command = false;
    last_command = command_delta = 0;
    command_pressed              = false;
    edit_press_available         = false;
}
void reset_pending_commands() {
    std::memset(pending_sequence, 0, sizeof pending_sequence);
    pending_world = {};
    cancel_pointer_gesture();
    cancel_modal_gesture(true);
}
void command_tile(b::surface surface, uint32_t x, uint32_t y, int pointer_x, int pointer_y) {
    record_command_press();
    const bool previous  = edit_press_available;
    edit_press_available = true;
    command(surface, x, y, pointer_x, pointer_y);
    edit_press_available = previous;
}
void begin_mouse_pump() {
    current_edges        = {};
    command_pressed      = false;
    edit_press_available = false;
}
uint32_t physical_buttons() {
    // Native PREV is still the preceding record here. The later edge detector updates PREV
    // before mouse_frame, and its frame-local flags are sticky across the record-drain loop.
    const uint32_t previous = read<uint8_t>(0x00e5899bu);
    const uint32_t current  = uint32_t(mh::call::llm_input_mouse_buttons_get());
    const uint32_t pressed = current & ~previous, released = previous & ~current;
    command_pressed      = (pressed & button_mask(b::command_button)) != 0;
    edit_press_available = command_pressed;
    // Match native per-PUMP sticky flags exactly. They reset only at441ba0..441bac,
    // not when another mouse record (or a no-transition record) is drained.
    current_edges.left_down |= (pressed & 1u) != 0;
    current_edges.right_down |= (pressed & 2u) != 0;
    current_edges.left_up |= (released & 1u) != 0;
    current_edges.right_up |= (released & 2u) != 0;
    return current;
}
void mouse_frame(uint8_t *frame) {
    if (!frame) return;
    // Copy the independent physical accumulator, never the prior semantic locals. Preserve
    // native sticky per-pump processing, including no-transition/default-profile iterations.
    const auto edges = current_edges;
    if (command_pressed) record_command_press();
    frame[-0x30] = uint8_t(edges.left_down);
    frame[-0x28] = uint8_t(edges.right_down);
    frame[-0x24] = uint8_t(edges.left_up);
    frame[-0x20] = uint8_t(edges.right_up);
    if (!main_context_active()) return;
    const int32_t x = read<int32_t>(mh::addr::_G_LLM_CURSOR_X), y = read<int32_t>(mh::addr::_G_LLM_CURSOR_Y);
    const bool    world   = x >= 0 && y >= 0 && x <= *ptr<const int32_t>(RID_G_WIN_W) && y <= *ptr<const int32_t>(RID_G_WIN_H);
    const bool    minimap = !world && read<int32_t>(0x005d01e4u) != 0;
    if (!world && !minimap) return; // HUD/menu/text/ImGui/tactical physical events untouched.
    const bool left_down = edges.left_down, right_down = edges.right_down, left_up = edges.left_up, right_up = edges.right_up;
    const bool select_left  = button_mask(b::select_button) == 1;
    const bool command_left = button_mask(b::command_button) == 1;
    frame[-0x30]            = uint8_t(select_left ? left_down : right_down);
    frame[-0x24]            = uint8_t(select_left ? left_up : right_up);
    const bool command_down = command_left ? left_down : right_down;
    frame[-0x28]            = uint8_t(world && command_down);
    frame[-0x20]            = uint8_t(command_left ? left_up : right_up);
    // The old RMB-order ladders are skipped only for these contextual command edges. The normal
    // native placement/selection/drag-release code continues on the remapped LOCAL edge records.
    if (command_down && minimap)
        command(b::surface::minimap, read<uint32_t>(0x00e15e40u), read<uint32_t>(0x00e15e44u));
}
void mouse_world_order(uint8_t *frame) {
    if (!frame || !frame[-0x28]) return;
    frame[-0x28] = 0;
    store<uint8_t>(0x00e589adu, 2);
    if (auto *group = selected(); group && group->count > 0) {
        store<int32_t>(reinterpret_cast<uintptr_t>(frame - 0x40), 0x0c);
        store<int32_t>(0x005d0c40u, 5);
    }
    const int32_t wx = read<int32_t>(0x00e589a4u), wy = read<int32_t>(0x00e589a8u);
    if (wx >= 0 && wy >= 0) command(b::surface::world, uint32_t(wx >> 5), uint32_t(wy >> 5));
}
} // namespace mh::seams::strategic_input_native
