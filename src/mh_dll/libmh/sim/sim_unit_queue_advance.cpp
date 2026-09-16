//
// sim/sim_unit_queue_advance.cpp -- see sim_unit_queue_advance.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_queue_advance_004dbf27.asm,
// tmp/decomp_sim/llm_strat_unit_queue_advance_search_004d7f3a.asm) -- see the header banner for the
// re-derived DFS-with-backtracking shape, the packed-tile / byte-width neighbour-delta transcription
// notes, and the `.building`-holds-a-unit-index idiom.
//
#include "sim/sim_unit_queue_advance.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_strat_order_issue_0xf_adjacent_by_offset -- bound live below
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_queue_advance_search_calls &live_unit_queue_advance_search_calls() {
    static const unit_queue_advance_search_calls c = {
        MH_LIBMH_BIND(llm_strat_order_issue_0xf_adjacent_by_offset),
    };
    return c;
}

namespace detail {

// _G_LLM_UNITQ_NEIGHBOR_DX / _DY are int32_t[8], 8-neighbour compass deltas -- see the header
// banner's DECLARED NEED. Written AS IF `v.unitq_neighbor_dx`/`v.unitq_neighbor_dy` already exist.

// Packs (x, y) the same way `_G_LLM_UNITQ_FRONTIER[].tile` / `_G_LLM_UNITQ_CUR_TILE` do: x in the
// high byte, y in the low byte -- the SAME packing sim_view::tile_at()'s `(tile_x<<8)|tile_y`
// indexing already assumes, so the result doubles directly as a flat passable[]/tile_objects[]
// index. Local to this TU per the "no new shared helpers" rule (sim_unit_state_move_walker.cpp's
// unit_full_id_word() is the precedent for a small per-TU packing helper of this kind).
inline uint32_t pack_tile(uint8_t x, uint8_t y) {
    return (static_cast<uint32_t>(x) << 8) | static_cast<uint32_t>(y);
}

int32_t unit_queue_advance_search(const sim_view &v, sim_store &own, uint32_t player,
                                  uint32_t unit_index, const unit_queue_advance_search_calls &c) {
    own.unitq_cur_tile_mut()       = 0; // 0x004d7f4e
    own.unitq_frontier_count_mut() = 0; // 0x004d7f58
    own.unitq_closed_count_mut()   = 0; // 0x004d7f62

    uint32_t cur_unit = unit_index; // EBX across outer-loop iterations

    for (;;) { // LAB_004d7f6c
        // ---- stage the current node into FRONTIER[frontier_count], UNCOMMITTED (count unchanged) --
        const int32_t stage_idx               = own.unitq_frontier_count_mut();
        const unit   &su                      = v.units[player * v.caps.units + cur_unit];
        const uint8_t stage_x                 = su.x; // 0x004d7f80
        const uint8_t stage_y                 = su.y; // 0x004d7f8d
        own.unitq_frontier_at(stage_idx).tile = pack_tile(stage_x, stage_y);
        own.unitq_frontier_at(stage_idx).unit = static_cast<uint16_t>(cur_unit); // 0x004d7f9a

        // ---- loop 1 (0x004d7fa9-0x004d801b): any of the 8 neighbours passable and unowned? --------
        bool found_free_tile = false;
        for (int32_t dir = 0; dir < 8; ++dir) {
            const uint8_t  nx        = static_cast<uint8_t>((static_cast<int32_t>(stage_x) + v.unitq_neighbor_dx[dir]) &
                                                            static_cast<int32_t>(*v.width_m));
            const uint8_t  ny        = static_cast<uint8_t>((static_cast<int32_t>(stage_y) + v.unitq_neighbor_dy[dir]) &
                                                            static_cast<int32_t>(*v.height_m));
            const uint32_t cur_tile  = pack_tile(nx, ny);
            own.unitq_cur_tile_mut() = cur_tile; // 0x004d7fe1

            if (v.passable[cur_tile] != 0 && v.passable[cur_tile] != 5 &&
                v.tile_objects[cur_tile].class_owner == 0) {
                own.unitq_frontier_at(stage_idx).dir = static_cast<uint16_t>(dir); // 0x004d8008
                own.unitq_frontier_count_mut()       = stage_idx + 1;              // 0x004d800f
                found_free_tile                      = true;
                break; // JMP 0x004d8157
            }
        }

        if (found_free_tile) {
            break; // straight to the issue-orders tail
        }

        // ---- loop 2 (0x004d802d-0x004d8109): a FRIENDLY, not-yet-visited neighbour to descend to? --
        bool descended = false;
        for (int32_t dir = 0; dir < 8; ++dir) {
            const uint8_t  nx        = static_cast<uint8_t>((static_cast<int32_t>(stage_x) + v.unitq_neighbor_dx[dir]) &
                                                            static_cast<int32_t>(*v.width_m));
            const uint8_t  ny        = static_cast<uint8_t>((static_cast<int32_t>(stage_y) + v.unitq_neighbor_dy[dir]) &
                                                            static_cast<int32_t>(*v.height_m));
            const uint32_t cur_tile  = pack_tile(nx, ny);
            own.unitq_cur_tile_mut() = cur_tile; // 0x004d8057

            if (static_cast<uint32_t>(v.tile_objects[cur_tile].class_owner) != (player | 0x80u)) {
                continue; // LAB_004d8108: INC ESI
            }

            // already committed in FRONTIER? (0x004d8077-0x004d8096, via the real
            // _G_LLM_UNITQ_ITER global -- not a local counter, so its final value after this call
            // matches the original byte-for-byte).
            bool already_seen    = false;
            own.unitq_iter_mut() = 0;
            while (own.unitq_iter_mut() < own.unitq_frontier_count_mut()) {
                if (own.unitq_frontier_at(own.unitq_iter_mut()).tile == cur_tile) {
                    already_seen = true;
                    break;
                }
                own.unitq_iter_mut() += 1;
            }
            // already CLOSED (fully explored, dead end)? (0x004d80a9-0x004d80d5)
            if (!already_seen) {
                own.unitq_iter_mut() = 0;
                while (own.unitq_iter_mut() < own.unitq_closed_count_mut()) {
                    if (own.unitq_closed_at(own.unitq_iter_mut()).tile == cur_tile) {
                        already_seen = true;
                        break;
                    }
                    own.unitq_iter_mut() += 1;
                }
            }
            if (already_seen) {
                continue; // LAB_004d8108
            }

            // new node -- commit the staged entry and descend (0x004d80d7-0x004d8103).
            own.unitq_frontier_at(stage_idx).dir = static_cast<uint16_t>(dir);
            own.unitq_frontier_count_mut()       = stage_idx + 1;
            // `.building` is the tile's generic occupant-index slot (see the header banner's idiom
            // note) -- the next node to explore is the friendly unit occupying this neighbour tile.
            cur_unit  = v.tile_objects[cur_tile].building;
            descended = true;
            break; // JMP 0x004d7f6c
        }

        if (descended) {
            continue; // back to the top of the outer loop with the new cur_unit
        }

        // ---- dead end: neither loop found anything (0x004d811a) -----------------------------------
        if (own.unitq_frontier_count_mut() == 0) {
            break; // nothing committed at all -- empty chain, straight to the tail
        }

        // BACKTRACK (0x004d8123-0x004d8152): move the staged (dead-end) entry to CLOSED wholesale,
        // pop the stack, and resume from the now-re-exposed parent.
        const int32_t closed_idx         = own.unitq_closed_count_mut();
        own.unitq_closed_at(closed_idx)  = own.unitq_frontier_at(stage_idx); // MOVSD x2 (tile+dir+unit)
        const int32_t new_frontier_count = stage_idx - 1;
        own.unitq_frontier_count_mut()   = new_frontier_count;
        cur_unit                         = own.unitq_frontier_at(new_frontier_count).unit;
        own.unitq_closed_count_mut()     = closed_idx + 1;
        // loop back to LAB_004d7f6c
    }

    // ---- tail (0x004d8157-0x004d81a1): issue the shuffle order to every committed chain node ------
    // DRIVES THE REAL _G_LLM_UNITQ_ITER GLOBAL, not a local counter (reimpl-verify caught a real
    // divergence 2026-08-20: 0x004d8157 unconditionally resets ITER to 0 and 0x004d8189 increments
    // it in lockstep with this loop, so ITER == frontier_count on every return -- a local `int i`
    // left the tracked ITER region stale after a call whose loop-1 success path never touches it
    // anywhere else in the function).
    const int32_t frontier_count = own.unitq_frontier_count_mut();
    own.unitq_iter_mut()         = 0;
    while (own.unitq_iter_mut() < frontier_count) {
        const unitq_search_node &f = own.unitq_frontier_at(own.unitq_iter_mut());
        c.order_issue_0xf_adjacent_by_offset(player, f.unit, v.unitq_neighbor_dx[f.dir],
                                             v.unitq_neighbor_dy[f.dir]);
        own.unitq_iter_mut() += 1;
    }
    return frontier_count;
}

void unit_queue_advance(const sim_view &v, sim_store &own, uint32_t player, uint32_t unit_index,
                        const unit_queue_advance_search_calls &c) {
    // 0x004dbf31: JMP llm_strat_unit_queue_advance_search -- a tail call whose return value this
    // function's own `void` prototype discards. Direct detail:: call, not mh::call:: (see header).
    unit_queue_advance_search(v, own, player, unit_index, c);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t unit_queue_advance_search(uint32_t player, uint32_t unit_index) {
    sim_state st = state();
    return detail::unit_queue_advance_search(st.read, st.own, player, unit_index,
                                             live_unit_queue_advance_search_calls());
}

void unit_queue_advance(uint32_t player, uint32_t unit_index) {
    sim_state st = state();
    detail::unit_queue_advance(st.read, st.own, player, unit_index,
                               live_unit_queue_advance_search_calls());
}


} // namespace mh::sim
