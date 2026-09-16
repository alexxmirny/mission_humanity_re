//
// tact_mission_start_selftest.cpp -- offline oracle for
//   llm_tact_mission_start @0x004290ea (libmh/tact/tact_mission_start.h)
//
// WHY OFFLINE, NOT RIG: see tact_mission_start.h's own "PROOF PATH: OFFLINE" banner -- this
// function's real callees include a 0x4b000-byte heap allocation (struct_array_malloc_impl) and a
// real abort() path (utils_abort), neither of which is safe to exercise for real in a selftest; all
// 14 outward calls are mocked via the header's own `mission_start_calls` table.
//
// This function's core behaviour is SEQUENCE, not just final state: 14 outward calls, an
// if/else branch (mission filename by race), a store-before-check allocation, a conditional
// resolution apply, and a post-call read of state a mocked callee can mutate. So every mock below
// both records into a SHARED call-order log and, where the spec calls for it, PEEKS or MUTATES the
// fixture's own fields at call-time -- proving order, not merely proving "it was called".
//
#include "tact/tact_mission_start.h"

#include <string>

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct mission_start_recorder {
    std::vector<std::string> call_order;

    // item 1: snapshot of the three blast/marker fields as OBSERVED at the very first outward call
    // (ui_info_media_draw_p1) -- proves the resets landed BEFORE any call, not merely by the time
    // the function returns.
    double  seen_mine_blast_time_end_at_first_call = -999.0;
    int32_t seen_blast_marker_col_at_first_call    = -999;
    int32_t seen_blast_marker_row_at_first_call    = -999;

    int32_t media_draw_p1_calls      = 0;
    int32_t frame_cursor_reset_calls = 0;

    int32_t                  sprintf_calls = 0;
    std::vector<std::string> sprintf_fmt;
    std::vector<int32_t>     sprintf_value;

    // item 3/8: snapshot of the 15 sparse bank_sprite_base writes as OBSERVED when
    // tact_gfx_load_banks_alt fires -- proves all 15 writes precede that call.
    std::vector<int32_t> seen_bank_sprite_base_at_load_banks; // indices 0-3,5-14,20, in that order
    int32_t              gfx_load_banks_alt_calls = 0;

    void                 *malloc_return_value = nullptr; // configured by each case before the call
    int32_t               malloc_calls        = 0;
    std::vector<uint32_t> malloc_count_args;
    std::vector<uint32_t> malloc_struct_size_args;

    int32_t              abort_calls = 0;
    std::vector<int32_t> abort_status_args;

    int32_t              apply_window_resolution_calls = 0;
    std::vector<int32_t> apply_window_resolution_width_args;
    std::vector<int32_t> apply_window_resolution_height_args;

    // item 5: snapshot of the unconditional writes (tile-height px, WindowWidth/Height, cam col/row)
    // as OBSERVED when gfx_view_metrics_init fires -- the call that immediately follows them, in
    // BOTH branches of the resolution-apply if.
    int32_t seen_gfx_view_tile_height_px_at_metrics = -999;
    int32_t seen_window_width_at_metrics            = -999;
    int32_t seen_window_height_at_metrics           = -999;
    int32_t seen_map_cam_col_at_metrics             = -999;
    int32_t seen_map_cam_row_at_metrics             = -999;
    int32_t gfx_view_metrics_init_calls             = 0;

    // item 7: tact_view_metrics_init's mock MUTATES map_cam_col/row to these values, so the LATER
    // cam_col_f/cam_row_f derivation is provably a post-call read rather than hoisted from the
    // (0,0) the function itself wrote just before gfx_view_metrics_init.
    int32_t mutate_map_cam_col           = 0;
    int32_t mutate_map_cam_row           = 0;
    int32_t tact_view_metrics_init_calls = 0;

    std::vector<std::string> map_reset_mission_names;

    int32_t ui_sel_panel_init_calls       = 0;
    int32_t selection_panel_refresh_calls = 0;
    int32_t snd_stop_all_channels_calls   = 0;
    int32_t sprite_pix_offsets_init_calls = 0;

    void reset() { *this = mission_start_recorder{}; }
};
mission_start_recorder g_rec;

// Set by each case to that case's own fixture. mission_start_calls holds RAW function pointers
// (translator brief 3b's mock boundary), so every mock below must be a captureless lambda -- it can
// only reach per-case state through a namespace-scope global, exactly like g_rec above. This is the
// mechanism items 1/5/7/8 need to prove ORDER (peek/mutate at call-time), not just final values.
tact_fixture *g_fx = nullptr;

const mission_start_calls &rec_calls() {
    static const mission_start_calls c = {
        // 1: llm_ui_info_media_draw_p1, 0x0042912a -- the FIRST outward call; its snapshot is item 1's proof.
        []() {
            g_rec.media_draw_p1_calls++;
            g_rec.call_order.push_back("ui_info_media_draw_p1");
            g_rec.seen_mine_blast_time_end_at_first_call = g_fx->mine_blast_time_end;
            g_rec.seen_blast_marker_col_at_first_call    = g_fx->blast_marker_col;
            g_rec.seen_blast_marker_row_at_first_call    = g_fx->blast_marker_row;
        },
        // 2: llm_tact_frame_cursor_and_reset, 0x0042912f.
        []() {
            g_rec.frame_cursor_reset_calls++;
            g_rec.call_order.push_back("tact_frame_cursor_and_reset");
        },
        // 3: utils_sprintf (single-%d overload), 0x0042915a (HUMAN) / 0x00429177 (else). Actually
        // formats into dst so the later tact_map_reset mock can capture the SAME resulting string.
        [](void *dst, const char *format, int32_t value) -> int32_t {
            g_rec.sprintf_calls++;
            g_rec.sprintf_fmt.push_back(format);
            g_rec.sprintf_value.push_back(value);
            g_rec.call_order.push_back("sprintf_int");
            return std::snprintf(static_cast<char *>(dst), 128, format, value);
        },
        // 4: llm_tact_gfx_load_banks_alt, 0x00429215 -- fires right after the 15 sparse writes.
        []() {
            g_rec.gfx_load_banks_alt_calls++;
            g_rec.call_order.push_back("tact_gfx_load_banks_alt");
            g_rec.seen_bank_sprite_base_at_load_banks = {
                g_fx->bank_sprite_base[0],
                g_fx->bank_sprite_base[1],
                g_fx->bank_sprite_base[2],
                g_fx->bank_sprite_base[3],
                g_fx->bank_sprite_base[5],
                g_fx->bank_sprite_base[6],
                g_fx->bank_sprite_base[7],
                g_fx->bank_sprite_base[8],
                g_fx->bank_sprite_base[9],
                g_fx->bank_sprite_base[10],
                g_fx->bank_sprite_base[11],
                g_fx->bank_sprite_base[12],
                g_fx->bank_sprite_base[13],
                g_fx->bank_sprite_base[14],
                g_fx->bank_sprite_base[20],
            };
        },
        // 5: struct_array_malloc_impl, 0x00429224. Returns the case's configured (possibly null) result.
        [](uint32_t count, uint32_t struct_size) -> void * {
            g_rec.malloc_calls++;
            g_rec.malloc_count_args.push_back(count);
            g_rec.malloc_struct_size_args.push_back(struct_size);
            g_rec.call_order.push_back("struct_array_malloc");
            return g_rec.malloc_return_value;
        },
        // 6: utils_abort, 0x00429239 -- real CALL on the null-allocation path (not a C++ exception).
        [](int32_t status) {
            g_rec.abort_calls++;
            g_rec.abort_status_args.push_back(status);
            g_rec.call_order.push_back("abort_alloc_failure");
        },
        // 7: llm_gfx_apply_window_resolution, 0x0042925e -- only when WindowWidth != 0x280.
        [](int32_t width, int32_t height) {
            g_rec.apply_window_resolution_calls++;
            g_rec.apply_window_resolution_width_args.push_back(width);
            g_rec.apply_window_resolution_height_args.push_back(height);
            g_rec.call_order.push_back("gfx_apply_window_resolution");
        },
        // 8: llm_gfx_view_metrics_init, 0x00429295 -- fires right after the FIRST unconditional-write
        // block (tile-height px, WindowWidth/Height, cam col/row reset to 0); item 5's snapshot point.
        []() {
            g_rec.gfx_view_metrics_init_calls++;
            g_rec.call_order.push_back("gfx_view_metrics_init");
            g_rec.seen_gfx_view_tile_height_px_at_metrics = g_fx->gfx_view_tile_height_px;
            g_rec.seen_window_width_at_metrics            = g_fx->window_width;
            g_rec.seen_window_height_at_metrics           = g_fx->window_height;
            g_rec.seen_map_cam_col_at_metrics             = g_fx->map_cam_col;
            g_rec.seen_map_cam_row_at_metrics             = g_fx->map_cam_row;
        },
        // 9: llm_tact_view_metrics_init, 0x004292c2 -- item 7's mutation point.
        []() {
            g_rec.tact_view_metrics_init_calls++;
            g_rec.call_order.push_back("tact_view_metrics_init");
            g_fx->map_cam_col = g_rec.mutate_map_cam_col;
            g_fx->map_cam_row = g_rec.mutate_map_cam_row;
        },
        // 10: llm_tact_map_reset, 0x004292cd -- captures the mission filename STRING (not just the
        // pointer), so item 2's format-string case is provably the same buffer sprintf just filled.
        [](char *mission_name) {
            g_rec.map_reset_mission_names.push_back(mission_name);
            g_rec.call_order.push_back("tact_map_reset");
        },
        // 11: llm_tact_ui_sel_panel_init, 0x004292d2.
        []() {
            g_rec.ui_sel_panel_init_calls++;
            g_rec.call_order.push_back("tact_ui_sel_panel_init");
        },
        // 12: llm_tact_selection_panel_refresh, 0x004292d7.
        []() {
            g_rec.selection_panel_refresh_calls++;
            g_rec.call_order.push_back("tact_selection_panel_refresh");
        },
        // 13: llm_snd_stop_all_channels, 0x0042935d.
        []() {
            g_rec.snd_stop_all_channels_calls++;
            g_rec.call_order.push_back("snd_stop_all_channels");
        },
        // 14: llm_gfx_sprite_pix_offsets_init, 0x00429362 -- the LAST call.
        []() {
            g_rec.sprite_pix_offsets_init_calls++;
            g_rec.call_order.push_back("gfx_sprite_pix_offsets_init");
        },
    };
    return c;
}

} // namespace

void run_mission_start_tests() {
    // T1: HUMAN race branch, a SUCCESSFUL allocation (no abort), and WindowWidth != 0x280 (so
    // gfx_apply_window_resolution IS called) -- the mainline path, 0x004290ea-0x00429367.
    {
        tact_fixture fx;
        g_fx = &fx;
        g_rec.reset();

        // item 1: sentinels distinct from both the reset targets AND each other -- a translation
        // that reset the wrong pair, or left one field alone, is caught either way.
        fx.mine_blast_time_end = 123.5;
        fx.blast_marker_col    = 111;
        fx.blast_marker_row    = 222;

        // item 2: HUMAN branch. target_owner nonzero so a translation hardcoding index 0 fails;
        // slot 0's race is the OPPOSITE of the target owner's, so a hardcoded-index-0 read would
        // select the wrong format string, not merely "a" format string.
        constexpr int32_t TARGET_OWNER      = 3;
        fx.squad_bb_target_owner            = TARGET_OWNER;
        fx.strat_players[TARGET_OWNER].race = 1; // RACE_HUMAN
        fx.strat_players[0].race            = 2; // opposite -- poisons a hardcoded index 0
        fx.current_system                   = 7;

        // item 3: every one of the 47 slots gets its OWN distinct sentinel, so a written slot's
        // exact expected value and a survivor slot's exact sentinel are both provable, not just
        // "changed" vs. "unchanged".
        for (int32_t i = 0; i < 47; ++i) fx.bank_sprite_base[i] = -(1000 + i);

        // item 5: sentinels for the unconditional writes, distinct from their targets; WindowWidth
        // != 0x280 so this case exercises the gfx_apply_window_resolution branch.
        fx.window_width            = 800;
        fx.window_height           = 480;
        fx.saved_window_width      = -1;
        fx.gfx_view_tile_height_px = 999;
        fx.map_cam_col             = 555;
        fx.map_cam_row             = 777;

        // item 7: distinct, non-symmetric, and distinguishable from the 0/0 the code itself writes
        // just before gfx_view_metrics_init.
        g_rec.mutate_map_cam_col = 4141;
        g_rec.mutate_map_cam_row = -2020;

        // item 4: a real (fake) non-null allocation result -- store-before-check, no abort.
        g_rec.malloc_return_value = reinterpret_cast<void *>(0xdeadbee0u);

        // item 6: a position-dependent byte pattern in the draw surface (so a copied row is
        // provably from the RIGHT source row, not zero/a neighbour), and an all-0xcd framebuffer
        // (so an untouched byte is provably untouched, not coincidentally equal to source).
        for (size_t i = 0; i < fx.draw_surface.size(); ++i)
            fx.draw_surface[i] = static_cast<uint8_t>(i * 7u + 13u);
        for (size_t i = 0; i < fx.framebuffer.size(); ++i) fx.framebuffer[i] = 0xcd;

        tact_store own = fx.store();
        tact_view  tv  = fx.view();
        detail::mission_start(tv, own, rec_calls());

        // --- item 1: the three resets landed BEFORE the first outward call ---
        ck_eq_d(g_rec.seen_mine_blast_time_end_at_first_call, 0.0,
                "T1: mine_blast_time_end already 0.0 at the first call (ui_info_media_draw_p1), "
                "0x00429102-0x0042910c before 0x0042912a");
        ck_eq((uint32_t)g_rec.seen_blast_marker_col_at_first_call, (uint32_t)-1000,
              "T1: blast_marker_col already -1000 at the first call, 0x00429116 before 0x0042912a");
        ck_eq((uint32_t)g_rec.seen_blast_marker_row_at_first_call, (uint32_t)-1000,
              "T1: blast_marker_row already -1000 at the first call, 0x00429120 before 0x0042912a");
        ck_eq_d(fx.mine_blast_time_end, 0.0, "T1: mine_blast_time_end == 0.0 after return, 0x00429102");
        ck_eq((uint32_t)fx.blast_marker_col, (uint32_t)-1000,
              "T1: blast_marker_col == -1000 after return, 0x00429116");
        ck_eq((uint32_t)fx.blast_marker_row, (uint32_t)-1000,
              "T1: blast_marker_row == -1000 after return, 0x00429120");

        ck_eq((uint32_t)g_rec.media_draw_p1_calls, 1u, "T1: ui_info_media_draw_p1 called once, 0x0042912a");
        ck_eq((uint32_t)g_rec.frame_cursor_reset_calls, 1u,
              "T1: tact_frame_cursor_and_reset called once, 0x0042912f");

        // --- item 2: HUMAN mission filename, correct owner index, correct value arg ---
        ck_eq((uint32_t)g_rec.sprintf_calls, 1u, "T1: sprintf called exactly once, 0x0042915a");
        if (g_rec.sprintf_calls == 1) {
            ck(g_rec.sprintf_fmt[0] == "poz%do.dat",
               "T1: race == RACE_HUMAN(1) at strat_players[target_owner] selects \"poz%do.dat\", "
               "0x0042913e-0x00429145 (JNZ not taken)");
            ck_eq((uint32_t)g_rec.sprintf_value[0], 7u,
                  "T1: sprintf value arg is *current_system, 0x00429147");
        }
        ck_eq((uint32_t)g_rec.map_reset_mission_names.size(), 1u,
              "T1: tact_map_reset called once, 0x004292cd");
        if (g_rec.map_reset_mission_names.size() == 1) {
            ck(g_rec.map_reset_mission_names[0] == "poz7o.dat",
               "T1: tact_map_reset receives the sprintf'd buffer \"poz7o.dat\" verbatim, "
               "0x004292c7-0x004292cd");
        }

        // --- item 3: the sparse bank_sprite_base writes, exact values + sentinel survivors ---
        static const int32_t kIdx[15] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20};
        static const int32_t kVal[15] = {0, 0x1e, 100, 0xbe, 200, 600, 1000,
                                         0x578, 2000, 0x9c4, 3000, 0xd48, 4000, 0x1194, 5000};
        for (int32_t k = 0; k < 15; ++k) {
            ck_eq((uint32_t)fx.bank_sprite_base[kIdx[k]], (uint32_t)kVal[k],
                  "T1: bank_sprite_base sparse write exact value, 0x0042917f-0x0042920b");
        }
        ck_eq((uint32_t)fx.bank_sprite_base[4], (uint32_t)-(1000 + 4),
              "T1: bank_sprite_base[4] untouched sentinel survives (never written), 0x0042917f-0x00429215");
        for (int32_t idx = 15; idx <= 19; ++idx) {
            ck_eq((uint32_t)fx.bank_sprite_base[idx], (uint32_t)-(1000 + idx),
                  "T1: bank_sprite_base[15..19] untouched sentinel survives, 0x0042917f-0x00429215");
        }
        ck_eq((uint32_t)fx.bank_sprite_base[21], (uint32_t)-(1000 + 21),
              "T1: bank_sprite_base[21] (past the last write, idx 20) untouched sentinel survives");
        ck_eq((uint32_t)fx.bank_sprite_base[46], (uint32_t)-(1000 + 46),
              "T1: bank_sprite_base[46] (table's last slot) untouched sentinel survives");
        ck_eq((uint32_t)g_rec.gfx_load_banks_alt_calls, 1u,
              "T1: tact_gfx_load_banks_alt called once, 0x00429215");
        if (g_rec.seen_bank_sprite_base_at_load_banks.size() == 15) {
            bool all_match = true;
            for (int32_t k = 0; k < 15; ++k)
                if (g_rec.seen_bank_sprite_base_at_load_banks[(size_t)k] != kVal[k]) all_match = false;
            ck(all_match,
               "T1: all 15 bank_sprite_base writes are visible BEFORE tact_gfx_load_banks_alt fires, "
               "0x0042917f-0x0042920b precede 0x00429215");
        }

        // --- item 4: allocation store-before-check, no abort on a successful allocation ---
        ck_eq((uint32_t)g_rec.malloc_calls, 1u, "T1: struct_array_malloc called once, 0x00429224");
        if (g_rec.malloc_calls == 1) {
            ck_eq(g_rec.malloc_count_args[0], 0x4b000u,
                  "T1: struct_array_malloc(count=0x4b000, ...), 0x0042921f");
            ck_eq(g_rec.malloc_struct_size_args[0], 1u,
                  "T1: struct_array_malloc(..., struct_size=1), 0x0042921a");
        }
        ck((void *)own.map_tile_height_sprites_ptr() == reinterpret_cast<void *>(0xdeadbee0u),
           "T1: the allocation result is stored into map_tile_height_sprites_ptr unconditionally, 0x00429229");
        ck_eq((uint32_t)g_rec.abort_calls, 0u,
              "T1: abort_alloc_failure NOT called on a successful allocation, 0x00429235-0x00429239");

        // --- item 5: resolution apply + the unconditional writes, observed AT gfx_view_metrics_init ---
        ck_eq((uint32_t)fx.saved_window_width, 800u,
              "T1: saved_window_width captures the PRE-value (800), 0x0042923e-0x00429243");
        ck_eq((uint32_t)g_rec.apply_window_resolution_calls, 1u,
              "T1: window_width(800) != 0x280 -> gfx_apply_window_resolution called, "
              "0x00429248-0x0042925e (JZ not taken)");
        if (g_rec.apply_window_resolution_calls == 1) {
            ck_eq((uint32_t)g_rec.apply_window_resolution_width_args[0], 0x280u,
                  "T1: gfx_apply_window_resolution(width=0x280, ...), 0x00429259");
            ck_eq((uint32_t)g_rec.apply_window_resolution_height_args[0], 0x1e0u,
                  "T1: gfx_apply_window_resolution(..., height=0x1e0), 0x00429254");
        }
        ck_eq((uint32_t)g_rec.gfx_view_metrics_init_calls, 1u,
              "T1: gfx_view_metrics_init called once, 0x00429295");
        ck_eq((uint32_t)g_rec.seen_gfx_view_tile_height_px_at_metrics, 0x18u,
              "T1: gfx_view_tile_height_px == 0x18 by the time gfx_view_metrics_init fires, "
              "0x00429263-0x00429295");
        ck_eq((uint32_t)g_rec.seen_window_width_at_metrics, 0x280u,
              "T1: WindowWidth forced to 0x280 unconditionally before gfx_view_metrics_init, 0x0042926d");
        ck_eq((uint32_t)g_rec.seen_window_height_at_metrics, 0x1e0u,
              "T1: WindowHeight forced to 0x1e0 unconditionally before gfx_view_metrics_init, 0x00429277");
        ck_eq((uint32_t)g_rec.seen_map_cam_col_at_metrics, 0u,
              "T1: map_cam_col reset to 0 unconditionally before gfx_view_metrics_init, 0x00429281");
        ck_eq((uint32_t)g_rec.seen_map_cam_row_at_metrics, 0u,
              "T1: map_cam_row reset to 0 unconditionally before gfx_view_metrics_init, 0x0042928b");

        ck_eq((uint32_t)fx.grid_width, (uint32_t)TACT_MAP_DIM, "T1: grid_width forced to 0x80, 0x0042929a");
        ck_eq((uint32_t)fx.grid_height, (uint32_t)TACT_MAP_DIM, "T1: grid_height forced to 0x80, 0x004292a4");
        ck_eq((uint32_t)fx.view_tiles_w, 0x14u, "T1: view_tiles_w forced to 0x14, 0x004292ae");
        ck_eq((uint32_t)fx.view_tiles_h, 0x14u, "T1: view_tiles_h forced to 0x14, 0x004292b8");
        ck_eq((uint32_t)g_rec.tact_view_metrics_init_calls, 1u,
              "T1: tact_view_metrics_init called once, 0x004292c2");

        // --- item 7: cam_col_f/cam_row_f reflect tact_view_metrics_init's MUTATION, not the earlier 0/0 ---
        ck_eq_d(fx.cam_col_f, 4141.0,
                "T1: cam_col_f == (double)map_cam_col AS MUTATED by tact_view_metrics_init "
                "(read-after-call, not hoisted), 0x00429331-0x00429337");
        ck_eq_d(fx.cam_row_f, -2020.0,
                "T1: cam_row_f == (double)map_cam_row AS MUTATED by tact_view_metrics_init "
                "(read-after-call, not hoisted), 0x0042933d-0x00429343");
        ck_eq((uint32_t)fx.click_action_taken, 0u, "T1: click_action_taken cleared, 0x00429349");
        ck_eq((uint32_t)fx.scroll_cmd, 0u, "T1: scroll_cmd cleared, 0x00429353");

        // --- item 6: the 480-row/0x140-byte framebuffer copy + the just-past-row-0 sentinel ---
        for (int32_t row : {0, 1, 100, 479}) {
            const uint8_t *src = fx.draw_surface.data() + (size_t)row * 0x140u;
            const uint8_t *dst = fx.framebuffer.data() + 0x3c0 + (size_t)row * 0x500u;
            ck(std::memcmp(dst, src, 0x140u) == 0,
               "T1: framebuffer row copy matches draw_surface row exactly, 0x004292dc-0x00429331");
        }
        ck_eq((uint32_t)fx.framebuffer[0x3c0 + 0x140], 0xcdu,
              "T1: framebuffer byte just past row 0's 0x140 bytes SURVIVES untouched (no overrun), "
              "0x00429320-0x0042932e");

        // --- item 8: the full recorded call order for this (HUMAN, malloc-ok, resolution-applied) path ---
        static const std::vector<std::string> kExpectedOrder = {
            "ui_info_media_draw_p1",
            "tact_frame_cursor_and_reset",
            "sprintf_int",
            "tact_gfx_load_banks_alt",
            "struct_array_malloc",
            "gfx_apply_window_resolution",
            "gfx_view_metrics_init",
            "tact_view_metrics_init",
            "tact_map_reset",
            "tact_ui_sel_panel_init",
            "tact_selection_panel_refresh",
            "snd_stop_all_channels",
            "gfx_sprite_pix_offsets_init",
        };
        ck(g_rec.call_order == kExpectedOrder,
           "T1: full call order matches the .asm, 0x0042912a-0x00429367 "
           "(no abort taken, resolution applied)");
    }

    // T2: ALIEN race branch, a FAILED allocation (abort called, execution continues), and
    // WindowWidth already == 0x280 (so gfx_apply_window_resolution is SKIPPED) -- the other half of
    // every branch T1 didn't take, with a distinct (fixture-rule) seed family throughout.
    {
        tact_fixture fx;
        g_fx = &fx;
        g_rec.reset();

        fx.mine_blast_time_end = -55.5;
        fx.blast_marker_col    = 4;
        fx.blast_marker_row    = 8;

        constexpr int32_t TARGET_OWNER      = 6; // distinct from T1's 3
        fx.squad_bb_target_owner            = TARGET_OWNER;
        fx.strat_players[TARGET_OWNER].race = 2;  // != RACE_HUMAN -> alien filename
        fx.strat_players[0].race            = 1;  // opposite -- poisons a hardcoded index 0
        fx.current_system                   = 12; // distinct from T1's 7

        for (int32_t i = 0; i < 47; ++i) fx.bank_sprite_base[i] = -(2000 + i); // distinct sentinel family

        fx.window_width            = 0x280; // already the tactical resolution -> apply is SKIPPED
        fx.window_height           = 111;
        fx.saved_window_width      = -1;
        fx.gfx_view_tile_height_px = 3;
        fx.map_cam_col             = 9;
        fx.map_cam_row             = 10;

        g_rec.mutate_map_cam_col = -808; // distinct, non-symmetric, distinct from T1's pair
        g_rec.mutate_map_cam_row = 909;

        g_rec.malloc_return_value = nullptr; // allocation FAILS this case

        for (size_t i = 0; i < fx.draw_surface.size(); ++i)
            fx.draw_surface[i] = static_cast<uint8_t>(i * 3u + 1u);                  // distinct pattern from T1
        for (size_t i = 0; i < fx.framebuffer.size(); ++i) fx.framebuffer[i] = 0x11; // distinct sentinel

        tact_store own = fx.store();
        tact_view  tv  = fx.view();
        detail::mission_start(tv, own, rec_calls());

        ck_eq_d(fx.mine_blast_time_end, 0.0, "T2: mine_blast_time_end == 0.0 after return, 0x00429102");
        ck_eq((uint32_t)fx.blast_marker_col, (uint32_t)-1000,
              "T2: blast_marker_col == -1000 after return, 0x00429116");
        ck_eq((uint32_t)fx.blast_marker_row, (uint32_t)-1000,
              "T2: blast_marker_row == -1000 after return, 0x00429120");

        // --- item 2: ALIEN mission filename, a different owner/current_system than T1 ---
        ck_eq((uint32_t)g_rec.sprintf_calls, 1u, "T2: sprintf called exactly once, 0x00429177");
        if (g_rec.sprintf_calls == 1) {
            ck(g_rec.sprintf_fmt[0] == "poz%dl.dat",
               "T2: race != RACE_HUMAN(1) at strat_players[target_owner] selects \"poz%dl.dat\", "
               "0x00429145 (JNZ taken) -> LAB_00429164");
            ck_eq((uint32_t)g_rec.sprintf_value[0], 12u,
                  "T2: sprintf value arg is *current_system, 0x00429164");
        }
        if (g_rec.map_reset_mission_names.size() == 1) {
            ck(g_rec.map_reset_mission_names[0] == "poz12l.dat",
               "T2: tact_map_reset receives the sprintf'd buffer \"poz12l.dat\" verbatim, "
               "0x004292c7-0x004292cd");
        }

        // --- item 4: allocation FAILS -- store still happens, abort called, no early return ---
        ck_eq((uint32_t)g_rec.malloc_calls, 1u, "T2: struct_array_malloc called once, 0x00429224");
        ck((void *)own.map_tile_height_sprites_ptr() == nullptr,
           "T2: a nullptr allocation result is STILL stored unconditionally "
           "(store precedes the null check), 0x00429229");
        ck_eq((uint32_t)g_rec.abort_calls, 1u, "T2: utils_abort called on a null allocation, "
                                               "0x0042922e-0x00429239 (JNZ not taken)");
        if (g_rec.abort_calls == 1) {
            ck_eq((uint32_t)g_rec.abort_status_args[0], 0u, "T2: abort_alloc_failure(0), 0x00429237");
        }
        // no separate control-flow path follows the abort call in the .asm -- everything after it
        // still runs exactly once, matching the header's "no early return" narrative.
        ck_eq((uint32_t)g_rec.seen_gfx_view_tile_height_px_at_metrics, 0x18u,
              "T2: gfx_view_tile_height_px == 0x18 despite the abort path, 0x00429263");
        ck_eq((uint32_t)g_rec.tact_view_metrics_init_calls, 1u,
              "T2: tact_view_metrics_init still called after the abort path, 0x004292c2");
        ck_eq((uint32_t)g_rec.ui_sel_panel_init_calls, 1u,
              "T2: tact_ui_sel_panel_init still called after the abort path, 0x004292d2");
        ck_eq((uint32_t)g_rec.selection_panel_refresh_calls, 1u,
              "T2: tact_selection_panel_refresh still called after the abort path, 0x004292d7");
        ck_eq((uint32_t)g_rec.snd_stop_all_channels_calls, 1u,
              "T2: snd_stop_all_channels still called after the abort path, 0x0042935d");
        ck_eq((uint32_t)g_rec.sprite_pix_offsets_init_calls, 1u,
              "T2: gfx_sprite_pix_offsets_init still called after the abort path, 0x00429362");

        // --- item 5: window already 0x280 -> gfx_apply_window_resolution is SKIPPED ---
        ck_eq((uint32_t)fx.saved_window_width, 0x280u,
              "T2: saved_window_width captures the PRE-value (0x280), 0x0042923e-0x00429243");
        ck_eq((uint32_t)g_rec.apply_window_resolution_calls, 0u,
              "T2: window_width(0x280) == 0x280 -> gfx_apply_window_resolution SKIPPED, "
              "0x00429248-0x00429252 (JZ taken)");

        // --- item 3: sparse writes again, with a different sentinel family ---
        static const int32_t kIdx[15] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20};
        static const int32_t kVal[15] = {0, 0x1e, 100, 0xbe, 200, 600, 1000,
                                         0x578, 2000, 0x9c4, 3000, 0xd48, 4000, 0x1194, 5000};
        for (int32_t k = 0; k < 15; ++k) {
            ck_eq((uint32_t)fx.bank_sprite_base[kIdx[k]], (uint32_t)kVal[k],
                  "T2: bank_sprite_base sparse write exact value (second seed family), "
                  "0x0042917f-0x0042920b");
        }
        ck_eq((uint32_t)fx.bank_sprite_base[4], (uint32_t)-(2000 + 4),
              "T2: bank_sprite_base[4] untouched sentinel survives, 0x0042917f-0x00429215");
        for (int32_t idx = 15; idx <= 19; ++idx) {
            ck_eq((uint32_t)fx.bank_sprite_base[idx], (uint32_t)-(2000 + idx),
                  "T2: bank_sprite_base[15..19] untouched sentinel survives, 0x0042917f-0x00429215");
        }

        // --- item 7: cam_col_f/cam_row_f again, distinct mutated values from T1 ---
        ck_eq_d(fx.cam_col_f, -808.0,
                "T2: cam_col_f == (double)map_cam_col AS MUTATED by tact_view_metrics_init, "
                "0x00429331-0x00429337");
        ck_eq_d(fx.cam_row_f, 909.0,
                "T2: cam_row_f == (double)map_cam_row AS MUTATED by tact_view_metrics_init, "
                "0x0042933d-0x00429343");

        // --- item 6: row-copy again, distinct pattern/sentinel, different sampled rows ---
        for (int32_t row : {0, 239, 479}) {
            const uint8_t *src = fx.draw_surface.data() + (size_t)row * 0x140u;
            const uint8_t *dst = fx.framebuffer.data() + 0x3c0 + (size_t)row * 0x500u;
            ck(std::memcmp(dst, src, 0x140u) == 0,
               "T2: framebuffer row copy matches draw_surface row exactly (second pattern), "
               "0x004292dc-0x00429331");
        }
        ck_eq((uint32_t)fx.framebuffer[0x3c0 + 0x140], 0x11u,
              "T2: framebuffer byte just past row 0's 0x140 bytes SURVIVES untouched, "
              "0x00429320-0x0042932e");

        // --- item 8: the full recorded call order for this (ALIEN, malloc-fail/abort,
        //     resolution-skipped) path -- abort present, gfx_apply_window_resolution absent ---
        static const std::vector<std::string> kExpectedOrder = {
            "ui_info_media_draw_p1",
            "tact_frame_cursor_and_reset",
            "sprintf_int",
            "tact_gfx_load_banks_alt",
            "struct_array_malloc",
            "abort_alloc_failure",
            "gfx_view_metrics_init",
            "tact_view_metrics_init",
            "tact_map_reset",
            "tact_ui_sel_panel_init",
            "tact_selection_panel_refresh",
            "snd_stop_all_channels",
            "gfx_sprite_pix_offsets_init",
        };
        ck(g_rec.call_order == kExpectedOrder,
           "T2: full call order matches the .asm, 0x0042912a-0x00429367 "
           "(abort taken, resolution skipped)");
    }
}

} // namespace mh::tact::test
