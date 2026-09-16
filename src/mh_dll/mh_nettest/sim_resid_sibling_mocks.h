//
// sim_resid_sibling_mocks.h -- the sibling calls tables a sim_resid COMPOSITE oracle substitutes.
//
// WHAT A COMPOSITE ORACLE IS TESTING, and why this file is small. `planet_session_begin` calls
// `session_state_reset` calls `new_game_init`; `land_players_on_planet` calls
// `planet_map_session_init`. Every one of those edges is a DIRECT `detail::` C++ call, which is
// what G21 requires of a seam-internal edge -- so the sibling BODY runs for real, and that is the
// point: a composite oracle asserts the aggregate post-state of the whole closure, not of one
// frame. What must NOT run for real is the sibling's own OUTWARD calls into the original binary.
//
// Those reach `mh::call::` thunks -- `__declspec(naked)` jumps to absolute game VAs -- and
// `net_selftest.exe` maps no game image, so executing one is undefined: at best a fault, at worst
// silently running whatever bytes this much smaller image happens to have at that address. Until
// 2026-08-31 the sibling call sites bound `live_<sibling>_calls()` inside the `detail::` body, so
// there was nothing a test could substitute and all three of these oracles were unrunnable. They
// now take the table as a defaulted parameter (Option B); this file is what gets passed instead.
//
// RECORDING, NOT INERT. A stub that only returns is enough to stop the fault, and not enough to
// notice a sibling that stopped being called at all -- which is exactly the regression a composite
// oracle exists to catch. Every entry counts its calls and keeps the last arguments, so a case can
// assert the edge FIRED as well as what the post-state is.
//
// THE VALUES ARE THE ORIGINALS' OBSERVABLE CONTRACT, not arbitrary: `claim_landing_spot` returns 0
// (spot granted), `unit_create` returns a non-zero unit id, `get_starting_unit` returns a valid
// proto, `fill_data` returns its destination, `time_get_current_time` returns a fixed clock so a
// composite's clock writes are deterministic. Anything a case needs to vary, it varies through
// `sibling_rec()` before the call.
//
#pragma once
#include <cstdint>
#include <cstring>

#include "sim/resid/sim_land_players_on_planet.h"
#include "sim/resid/sim_new_game_init.h"
#include "sim/resid/sim_planet_map_session_init.h"
#include "sim/resid/sim_session_begin_multi.h"
#include "sim/resid/sim_session_state_reset.h"

namespace mh::sim::test {

// The one record every sibling table writes into. Reset it with `sibling_rec() = {};` at the top of
// a case -- the fixture's own reset() does not know about this file.
struct sibling_record {
    // session_state_reset_calls
    int    ssr_time_get_current_time    = 0;
    int    ssr_cam_jump_queue_clear     = 0;
    int    ssr_invasion_alert_reset_all = 0;
    int    ssr_prod_reset_system        = 0;
    double ssr_clock_value              = 1234.5; // what time_get_current_time hands back

    // new_game_init_calls
    int      ngi_clear_available_projects  = 0;
    int      ngi_player_profile_init       = 0;
    int      ngi_prod_shuttle_slot_release = 0;
    int      ngi_invasion_alert_reset_all  = 0;
    int      ngi_fill_data                 = 0;
    int      ngi_map_set_zoom_scale        = 0;
    void    *ngi_fill_data_last_ptr        = nullptr;
    uint32_t ngi_fill_data_last_size       = 0;

    // planet_map_session_init_calls
    int pmsi_rng_seed_channel         = 0;
    int pmsi_bldg_recompute_cell_grid = 0;
    int pmsi_ai_spiral_table_init     = 0;

    // land_players_on_planet_calls -- only the edges a composite asserts on; the row's OWN oracle
    // (sim_land_players_on_planet_selftest.cpp) mocks these itself and in more detail.
    int lpop_claim_landing_spot      = 0;
    int lpop_unit_create             = 0;
    int lpop_spawn_ai_base           = 0;
    int lpop_reroll                  = 0;
    int lpop_init_human_player_data  = 0;
    int lpop_cam_set_col             = 0;
    int lpop_cam_set_row             = 0;
    int lpop_cam_mark_viewport_dirty = 0;
    int lpop_get_starting_unit       = 0;
    int lpop_land_dmp_sprintf        = 0;
    int lpop_coord_msg_sprintf       = 0;

    // session_begin_multi_calls -- added 2026-09-01 for llm_game_start_tutorial's oracle, which is
    // the SECOND composite over this closure and the first one entered from ABOVE session_begin_multi.
    // Only the edges a parent asserts on are counted; the row's own oracle
    // (sim_session_begin_multi_selftest.cpp) mocks all sixteen itself and in far more detail.
    int sbm_scenario_planet_clone      = 0;
    int sbm_player_profile_init        = 0;
    int sbm_ui_message_queue_clear_all = 0;
    int sbm_map_read_map_pre           = 0;
    int sbm_diplomacy_init_multiplayer = 0;
    int sbm_menu_force_return_to_main  = 0;
    // The cfg blob the parent handed down -- llm_game_start_tutorial passes &current_map_data, and a
    // parent that passed something else would still count the call.
    const void *sbm_clone_last_blob = nullptr;
};

inline sibling_record &sibling_rec() {
    static sibling_record r;
    return r;
}

inline const session_state_reset_calls &mock_ssr_calls() {
    static const session_state_reset_calls c = {
        []() -> double {
            ++sibling_rec().ssr_time_get_current_time;
            return sibling_rec().ssr_clock_value;
        },
        []() { ++sibling_rec().ssr_cam_jump_queue_clear; },
        []() { ++sibling_rec().ssr_invasion_alert_reset_all; },
        []() { ++sibling_rec().ssr_prod_reset_system; },
    };
    return c;
}

inline const new_game_init_calls &mock_ngi_calls() {
    static const new_game_init_calls c = {
        []() { ++sibling_rec().ngi_clear_available_projects; },
        [](uint32_t, uint32_t, uint32_t, double, uint32_t, char *, int32_t) {
            ++sibling_rec().ngi_player_profile_init;
        },
        [](int32_t, int32_t) { ++sibling_rec().ngi_prod_shuttle_slot_release; },
        []() { ++sibling_rec().ngi_invasion_alert_reset_all; },
        [](void *ptr, uint32_t size, uint8_t default_) -> void * {
            ++sibling_rec().ngi_fill_data;
            sibling_rec().ngi_fill_data_last_ptr  = ptr;
            sibling_rec().ngi_fill_data_last_size = size;
            // Do the fill for real: new_game_init's fog-of-war clear is a WHOLE-REGION write the
            // composite may assert on, and a mock that skips it logs identically to one that works.
            if (ptr != nullptr && size != 0)
                memset(ptr, default_, size);
            return ptr;
        },
        [](double, double) { ++sibling_rec().ngi_map_set_zoom_scale; },
    };
    return c;
}

inline const planet_map_session_init_calls &mock_pmsi_calls() {
    static const planet_map_session_init_calls c = {
        // The pack_rgb16 mock is GONE (LIFT-TABLE S4, 2026-09-09) -- the nine-call palette block
        // moved host-side with its nine MF_VIEW cells, so this TU has no colour callee to mock.
        [](int32_t, uint32_t) { ++sibling_rec().pmsi_rng_seed_channel; },
        []() { ++sibling_rec().pmsi_bldg_recompute_cell_grid; },
        []() { ++sibling_rec().pmsi_ai_spiral_table_init; },
    };
    return c;
}

inline const land_players_on_planet_calls &mock_lpop_calls() {
    static const land_players_on_planet_calls c = {
        [](uint32_t, uint32_t) -> int32_t {
            ++sibling_rec().lpop_claim_landing_spot;
            return 0; // spot granted
        },
        [](uint32_t, uint32_t, uint16_t, uint16_t, uint8_t) -> uint32_t {
            ++sibling_rec().lpop_unit_create;
            return 1; // a valid unit id
        },
        [](int32_t, int32_t, int32_t, int32_t) { ++sibling_rec().lpop_spawn_ai_base; },
        []() { ++sibling_rec().lpop_reroll; },
        [](uint32_t, int32_t) { ++sibling_rec().lpop_init_human_player_data; },
        [](int32_t) { ++sibling_rec().lpop_cam_set_col; },
        [](int32_t) { ++sibling_rec().lpop_cam_set_row; },
        []() { ++sibling_rec().lpop_cam_mark_viewport_dirty; },
        [](uint32_t) -> int32_t {
            ++sibling_rec().lpop_get_starting_unit;
            return 1;
        },
        [](void *dst, const char *, const char *, const char *, int32_t, int32_t) -> int32_t {
            ++sibling_rec().lpop_land_dmp_sprintf;
            if (dst != nullptr)
                *static_cast<char *>(dst) = '\0';
            return 0;
        },
        [](void *dst, const wchar_t *, int32_t, int32_t) -> int32_t {
            ++sibling_rec().lpop_coord_msg_sprintf;
            if (dst != nullptr)
                *static_cast<wchar_t *>(dst) = L'\0';
            return 0;
        },
    };
    return c;
}

// The session_begin_multi table a parent composite substitutes. Same posture as the four above:
// every entry records, none reaches a `mh::call::` thunk. session_begin_multi itself runs FOR REAL
// under this -- that is the point of a composite -- so its own writes to PlayerSide, session_mode,
// the home-planet slot and the rest land in the fixture and are assertable by the parent.
inline const session_begin_multi_calls &mock_sbm_calls() {
    static const session_begin_multi_calls c = {
        [](void *blob) {
            ++sibling_rec().sbm_scenario_planet_clone;
            sibling_rec().sbm_clone_last_blob = blob;
        },
        [](uint32_t) {},
        [](uint8_t) {},
        []() { ++sibling_rec().sbm_ui_message_queue_clear_all; },
        [](uint32_t) { ++sibling_rec().sbm_map_read_map_pre; },
        []() {},
        [](uint32_t, uint32_t, uint32_t, double, uint32_t, char *, int32_t) {
            ++sibling_rec().sbm_player_profile_init;
        },
        []() { ++sibling_rec().sbm_diplomacy_init_multiplayer; },
        [](double) {},
        [](uint16_t) {},
        []() {},
        []() {},
        []() -> int32_t { return 0; }, // non-negative: the success branch
        []() { ++sibling_rec().sbm_menu_force_return_to_main; },
    };
    return c;
}

} // namespace mh::sim::test
