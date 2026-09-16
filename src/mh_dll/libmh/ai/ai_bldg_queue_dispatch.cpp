//
// ai/ai_bldg_queue_dispatch.cpp -- see ai_bldg_queue_dispatch.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_bldg_queue_{handle_recruit_state_004e80ef, process_entry_004e7dd7,
// handle_state2_empty_004e81df, handle_upgrade_or_cancel_004e81ea}.asm), not from Ghidra's C.
//
// SIGNEDNESS AND WIDTH, the four that are not what they look like:
//   0x004e8136  CMP dword [ai_promo_credit],0 / JLE   SIGNED, and the field is int32 -- a negative
//                                                credit takes the paying path, not the free one.
//   0x004e7e0a  CMP word [build_tile_x],-1             SIGNED word compare against the sentinel...
//   0x004e7e95  MOVZX word [build_tile_x]              ...but every downstream USE is zero-extended,
//                                                so the tile travels as 0..0xffff, never negative.
//   0x004e81c2  CMP EBX,0x4 / JC                       the four-resource loops are UNSIGNED, as in
//               (also 0x004e7fba, 0x004e83e1)    the parent's affordability loop.
//
// THE RESOURCE DEBIT IS A SHORT READ ZERO-EXTENDED AND SUBTRACTED FROM AN INT (MOVZX @0x004e8197 /
// SUB @0x004e819f, and again @0x004e7fab / 0x004e7fb3), the same shape ai_bldg_queue.cpp documents
// on the affordability side: a reserved cost with the high bit set reads as 32768..65535.
//
#include "ai/ai_bldg_queue_dispatch.h"


namespace mh::ai {
namespace detail {

namespace {

// The four-resource debit both the recruit and the construction arm end with, and the upgrade arm
// too: resource_spent[j] -= (uint16)resource_reserved[1 + j], j = 0..3 UNSIGNED -- the two arrays
// index differently because resource_spent was reshaped to int[4] over ids 1..4 on 2026-08-03. One helper
// rather than three copies because all three loops are the same instructions with different
// surroundings (0x004e818e, 0x004e7f86, 0x004e822a).
void debit_reserved_costs(const ai_view &v, const ai_store &own, uint32_t player,
                          int32_t queue_index) {
    const auto &qe = v.players[player].ai_bldg_queue[queue_index];
    for (int32_t j = 0; j < QUEUE_COST_RESOURCE_COUNT; ++j)
        own.players[player].resource_spent[j] -=
            (int32_t)(uint16_t)qe.resource_reserved[1 + j]; // MOVZX, then a 32-bit SUB
}

// build_tile_x / resource_reserved[0] hold the cached tile, and EVERY read of them downstream of the
// -1 test is a MOVZX. Two accessors so that stays true by construction.
uint32_t cached_tile_x(const mh::game::mh_llm_strat_ai_bldg_queue_entry &qe) {
    return (uint32_t)(uint16_t)qe.build_tile_x;
}
uint32_t cached_tile_y(const mh::game::mh_llm_strat_ai_bldg_queue_entry &qe) {
    return (uint32_t)(uint16_t)qe.resource_reserved[0]; // slot [0] is dead as a cost -- see the field
}

} // namespace

// ---- nibble 0 -----------------------------------------------------------------------------------
void bldg_queue_handle_recruit_state(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     uint32_t player, int32_t queue_index,
                                     queue_dispatch_report &rep) {
    ++rep.recruit_calls;
    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];
    const auto        &qe = pd.ai_bldg_queue[queue_index];

    // 0x004e8129: the queue entry's tick_or_unit_id is a cfg Unit TYPE id on a kind-0 entry.
    const int32_t producer =
        gc.bldg_find_idle_producer_for_unit((int32_t)player, (int32_t)qe.tick_or_unit_id);
    if (producer == 0) { // JZ to the shared epilogue @0x004e8130 -- entry left untouched, retried
        ++rep.no_producer;
        return;
    }

    if (pd.ai_promo_credit > 0) { // CMP/JLE @0x004e8136, SIGNED
        ++rep.promo_spent;
        // The FREE path: an order, a credit debit, and no resource movement at all.
        gc.order_recruit_unit_enqueue((uint32_t)qe.tick_or_unit_id, (uint32_t)(uint16_t)player);
        wp.ai_promo_credit -= *v.promo_add;                                    // SUB @0x004e8153 -- see the header on the inversion
        wp.ai_bldg_queue[queue_index].status |= QUEUE_STATUS_DONE_AND_REMOVED; // OR 0xc0 @0x004e8159
        return;
    }

    ++rep.paid;
    // 0x004e8165-0x004e8177. THE THIRD ARGUMENT IS THE UNIT TYPE, loaded into EBX before the other
    // two -- which is what exposed the callee's old `repair_start` name as wrong (renamed
    // 2026-08-23; order 0x6d adds to the producer's production queue).
    gc.bldg_order_production_add_enqueue((uint16_t)player, producer, (int32_t)qe.tick_or_unit_id);
    gc.econ_track_unit_resource_spend((int32_t)player, (int32_t)qe.tick_or_unit_id); // 0x004e8185
    debit_reserved_costs(v, own, player, queue_index);
    wp.ai_promo_credit += *v.promo_sub;                             // ADD @0x004e81cd
    wp.ai_bldg_queue[queue_index].status |= QUEUE_STATUS_COMMITTED; // OR 0x80 @0x004e81d3
}

// ---- nibble 1 -----------------------------------------------------------------------------------
void bldg_queue_process_entry(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              uint32_t player, int32_t queue_index, queue_dispatch_report &rep) {
    ++rep.entry_calls;
    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    // ---- stage A: resolve a build tile, only when none is cached ----
    if (pd.ai_bldg_queue[queue_index].build_tile_x == QUEUE_TILE_UNRESOLVED) { // 0x004e7e0a, SIGNED
        // This is what FILLS the shared site-candidate scratch. It is not a query.
        gc.bldg_production_type_dispatch(
            player, (int32_t)pd.ai_bldg_queue[queue_index].tick_or_unit_id); // 0x004e7e1d

        if (*v.site_candidate_count != 0) { // 0x004e7e22
            ++rep.tile_resolved;
            // Candidate [0] only, and both coordinates are stored as the LOW WORD of the int32
            // columns (MOV AX,[...] @0x004e7e2b / 0x004e7e38).
            auto &we                = wp.ai_bldg_queue[queue_index];
            we.build_tile_x         = (int16_t)(uint16_t)v.site_candidates[0].tile_x;
            we.resource_reserved[0] = (int16_t)(uint16_t)v.site_candidates[0].tile_y;

            const auto &qe = pd.ai_bldg_queue[queue_index];
            gc.notify_map_changed((int32_t)player, (int32_t)qe.tick_or_unit_id,
                                  (int32_t)cached_tile_x(qe), (int32_t)cached_tile_y(qe));
        } else {
            // Nothing buildable anywhere: retire the entry outright. 0x004e7e61.
            ++rep.tile_scan_empty;
            wp.ai_bldg_queue[queue_index].status |= QUEUE_STATUS_DONE_AND_REMOVED;
        }
    }

    // ---- stage B: the footprint test, re-read from memory (stage A may have just written it) ----
    if (pd.ai_bldg_queue[queue_index].build_tile_x == QUEUE_TILE_UNRESOLVED) { // 0x004e7e87
        ++rep.no_tile;
        return; // JZ to the shared epilogue @0x004e7e8f
    }
    const auto &qe = pd.ai_bldg_queue[queue_index];

    uint8_t *passable       = const_cast<uint8_t *>(v.passable);
    uint8_t *footprint_mask = const_cast<uint8_t *>(&v.cfg_buildings[qe.tick_or_unit_id].area[0][0]);
    // Inverted polarity, as everywhere else: NONZERO means the whole footprint fits.
    const int32_t empty = gc.footprint_scan_for_blocked_cell(
        passable, *v.map_width, *v.map_height, footprint_mask, FOOTPRINT_SPAN, FOOTPRINT_SPAN,
        (int32_t)cached_tile_x(qe), (int32_t)cached_tile_y(qe)); // 0x004e7ed0

    if (empty != 0) {
        ++rep.fits;
        // ---- stage C: build it ----
        if (qe.status & QUEUE_STATUS_AFFORD_WAIVED) { // TEST 0x20 @0x004e7ee0
            ++rep.built_waived;
            wp.ai_bldg_queue[queue_index].status &= (uint8_t)~QUEUE_STATUS_AFFORD_WAIVED; // AND 0xdf
            gc.order_queue_construction_enqueue(cached_tile_x(qe), cached_tile_y(qe),
                                                (uint32_t)qe.tick_or_unit_id,
                                                (uint16_t)player); // 0x004e7f08
            gc.notify_map_changed((int32_t)player, (int32_t)qe.tick_or_unit_id,
                                  (int32_t)cached_tile_x(qe), (int32_t)cached_tile_y(qe));
            // Only when this is the type the construction planner is currently pushing.
            if ((int32_t)qe.tick_or_unit_id == pd.ai_build_candidate_primary) { // CMP @0x004e7f33
                ++rep.population_delta;
                gc.order_population_delta_enqueue(
                    player, v.cfg_buildings[qe.tick_or_unit_id].worker_count); // 0x004e7f4d
            }
            // NO resource debit on this path -- falls straight to the shared tail.
        } else {
            ++rep.built_instant;
            gc.bldg_instant_construct_find_slot_enqueue(cached_tile_x(qe), cached_tile_y(qe),
                                                        (int32_t)qe.tick_or_unit_id,
                                                        /*param_4=*/0u,    // XOR ECX,ECX @0x004e7f6d
                                                        (uint16_t)player); // 0x004e7f6f
            gc.bldg_record_resource_expenditure_stats(player,
                                                      (int32_t)qe.tick_or_unit_id); // 0x004e7f7d
            debit_reserved_costs(v, own, player, queue_index);
        }
        // The shared tail both paths converge on, 0x004e7fbf-0x004e7fe1.
        auto &we = wp.ai_bldg_queue[queue_index];
        we.status |= QUEUE_STATUS_COMMITTED;
        we.building_index = QUEUE_BUILDING_INDEX_NONE;
        return;
    }

    // ---- stage D: it did not fit ----
    // The two race-paired building types that are RETIRED rather than retried. Both comparisons read
    // Building[type].type and both are selected by is_alien_race, exactly like the race_* helpers.
    const uint8_t bldg_type = v.cfg_buildings[qe.tick_or_unit_id].type;            // 0x004e7ffe
    const bool    retire    = (bldg_type == race_turret_type(pd.is_alien_race)) || // 0x004e801d
                        // The type is RE-READ from the entry for the second test (0x004e803b);
                        // that is the same value, so it is written as one read.
                        (v.cfg_buildings[qe.tick_or_unit_id].type ==
                         race_relay_type(pd.is_alien_race)); // 0x004e8065
    if (retire) {
        ++rep.blocked_retired;
        wp.ai_bldg_queue[queue_index].status |= QUEUE_STATUS_REMOVED; // OR 0x40 @0x004e8083
    } else {
        ++rep.blocked_requeued;
        // Invalidate the cached tile so stage A runs again next tick. Written as the WORD 0xffff
        // (0x004e80a7), which is the same bit pattern the -1 test reads.
        wp.ai_bldg_queue[queue_index].build_tile_x = QUEUE_TILE_UNRESOLVED;
    }
    gc.notify_map_changed_2((int32_t)player, (int32_t)qe.tick_or_unit_id,
                            (int32_t)cached_tile_x(qe), (int32_t)cached_tile_y(qe)); // 0x004e80e5
}

// ---- nibble 2 -----------------------------------------------------------------------------------
void bldg_queue_handle_state2_empty(queue_dispatch_report &rep) {
    // The whole original: PUSH 0x4 / CALL assert_stack_capacity / RET. Nothing to reproduce but the
    // fact that the slot exists and does nothing.
    ++rep.noop_calls;
}

// ---- nibbles 3 and 4 ----------------------------------------------------------------------------
void bldg_queue_handle_upgrade_or_cancel(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         uint32_t player, int32_t queue_index,
                                         queue_dispatch_report &rep) {
    ++rep.upgrade_calls;
    player_data &wp = own.players[player];

    // Stamped FIRST, then the byte is RE-READ and masked -- the write happens whichever arm is
    // taken, so it cannot be moved below the branch. 0x004e8215-0x004e8228.
    wp.ai_bldg_queue[queue_index].status |= QUEUE_STATUS_COMMITTED;
    const uint8_t kind = (uint8_t)(v.players[player].ai_bldg_queue[queue_index].status &
                                   QUEUE_KIND_MASK);

    const int32_t building_index = v.players[player].ai_bldg_queue[queue_index].building_index;
    if (kind == 4) {
        ++rep.upgraded;
        debit_reserved_costs(v, own, player, queue_index);
        gc.bldg_order_upgrade_enqueue((uint32_t)(uint16_t)player, building_index); // 0x004e8267
    } else {
        // Nibble 3 in practice, but the original tests only `!= 4`, so any other nibble routed here
        // by the parent's table would take this arm too.
        ++rep.cancelled;
        gc.bldg_order_repair_cycle_start_enqueue((uint32_t)(uint16_t)player,
                                                 building_index); // 0x004e8278
    }
}

} // namespace detail

void bldg_queue_handle_recruit_state(uint32_t player, int32_t queue_index) {
    const ai_state                st = state();
    detail::queue_dispatch_report rep{};
    detail::bldg_queue_handle_recruit_state(st.read, st.own, live_calls(), player, queue_index, rep);
}

void bldg_queue_process_entry(uint32_t player, int32_t queue_index) {
    const ai_state                st = state();
    detail::queue_dispatch_report rep{};
    detail::bldg_queue_process_entry(st.read, st.own, live_calls(), player, queue_index, rep);
}

void bldg_queue_handle_state2_empty() {
    detail::queue_dispatch_report rep{};
    detail::bldg_queue_handle_state2_empty(rep);
}

void bldg_queue_handle_upgrade_or_cancel(uint32_t player, int32_t queue_index) {
    const ai_state                st = state();
    detail::queue_dispatch_report rep{};
    detail::bldg_queue_handle_upgrade_or_cancel(st.read, st.own, live_calls(), player, queue_index,
                                                rep);
}


} // namespace mh::ai
