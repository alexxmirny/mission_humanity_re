//
// sim/sim_storage_exit_placement.cpp -- see sim_storage_exit_placement.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_place_exit_ground_00489f54.asm,
// tmp/decomp_sim/llm_strat_storage_exit_air_0048b01d.asm) -- see the header banner for the field-offset
// derivation and the two declared CORRECTIONs (the padding-inclusive park_x/park_y dword read, and the
// passable-cell double-read).
//
#include "sim/sim_storage_exit_placement.h"

#include <cstring> // std::memcpy -- the cargo_manifest_raw leading-uint16_t reads, same idiom
                   // sim_prod_shuttle_slot_release.cpp/sim_prod_unload_cargo_unit.cpp already use

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const storage_exit_placement_calls &live_storage_exit_placement_calls() {
    static const storage_exit_placement_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_remove_docked_unit),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_prod_shuttle_unload_passengers),
        MH_LIBMH_BIND(llm_strat_bldg_flush_cargo_hold),
        MH_LIBMH_BIND(llm_strat_unit_soldiers_set_heading),
        MH_PROMOTED_ROW(llm_strat_squad_placement_offset_lookup) /* renamed from llm_ui_cursor_lookup_offset_pair 2026-09-02 */,
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_strat_storage_setup_exit_path),
    };
    return c;
}

namespace {

// 0x0048a123-0x0048a184: classify a storage building's cfg TYPE into the 3-way tier
// llm_strat_storage_place_exit_ground uses to seed move_microstep's exit-delay countdown -- see the
// header banner's derivation. The 1/2/3 TIER itself has no Ghidra enum (a purely local grouping
// computed only for this one arithmetic step, per translator-brief 17a); the building TYPE values it
// groups are all already-named BUILDING_TYPE_* members, reused verbatim.
int32_t exit_ground_delay_tier(uint8_t bldg_type) {
    if (bldg_type == BUILDING_TYPE_A_GARAGE || bldg_type == BUILDING_TYPE_A_SHUTTLE ||
        bldg_type == BUILDING_TYPE_H_GARAGE || bldg_type == BUILDING_TYPE_H_SHUTTLE) {
        return 1;
    }
    if (bldg_type == BUILDING_TYPE_H_BARRACKS) {
        return 2;
    }
    return 3;
}

// cargo_manifest_raw's own entry stride/scan bound -- same values sim_prod_shuttle_slot_release.cpp's
// local `kCargoEntryStride` and sim_unit_load_into_shuttle_cargo.cpp's `CARGO_ENTRY_STRIDE` already use
// for the SAME field (not shared across TUs, per the "no new shared helpers" rule).
inline constexpr uint32_t kCargoEntryStride       = 0xe;  // 14 bytes/manifest entry
inline constexpr int32_t  kCargoManifestScanCount = 0x32; // 50, the assembly's own loop bound

// unit.unit_above is a 2-byte little-endian pair (map_t_unit_full_id) the original addresses with one
// `MOVZX reg, word ptr [...]` -- reassembled here, same per-TU local helper every other sim/ TU
// touching this field defines (e.g. sim_unit_state_squad_merge.cpp; not shared across TUs).
uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}

} // namespace

namespace detail {

void storage_place_exit_ground(const sim_view &v, sim_store &own, const storage_exit_placement_calls &c,
                               uint16_t player, int32_t unit_index, int32_t storage_slot) {
    // 0x00489f73-0x00489fc4: snapshot three unit_storage fields BEFORE the remove_docked_unit call.
    const unit_storage &storage_before = storage_of(v, player, storage_slot);
    const int32_t       exit_x         = storage_before.exit_tile_x; // +0xe4
    const int32_t       exit_y         = storage_before.exit_tile_y; // +0xe8
    const int32_t       building_index = storage_before.b_index;     // +0x0

    // 0x00489fc7-0x00489fd1.
    c.storage_remove_docked_unit(player, unit_index, storage_slot);

    // 0x00489fd6-0x0048a01a: direction from the storage's park position to its exit tile, both scaled
    // to fine (tile*32) coords. park_x/park_y are re-read FRESH (post-call) here -- see the header
    // banner's CORRECTION on the padding-inclusive dword read this reproduces as a zero-extended byte.
    const unit_storage &storage_after = storage_of(v, player, storage_slot);
    const int32_t       park_x        = storage_after.park_x; // +0xdc
    const int32_t       park_y        = storage_after.park_y; // +0xe0
    const int32_t       dir           = c.dir_from_to(park_x << 5, park_y << 5, exit_x << 5, exit_y << 5);
    const uint8_t       facing        = static_cast<uint8_t>(dir);

    unit &u = own.unit_at(player, unit_index);

    // 0x0048a035-0x0048a08c.
    u.facing_current = facing;
    u.facing_target  = facing;
    u.x              = static_cast<uint8_t>(exit_x);
    u.y              = static_cast<uint8_t>(exit_y);

    // 0x0048a092-0x0048a0f3: claim the exit tile on the passable grid. Read once (the original reads
    // the SAME cell twice from two independently-verified-identical address computations -- see the
    // header banner's CORRECTION), stash it (an int-to-double CONVERSION for move_step_speed_scale, not
    // a bit-reinterpret), then clear the cell.
    const uint8_t passable_val      = v.passable[(exit_x << 8) | exit_y];
    u.origin_tile_was_passable      = passable_val;
    u.move_step_speed_scale         = static_cast<double>(passable_val);
    own.passable_at(exit_x, exit_y) = 0;

    // 0x0048a0fa-0x0048a184: classify the storage building's cfg type into a 3-way exit-delay tier.
    const uint8_t bldg_type = v.cfg_buildings[building_of(v, player, building_index).building_id].type;
    u.move_microstep        = 0x20 - exit_ground_delay_tier(bldg_type) * 0x20;

    // 0x0048a18a-0x0048a346: shuttle/cargo housekeeping, gated on the storage building being an
    // A_SHUTTLE/H_SHUTTLE with a bound shuttle slot. Both callees here are the shuttle/cargo-cycle
    // frontier pair the batch context documents; not modeled further ("callees stay original").
    if (bldg_type == BUILDING_TYPE_A_SHUTTLE || bldg_type == BUILDING_TYPE_H_SHUTTLE) {
        const building &shuttle_bldg = building_of(v, player, building_index);
        if (shuttle_bldg.shuttle_slot != 0) {
            if (v.cfg_units[u.unit_proto_id].soldier_count == 0) {
                // 0x0048a249-0x0048a276.
                c.prod_shuttle_unload_passengers(player, building_index,
                                                 v.cfg_units[u.unit_proto_id].human);
            }
            const int32_t shuttle_slot_index =
                player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_bldg.shuttle_slot;
            if (v.prod_shuttle_slots[shuttle_slot_index].passengers_reserved == 0) {
                // 0x0048a2b4-0x0048a316: count nonzero leading uint16_t entries across the first 50
                // conceptual cargo_manifest_raw entries.
                int32_t nonzero_cargo_entries = 0;
                for (int32_t i = 0; i < kCargoManifestScanCount; ++i) {
                    uint16_t entry = 0;
                    std::memcpy(&entry,
                                &v.prod_shuttle_slots[shuttle_slot_index]
                                     .cargo_manifest_raw[static_cast<uint32_t>(i) * kCargoEntryStride],
                                sizeof(entry));
                    if (entry != 0) {
                        ++nonzero_cargo_entries;
                    }
                }
                if (storage_of(v, player, storage_slot).docked_count == 0 && nonzero_cargo_entries == 0) {
                    // 0x0048a33a-0x0048a341.
                    c.bldg_flush_cargo_hold(static_cast<uint32_t>(player), building_index);
                }
            }
        }
    }

    // 0x0048a346-0x0048a52f: soldier-carrying units re-seat their crew's screen position and set a
    // fresh walk-in heading; everything else (no crew) instead adjusts the base population ledger
    // (SIGNS REVERSED from production_ready's own use of the same pair -- this function is the unit
    // LEAVING housing).
    if (v.cfg_units[u.unit_proto_id].soldier_count > 0) {
        // 0x0048a37d.
        c.unit_soldiers_set_heading(player, unit_index, facing);

        const int32_t total     = v.cfg_units[u.unit_proto_id].soldier_count; // snapshotted once
        int32_t       remaining = total;
        uint16_t      chain     = unit_full_id_word(u.unit_above);
        do {
            // 0x0048a3ce-0x0048a408: a cursor-table lookup that writes the (start_x, start_y) pair
            // directly through the out-pointers -- PURE QUERY, do not suppress under an effect seam.
            c.cursor_lookup_offset_pair(
                total, remaining, reinterpret_cast<char *>(&own.soldier_at(player, chain).start_x),
                reinterpret_cast<char *>(&own.soldier_at(player, chain).start_y));
            own.soldier_at(player, chain).start_x = own.soldier_at(player, chain).cur_x;
            own.soldier_at(player, chain).end_x   = own.soldier_at(player, chain).start_x;
            own.soldier_at(player, chain).start_y = own.soldier_at(player, chain).cur_y;
            own.soldier_at(player, chain).end_y   = own.soldier_at(player, chain).start_y;
            chain                                 = own.soldier_at(player, chain).next_soldier;
            --remaining;
        } while (chain != 0);
    } else {
        // 0x0048a4c9-0x0048a52f.
        own.population_at(player).human -= v.cfg_units[u.unit_proto_id].human;
        own.population_at(player).human_in_field += v.cfg_units[u.unit_proto_id].human;
    }

    // 0x0048a52f-0x0048a53b: unconditional final transition -- the ONE state-setting call in the whole
    // function.
    c.unit_set_state_of(static_cast<int32_t>(player), unit_index,
                        static_cast<int16_t>(UNIT_STATE_EXIT_WALK_OUT));
}

void storage_exit_air(const sim_view &v, sim_store &own, const storage_exit_placement_calls &c,
                      uint16_t player, int32_t unit_index, int32_t storage_slot, uint32_t param_4) {
    // This function never writes a `units[]` field (unlike storage_place_exit_ground) -- read-only, so
    // the const view suffices here.
    const unit &u = unit_of(v, player, unit_index);

    // 0x0048b03e-0x0048b09e: population ledger adjust -- SAME two fields/signs as
    // storage_place_exit_ground's soldier_count<=0 arm (both are "unit leaving housing").
    own.population_at(player).human -= v.cfg_units[u.unit_proto_id].human;
    own.population_at(player).human_in_field += v.cfg_units[u.unit_proto_id].human;

    // 0x0048b0a4-0x0048b0d9: park position, read as a wide dword in the assembly (see the header
    // banner's CORRECTION) but truncated to a byte before use here -- provably safe regardless of the
    // padding's contents.
    const unit_storage &storage = storage_of(v, player, storage_slot);
    const uint8_t       exit_x  = storage.park_x;
    const uint8_t       exit_y  = storage.park_y;

    // 0x0048b0dc-0x0048b0f3.
    c.storage_setup_exit_path(player, unit_index, exit_x, exit_y, static_cast<int32_t>(param_4),
                              storage_slot);

    // 0x0048b0f8-0x0048b15b: ONE callee, a computed state literal (factored per the batch brief).
    const building &b         = building_of(v, player, storage.b_index);
    const uint8_t   bldg_type = v.cfg_buildings[b.building_id].type;
    const uint16_t  state =
        (bldg_type == BUILDING_TYPE_H_HELIPAD) ? UNIT_STATE_EXIT_AIR_HELIPAD : UNIT_STATE_EXIT_AIR_DEFAULT;
    c.unit_set_state_of(static_cast<int32_t>(player), unit_index, static_cast<int16_t>(state));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void storage_place_exit_ground(uint16_t player, int32_t unit_index, int32_t storage_slot) {
    sim_state st = state();
    detail::storage_place_exit_ground(st.read, st.own, live_storage_exit_placement_calls(), player,
                                      unit_index, storage_slot);
}

void storage_exit_air(uint16_t player, int32_t unit_index, int32_t storage_slot, uint32_t param_4) {
    sim_state st = state();
    detail::storage_exit_air(st.read, st.own, live_storage_exit_placement_calls(), player, unit_index,
                             storage_slot, param_4);
}


} // namespace mh::sim
