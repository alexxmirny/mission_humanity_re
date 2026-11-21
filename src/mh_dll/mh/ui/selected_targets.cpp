#include "ui/selected_targets.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "addr/mh_calls.gen.h"
#include "addr/mh_structs.gen.h"
#include "en_guard.h"
#include "gfx/strat_world_view.h"
#include "hook/detour.h"
#include "hook/patch.h"
#include "include/mh_tile_dirty.h"
#include "state/region_runtime.h"
#include "ui/target_line_intents.h"
#include "ui/target_line_painter.h"
#include "ui/target_line_routes.h"
#include "seams/strategic_input.h"
#include "mh_strategic_bindings.h"
#include "ui/target_line_weapon_events.h"
#include "ui/target_line_deferred_events.h"
#include "ui/target_line_attack_events.h"
#include "ui/target_line_order_events.h"

namespace mh::ui::selected_targets {
namespace {
namespace sv = mh::gfx::stratview;
using namespace mh::state;
constexpr uintptr_t      ground_call  = 0x0044e5f1u;
const uintptr_t          ground_entry = 0x004a4ff4u;
bool                     armed        = false;
sv::rect                 previous{};
detail::route_collection routes;
detail::route_geometry   geometry;

void unite(sv::rect &into, const sv::rect &damage) {
    if (damage.right <= damage.left || damage.bottom <= damage.top) return;
    if (into.right <= into.left || into.bottom <= into.top) into = damage;
    else {
        into.left   = std::min(into.left, damage.left);
        into.top    = std::min(into.top, damage.top);
        into.right  = std::max(into.right, damage.right);
        into.bottom = std::max(into.bottom, damage.bottom);
    }
}
void __cdecl draw_impl() { draw(); }
__declspec(naked) void after_ground() {
    __asm {
        call dword ptr [ground_entry]
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        call draw_impl
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    }
}
} // namespace

void invalidate_view() {
    if (previous.right > previous.left && previous.bottom > previous.top)
        mh::gfx::mark_ground_tiles_dirty(previous.left, previous.top, previous.right - previous.left, previous.bottom - previous.top);
    // Imports may replace the dirty bytes themselves. Invalidate the current world
    // after publication; resolution changes do this without forgetting issued commands.
    const auto *w = ptr<const int32_t>(RID_G_WIN_W), *h = ptr<const int32_t>(RID_G_WIN_H);
    if (w && h && *w > 0 && *h > 0) mh::gfx::mark_ground_tiles_dirty(0, 0, *w, *h);
    previous = {};
}
void reset() {
    invalidate_view();
    detail::reset_intents();
}
void draw() {
    // The preceding ground pass consumed the damage stamped on the previous frame.
    previous         = {};
    const auto *mode = ptr<const uint8_t>(RID_GAME_MODE);
    const auto *side = ptr<const uint16_t>(RID_PLAYERSIDE);
    if (!mode || (*mode != 2 && *mode != 5) || !side || *side >= 8) return;
    const auto    *selected = ptr<const mh::game::mh_llm_strat_ctrl_group>(RID_STRAT_CTRL_GROUPS);
    const unsigned n        = selected ? static_cast<unsigned>(std::clamp(selected->count, 0, static_cast<int>(detail::selection_capacity))) : 0;
    const auto    *building = ptr<const uint16_t>(RID_STRAT_UI_SELECTED_BLDG_INDEX);
    if (!n && (!building || !*building)) return;
    sv::world_view view;
    sv::crop       native;
    if (!sv::read_world_view(view, sv::read_native_crop(native) ? &native : nullptr)) return;
    uint16_t ids[detail::selection_capacity];
    for (unsigned i = 0; i < n; ++i) ids[i] = selected->unit_ids[i];
    std::sort(ids, ids + n);
    sv::world_units mapped;
    detail::collect_routes(view, mapped, *side, ids, n, building ? *building : 0, routes);
    const bool reveal = mh::seams::strategic_input::role_held(mh::strategic_bindings::role_index::shift_modifier);
    if (!detail::build_route_geometry(view, routes, reveal, geometry)) return;
    const double milliseconds = mh::call::time_GetCurrentTime() * 1000.0;
    unite(previous, detail::paint_target_routes(view, geometry, milliseconds));
}
bool install() {
    if (armed) return true;
    if (!mh::en_build_ok() || mh::hook::promoted_owner_of(ground_call) || !detail::reserve_intents()) return false;
    constexpr uint8_t expected[5] = {0xe8, 0xfe, 0x69, 0x05, 0x00};
    if (std::memcmp(reinterpret_cast<const void *>(ground_call), expected, sizeof expected) != 0 ||
        !detail::install_weapon_observers() || !detail::install_deferred_observer() ||
        !detail::install_attack_observers() || !detail::install_order_mutation_observers()) return false;
    uint8_t       replacement[5] = {0xe8, 0, 0, 0, 0};
    const int32_t relative       = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&after_ground) - (ground_call + 5));
    std::memcpy(replacement + 1, &relative, sizeof relative);
    armed = mh::hook::patch_bytes_guarded(ground_call, expected, replacement, sizeof expected);
    return armed;
}
} // namespace mh::ui::selected_targets
