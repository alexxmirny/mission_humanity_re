#pragma once
#include <cstdint>

#include "tact/tact_state.h"
#include "state/host_api.h"

namespace mh::tact {

// Mockable callee seam. Every member is a frontier/CRT callee this body reaches (Law 3/3b) -- none
// of them is a migration member of this batch.
// The whole draw tail is ONE scope emit (LIFT-TACT L4b): `hud_readout_changed(active, cached)`
// stands where draw_surface + pack_rgb16 + sprintf_count + ansi_to_wide_scratch + text_draw x2 sit
// in the original. The two COUNTS cross the boundary; the icon blit, the format
// string, the anchor coordinates and the packed colour stay behind in mh.dll's sink
// (seams/host_event_sink.cpp draw_active_count_hud). See libmh_host_events.h kind 11 for why.
struct active_unit_count_hud_draw_calls {
    // mh::state::evt::inv_tact_active_count_hud -- LIBMH_EVK_INV_TACT_ACTIVE_COUNT_HUD.
    void (*hud_readout_changed)(int32_t active, int32_t cached);
    // llm_tact_vis_map_clear_right_margin @0x0043ac28 -- argumentless. Already an emit
    // (LIFT-NOTIFY); it stays SECOND, because the original clears the margin after the draws.
    void (*vis_map_clear_right_margin)();
};

const active_unit_count_hud_draw_calls &live_active_unit_count_hud_draw_calls();

namespace detail {

// llm_tact_active_unit_count_hud_draw @0x00434dc2. See the header banner above for the full
// derivation; every @-address comment in the .cpp cross-references it. `v` supplies the read-only
// globals (the roster, the draw surface + its pitch); `own` supplies both the write path (the two
// counters) and the one store-only read (`sel_panel_icon_gfx_at`, which has no view binding).
void active_unit_count_hud_draw(const tact_view &v, tact_store &own,
                                const active_unit_count_hud_draw_calls &c);

} // namespace detail

void active_unit_count_hud_draw();

// Declared here per the module convention; DEFINED in tact_active_unit_count_hud_draw.cpp, CALLED
// from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
