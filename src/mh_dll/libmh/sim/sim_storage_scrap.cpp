//
// sim/sim_storage_scrap.cpp -- see sim_storage_scrap.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_{scrap_home_docked_units,scrap_docked_units_of_type,
// resolve_exit_blockage}_*.asm), not from any exported Ghidra .c draft.
//
#include "sim/sim_storage_scrap.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_scrap_home_calls &live_storage_scrap_home_calls() {
    static const storage_scrap_home_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_remove_docked_unit),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
    };
    return c;
}

const storage_scrap_of_type_calls &live_storage_scrap_of_type_calls() {
    static const storage_scrap_of_type_calls c = {
        MH_LIBMH_BIND(llm_strat_order_queue_find_index),
        MH_LIBMH_BIND(llm_strat_order_queue_apply_and_dequeue),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
    };
    return c;
}

const storage_resolve_exit_blockage_calls &live_storage_resolve_exit_blockage_calls() {
    static const storage_resolve_exit_blockage_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_queue_advance),
        MH_LIBMH_BIND(llm_strat_storage_scrap_docked_units_of_type),
    };
    return c;
}

namespace {

// llm_strat_unit_state members read directly off the raw disassembly (no backing Ghidra enum member
// found for the two flagged below), own local copy per this codebase's established per-TU-constant
// convention (sim_storage_can.cpp's own header note; sim_order_enqueue.h carries the ones that ARE
// shared -- UNIT_STATE_PARKED/UNIT_STATE_EXIT_WAIT are pulled from there instead of redeclared here).
constexpr uint16_t UNIT_STATE_GROUP_MARSHAL = 0x0a; // same value sim_unit_state_move_walker.cpp names
                                                    // MOVE_WALKER_STATE_GROUP_MARSHAL; own copy here.
constexpr uint16_t UNIT_STATE_MOVE_WALKER = 0x0f;   // same value sim_order_dispatch.cpp/
                                                    // sim_unit_state_misc2.h already name; own copy.
constexpr uint16_t UNIT_STATE_EXIT_WALK_OUT = 0x21; // same value sim_storage_exit_placement.h names
                                                    // UNIT_STATE_EXIT_WALK_OUT; own copy here.
constexpr uint16_t UNIT_STATE_ENTER_WAIT = 0x25;    // same value sim_unit_state_enter.h names
                                                    // ENTER_STORAGE_BEGIN_STATE_ENTER_WAIT; own copy.
// RESOLVED (conductor, SIM1-G3, 2026-08-21): 0x36 IS a real, named Ghidra enum member,
// STEP_ADJACENT (`/llm/llm_strat_unit_state`, get-data-type-by-string) -- the SAME state
// llm_strat_unit_state_step_adjacent (this batch) handles. The translator's tree-grep
// found no C++-side name (this codebase does not generate a bound C++ enum from it); renamed to match.
constexpr uint16_t UNIT_STATE_STEP_ADJACENT = 0x36;

// RESOLVED (conductor, prep session, 2026-08-21, finding 2026-08-21-1329-6 -- corrects the prior
// "no /llm/E_ORDER_CODE member at 0x80" framing, which was chasing the wrong enum). This is NOT an
// order_code at all: llm_strat_order_queue_find_index's third parameter (renamed order_code ->
// kind_tag in Ghidra) is compared only against the queue entry's owner_and_kind HIGH NIBBLE
// (`& 0xf0`), never against its order_code field -- so 0x80 is the "unit target" KIND tag
// (owner_and_kind's own field comment: "0x20/0x80 = unit target"), the SAME `mh::sim::ORDER_KIND_UNIT`
// sim_order_enqueue.h already declares (pulled in transitively via this TU's own sim_storage_scrap.h
// include, hence no local redeclaration here -- unlike sim_unit_state_hover_engage.cpp, which does not
// include that header and so keeps its own local copy) / sim_order_dispatch.h's ORDER_KIND_UNIT_EX.

} // namespace

namespace detail {

void storage_scrap_home_docked_units(const sim_view &v, const storage_scrap_home_calls &c,
                                     uint16_t player, int32_t unit_index) {
    // 0x0048f8bb-0x0048f8d5: home_slot = units[player][unit_index].home_storage_slot, read ONCE
    // before the loop -- the literal 0xdd8c78 is units_base(0xdd8c48) + 0x30 (home_storage_slot's
    // own offset), NOT +0x0 (unit_above[0]); re-derived field-by-field against mh_structs.gen.h.
    const uint8_t home_slot = unit_of(v, player, unit_index).home_storage_slot;

    // 0x0048f8df-0x0048f947: for (i = 0; i < storage[player][home_slot].docked_count [RE-READ fresh
    // every pass, never cached]; ++i). remove_docked_unit may shrink docked_count and compact the
    // array UNDER this loop; the original does not special-case that, so neither do we -- see the
    // header banner's note on why this is a real, reproduced trap, not a bug to fix.
    for (int32_t i = 0; i < storage_of(v, player, home_slot).docked_count; ++i) {
        // 0x0048f922: docked_units[i], full 32-bit read.
        const int32_t docked_unit = storage_of(v, player, home_slot).docked_units[i];

        // 0x0048f92e-0x0048f935: remove_docked_unit(player, docked_unit, home_slot) -- FULL-WIDTH
        // docked_unit (EDX is a plain 32-bit MOV, no MOVZX/MOVSX).
        c.storage_remove_docked_unit(player, docked_unit, static_cast<int32_t>(home_slot));

        // 0x0048f93a-0x0048f942: teardown(player, docked_unit) -- docked_unit TRUNCATED to 16 bits
        // here (MOVZX word), matching the committed teardown prototype's own uint16_t parameter.
        c.unit_teardown(player, static_cast<uint16_t>(docked_unit));
    }
}

void storage_scrap_docked_units_of_type(const sim_view &v, sim_store &own,
                                        const storage_scrap_of_type_calls &c, uint32_t player,
                                        int32_t building_index) {
    // 0x0046cb77-0x0046cb91: sub_id = buildings[player][building_index].sub_id, read ONCE before
    // the loop.
    const uint8_t sub_id = building_of(v, player, building_index).sub_id;

    // 0x0046cb9b-0x0046cd7d: for (i = 0; i < storage[player][sub_id].docked_count [RE-READ fresh
    // every pass]; ++i). Unlike scrap_home_docked_units above, this loop's own direct writes
    // (door_waiter_count, target_ref, target_index) never touch docked_count/docked_units/b_index, so
    // there is no compaction hazard here to preserve or reproduce.
    for (int32_t i = 0; i < storage_of(v, player, sub_id).docked_count; ++i) {
        // 0x0046cbe1: docked_units[i], full 32-bit read.
        const int32_t docked_unit = storage_of(v, player, sub_id).docked_units[i];

        // 0x0046cbfd-0x0046cc05: gate -- everything below is skipped entirely unless the docked
        // unit's state is UNIT_STATE_EXIT_WAIT (mid-exit, waiting).
        if (unit_of(v, player, docked_unit).state != UNIT_STATE_EXIT_WAIT) {
            continue;
        }

        // 0x0046cc1e: DIRECT WRITE, unconditional once the state gate passes -- see the header
        // banner's correction of the batch hazard note. DEC in the original, not a re-read.
        own.storage_at(player, sub_id).door_waiter_count -= 1;

        // 0x0046cc37-0x0046cc66: if order_queued != 0, look up the queue index for the AMBIENT
        // cur_player/cur_index (NOT this function's own player/docked_unit -- re-verified against
        // the raw MOVZX-from-global sequence at 0x0046cc45-0x0046cc4c) and apply/dequeue it for
        // THIS docked unit.
        if (unit_of(v, player, docked_unit).order_queued != 0) {
            const int32_t queue_idx_full = c.order_queue_find_index(
                static_cast<int32_t>(*v.cur_player), static_cast<int32_t>(*v.cur_index),
                ORDER_KIND_UNIT);
            // 0x0046cc5b: MOVZX EBX, word ptr [queue_idx_full] -- truncated to 16 bits before the
            // call, same narrowing shape as scrap_home_docked_units's teardown argument.
            c.order_queue_apply_and_dequeue(
                player, docked_unit, static_cast<int32_t>(static_cast<uint16_t>(queue_idx_full)));
        }

        // 0x0046cc7e-0x0046ccc8: if target_ref != 0, release it (mode=1) THEN DIRECTLY zero both
        // target_ref and target_index here -- target_release_ref's mode-1 arm does NOT touch these
        // two fields (it clears engagement_flags/order_status_flags bits instead), so this is a
        // genuine second write site in THIS function, not a redundant mirror of the callee.
        if (unit_of(v, player, docked_unit).target_ref != 0) {
            c.target_release_ref(player, docked_unit, 1u);
            own.unit_at(player, docked_unit).target_ref   = 0;
            own.unit_at(player, docked_unit).target_index = 0;
        }

        // 0x0046ccd1-0x0046cd6c: re-read proto/state fresh (neither cached from an earlier read in
        // this iteration -- there isn't one). state is re-read TWICE independently in the raw
        // assembly (once per compare); cached once here since nothing between the two reads writes
        // it, which is behaviourally identical.
        const uint16_t  proto2    = unit_of(v, player, docked_unit).unit_proto_id;
        const cfg_unit &cu2       = v.cfg_units[proto2];
        const uint16_t  state_now = unit_of(v, player, docked_unit).state;
        if (state_now == cu2.move_op_arg || state_now == cu2.move_op_code) {
            c.unit_notify_status(player, docked_unit, 0x67u);
        }
        // 0x0046cd6c-0x0046cd78: unconditional, fires whether or not the notify above fired.
        c.unit_set_state_of(static_cast<int32_t>(player), docked_unit,
                            static_cast<int16_t>(UNIT_STATE_PARKED));
    }
}

void storage_resolve_exit_blockage(const sim_view &v, const storage_resolve_exit_blockage_calls &c,
                                   uint32_t player, int32_t storage_slot) {
    // 0x00489b5d-0x00489bae: b_index/exit_x/exit_y, each read once.
    const unit_storage &st      = storage_of(v, player, storage_slot);
    const int32_t       b_index = st.b_index;
    const int32_t       exit_x  = st.exit_tile_x;
    const int32_t       exit_y  = st.exit_tile_y;

    // 0x00489bb1-0x00489bbf: tile_at(v, exit_x, exit_y) reproduces the byte math exactly -- exit_x is
    // the OUTER (<<11) term, exit_y the INNER (<<3) term, matching tile_at's own convention and every
    // other reader in this codebase (e.g. sim_storage_exit_query.cpp's `tile_at(v, x, y)` with
    // x=exit_tile_x).
    const tile_object &tile = tile_at(v, exit_x, exit_y);

    if ((tile.class_owner & 0x80u) != 0) {
        // 0x00489bcc-0x00489bfe: a UNIT (or building-slot-type unit) occupies the tile. owner = low
        // nibble of class_owner; index = the tile's `.building` field taken RAW -- for a
        // stationary/building-slot occupant this field doubles as a plain roster index, the same
        // idiom sim_unit_purge_unregistered.h's `tile_at(v,x,y).building == i` test documents; the
        // tile's OTHER occupant field (`.unit`, a packed linked-list head for mobile units) is not
        // read here at all.
        const uint32_t occupant_player = static_cast<uint32_t>(tile.class_owner & 0x0fu);
        const int32_t  occupant_index  = static_cast<int32_t>(tile.building);

        // 0x00489c14-0x00489c95: skip everything below unless order_queued == 0 AND state is none of
        // {GROUP_MARSHAL, MOVE_WALKER, the unnamed 0x36, EXIT_WALK_OUT}.
        const unit &ou = unit_of(v, occupant_player, occupant_index);
        if (ou.order_queued == 0 && ou.state != UNIT_STATE_GROUP_MARSHAL &&
            ou.state != UNIT_STATE_MOVE_WALKER && ou.state != UNIT_STATE_STEP_ADJACENT &&
            ou.state != UNIT_STATE_EXIT_WALK_OUT) {
            // 0x00489ca0: side-effect-only call -- the committed prototype is void, and the .asm's
            // own EAX capture of the result is dead (never read again).
            c.unit_queue_advance(occupant_player, static_cast<uint32_t>(occupant_index));

            // 0x00489cbb-0x00489cc3: re-read state AFTER queue_advance -- it may have changed it.
            if (unit_of(v, occupant_player, occupant_index).state == UNIT_STATE_ENTER_WAIT) {
                // 0x00489cc5-0x00489ccc: note the argument is THIS function's own `b_index` (the
                // storage's own building), never the tile occupant's index.
                c.storage_scrap_docked_units_of_type(player, b_index);
            }
        }
    } else if ((tile.class_owner & 0x40u) != 0) {
        // 0x00489ce1-0x00489cf1: a BUILDING occupies the tile -- unconditional, same b_index.
        c.storage_scrap_docked_units_of_type(player, b_index);
    }
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void storage_scrap_home_docked_units(uint16_t player, int32_t unit_index) {
    const sim_view v = state().read;
    detail::storage_scrap_home_docked_units(v, live_storage_scrap_home_calls(), player, unit_index);
}

void storage_scrap_docked_units_of_type(uint32_t player, int32_t building_index) {
    sim_state st = state();
    detail::storage_scrap_docked_units_of_type(st.read, st.own, live_storage_scrap_of_type_calls(),
                                               player, building_index);
}

void storage_resolve_exit_blockage(uint32_t player, int32_t storage_slot) {
    const sim_view v = state().read;
    detail::storage_resolve_exit_blockage(v, live_storage_resolve_exit_blockage_calls(), player,
                                          storage_slot);
}


} // namespace mh::sim
