//
// sim/resid/sim_map_fill_defaults.cpp -- see sim_map_fill_defaults.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/map_FillDefaults_0045603e.asm), the Ghidra .c being a draft.
//
#include "sim/resid/sim_map_fill_defaults.h"

#include <cstring> // std::memcpy -- the PRESERVE-BUG's straddling double store, see below

#include "addr/mh_calls.gen.h" // typed callables for the effectful/frontier originals we still call OUT to
#include "state/host_events.h" // bldg_panel_open is a SCREEN record (LIFT-SCREEN)
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const map_fill_defaults_calls &live_map_fill_defaults_calls() {
    static const map_fill_defaults_calls c = {
        mh::state::evt::bldg_panel_open,
        MH_CRT(utils_fill_data),
    };
    return c;
}

namespace detail {

// ---- map_FillDefaults @0x0045603e --------------------------------------------------------------
void map_fill_defaults(const sim_view &v, sim_store &own, const map_fill_defaults_calls &c) {
    // 0x00456056: outward UI call, no args, no return used -- the reason this unit is
    // offline-oracle-only (header banner).
    c.bldg_panel_open();

    // LIB-ABI stage E hoist -- the callee's LOAD-BEARING writes, re-stored AFTER the call so
    // libmh's value is the last writer under either host arm (real or no-op): the
    // AvailableBuildings memset-0 (@0x0041c0fb -- libmh reads AND writes this region since LT1B's
    // remove_from_available_buildings) and the 8 base-marker {-1,-1} resets (@0x0041c090-b2 --
    // the save driver decodes this block, and completion_dispatch writes index [0]). The row-state
    // scratch and the HUD_REDRAW_ALL event have no libmh reader and stay behind the host arm as
    // the visual remainder.
    //
    // THE PANEL MODE/PAGE STORES ARE NOT READER-FREE, and this comment said they were until
    // 2026-09-09 (LIFT-R3B). llm_ui_bldg_panel_open @0x0041c061 unconditionally writes
    // _G_LLM_STRAT_UI_PANEL_MODE = 1 and _G_LLM_STRAT_UI_PANEL_PAGE at its tail, and BOTH are read
    // back for real decisions: sim_game_set_event.cpp at 25+ sites (e.g. `if (own.ui_panel_mode()
    // == 1 && own.ui_panel_page() == 0)`) and sim_bldg_unmap_footprint.cpp:222/230/243 through the
    // read-only view (`*v.ui_panel_mode`, bound at sim_state.cpp:648-649), to decide whether to
    // fire a panel-refresh event. So this is an open R3b readback, not a visual remainder -- see
    // tools/data/notify_readback_dispositions.json for its disposition. std::memset, NOT c.fill_data: the fill
    // seam records this body's TRANSCRIBED fill sequence (simtest T1 pins it in original order),
    // and the hoist is stage-E behavior, not transcription -- it must not appear in that record.
    // R3b HOIST (LIFT-R3B, 2026-09-09) -- the pair the comment above used to call reader-free.
    // llm_ui_bldg_panel_open @0x0041c14b-0x0041c151 ends with PANEL_MODE = 1 and
    // PANEL_PAGE = (primary_mother_bldg[planet] == 0), and both are read back for real decisions
    // (sim_game_set_event.cpp, sim_bldg_unmap_footprint.cpp:222/230/243). Re-stored HERE so libmh
    // is the last writer under either host arm, exactly as the two clears below are. The
    // expression is the one game_SetEvent's EV_BUILD_OPEN_AUTOPAGE case already computes
    // (sim_game_set_event.cpp:170-171) -- the original reaches the same pair by both routes.
    own.ui_panel_mode() = 1;
    own.ui_panel_page() =
        (v.profiles[*v.player_side].primary_mother_bldg[*v.planet_index] == 0) ? 1 : 0;

    std::memset(own.available_buildings_row(0), 0, 0x640);
    for (int32_t m = 0; m < 8; ++m) {
        own.ui_base_marker_coords_at(m).cam_col = -1;
        own.ui_base_marker_coords_at(m).cam_row = -1;
    }

    // 0x0045605b-0x004561ff: 22 of the 25 `utils_fill_data(dest, len, 0)` bulk zero-fills in this
    // function (three more of the 25 -- MAP_OBJECTS/BLK_00CC46E0/BLK_00D06C80 -- have no sim_store
    // binding; see the header's declared_needs). Each `len`/fill-byte is transcribed straight off
    // the EBX/EDX register loads immediately preceding its CALL; base pointers are `&accessor(0[,0[,0]])`
    // rather than the raw VA the disassembly loads into EAX.
    c.fill_data(&own.power_stats_at(0), 0xc0, 0);         // 0x00456067 -> 0x00bf4fc0 _G_LLM_STRAT_POWER_STATS
    c.fill_data(&own.unit_housing_at(0), 0x200, 0);       // 0x00456078 -> 0x00c38530 _G_LLM_STRAT_UNIT_HOUSING_STATS
    c.fill_data(&own.population_at(0), 0x1a0, 0);         // 0x00456089 -> 0x00c3a370 _G_LLM_STRAT_POP_STATS
    c.fill_data(&own.player_resource_at(0, 0), 0x140, 0); // 0x0045609a -> 0x00e0e468 player_resources
    c.fill_data(&own.unit_at(0, 0), 0x2d820, 0);          // 0x004560ab -> 0x00dd8c48 units
    c.fill_data(&own.building_at(0, 0), 0x35520, 0);      // 0x004560bc -> 0x00c3d2a0 buildings
    c.fill_data(own.fog_of_war_base(), 0x90000, 0);       // 0x004560cd -> 0x00794fa0 fog_of_war (whole region)
    c.fill_data(&own.projectile_pool_at(0), 0x2c4fc, 0);  // 0x004560de -> 0x00e1db98 _G_LLM_STRAT_PROJECTILE_POOL
    c.fill_data(&own.fx_anim_pool_at(0), 210000, 0);      // 0x004560ef -> 0x00bf50e0 _G_LLM_STRAT_FX_ANIMS (0x33450 == 210000)
    c.fill_data(&own.tile_object_at(0, 0), 0x80000, 0);   // 0x00456100 -> 0x00d1ec80 tile_objects

    // The three map-plane fills this row was DEFERRED on. They are whole-region BYTE fills through
    // base pointers, not per-record loops, for the reason sim_state.h states at each accessor: a
    // byte fill of a save-serialized region is not `= T{}` over its elements, because
    // value-initialisation writes a different pattern wherever a struct has padding and these
    // regions' padding bytes are part of the on-disk format. Two of the three have no known
    // element type at all, which is the same conclusion from the other direction.
    c.fill_data(own.map_object_table_base(), 0x3a980, 0); // 0x00456111 -> 0x00cc46e0 _G_LLM_MAP_OBJECT_TABLE (240000)
    c.fill_data(own.map_objects_base(), 0x3a980, 0);      // 0x00456122 -> 0x00c86660 _G_LLM_MAP_OBJECTS (240000)
    c.fill_data(own.map_halfres_grid_base(), 0x10000, 0); // 0x00456133 -> 0x00d06c80 _G_LLM_MAP_HALFRES_GRID (65536)

    c.fill_data(&own.order_at(0), 0x4fb0, 0);              // 0x00456144 -> 0x00bb4ed0 _G_LLM_STRAT_ORDER_QUEUE
    c.fill_data(&own.resources_at(0, 0), 0x10000, 0);      // 0x00456155 -> 0x00c28530 resources
    c.fill_data(&own.landing_spot_at(0), 0xc0, 0);         // 0x00456166 -> 0x00dd8b88 _G_LLM_STRAT_LANDING_SPOTS
    c.fill_data(&own.soldier_at(0, 0), 0x5aa0, 0);         // 0x00456177 -> 0x00e0e5a8 _G_LLM_STRAT_SOLDIERS
    c.fill_data(&own.production_at(0, 0), 0x6540, 0);      // 0x00456188 -> 0x00dd2648 productions
    c.fill_data(&own.mine_at(0, 0), 0x3800, 0);            // 0x00456199 -> 0x00d03480 mines
    c.fill_data(&own.storage_at(0, 0), 0xbea0, 0);         // 0x004561aa -> 0x00c727c0 unit_storage
    c.fill_data(&own.turret_at(0, 0), 0x3700, 0);          // 0x004561bb -> 0x00cc0fe0 turrets
    c.fill_data(&own.lab_at(0, 0), 0x640, 0);              // 0x004561cc -> 0x00d02d80 labs
    c.fill_data(&own.storage_stats_at(0), 0x2c0, 0);       // 0x004561dd -> 0x00bf4d00 _G_LLM_STRAT_STORAGE_STATS
    c.fill_data(&own.path_buffer_at(0, 0, 0), 0x75300, 0); // 0x004561ee -> 0x00aee8c0 _G_LLM_STRAT_PATH_BUFFERS (480000)
    c.fill_data(&own.passable_at(0, 0), 0x10000, 0);       // 0x004561ff -> 0x00b64bb0 passable

    // 0x00456204: _G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING = 0.
    own.foreign_bldg_event_pending() = 0;

    // 0x0045620e-0x0045644b: the per-player (local_18, p in 0..MAX_PLAYERS) block.
    for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
        // 0x00456225-0x0045622b: _G_LLM_STRAT_PATH_FREE_SLOT_COUNT[p] = 100.
        own.path_free_slot_count_at(p) = 100;

        // 0x0045623c-0x00456257: _G_LLM_STRAT_PATH_SLOT_FLAGS[p][0..PATH_SLOTS_PER_PLAYER) = 0.
        for (int32_t slot = 0; slot < PATH_SLOTS_PER_PLAYER; ++slot) {
            own.path_slot_flag_at(p, slot) = 0;
        }

        // 0x00456259-0x00456267: power_stats[p].ratio = 1.0 (offset 0x10, matches the 64-bit
        // pattern 0x3ff0000000000000 stored as two dword halves in the disassembly).
        own.power_stats_at(p).ratio = 1.0;

        // 0x00456271-0x0045627b: pop_stats[p].subtick_a_clock = *game_clock (offset 0x20, a
        // straight FLD/FSTP copy, no addition).
        own.population_at(p).subtick_a_clock = *v.game_clock;

        // 0x00456288-0x004562c1: storage_stats[p].cap_accum[i]/cap_prev[i] = 0, i in 0..10 (the
        // struct's own array bound, mh_llm_strat_storage_stats::cap_accum[10]/cap_prev[10]).
        for (int32_t i = 0; i < 10; ++i) {
            own.storage_stats_at(p).cap_accum[i] = 0;
            own.storage_stats_at(p).cap_prev[i]  = 0;
        }

        // 0x004562c3-0x004562d9: PRESERVE-BUG, REPRODUCED -- see the header banner's
        // "PRESERVE-BUG" section for the full derivation. The original means to seed
        // storage_stats[p].subtick_b_clock, but it indexes with the INNER loop counter that the
        // cap_accum/cap_prev loop above just left at 10, so `IMUL EAX,[EBP-0x1c],0x58` +
        // `FSTP [EAX+0xbf4d50]` resolves to the FIXED address 0xbf50c0 on all eight iterations --
        // 0x40 bytes into _G_LLM_STRAT_DEATH_ANIM_TABLE, which is therefore clobbered eight times
        // per call while no player's subtick_b_clock is ever seeded.
        //
        // The memcpy is the shape sim_store::death_anim_table_mut()'s own comment prescribes: the
        // accessor is `int32_t *` because the region is an int32_t[6][4], and the original's store
        // is an 8-byte FSTP that straddles elements [4][0] and [4][1]. 0x40 / 4 == 16, so the
        // destination is in bounds in exactly the way the original's is.
        const double subtick_b_seed = *v.game_clock + *v.storage_stats_init_delay;
        std::memcpy(own.death_anim_table_mut() + 16, &subtick_b_seed, sizeof(double));

        // 0x004562e0-0x00456309: productions[p][j].b_index = 0, j in 0..PRODUCTIONS_PER_PLAYER.
        for (int32_t j = 0; j < v.caps.productions; ++j) {
            own.production_at(p, j).b_index = 0;
        }

        // 0x00456309-0x00456339: unit_storage[p][j].b_index = 0, j in 0..STORAGE_PER_PLAYER.
        for (int32_t j = 0; j < v.caps.storage; ++j) {
            own.storage_at(p, j).b_index = 0;
        }

        // 0x00456339-0x00456365: turrets[p][j].b_index = 0, j in 0..TURRETS_PER_PLAYER. WORD
        // write in the disassembly (`MOV word ptr ...`) -- turret::b_index is genuinely int16_t,
        // unlike every sibling roster's int32_t b_index (mh_structs.gen.h comment on the field).
        for (int32_t j = 0; j < v.caps.turrets; ++j) {
            own.turret_at(p, j).b_index = 0;
        }

        // 0x00456365-0x00456394: labs[p][j].b_index = 0, j in 0..LABS_PER_PLAYER.
        for (int32_t j = 0; j < v.caps.labs; ++j) {
            own.lab_at(p, j).b_index = 0;
        }

        // 0x00456394-0x004563c1: mines[p][j].b_index = 0, j in 0..MINES_PER_PLAYER.
        for (int32_t j = 0; j < v.caps.mines; ++j) {
            own.mine_at(p, j).b_index = 0;
        }

        // 0x004563c1-0x004563ed: soldiers[p][j].owner_unit = 0, j in 0..SOLDIERS_PER_PLAYER. WORD
        // write (owner_unit is int16_t).
        for (int32_t j = 0; j < v.caps.soldiers; ++j) {
            own.soldier_at(p, j).owner_unit = 0;
        }

        // 0x004563f4-0x0045644b: progress[p][row].{available,acquired,f3} = false, row in
        // 0..Progress[0].index. The loop bound is read from the CFG Invention table's row-0
        // `.index` field (0x00e16305 = Progress base 0x00e162e4 + field offset 0x21, confirmed
        // against mh_cfg_final_struct_Invention's committed layout) -- NOT from
        // G_PROGRESS_COUNT_TOTAL (a different global, 0x00e5c9e0). Read straight off the address
        // per rule 2b; `v.cfg_inventions` is the existing read binding for the same table.
        // MOVZX zero-extends the u16 field before the signed JG compare, reproduced by the
        // implicit uint16_t -> int32_t widening below.
        const int32_t progress_row_limit = v.cfg_inventions[0].index; // Progress[0].index @0x00e16305
        for (int32_t row = 0; row < progress_row_limit; ++row) {
            own.progress_at(p, row).available = false;
            own.progress_at(p, row).acquired  = false;
            own.progress_at(p, row).f3        = false;
        }
    }

    // 0x00456450-0x00456477: ctrl_groups[g].count = 0, g in 0..10 (_G_LLM_STRAT_CTRL_GROUPS[10]).
    for (int32_t g = 0; g < 10; ++g) {
        own.ctrl_group_at(g).count = 0;
    }

    // 0x00456477-0x004564af: the tail scalar resets.
    own.order_count()         = 0; // _G_LLM_STRAT_ORDER_QUEUE_COUNT   @0x005d0194
    own.order_staging_count() = 0; // _G_LLM_STRAT_ORDER_STAGING_COUNT @0x005d01a4
    own.order_pending_count() = 0; // _G_LLM_STRAT_ORDER_PENDING_COUNT @0x005d01a8
    own.mouse_buttons_prev()  = 0; // _G_LLM_MOUSE_BUTTONS_PREV        @0x00e5899b

    // 0x0045649c: PRESERVE-BUG (harmless): re-zeroes ctrl_groups[0].count, already zeroed by the
    // g-loop above. Literal instruction-for-instruction transcription of `MOV dword ptr
    // [0x00b63be0],0x0` -- same address as ctrl_group_at(0)'s `count` field (offset 0).
    own.ctrl_group_at(0).count = 0;

    own.ui_selected_bldg_index() = 0; // _G_LLM_STRAT_UI_SELECTED_BLDG_INDEX @0x00e5813c
    own.click_select_target_id() = 0; // _G_LLM_CLICK_SELECT_TARGET_ID       @0x00e1629c
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void map_fill_defaults() {
    sim_state st = state();
    detail::map_fill_defaults(st.read, st.own, live_map_fill_defaults_calls());
}

} // namespace mh::sim
