#include "seams/strategic_input_native.h"
// SEL1: observe unchanged native mouse events and wrap only completed selection calls.
#include "include/mh_unitselect_export.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "en_guard.h"
#include "gfx/strat_world_view.h"
#include "hook/patch.h"
#include "state/region_runtime.h"

namespace {
using namespace mh::game;
using namespace mh::state;
namespace view               = mh::gfx::stratview;
constexpr uintptr_t POP_SITE = 0x004266a5u, FINISH_SITE = 0x00443a1cu;
constexpr uintptr_t NATIVE_POP = 0x004d10f8u;
constexpr uintptr_t INTERVAL = 0x0065435eu, EPOCH = 0x00e654ecu;
constexpr uintptr_t ANCHOR_X = 0x00e5899cu, ANCHOR_Y = 0x00e589a0u;
constexpr uintptr_t WORLD_X = 0x00e589a4u, WORLD_Y = 0x00e589a8u;
constexpr uintptr_t SELECTED_BUILDING = 0x00e5813cu, CLICK_TARGET = 0x00e1629cu;
constexpr uint8_t   EXPECT_POP[5]    = {0xe8, 0x4e, 0xaa, 0x0a, 0x00};
constexpr uint8_t   EXPECT_FINISH[5] = {0xe8, 0x75, 0x25, 0x00, 0x00};
bool                g_armed          = false;
uintptr_t           g_pop            = NATIVE_POP;

template <class T>
T read(uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
    return value;
}
template <class T>
void write(uintptr_t address, T value) {
    std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
}
unsigned modifiers() {
    const bool ctrl  = mh::seams::strategic_input_native::held(mh::strategic_bindings::toggle_selection);
    const bool shift = mh::seams::strategic_input_native::held(mh::strategic_bindings::shift_modifier);
    return (ctrl ? 2u : 0u) | (shift ? 1u : 0u);
}
struct gesture {
    bool     down = false, released = false, moved = false;
    uint32_t x = 0, y = 0, timestamp = 0, epoch = 0;
    unsigned mods = 0, player = 0;
    int      planet = 0;
};
struct completed {
    bool     valid  = false;
    unsigned player = 0, id = 0, proto = 0, mods = 0;
    uint32_t timestamp = 0, epoch = 0;
    int      planet = 0;
};
gesture   g_gesture;
completed g_previous;

void reset() {
    g_gesture  = {};
    g_previous = {};
}
bool strategic() {
    const uint8_t mode = read<uint8_t>(mh::addr::_G_LLM_GAME_MODE);
    return mode == 2 || mode == 5;
}
void __cdecl observe(const mh_llm_input_mouse_event *event) {
    if (!g_armed || !event) return;
    mh::seams::strategic_input::note_input_record();
    if (!strategic()) {
        reset();
        return;
    }
    const unsigned player = *ptr<const uint16_t>(RID_PLAYERSIDE);
    const int      planet = *ptr<const int32_t>(RID_G_PLANET_INDEX);
    const uint32_t epoch  = read<uint32_t>(EPOCH);
    const unsigned mods   = modifiers();
    const int      w = *ptr<const int32_t>(RID_G_WIN_W), h = *ptr<const int32_t>(RID_G_WIN_H);
    const bool     inside = w > 0 && h > 0 && event->x < static_cast<unsigned>(w) && event->y < static_cast<unsigned>(h);
    if (g_previous.valid && (g_previous.player != player || g_previous.planet != planet || g_previous.epoch != epoch)) reset();
    const bool     left           = mh::seams::strategic_input_native::button_mask(mh::strategic_bindings::select_button) == 1;
    const uint32_t selection_down = left ? 2u : 8u, selection_up = left ? 4u : 16u;
    if (event->event_type & (left ? (8u | 16u) : (2u | 4u))) {
        reset();
        return;
    }
    if (event->event_type & selection_down) {
        // An intervening UI/uncompleted click cannot pair with an earlier world selection.
        if (g_gesture.down || g_gesture.released || !inside || (mods & 2)) reset();
        g_gesture = {};
        if (!inside || (mods & 2)) return;
        g_gesture.down      = true;
        g_gesture.x         = event->x;
        g_gesture.y         = event->y;
        g_gesture.timestamp = event->timestamp;
        g_gesture.epoch     = epoch;
        g_gesture.mods      = mods;
        g_gesture.player    = player;
        g_gesture.planet    = planet;
    }
    if (g_gesture.down && ((event->x != g_gesture.x || event->y != g_gesture.y) || mods != g_gesture.mods || !inside))
        g_gesture.moved = true;
    if (event->event_type & selection_up) {
        if (!g_gesture.down) {
            reset();
            return;
        }
        g_gesture.down     = false;
        g_gesture.released = true;
        // A completed click is timed by its consumed release event, including replay's original timestamp.
        g_gesture.timestamp = event->timestamp;
    }
}

bool eligible(const view::world_view &world, view::world_units &mapped, unsigned player, unsigned id, unsigned proto) {
    const auto &geometry = mh::caps::g(mh::caps::UNITS);
    if (!id || id >= geometry.limit || player >= geometry.players) return false;
    const auto    &u     = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, id);
    const auto    *cfg   = ptr<const mh_cfg_final_struct_Unit>(RID_UNIT);
    const unsigned count = read<uint32_t>(mh::addr::G_UNIT_COUNT_TOTAL);
    if (!cfg || !u.unit_proto_id || u.unit_proto_id > count || u.unit_proto_id >= REGIONS[RID_UNIT].size / sizeof(*cfg) ||
        u.unit_proto_id != proto || !(u.energy > 0) || (u.state >= 2 && u.state <= 4)) return false;
    // Native click-select 445e06 and drag_finish both reject the two cargo helicopter classes.
    // All selectable native categories are 1..22; 23/24 are A/H_HELI_CARGO.
    const unsigned type = cfg[u.unit_proto_id].type;
    if (!type || type >= 23) return false;
    view::unit_projection projection;
    return mapped.contains(world, player, id) && view::project_unit(world, player, id, projection) && view::visible_body(world, projection);
}
unsigned clicked_unit() {
    int32_t id = 0;
    // Same priority/order as native drag_finish's click arm; find_own scans hits back-to-front.
    if (mh::call::llm_hit_list_find_own_of_type(&id, 0x20) || mh::call::llm_hit_list_find_own_of_type(&id, 0x80))
        return id > 0 ? static_cast<unsigned>(id) : 0;
    // Native no-hit fallback permits a transformed own unit occupying the cursor's tile.
    if (read<int32_t>(0x00e153fcu) != 0) return 0;
    const auto *tiles = ptr<const mh_map_tile_object_data>(RID_TILE_OBJECTS);
    const int   x = read<int32_t>(WORLD_X), y = read<int32_t>(WORLD_Y);
    if (!tiles || x < 0 || y < 0) return 0;
    const auto &tile = tiles[((x >> 5) << 8) | (y >> 5)];
    return tile.class_owner == (0x80 | *ptr<const uint16_t>(RID_PLAYERSIDE)) ? tile.building : 0;
}

int __cdecl finish() {
    if (!g_armed) return mh::call::llm_strat_selection_drag_finish();
    const gesture click   = g_gesture;
    g_gesture             = {};
    const unsigned player = *ptr<const uint16_t>(RID_PLAYERSIDE), mods = modifiers();
    const bool     non_drag = read<int32_t>(ANCHOR_X) == read<int32_t>(WORLD_X) && read<int32_t>(ANCHOR_Y) == read<int32_t>(WORLD_Y);
    if (!strategic() || !click.released || click.moved || !non_drag || (mods & 2) || mods != click.mods || click.player != player ||
        click.planet != *ptr<const int32_t>(RID_G_PLANET_INDEX) || click.epoch != read<uint32_t>(EPOCH)) {
        g_previous = {};
        return mh::call::llm_strat_selection_drag_finish();
    }
    const unsigned id       = clicked_unit();
    const auto    &geometry = mh::caps::g(mh::caps::UNITS);
    if (player >= geometry.players || !id || id >= geometry.limit) {
        g_previous = {};
        return mh::call::llm_strat_selection_drag_finish();
    }
    const auto       &unit = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, id);
    view::crop        fallback;
    view::world_view  world;
    view::world_units mapped;
    const bool        have_fallback = view::read_native_crop(fallback);
    if (!view::read_world_view(world, have_fallback ? &fallback : nullptr) || !eligible(world, mapped, player, id, unit.unit_proto_id)) {
        g_previous = {};
        return mh::call::llm_strat_selection_drag_finish();
    }
    const completed now   = {true, player, id, unit.unit_proto_id, mods, click.timestamp, click.epoch, click.planet};
    const bool      twice = g_previous.valid && g_previous.player == player && g_previous.id == id && g_previous.proto == unit.unit_proto_id &&
                       g_previous.mods == mods && g_previous.epoch == click.epoch && g_previous.planet == click.planet &&
                       static_cast<uint32_t>(click.timestamp - g_previous.timestamp) < read<uint32_t>(INTERVAL);
    g_previous = now;
    if (!twice) return mh::call::llm_strat_selection_drag_finish();
    // Do not turn the third click of a triple-click into another double-click.
    g_previous  = {};
    auto *group = ptr<mh_llm_strat_ctrl_group>(RID_STRAT_CTRL_GROUPS);
    if (!group || group->count < 0 || group->count > 200) return mh::call::llm_strat_selection_drag_finish();
    uint16_t result[200];
    int      count = 0;
    uint32_t added[128]{};
    auto     append = [&](unsigned candidate) {
        if (count == 200 || !candidate || candidate >= 4096 || (added[candidate >> 5] & (1u << (candidate & 31)))) return;
        result[count++] = static_cast<uint16_t>(candidate);
        added[candidate >> 5] |= 1u << (candidate & 31);
    };
    if (mods & 1)
        for (int i = 0; i < group->count; ++i) append(group->unit_ids[i]);
    append(id);
    for (unsigned candidate = 1; candidate < geometry.limit && count < 200; ++candidate)
        if (candidate != id && eligible(world, mapped, player, candidate, unit.unit_proto_id)) append(candidate);
    // Construct the entire bounded result before mutating the selection or emitting native notifications.
    // Group zero's roster is local. The native helpers retain their existing select/flash dispatch ordering.
    if (!(mods & 1)) {
        for (int i = 0; i < group->count; ++i) mh::call::llm_strat_order_ctrlgrp_flash_member(static_cast<uint16_t>(player), group->unit_ids[i], 0);
        group->count = 0;
    }
    for (int i = group->count; i < count; ++i) mh::call::llm_strat_unit_ctrlgroup_add_member(result[i], &group->count, 0);
    write<uint16_t>(SELECTED_BUILDING, 0);
    write<uint16_t>(CLICK_TARGET, 0);
    mh::call::game_SetEvent(2); // PANEL_SHOW_INFO, same event as native click selection
    return group->count != 0;
}

// 4d10f8 preserves ECX/EDX/ESI/EDI; EAX is its only changed GP register. Observe
// after that call, retaining its actual EAX, flags and every other register.
__declspec(naked) void pop_thunk() {
    __asm {
        push eax
        call dword ptr [g_pop]
        pushfd
        pushad
        mov eax, dword ptr [esp + 36]
        push eax
        call observe
        add esp, 4
        popad
        popfd
        lea esp, [esp + 4]
        ret
    }
}
// Native 445f96 preserves EBX/ECX/EDX/ESI/EDI/EBP and returns bool in EAX.
// Caller immediately TESTs EAX, so flags are not an output contract.
__declspec(naked) void finish_thunk() {
    __asm {
        push ebx
        push ecx
        push edx
        push esi
        push edi
        call finish
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret
    }
}
void call_bytes(uint8_t (&bytes)[5], uintptr_t site, uintptr_t target) {
    bytes[0]          = 0xe8;
    const int32_t rel = static_cast<int32_t>(target - (site + 5));
    std::memcpy(bytes + 1, &rel, sizeof(rel));
}
} // namespace

extern "C" void MH_UnitSelect_Reset(void) { reset(); }
extern "C" int  MH_UnitSelect_Install(void) {
    if (g_armed) return 1;
    if (!mh::en_build_ok()) return 0;
    // Preflight BOTH before making either write. The body and its drag-direction patches stay intact.
    if (std::memcmp(reinterpret_cast<const void *>(POP_SITE), EXPECT_POP, 5) ||
        std::memcmp(reinterpret_cast<const void *>(FINISH_SITE), EXPECT_FINISH, 5)) return 0;
    uint8_t pop[5], completed_call[5];
    call_bytes(pop, POP_SITE, reinterpret_cast<uintptr_t>(&pop_thunk));
    call_bytes(completed_call, FINISH_SITE, reinterpret_cast<uintptr_t>(&finish_thunk));
    if (!mh::hook::patch_bytes_guarded(POP_SITE, EXPECT_POP, pop, 5)) return 0;
    if (!mh::hook::patch_bytes_guarded(FINISH_SITE, EXPECT_FINISH, completed_call, 5)) {
        mh::hook::patch_bytes_guarded(POP_SITE, pop, EXPECT_POP, 5);
        return 0;
    }
    reset();
    g_armed = true;
    return 1;
}
