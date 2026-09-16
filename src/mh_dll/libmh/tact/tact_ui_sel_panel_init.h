#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The four outward calls this body still makes, indirected per Law 3b so both the live game and
// the offline harness can drive it. It was ELEVEN until LIFT-TACT slice A: the seven that are gone
// -- utils_sprintf__vs, GetResourseFilePtr, convert_565_to_555, set_draw_surface, font_select,
// w_sprintf__v, pack_rgb16, text_draw_rgb16 -- were the whole draw head, and they moved host-side
// together as one scope rather than one at a time, because a per-primitive record would have put
// pixel geometry and a raw resource POINTER on an abstract boundary R4 exists to keep them off.
struct ui_sel_panel_init_calls {
    // LIBMH_EVK_INV_TACT_SEL_PANEL_INIT: the icon bank's load + 565->555 conversion, the three
    // fixed background panels, both font selections and both labels, as one scope.
    void (*sel_panel_init_draw)();
    void (*selection_panel_refresh)();                  // llm_tact_selection_panel_refresh @0x00434af7
    void (*sel_panel_draw)();                           // LIBMH_EVK_INV_TACT_SEL_PANEL (kind 13)
    void (*draw_player_row_list)(int32_t selected_row); // LIBMH_EVK_INV_TACT_PLAYER_ROWS (kind 9)
};

// The icon-name stride, the two G_TEXT_PTRS label indices and the "panelb\%s.gfx" path format that
// used to live here moved to mh.dll's sink with the draws they parameterise
// (seams/host_event_sink.cpp, draw_sel_panel_init) -- they are panel layout, and layout is the
// host's. They are not duplicated here.

const ui_sel_panel_init_calls &live_ui_sel_panel_init_calls();

namespace detail {

// llm_tact_ui_sel_panel_init @0x00433e94. See this header's own SHAPE banner for the two phases.
void ui_sel_panel_init(const tact_view &v, tact_store &own, const ui_sel_panel_init_calls &c);

} // namespace detail

void ui_sel_panel_init();

// Declared here per the module convention; DEFINED in tact_ui_sel_panel_init.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
