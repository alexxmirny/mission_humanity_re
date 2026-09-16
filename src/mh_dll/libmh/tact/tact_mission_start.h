#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The 14 outward calls this function makes, indirected for offline testability (translator brief
// 3b): a `detail::` body reaches every callee through this struct, never through `mh::call::`
// directly, so the offline oracle can mock every site (struct_array_malloc_impl's real allocation
// and utils_abort's real abort path both need mocking to keep the test hermetic).
struct mission_start_calls {
    void (*ui_info_media_draw_p1)();       // llm_ui_info_media_draw_p1 @0x004cac85
    void (*tact_frame_cursor_and_reset)(); // LIBMH_EVK_SCR_TACT_FRAME_PRESENT (slice B's kind --
                                           // this site shares the entry with llm_tact_frame, so it
                                           // converted with it rather than waiting for B2)
    // utils_sprintf @0x004cfb9c, __cdecl. Only the single-%d-substitution overload
    // (utils_sprintf__vi in mh_calls.gen.h) is exercised here; return value is read by nothing in
    // the .asm (no post-call EAX use) and is likewise discarded by the caller here.
    int32_t (*sprintf_int)(void *dst, const char *format, int32_t value);
    void (*tact_gfx_load_banks_alt)(); // llm_tact_gfx_load_banks_alt @0x00428d9e
    // struct_array_malloc_impl @0x004d013d, default convention (EAX=count, EDX=struct_size).
    void *(*struct_array_malloc)(uint32_t count, uint32_t struct_size);
    // utils_abort @0x004da944. Preserved as a REAL CALL on the allocation-failure path -- not
    // reshaped into a C++ exception or a process exit (translator brief / task hazard note).
    void (*abort_alloc_failure)(int32_t status);
    void (*gfx_apply_window_resolution)(int32_t width, int32_t height); // @0x00428a0f
    void (*gfx_view_metrics_init)();                                    // @0x0044e150
    void (*tact_view_metrics_init)();                                   // @0x00429776
    // llm_tact_map_reset @0x0042e713. FRONTIER callee: the original, even though a sibling
    // translation of this same function exists elsewhere in this batch (translator brief's
    // bit-independence rule) -- never call the sibling's C++, never #include its header.
    void (*tact_map_reset)(char *mission_name);
    void (*tact_ui_sel_panel_init)();       // @0x00433e94
    void (*tact_selection_panel_refresh)(); // @0x00434af7
    void (*snd_stop_all_channels)();        // @0x0042ef19
    void (*gfx_sprite_pix_offsets_init)();  // @0x0049ac8d
};

const mission_start_calls &live_mission_start_calls();

namespace detail {

// llm_tact_mission_start @0x004290ea. Instruction-order narrative (addresses are the .asm's):
//
// 1. @0x00429102-0x0042912a: zero mine_blast_time_end (double, two dword stores of 0 == 0.0 exactly)
//    and reset blast_marker_col/row to -1000 (0xfffffc18).
// 2. @0x0042912a-0x00429134: ui_info_media_draw_p1(); tact_frame_cursor_and_reset(); both zero-arg.
// 3. @0x00429134-0x0042917f: sprintf the mission filename into a 128-byte local buffer, keyed by the
//    squad-assault target owner's race: HUMAN(1) -> "poz%do.dat" % CurrentSystem, else ->
//    "poz%dl.dat" % CurrentSystem (see DECLARED NEED 4 above for the strat_players type and
//    DECLARED NEED 1/rule-17b note on the two format-string literals, which have no named symbol).
// 4. @0x0042917f-0x0042921a: writes a SPARSE subset of the 47-slot bank-sprite-base table -- indices
//    0-3, 5-14, 20. Indices 4 and 15-19 are never written by this function; transcribed literally,
//    not filled in as a contiguous run. Then tact_gfx_load_banks_alt() (zero-arg).
// 5. @0x0042921a-0x0042923e: allocate the 0x4b000-byte tile-height-sprite table
//    (struct_array_malloc(0x4b000, 1)), store the result into the pointer variable UNCONDITIONALLY,
//    THEN check it for null and call abort_alloc_failure(0) if so -- store-before-check, matching
//    instruction order exactly. No separate control-flow path follows the abort call in the
//    assembly (it falls straight into the next block), so this is translated as a bare `if`, not an
//    early return.
// 6. @0x0042923e-0x00429263: save the current window width, then force 640x480
//    (gfx_apply_window_resolution(0x280, 0x1e0)) only if it wasn't already 640 wide.
// 7. @0x00429263-0x0042929a: view-tile-height-px = 0x18; WindowWidth/Height forced to 0x280/0x1e0
//    unconditionally (regardless of step 6's branch); camera reset to (0,0); gfx_view_metrics_init()
//    (zero-arg).
// 8. @0x0042929a-0x004292c7: grid_width/grid_height forced to TACT_MAP_DIM (0x80) -- the mission's
//    top-left 128x128 sub-block, exactly as tact_state.h's own TACT_MAP_DIM comment already
//    documents this function as doing; view_tiles_w/h forced to 0x14; tact_view_metrics_init()
//    (zero-arg).
// 9. @0x004292c7-0x004292dc: tact_map_reset(mission_file); tact_ui_sel_panel_init();
//    tact_selection_panel_refresh() -- all using/after the sprintf'd buffer from step 3.
// 10. @0x004292dc-0x00429331: 480-row copy from gfx_draw_surface into gfx_framebuffer+0x3c0, 320
//     bytes/row, dest row stride 0x500. The Ghidra .c draft renders this as the REP MOVSD/MOVSB pair
//     it actually is -- a `memcpy(...,0x140)` plus a trailing `memcpy(...,0)`. The second call is
//     genuinely a no-op (CL = (0x140 & 3) == 0, i.e. the tail REP MOVSB runs zero iterations because
//     0x140 is a multiple of 4), not a second real copy; folded here into one memcpy per row.
// 11. @0x00429331-0x0042935d: cam_col_f/cam_row_f = (double)grid_cam_col/row -- READ AFTER the
//     gfx_view_metrics_init()/tact_view_metrics_init() calls in step 7-8 (an opaque outward call
//     could in principle have touched map_cam_col/row in between; the read is not cached from step
//     7's write, matching "const does not mean immutable" / do-not-hoist). click_action_taken = 0;
//     scroll_cmd = 0.
// 12. @0x0042935d-0x00429367: snd_stop_all_channels(); gfx_sprite_pix_offsets_init() -- both
//     zero-arg.
void mission_start(const tact_view &tv, tact_store &own, const mission_start_calls &c);

} // namespace detail

void mission_start();


} // namespace mh::tact
