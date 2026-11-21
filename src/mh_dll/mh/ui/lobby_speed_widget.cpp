#include "ui/lobby_speed.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "addr/mh_addrs.gen.h"
#include "seams/lobby_speed.h"
#include "ui/lobby_ui.h"
#include "ui/player_strings.h"

namespace mh::ui {
namespace {

constexpr unsigned W_BYTES   = 0x44;
constexpr unsigned SP_BYTES  = 0x14;
constexpr unsigned WO_ACTION = 0x04, WO_FLAGS = 0x08, WO_X = 0x1c, WO_Y = 0x20,
                   WO_W = 0x24, WO_SPIN = 0x30, WO_LABEL = 0x38;
constexpr unsigned HIDDEN = 0x80, DISABLED = 0x40;
constexpr int      VALUES = 5;

alignas(4) std::uint8_t g_widget[W_BYTES];
alignas(4) std::uint8_t g_spinner[SP_BYTES];
wchar_t         g_text[VALUES][STR_MAX];
const wchar_t  *g_text_ptr[VALUES];
const wchar_t **g_cells[VALUES];
bool            g_ready = false;

std::int32_t &field(void *p, unsigned offset) {
    return *reinterpret_cast<std::int32_t *>(static_cast<std::uint8_t *>(p) + offset);
}

bool lobby_active() {
    return *reinterpret_cast<const std::uintptr_t *>(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST) == mh::addr::lobby_widget_origin;
}

void click_impl() {
    // Disabled rendering is not authority. A queued native callback must pass the same guards.
    if (!g_ready || !lobby_active() || !map_preview_lobby_settings_page() ||
        !*reinterpret_cast<const int *>(mh::addr::_G_LLM_NET_IS_HOST) ||
        !mh::seams::lobby_speed::editable()) return;
    if (mh::seams::lobby_speed::host_cycle()) {
        field(g_spinner, 0xc) = (mh::seams::lobby_speed::display_percent() - 100) / 25;
    }
}

// Same zero-argument Watcom callback contract as Mode: integer1 in EAX, every other register kept.
// clang-format off
__declspec(naked) int click_thunk() {
    __asm {
        pushad
        pushfd
        cld
        call click_impl
        popfd
        popad
        mov eax, 1
        ret
    }
}
// clang-format on

} // namespace

void lobby_speed_widget_init(const void *mode) {
    if (!mode) return;
    std::memcpy(g_widget, mode, W_BYTES);
    std::memset(g_spinner, 0, sizeof(g_spinner));
    for (int i = 0; i < VALUES; ++i) {
        _snwprintf_s(g_text[i], STR_MAX, _TRUNCATE, tr(Str::LOBBY_SPEED), 100 + i * 25);
        g_text_ptr[i] = g_text[i];
        g_cells[i]    = &g_text_ptr[i];
    }
    field(g_widget, WO_ACTION) = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(click_thunk));
    field(g_widget, WO_SPIN)   = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(g_spinner));
    field(g_widget, WO_LABEL)  = 0;
    // Both widgets keep the previous resolved draw origin/width. Speed follows Mode, not the pager:
    // x/y/width are deltas, so these are exactly the approved whole row21 px below Mode at any scale.
    field(g_widget, WO_X) = 0;
    field(g_widget, WO_Y) = 21;
    field(g_widget, WO_W) = 0;
    field(g_widget, WO_FLAGS) &= ~0x800000; // no flow: keep predecessor geometry
    field(g_spinner, 0) = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(g_cells));
    field(g_spinner, 4) = VALUES;
    g_ready             = true;
    lobby_speed_widget_tick();
}

void *lobby_speed_widget() {
    return g_ready ? g_widget : nullptr;
}

void lobby_speed_widget_tick() {
    if (!g_ready) return;
    field(g_spinner, 0xc) = (mh::seams::lobby_speed::display_percent() - 100) / 25;
    lobby_speed_widget_sync_flags();
}

void lobby_speed_widget_sync_flags() {
    if (!g_ready) return;
    std::int32_t &flags = field(g_widget, WO_FLAGS);
    const bool    page  = map_preview_lobby_settings_page();
    const bool    edit  = page && lobby_active() &&
                      *reinterpret_cast<const int *>(mh::addr::_G_LLM_NET_IS_HOST) != 0 &&
                      mh::seams::lobby_speed::editable();
    flags = page ? (flags & ~HIDDEN) : (flags | HIDDEN);
    flags = edit ? (flags & ~DISABLED) : (flags | DISABLED);
}

} // namespace mh::ui
