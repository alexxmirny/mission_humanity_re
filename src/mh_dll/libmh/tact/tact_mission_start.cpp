//
// tact/tact_mission_start.cpp -- see tact_mission_start.h.
// Translated from the DISASSEMBLY, not from Ghidra's C (the .c draft's control flow matches, but see
// the header banner's DECLARED NEEDS -- several globals it touches directly are not yet routed
// through tact_view/tact_store).
//
#include "tact/tact_mission_start.h"

#include <cstring> // std::memcpy -- CRT, called directly (not through the calls struct; see
                   // translator brief 9: memcpy is safe to call outside the mock boundary)

#include "addr/mh_calls.gen.h" // frontier callees
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const mission_start_calls &live_mission_start_calls() {
    static const mission_start_calls c = {
        mh::state::evt::scr_tact_mission_media,
        mh::state::evt::tact_frame_present,
        MH_CRT(utils_sprintf__vi),
        mh::state::evt::inv_tact_sprite_banks,
        MH_CRT(struct_array_malloc_impl),
        mh::tact_host().utils_abort,
        mh::tact_host().llm_gfx_apply_window_resolution,
        mh::state::evt::inv_gfx_view_metrics,
        mh::state::evt::inv_tact_view_metrics,
        MH_LIBMH_BIND(llm_tact_map_reset),
        MH_LIBMH_BIND(llm_tact_ui_sel_panel_init),
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        mh::state::evt::snd_stop_all_channels,
        mh::state::evt::inv_sprite_pix_offsets,
    };
    return c;
}

namespace {

// TU-local, matching the established project pattern for the still-unresolved `game_e_race` enum
// (no committed Ghidra enum exists yet -- see sim_game_get_starting_unit.h's RACE_HUMAN/RACE_ALIEN
// and sim_landing_spot.cpp's cross-checked copy; value cross-checked against both). Declared
// TU-local (not header-exported) per sim_landing_spot.h's own 2026-08-17 CONDUCTOR FIX note, which
// hit a same-TU redefinition when the identical constant was header-exported from two sim/ files.
constexpr uint32_t RACE_HUMAN = 1u;

// Byte-verified against the live .rdata (conductor, ReVA read-memory @0x00500232, 2026-08-26):
// `70 6F 7A 25 64 6F 2E 64 61 74 00` = "poz%do.dat\0" then "poz%dl.dat\0" at 0x0050023d --
// identical to these literals. The globals keep their Ghidra auto-labels (s_poz%do.dat_00500232 /
// s_poz%dl.dat_0050023d); baked TU-locally per rule 17b since no manifest symbol exists.
const char *const MISSION_FILE_FMT_HUMAN = "poz%do.dat"; // s_poz%do.dat_00500232
const char *const MISSION_FILE_FMT_ALIEN = "poz%dl.dat"; // s_poz%dl.dat_0050023d

} // namespace

namespace detail {

void mission_start(const tact_view &tv, tact_store &own, const mission_start_calls &c) {
    // @0x00429102-0x0042912a
    own.mine_blast_time_end() = 0.0;
    own.blast_marker_col()    = -1000;
    own.blast_marker_row()    = -1000;

    // @0x0042912a-0x00429134
    c.ui_info_media_draw_p1();
    c.tact_frame_cursor_and_reset();

    // @0x00429134-0x0042917f: mission filename keyed by the squad-assault target owner's race.
    // tv.strat_players is written here assuming the DECLARED NEED 4 retype (see header banner) --
    // the currently-committed `player_data` alias (mh_game_player_data) has no `.race` field at
    // this offset.
    char          mission_file[128];
    const int32_t target_owner = *tv.squad_bb_target_owner;
    if (tv.strat_players[target_owner].race == RACE_HUMAN) {
        c.sprintf_int(mission_file, MISSION_FILE_FMT_HUMAN, *tv.current_system);
    } else {
        c.sprintf_int(mission_file, MISSION_FILE_FMT_ALIEN, *tv.current_system);
    }

    // @0x0042917f-0x00429215: SPARSE writes -- indices 4 and 15-19 are never touched by the
    // original. Preserved literally, not filled in as a contiguous run.
    own.bank_sprite_base_at(0)  = 0;
    own.bank_sprite_base_at(1)  = 0x1e;
    own.bank_sprite_base_at(2)  = 100;
    own.bank_sprite_base_at(3)  = 0xbe;
    own.bank_sprite_base_at(5)  = 200;
    own.bank_sprite_base_at(6)  = 600;
    own.bank_sprite_base_at(7)  = 1000;
    own.bank_sprite_base_at(8)  = 0x578;
    own.bank_sprite_base_at(9)  = 2000;
    own.bank_sprite_base_at(10) = 0x9c4;
    own.bank_sprite_base_at(11) = 3000;
    own.bank_sprite_base_at(12) = 0xd48;
    own.bank_sprite_base_at(13) = 4000;
    own.bank_sprite_base_at(14) = 0x1194;
    own.bank_sprite_base_at(20) = 5000;
    // @0x00429215-0x0042921a
    c.tact_gfx_load_banks_alt();

    // @0x0042921a-0x0042923e: store the allocation result UNCONDITIONALLY first, exactly as the
    // .asm does (MOV to the global precedes the null test), then abort as a real CALL -- not a
    // C++ exception or process exit -- on failure. No separate control-flow path follows the abort
    // call in the assembly, so this stays a bare `if` with no early return.
    void *sprites                     = c.struct_array_malloc(0x4b000u, 1u);
    own.map_tile_height_sprites_ptr() = static_cast<uint16_t *>(sprites);
    if (sprites == nullptr) {
        c.abort_alloc_failure(0);
    }

    // @0x0042923e-0x00429263: remember the current width; force 640x480 only if not already 640
    // wide (the height is never checked, matching the original).
    own.saved_window_width() = own.window_width();
    if (own.window_width() != 0x280) {
        c.gfx_apply_window_resolution(0x280, 0x1e0);
    }

    // @0x00429263-0x0042929a: unconditional regardless of the branch above.
    own.gfx_view_tile_height_px() = 0x18;
    own.window_width()            = 0x280;
    own.window_height()           = 0x1e0;
    own.map_cam_col()             = 0;
    own.map_cam_row()             = 0;
    c.gfx_view_metrics_init();

    // @0x0042929a-0x004292c7: the mission runs on the top-left 128x128 sub-block (TACT_MAP_DIM),
    // exactly as tact_state.h's own TACT_MAP_DIM comment already documents this function as doing.
    own.grid_width()   = TACT_MAP_DIM;
    own.grid_height()  = TACT_MAP_DIM;
    own.view_tiles_w() = 0x14;
    own.view_tiles_h() = 0x14;
    c.tact_view_metrics_init();

    // @0x004292c7-0x004292dc
    c.tact_map_reset(mission_file);
    c.tact_ui_sel_panel_init();
    c.tact_selection_panel_refresh();

    // @0x004292dc-0x00429331: 480 rows, 320 bytes/row, gfx_draw_surface -> gfx_framebuffer+0x3c0
    // (dest row stride 0x500). The Ghidra .c draft's trailing `memcpy(...,0)` is the REP MOVSB tail
    // of the REP MOVSD/MOVSB pair the compiler emitted for one 0x140-byte block copy -- CL is
    // computed as (0x140 & 3) == 0, so that tail genuinely runs zero iterations every row. Folded
    // here into the one real memcpy; the second call is not reproduced because it copies nothing.
    for (int32_t row = 0; row < 0x1e0; ++row) {
        // *tv.gfx_draw_surface: the view binds the pointer VARIABLE (same shape as
        // map_tile_height_sprites); the deref is the live surface base.
        const uint8_t *src = *tv.gfx_draw_surface + static_cast<uint32_t>(row) * 0x140u;
        uint8_t       *dst = own.gfx_framebuffer() + 0x3c0 + static_cast<uint32_t>(row) * 0x500u;
        std::memcpy(dst, src, 0x140u);
    }

    // @0x00429331-0x0042935d: read AFTER the metrics-init calls above, not cached from the writes
    // in the previous block -- an opaque outward call could in principle have touched
    // map_cam_col/row in between.
    own.cam_col_f()          = static_cast<double>(own.map_cam_col());
    own.cam_row_f()          = static_cast<double>(own.map_cam_row());
    own.click_action_taken() = 0;
    own.scroll_cmd()         = 0;

    // @0x0042935d-0x00429367
    c.snd_stop_all_channels();
    c.gfx_sprite_pix_offsets_init();
}

} // namespace detail

void mission_start() {
    tact_state st = state();
    detail::mission_start(st.read, st.own, live_mission_start_calls());
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
