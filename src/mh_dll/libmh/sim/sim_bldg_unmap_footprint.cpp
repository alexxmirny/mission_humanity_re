//
// sim/sim_bldg_unmap_footprint.cpp -- see sim_bldg_unmap_footprint.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_unmap_footprint_0047b36b.asm), not from the Ghidra .c draft.
//
#include "sim/sim_bldg_unmap_footprint.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unmap_footprint_calls &live_unmap_footprint_calls() {
    static const unmap_footprint_calls gc = {
        MH_LIBMH_BIND(llm_strat_sight_remove_circle),
        MH_LIBMH_BIND(llm_map_region_apply_area),
        MH_LIBMH_BIND(llm_strat_bldg_unassign_workers),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
        MH_LIBMH_BIND(llm_strat_bldg_power_network_recompute),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_mother_reelect_primary),
        mh::state::evt::text_queue_id,
        MH_LIBMH_BIND(llm_player_teardown_hook_stub),
    };
    return gc;
}

namespace {

// ---- cfg_enum_E_BUILDING members this switch compares Building[].type against -------------------
// Same posture as sim_order_dispatch_bldg.cpp's own local BLDG_TYPE_* block: the generated header
// renders the enum field as its bare uint8_t scalar, so the literals are named here rather than
// included from another TU (libmh/sim/ duplicates these small constants per-file, same as every other
// alias in this tree). MOTHER / the 12-value storage group reuse the EXACT values
// sim_order_dispatch_bldg.cpp already carries (independently re-derived here from this function's own
// raw CMPs, not copy-pasted); MINE/TURRET reuse the values ai/ai_state.h already carries. PRODUCTION
// and LAB are NEW to the tree -- see the header's derivation and its DECLARED NEED asking the
// conductor to confirm these two pairs against Ghidra's actual enum.
inline constexpr uint8_t BLDG_TYPE_A_PRODUCTION = 1;
inline constexpr uint8_t BLDG_TYPE_H_PRODUCTION = 21;
inline constexpr uint8_t BLDG_TYPE_A_MINE       = 2;
inline constexpr uint8_t BLDG_TYPE_H_MINE       = 22;
inline constexpr uint8_t BLDG_TYPE_A_TURRET     = 5;
inline constexpr uint8_t BLDG_TYPE_H_TURRET     = 25;
inline constexpr uint8_t BLDG_TYPE_A_MOTHER     = 6;
inline constexpr uint8_t BLDG_TYPE_H_MOTHER     = 0x1a;
inline constexpr uint8_t BLDG_TYPE_A_BARRACKS   = 7;
inline constexpr uint8_t BLDG_TYPE_H_BARRACKS   = 0x1b;
inline constexpr uint8_t BLDG_TYPE_A_GARAGE     = 8;
inline constexpr uint8_t BLDG_TYPE_H_GARAGE     = 0x1c;
inline constexpr uint8_t BLDG_TYPE_A_AIRFIELD   = 9;
inline constexpr uint8_t BLDG_TYPE_H_AIRFIELD   = 0x1d;
inline constexpr uint8_t BLDG_TYPE_A_HELIPAD    = 0x0a;
inline constexpr uint8_t BLDG_TYPE_H_HELIPAD    = 0x1e;
inline constexpr uint8_t BLDG_TYPE_A_PORT       = 0x0c;
inline constexpr uint8_t BLDG_TYPE_H_PORT       = 0x20;
inline constexpr uint8_t BLDG_TYPE_A_SHUTTLE    = 0x0d;
inline constexpr uint8_t BLDG_TYPE_H_SHUTTLE    = 0x21;
inline constexpr uint8_t BLDG_TYPE_A_LAB        = 0x0b;
inline constexpr uint8_t BLDG_TYPE_H_LAB        = 0x1f;

// game::e::event members this function raises (see the header's DECLARED NEED note -- the real
// 25-member Ghidra enum has no generated C++ form yet; matches the strategic-sim notes' table exactly,
// same naming convention as sim_unit_remove_from_map.h's EVENT_INFO_REFRESH).
inline constexpr uint32_t EVENT_BUILD_TAB_BUILDINGS = 3;
inline constexpr uint32_t EVENT_BUILD_TAB_UNITS     = 4;

} // namespace

namespace detail {

void bldg_unmap_footprint(const sim_view &v, sim_store &own, const unmap_footprint_calls &gc,
                          uint16_t player, int32_t building_index) {
    // 0x0047b382-0x0047b3c2: snapshot the building's ORIGINAL x/y before anything else mutates map
    // state -- these are read again at the very end as llm_strat_mother_reelect_primary's (x,y)
    // argument, so they must be the entry-time coords, not re-derived later.
    const building     &b      = building_of(v, player, building_index);
    const int32_t       orig_x = b.x;
    const int32_t       orig_y = b.y;
    const uint16_t      bid    = b.building_id;
    const cfg_building &cb     = v.cfg_buildings[bid];
    const int32_t       sub_id = b.sub_id;

    // ---- step 2 (0x0047b3c2-0x0047b546): the 10x10 footprint-mask walk. `dx` is the OUTER loop var
    // (area's row index, added to x); `dy` is the INNER loop var (area's column index, added to y) --
    // re-derived from the raw `area_index = dx*10 + dy` addressing, matching
    // llm_strat_bldg_footprint_set_passable's own wrap-arithmetic pattern (SAME (col&width_mask)<<11 |
    // (row&height_mask)<<3 addressing tile_object_at()/passable_at() already implement).
    for (int32_t dx = 0; dx < 10; ++dx) {
        for (int32_t dy = 0; dy < 10; ++dy) {
            if (cb.area[dx][dy] != 0) {
                const int32_t col                        = (orig_x + dx) & (int32_t)map_width_mask(v);
                const int32_t row                        = (orig_y + dy) & (int32_t)map_height_mask(v);
                own.tile_object_at(col, row).building    = 0;
                own.tile_object_at(col, row).class_owner = 0;
                own.passable_at(col, row)                = 1;
            }
        }
    }

    // ---- step 3 (0x0047b58e-0x0047b592): drop the building's sight circle.
    gc.sight_remove_circle((int32_t)player, orig_x, orig_y, (int32_t)bid, cb.sight);

    // ---- step 4 (0x0047b5c1-0x0047b5c7): re-apply the region-area bitmap over the vacated footprint.
    // `area_mask` points at ORIGINAL (read-only) cfg data -- safe to hand out even though sim_store's
    // W2 rule forbids handing out SIM addresses; this is not one.
    gc.map_region_apply_area((uint32_t)orig_x, orig_y,
                             const_cast<char *>(reinterpret_cast<const char *>(&cb.area[0][0])));

    // ---- step 5 (0x0047b5e6-0x0047b5ed): release the building's assigned workers. Return value
    // discarded -- the original never tests EAX after this call.
    gc.unassign_workers(player, (uint32_t)building_index, b.current_workers);

    // ---- step 6 (0x0047b62f-0x0047b89d): the type-specific slot clear. See the header banner for the
    // full case<->type-value derivation.
    switch (cb.type) {
        case BLDG_TYPE_A_PRODUCTION:
        case BLDG_TYPE_H_PRODUCTION: {
            own.production_at(player, sub_id).b_index = 0;
            own.production_at(player, 0).b_index -= 1;
            break;
        }
        case BLDG_TYPE_A_MINE:
        case BLDG_TYPE_H_MINE: {
            own.mine_at(player, sub_id).b_index = 0;
            own.mine_at(player, 0).b_index -= 1;
            break;
        }
        case BLDG_TYPE_A_TURRET:
        case BLDG_TYPE_H_TURRET: {
            own.turret_at(player, sub_id).b_index = 0;
            own.turret_at(player, 0).b_index -= 1;
            break;
        }
        case BLDG_TYPE_A_BARRACKS:
        case BLDG_TYPE_A_GARAGE:
        case BLDG_TYPE_A_AIRFIELD:
        case BLDG_TYPE_A_HELIPAD:
        case BLDG_TYPE_A_PORT:
        case BLDG_TYPE_A_SHUTTLE:
        case BLDG_TYPE_H_BARRACKS:
        case BLDG_TYPE_H_GARAGE:
        case BLDG_TYPE_H_AIRFIELD:
        case BLDG_TYPE_H_HELIPAD:
        case BLDG_TYPE_H_PORT:
        case BLDG_TYPE_H_SHUTTLE: {
            // 0x0047b6e2-0x0047b70f: decrement the per-player used-slot COUNT held at index 0 (same
            // convention as productions[0]/mines[0]/turrets[0]/labs[0] above), then zero this slot's
            // own b_index.
            own.storage_at(player, 0).b_index -= 1;
            own.storage_at(player, sub_id).b_index = 0;

            // 0x0047b722-0x0047b7e0: docked_count is read ONCE into a local before the loop (unlike
            // sim_bldg_scrap_stored_units.cpp's live re-read every iteration) -- reproduced as read.
            const int32_t docked_count = own.storage_at(player, sub_id).docked_count;
            if (docked_count != 0) {
                for (int32_t i = 0; i < docked_count; ++i) {
                    const uint16_t docked_unit =
                        (uint16_t)own.storage_at(player, sub_id).docked_units[i];
                    gc.unit_teardown(player, docked_unit);
                    own.storage_at(player, sub_id).docked_units[i] = 0;
                }
                own.storage_at(player, sub_id).docked_count = 0;
                own.storage_at(player, sub_id).occupancy    = 0;
            }

            // 0x0047b7e0-0x0047b86f: THEN, regardless of whether anything was docked, walk the
            // roster clearing home_storage_slot on any unit that ever referenced this slot.
            //
            // `unit_above` is `uint8_t[2]` (`map_t_unit_full_id`, a packed {id, owner} pair) at
            // offset 0x0 of mh_map_object_unit. Both the seed read (index 0, reused as a per-player
            // COUNT of live units -- same "slot 0 holds a count" convention the four type-specific
            // arms above already use) and the per-iteration occupancy test read this SAME field as a
            // single 16-bit quantity in the original (`MOVZX ... word ptr [...]`); testing
            // `unit_above[0] != 0 || unit_above[1] != 0` is bit-identical to that word test (a
            // little-endian word is zero iff both its bytes are) and needs no reinterpret_cast.
            unit    &seed      = own.unit_at(player, 0);
            uint32_t remaining = (uint32_t)seed.unit_above[0] | ((uint32_t)seed.unit_above[1] << 8);
            for (int32_t idx = 1; idx < 100 && remaining != 0; ++idx) {
                unit &u = own.unit_at(player, idx);
                if (u.unit_above[0] != 0 || u.unit_above[1] != 0) {
                    --remaining;
                    if (u.home_storage_slot == (uint8_t)sub_id) {
                        u.home_storage_slot = 0;
                    }
                }
            }
            break;
        }
        case BLDG_TYPE_A_LAB:
        case BLDG_TYPE_H_LAB: {
            // 0x0047b871-0x0047b89d: decrement BEFORE zeroing this slot -- opposite order from the
            // other three groups above, reproduced as read, not "normalised".
            own.lab_at(player, 0).b_index -= 1;
            own.lab_at(player, sub_id).b_index = 0;
            break;
        }
        default:
            break;
    }

    // ---- step 7 (0x0047b89f-0x0047b8a8): unconditional.
    gc.power_network_recompute(player);

    // ---- step 8 (0x0047b8a8-0x0047b949): UI-selection cleanup. See the header banner: both branches
    // converge on step 9's unconditional notify_ui below, confirmed by tracing every JMP target.
    //
    // own.ui_selected_bldg_index() is used for BOTH the read here and the write below -- own is a
    // non-const sim_store&, so a read through the mutable accessor is legal, matching this same
    // function's own click_select_target_id() precedent two lines down (read-then-write through the
    // one mutable accessor, no separate const view member for a scalar this function both reads and
    // writes itself).
    if (player == *v.player_side) {
        if ((int32_t)(uint32_t)own.ui_selected_bldg_index() == building_index) {
            gc.notify_ui(player, (uint32_t)building_index);
            own.ui_selected_bldg_index() = 0;
            if (*v.ui_panel_mode == 1 && *v.ui_panel_page == 2) {
                gc.set_event(EVENT_BUILD_TAB_BUILDINGS);
            }
        }
    } else if (*v.click_select_target_flags == (uint16_t)(player | 0x40u) &&
               own.click_select_target_id() == (uint16_t)building_index) {
        gc.notify_ui(player, (uint32_t)building_index);
        own.click_select_target_id() = 0;
        if (*v.ui_panel_mode == 1 && *v.ui_panel_page == 2) {
            gc.set_event(EVENT_BUILD_TAB_BUILDINGS);
        }
    }

    // ---- step 9 (0x0047b949-0x0047b955): UNCONDITIONAL -- the third notify_ui call site.
    gc.notify_ui(player, (uint32_t)building_index);

    // ---- step 10 (0x0047b955-0x0047ba63): MOTHER-type handling.
    if ((cb.type == BLDG_TYPE_H_MOTHER || cb.type == BLDG_TYPE_A_MOTHER) &&
        own.profile_at(player).primary_mother_bldg[*v.planet_index] == building_index) {
        own.profile_at(player).primary_mother_bldg[*v.planet_index] = 0;

        if (player == *v.player_side && *v.ui_panel_mode == 1 && *v.ui_panel_page == 0) {
            gc.set_event(EVENT_BUILD_TAB_UNITS);
        }

        // x/y here are the ORIGINAL coords captured at function entry, not re-read after the
        // tile-clear/switch mutations above.
        const int32_t reelected = gc.mother_reelect_primary((int32_t)player, orig_x, orig_y);
        if (reelected == 0 && player == *v.player_side) {
            const int32_t text_id = (*v.player_race == 1) ? 0x69 : 0xb1;
            gc.print_queue_text_id(text_id);
        }

        // Unconditional within this branch, reached on BOTH the reelected==0 and reelected!=0 paths.
        gc.player_teardown_hook_stub((int32_t)player);
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_unmap_footprint(uint16_t player, int32_t building_index) {
    sim_state st = state();
    detail::bldg_unmap_footprint(st.read, st.own, live_unmap_footprint_calls(), player, building_index);
}

// ---- the differential-oracle arm ------------------------------------------------------------------


} // namespace mh::sim
