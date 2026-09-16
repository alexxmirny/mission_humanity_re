#pragma once
#include <cstdint>
#include <cstring>

#include "sim/resid/sim_session_begin_multi.h" // SIBLING (frontier-of-batch, already translated): detail::session_begin_multi
#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// are frontier originals (none are among the sim_resid sibling translations) EXCEPT
// llm_strat_session_begin_multi, which is the SIBLING reached directly via detail:: (see the header
// banner), not through this struct.
struct start_tutorial_calls {
    // SIMABI-DISPLAY: was `int32_t (*view_set_size_mode)(int32_t)`, the host entry
    // llm_view_set_size_mode @0x0044e47d. Now LIBMH_EVK_SCR_SET_DISPLAY_MODE on the screen channel
    // -- VOID, because the only thing libmh did with the return was store it into a cell that is
    // MF_VIEW-only and unhashed, and the value itself is an OS-granted resolution readback libmh
    // has no way to compute. The hosted sink keeps making that store. See the kind's own comment.
    void (*set_display_mode)(int32_t size_mode);
    uint32_t (*game_set_event)(uint32_t type); // game_SetEvent @0x00413a52 -- EV_HUD_REDRAW_ALL=0xa
    // Completes the ARMED fade transition. The original polled llm_ui_screen_fade_transition_tick
    // @0x004bc4af in a `while (tick() == 0)` spin; a libmh loop may not poll a host entry, so the
    // wait is one request and the host performs it.
    void (*fade_transition_run)();
    void (*ui_widget_list_draw)(int32_t list_id); // LIBMH_SCR_WGTL_* id, not an address (R4)
    void (*gfx_present_flip)();                   // llm_gfx_present_flip @0x0042644a
    int32_t (*tutorial_load_script)();            // llm_tutorial_load_script @0x004b9cc2
    // Lays out the hint widget from a UI sprite's metrics. Replaces llm_gfx_font_desc_for_flags
    // @0x004b6155 (return discarded), llm_gfx_sprite_width @0x004bc1b6 and
    // llm_gfx_ui_sprite_get_header_field2 @0x004bc203 plus the four assignments they fed: the
    // metrics have no other consumer and the hint widget is unhashed + host-relocatable.
    void (*tut_hint_layout)(int32_t sprite_id);
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_value); // utils_fill_data @0x004d1780
    uint32_t (*cfg_read_map_file)(map_header *hdr);                      // cfg_ReadMapFile @0x004a3b9d
    char *(*wide_to_short_str)(void *src, char *dst);                    // utils_wide_to_short_str @0x004cf331 -- WRITES A HASHED FIELD (player_desc[1].name), so it stays libmh-side; internalize rather than convert
    void (*ui_widget_list_center)(int32_t list_id);                      // LIBMH_SCR_WGTL_* id (R4)
    void (*strat_frame)();                                               // llm_strat_frame @0x0043ecfa -- called twice
};

const start_tutorial_calls &live_start_tutorial_calls();

namespace detail {

// llm_game_start_tutorial @0x004bafb1. Reads the two screen ids and the two localized name text
// pointers through `v`; writes the tutorial's own state, the shared UI chrome regions, the fixed
// player setup and the injected planet slot through `own`; reaches every frontier callee through
// `c`; and reaches the sibling llm_strat_session_begin_multi directly, forwarding its own trailing
// `c_*` tables transitively (rule 3c -- session_begin_multi's closure is itself three deep). Always
// returns 1 (see the header's RETURN note).
int32_t start_tutorial(
    const sim_view &v, sim_store &own, const start_tutorial_calls &c,
    const session_begin_multi_calls     &c_sbm  = live_session_begin_multi_calls(),
    const session_state_reset_calls     &c_ssr  = live_session_state_reset_calls(),
    const new_game_init_calls           &c_ngi  = live_new_game_init_calls(),
    const land_players_on_planet_calls  &c_lpop = live_land_players_on_planet_calls(),
    const planet_map_session_init_calls &c_pmsi = live_planet_map_session_init_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_start_tutorial_calls().
int32_t start_tutorial();

} // namespace mh::sim
