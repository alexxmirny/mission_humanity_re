//
// sim/sim_storage_dock.cpp -- see sim_storage_dock.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_{dock_list_append,board_unit,accept_landing}_*.asm), not from any
// exported Ghidra .c draft -- every address cited in the header was independently walked against the
// raw IMUL/MOVZX/CMP/JC/JZ/JNZ opcodes per house rules.
//
#include "sim/sim_storage_dock.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_board_unit_calls &live_storage_board_unit_calls() {
    static const storage_board_unit_calls c = {
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_prod_shuttle_load_passengers),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(llm_strat_ctrl_group_contains_unit),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_remove_member),
        MH_LIBMH_BIND(llm_strat_ai_group_member_count_adjust),
    };
    return c;
}

const storage_accept_landing_calls &live_storage_accept_landing_calls() {
    static const storage_accept_landing_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_setup_approach_path),
        MH_LIBMH_BIND(llm_strat_ctrl_group_contains_unit),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_remove_member),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
    };
    return c;
}

namespace {

// game::e::event member 7 -- BUILD_PROJECTS_REFRESH. Kept FILE-LOCAL (anonymous namespace, not
// `mh::sim` scope) deliberately: sim_unit_population_remove.h already declares an identically-named,
// identically-valued `inline constexpr uint32_t EVENT_BUILD_PROJECTS_REFRESH = 7;` at mh::sim
// namespace scope, and mh/seams/reimpl_probe.cpp includes both that header and this TU's own header
// in one translation unit -- a second namespace-scope declaration of the same name would be a hard
// redefinition error there (same situation sim_bldg_notify_ui.cpp's/sim_storage_dock_unit_at_
// building.cpp's identical constants document).
constexpr uint32_t kEventBuildProjectsRefresh = 7;

// llm_strat_unit_state value 0x26 (38) = ENTER_WALK_IN, board_unit's terminal state. RESOLVED
// (conductor, SIM1-G3, 2026-08-21): the translator found no C++-side name for this value
// and guessed UNIT_STATE_STORAGE_DOCKED; the real name IS in the Ghidra enum `/llm/llm_strat_unit_state`
// (get-data-type-by-string; this codebase just doesn't generate a C++ enum from it -- other TUs
// reference these values by local named constant + comment, e.g. sim_unit_state_enter.h's own
// ENTER_WALK_IN state handler, sim_dock_slot_is_busy.cpp's skip-state list). Renamed to match. File-
// local per this codebase's established per-TU-constant convention for unenum'd unit-state literals.
constexpr uint16_t UNIT_STATE_ENTER_WALK_IN = 0x26;

// llm_strat_unit_state value 0x16 -- accept_landing's terminal state. Independently corroborated by
// THREE sibling TUs' own local copies of the identical value (sim_unit_state_takeoff.h's
// UNIT_STATE_LANDING, sim_order_dispatch.cpp's anonymous-namespace UNIT_STATE_LANDING,
// sim_unit_state_taxi_dock.h's TAXI_DOCK_STATE_TAKEOFF corroborating the paired 0x15) -- own local
// copy here rather than an include, for the same ODR reason as kEventBuildProjectsRefresh above.
constexpr uint16_t UNIT_STATE_LANDING = 0x16;

} // namespace

namespace detail {

void storage_dock_list_append(const sim_view &v, sim_store &own, uint16_t player, uint16_t unit_idx,
                              uint16_t storage_slot) {
    // 0x0048b5c6-0x0048b601: append unit_idx at the CURRENT docked_count -- UNCHECKED against the
    // 50-entry docked_units[] capacity, matching every other docked_units writer in this closure.
    unit_storage &storage                      = own.storage_at(player, storage_slot);
    storage.docked_units[storage.docked_count] = unit_idx;

    // 0x0048b607-0x0048b61d: docked_count += 1, AFTER the append above.
    storage.docked_count += 1;

    // 0x0048b623-0x0048b6b2: occupancy weighting, mutually exclusive (the asm's JZ/JMP pair runs
    // exactly one arm) -- an unarmed/crewless unit (soldier_count == 0) occupies ONE nominal slot; a
    // crewed unit adds its FULL soldier_count instead, never both.
    const unit &u = unit_of(v, player, unit_idx);
    if (v.cfg_units[u.unit_proto_id].soldier_count == 0) {
        storage.occupancy += 1;
    } else {
        storage.occupancy += v.cfg_units[u.unit_proto_id].soldier_count;
    }
}

void storage_board_unit(const sim_view &v, sim_store &own, const storage_board_unit_calls &c,
                        uint16_t player, uint32_t unit_idx, uint32_t storage_idx) {
    // 0x0048b6d9-0x0048b6e5: append + occupancy, via a REAL CALL to the sibling function this same
    // slice translates -- dock_list_append is translated in THIS SAME FILE, in THIS SAME change, so
    // there is no separate shadow arm to bypass. FIXED (offline-oracle authoring, 2026-08-21): an
    // earlier draft called the PUBLIC wrapper `mh::sim::storage_dock_list_append`, which internally
    // calls `state()` and re-binds to the LIVE region registry (sim_state.cpp: `ptr<T>(RID_...)`) --
    // harmless in production (state() always resolves to the same live game addresses regardless of
    // when it's called) but it silently discards the `v`/`own` this function was CALLED WITH, breaking
    // offline testability under net_selftest.exe (no injected game, so state()'s addresses are not
    // this process's memory) and defeating the whole point of the detail:: parameter-passing split.
    // Every other same-TU sibling call in this codebase (sim_combat_kill_credit.cpp's
    // detail::bldg_kill_credit/unit_kill_credit, sim_target_class.cpp's detail::target_class) calls the
    // detail:: overload directly with the caller's own v/own, and this now matches that precedent.
    detail::storage_dock_list_append(v, own, player, static_cast<uint16_t>(unit_idx),
                                     static_cast<uint16_t>(storage_idx));

    // 0x0048b6ea-0x0048b6ef: ALWAYS, unconditionally.
    c.set_event(kEventBuildProjectsRefresh);

    // 0x0048b6f4-0x0048b745: storage.park_x/park_y are read here as full dwords in the original but
    // NEITHER resulting value is read again anywhere in this function -- a pure, side-effect-free
    // dead load. Omitted. (The header's old HAZARD note about those dword reads straddling reserved
    // bytes is retired: the fields are int32_t as of EN v313, so there are no reserved bytes.)

    // 0x0048b748-0x0048b77f: the unit's CURRENT tile, read once here and reused at steps 8-9 below
    // (nothing between here and there writes unit.x/y).
    const unit_storage &storage = storage_of(v, player, storage_idx);
    const unit         &u0      = unit_of(v, player, unit_idx);
    const int32_t       ux      = u0.x;
    const int32_t       uy      = u0.y;

    // unit_proto_id is read 5 times across this body in the raw asm (0x0048b795, 0x0048b7db,
    // 0x0048b890, 0x0048b8c2, 0x0048b9a2) -- collapsed to one read here since nothing in this function
    // writes units[player][unit_idx].unit_proto_id, matching this codebase's established
    // (sim_storage_can.cpp et al.) "cache a provably-invariant field read" convention rather than
    // sim_unit_state_deploy.cpp's "production_ready" precedent, which applies only when a WRITE
    // could sit between the reads.
    const uint16_t  proto = u0.unit_proto_id;
    const cfg_unit &cu    = v.cfg_units[proto];

    // 0x0048b782-0x0048b90d: the SAME delta value the asm computes independently at two separate call
    // sites under the identical `soldier_count <= 0` condition (step 5's flag test, step 6's shuttle
    // capacity test) -- computed ONCE here and reused, a safe collapse of the asm's own redundant
    // re-derivation (see the header's SHAPE step 6 note).
    const int32_t human_delta = (cu.soldier_count > 0) ? 0 : cu.human;

    // 0x0048b782-0x0048b827: population bookkeeping for a non-soldier-crewed unit only.
    if (cu.soldier_count <= 0) {
        pop_stats &pop = own.population_at(player);
        pop.human += human_delta;
        pop.human_in_field -= human_delta;
    }

    // 0x0048b827-0x0048b90d: shuttle-only passenger-load, gated on building type THEN on a bound
    // shuttle slot AND a nonzero delta.
    const building &b         = building_of(v, player, storage.b_index);
    const uint8_t   bldg_type = v.cfg_buildings[b.building_id].type;
    if (bldg_type == BUILDING_TYPE_A_SHUTTLE || bldg_type == BUILDING_TYPE_H_SHUTTLE) {
        if (b.shuttle_slot != 0 && human_delta != 0) {
            // 0x0048b8fa-0x0048b90a: result discarded (dead store in the original).
            c.shuttle_load_passengers(player, storage.b_index, static_cast<uint16_t>(human_delta));
        }
    }

    // 0x0048b90d-0x0048b919: ALWAYS.
    c.unit_set_state_of(player, static_cast<int32_t>(unit_idx),
                        static_cast<int16_t>(UNIT_STATE_ENTER_WALK_IN));

    // 0x0048b91e-0x0048b931: ALWAYS, unconditionally -- reimpl-verify (SIM1-G3,
    // 2026-08-21) caught this as a real divergence: the original recomputes the units[player][unit_idx]
    // base and writes move_microstep=0x20 (0xdd8cea = base+0xa2, mh_map_object_unit::move_microstep,
    // confirmed against mh_structs.gen.h's static_assert) immediately after unit_set_state_of returns.
    // Previously fell silently inside the SHAPE step 8 address-range comment below without being
    // turned into code.
    own.unit_at(player, unit_idx).move_microstep = 0x20;

    // 0x0048b91e-0x0048b988: ALWAYS -- clear the tile the unit is leaving, then restore its cached
    // passable flag (see the header's SHAPE step 8 note and origin_tile_was_passable's own struct
    // comment).
    tile_object &t          = own.tile_object_at(ux, uy);
    t.building              = 0;
    t.class_owner           = 0;
    own.passable_at(ux, uy) = u0.origin_tile_was_passable;

    // 0x0048b98f-0x0048b9c0: ALWAYS -- remove the unit's sight radius from FOW at the tile it is
    // leaving.
    c.fow_remove_sight(player, ux, uy, cu.sight);

    // 0x0048b9c5-0x0048b9fe: local-player-only control-group cleanup. Same idiom
    // sim_unit_state_deploy.cpp's identical block already documents.
    if (player == static_cast<uint16_t>(*v.player_side) &&
        c.ctrl_group_contains_unit(unit_idx, v.ctrl_groups[0].count, 0) != 0) {
        c.unit_ctrlgroup_remove_member(unit_idx, &own.ctrl_group_at(0).count, 0);
        c.set_event(EVENT_INFO_REFRESH);
    }

    // 0x0048b9fe-0x0048ba0d: ALWAYS -- `storage_idx` here is the FULL, untruncated dword param (see
    // the header's SHAPE step 11 note).
    c.ai_group_member_count_adjust(player, unit_idx, storage_idx, 1);
}

void storage_accept_landing(const sim_view &v, sim_store &own, const storage_accept_landing_calls &c,
                            int32_t player, int32_t unit_index, int32_t storage_slot,
                            int32_t path_slot) {
    // reimpl-verify precedent (sim_storage_can.cpp's storage_can_exit/_enter/_land): the ORIGINAL
    // reloads `player` via a 16-bit MOVZX at EVERY one of its four uses (0x0048ba44 into
    // setup_approach_path, 0x0048ba50's PlayerSide compare, 0x0048ba8e into unit_set_state_of) --
    // never a plain 32-bit reload, despite `player` being a genuine 4-byte `int` at this function's
    // OWN calling-convention boundary (unlike board_unit/dock_list_append, whose `player` is already
    // a committed `uint16_t`, so C++'s own parameter type does this narrowing for free). Narrow ONCE
    // here so a caller value with garbage above bit 15 cannot leak into a comparison/callee the
    // original always masks first.
    const uint16_t p = static_cast<uint16_t>(player);

    // 0x0048ba3b-0x0048ba48: ALWAYS -- passed through in accept_landing's OWN parameter names/order
    // (see the header's SHAPE step 1 note on why this is NOT a swap despite the two functions'
    // opposite positional param order).
    c.setup_approach_path(p, unit_index, path_slot, storage_slot);

    // 0x0048ba50-0x0048ba86: local-player-only control-group cleanup. Same idiom board_unit's
    // identical block above (and sim_unit_state_deploy.cpp's) already documents.
    if (p == static_cast<uint16_t>(*v.player_side) &&
        c.ctrl_group_contains_unit(static_cast<uint32_t>(unit_index), v.ctrl_groups[0].count, 0) != 0) {
        c.unit_ctrlgroup_remove_member(static_cast<uint32_t>(unit_index), &own.ctrl_group_at(0).count, 0);
        c.set_event(EVENT_INFO_REFRESH);
    }

    // 0x0048ba86-0x0048ba97: ALWAYS.
    c.unit_set_state_of(static_cast<int32_t>(p), unit_index, static_cast<int16_t>(UNIT_STATE_LANDING));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void storage_dock_list_append(uint16_t player, uint16_t unit_idx, uint16_t storage_slot) {
    sim_state st = state();
    detail::storage_dock_list_append(st.read, st.own, player, unit_idx, storage_slot);
}

void storage_board_unit(uint16_t player, uint32_t unit_idx, uint32_t storage_idx) {
    sim_state st = state();
    detail::storage_board_unit(st.read, st.own, live_storage_board_unit_calls(), player, unit_idx,
                               storage_idx);
}

void storage_accept_landing(int32_t player, int32_t unit_index, int32_t storage_slot,
                            int32_t path_slot) {
    sim_state st = state();
    detail::storage_accept_landing(st.read, st.own, live_storage_accept_landing_calls(), player,
                                   unit_index, storage_slot, path_slot);
}


} // namespace mh::sim
