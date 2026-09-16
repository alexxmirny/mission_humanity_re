//
// sim/sim_map_region_routing.cpp -- see sim_map_region_routing.h. Translated address-by-address
// from the DISASSEMBLY:
//   tmp/decomp_sim/llm_map_region_route_search_004236c2.asm
//   tmp/decomp_sim/llm_map_region_find_route_00423d86.asm
//   tmp/decomp_sim/llm_map_region_flood_reachable_00424ee0.asm
//
#include "sim/sim_map_region_routing.h"

#include "addr/mh_calls.gen.h"  // typed callables for the ORIGINAL functions this closure still calls out to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const map_region_routing_calls &live_map_region_routing_calls() {
    static const map_region_routing_calls c = {
        MH_LIBMH_BIND(llm_map_region_route_prepass),
        MH_LIBMH_BIND(llm_map_region_route_mark_shared_nodes),
    };
    return c;
}

namespace detail {

// llm_map_region_route_search @0x004236c2. See the header banner for the full derivation.
int32_t region_route_search(const sim_view &v, sim_store &own, uint16_t start_region,
                            int16_t target_region, uint8_t *out_path) {
    // 0x004236e1-0x004236f1: prologue writes a blank {0,0} entry at step 0 unconditionally.
    out_path[0] = 0;
    out_path[1] = 0;

    uint16_t       current     = start_region;
    const uint16_t target      = static_cast<uint16_t>(target_region);
    int32_t        step_index  = 0; // [EBP-0x28]
    const uint16_t wrap_mask16 = static_cast<uint16_t>(*v.region_coord_wrap_mask);

    // Writes candidate direction `dir`'s cell into scratch slot `dir` (row/y-updated byte at +0,
    // col/x-updated byte at +1 -- see the header banner's byte-layout derivation, which disagrees
    // with the existing manifest note), wrap-masks it in place, and returns the masked packed cell.
    auto write_candidate = [&](int32_t dir) -> uint16_t {
        const uint8_t cur_row = static_cast<uint8_t>(current & 0xFFu);
        const uint8_t cur_col = static_cast<uint8_t>((current >> 8) & 0xFFu);
        // DECLARED NEED (header item 1): v.region_route_step_deltas does not exist yet.
        const uint8_t row_delta                 = v.region_route_step_deltas[dir * 4 + 0];
        const uint8_t col_delta                 = v.region_route_step_deltas[dir * 4 + 1];
        own.region_route_cand_byte(dir * 4 + 1) = static_cast<uint8_t>(cur_col + col_delta);
        own.region_route_cand_byte(dir * 4 + 0) = static_cast<uint8_t>(cur_row + row_delta);
        uint16_t packed                         = static_cast<uint16_t>(
            static_cast<uint16_t>(own.region_route_cand_byte(dir * 4 + 0)) |
            static_cast<uint16_t>(own.region_route_cand_byte(dir * 4 + 1) << 8));
        packed &= wrap_mask16;
        own.region_route_cand_byte(dir * 4 + 0) = static_cast<uint8_t>(packed & 0xFFu);
        own.region_route_cand_byte(dir * 4 + 1) = static_cast<uint8_t>((packed >> 8) & 0xFFu);
        return packed;
    };
    // Reads scratch bytes [byte_index, byte_index+1) back as a packed word. `byte_index==32` (the
    // direction-7 "+4" fold) legitimately lands on _G_LLM_MAP_REGION_ROUTE_BEST_CAND, one slot past
    // the 32-byte array -- see the header banner's OOB-read note and DECLARED NEED #3. NOT
    // bounds-checked, matching own.region_route_cand_byte()'s own documented posture.
    auto scratch_word = [&](int32_t byte_index) -> uint16_t {
        return static_cast<uint16_t>(
            static_cast<uint16_t>(own.region_route_cand_byte(byte_index)) |
            static_cast<uint16_t>(own.region_route_cand_byte(byte_index + 1) << 8));
    };
    auto cell_value = [&](uint16_t packed) -> uint32_t {
        return own.region_cell_at(packed >> 8, packed & 0xFFu).terrain_flags & 0xFFFFFF00u;
    };

    while (current != target) {
        int32_t  best_dir   = -1;                  // [EBP-0x24]
        uint32_t best_value = cell_value(current); // seeded from the CURRENT cell's own value

        // ---- loop 1 (0x0042372f-0x004237be): even directions 0,2,4,6 --------------------------
        for (int32_t dir = 0; dir < 8; dir += 2) {
            const uint16_t packed = write_candidate(dir);
            const uint32_t val    = cell_value(packed);
            if (val != 0 && val < best_value) {
                best_value = val;
                best_dir   = dir;
            }
        }

        // 0x004237be-0x004237c3: copy scratch slot 0's raw 4 bytes into BEST_CAND, unconditionally
        // (regardless of which direction actually won loop 1) -- see the header banner's OOB-read
        // note; reproduced via the same unchecked byte accessor.
        for (int32_t b = 0; b < 4; ++b) {
            own.region_route_cand_byte(32 + b) = own.region_route_cand_byte(b);
        }

        // ---- loop 2 (0x004237d5-0x0042389c): odd directions 1,3,5,7, with the extra flank guard --
        for (int32_t dir = 1; dir < 8; dir += 2) {
            const uint16_t packed = write_candidate(dir);
            const uint32_t val    = cell_value(packed);
            if (val == 0) continue; // 0x00423849: blocked, skip entirely

            // 0x0042384b-0x00423881: at least one of slot(dir-1)/slot(dir+1)'s OWN region value
            // must have nonzero upper-24-bits, else this candidate is skipped too. dir==7's "+4"
            // fold is the shipped one-past-the-end BEST_CAND read (see the header banner).
            const uint16_t prev_packed = scratch_word((dir - 1) * 4);
            bool           flank_open =
                (own.region_cell_at(prev_packed >> 8, prev_packed & 0xFFu).terrain_flags &
                 0xFFFFFF00u) != 0;
            if (!flank_open) {
                const uint16_t next_packed = scratch_word((dir + 1) * 4);
                flank_open =
                    (own.region_cell_at(next_packed >> 8, next_packed & 0xFFu).terrain_flags &
                     0xFFFFFF00u) != 0;
                if (!flank_open) continue; // 0x00423897
            }

            if (val < best_value) {
                best_value = val;
                best_dir   = dir;
            }
        }

        if (best_dir == -1) {
            // 0x0042389c-0x004238c6: no viable direction at all -- terminate with a blank entry.
            ++step_index;
            out_path[step_index * 2 + 0] = 0;
            out_path[step_index * 2 + 1] = 0;
            return 0;
        }

        // 0x004238cb-0x004238e2: advance `current` to the winning candidate's (already wrap-masked)
        // cell -- the two padding bytes of the original's dword copy are never read again, so a
        // plain word reconstruction is equivalent.
        current = scratch_word(best_dir * 4);

        // DECLARED NEED (header item 2): v.region_route_dir_codes does not exist yet.
        const int32_t dir_code = v.region_route_dir_codes[best_dir];
        if (out_path[step_index * 2 + 0] == 0) {
            // 0x004238e5-0x00423909: fresh step slot.
            out_path[step_index * 2 + 0] = static_cast<uint8_t>(dir_code);
            out_path[step_index * 2 + 1] = 1;
        } else if (static_cast<uint32_t>(out_path[step_index * 2 + 0]) ==
                   static_cast<uint32_t>(dir_code)) {
            // 0x0042390b-0x0042392f: same direction as the current run -- extend it in place.
            ++out_path[step_index * 2 + 1];
        } else {
            // 0x00423931-0x00423959: direction changed -- open a new run.
            ++step_index;
            out_path[step_index * 2 + 0] = static_cast<uint8_t>(dir_code);
            out_path[step_index * 2 + 1] = 1;
        }
    }

    // 0x0042395e-0x0042397b: current==target -- terminate with a blank entry, success.
    ++step_index;
    out_path[step_index * 2 + 0] = 0;
    out_path[step_index * 2 + 1] = 0;
    return 1;
}

// llm_map_region_find_route @0x00423d86. See the header banner for the full derivation.
int32_t region_find_route(const sim_view &v, sim_store &own, const map_region_routing_calls &c,
                          uint32_t to_tile_idx, uint32_t from_tile_idx) {
    const uint16_t to_idx   = static_cast<uint16_t>(to_tile_idx);   // 0x00423da3: MOVZX word
    const uint16_t from_idx = static_cast<uint16_t>(from_tile_idx); // 0x00423db3: MOVZX word

    llm_map_region *to_region   = own.region_cell_at(to_idx >> 8, to_idx & 0xFFu).region;
    llm_map_region *from_region = own.region_cell_at(from_idx >> 8, from_idx & 0xFFu).region;

    own.region_route_bfs_at(0) = from_region; // 0x00423dd1-0x00423dd4: queue[0] = from_region

    // 0x00423de1-0x00423e04: walk the WHOLE region list via ->next, resetting EVERY region's
    // route_bfs_dist to 0 -- unconditional, not gated on BFS reachability.
    for (llm_map_region *r = *v.region_list_head; r != nullptr; r = r->next) {
        r->route_bfs_dist = 0;
    }

    from_region->route_parent   = nullptr; // 0x00423e06-0x00423e0d
    from_region->route_bfs_dist = 1;       // 0x00423e13-0x00423e1a: source dist=1
    c.route_prepass();                     // 0x00423e20: llm_map_region_route_prepass(), no args

    uint32_t read_idx  = 0; // [EBP-0x20]
    uint32_t write_idx = 1; // [EBP-0x1c]: queue[0] already seeded

    for (;;) {
        if (read_idx >= write_idx) break; // 0x00423e3c false-path / queue exhausted

        llm_map_region *cur = own.region_route_bfs_at(read_idx);
        if (cur == to_region) break; // 0x00423e31 JZ -- dequeued the target, stop expanding

        for (uint32_t j = 0; j < cur->neighbor_count; ++j) {
            llm_map_region *neighbor = cur->neighbors[j];
            if (neighbor->route_bfs_dist != 0) continue; // 0x00423e89: already visited

            neighbor->route_parent             = cur;                     // 0x00423eb2
            neighbor->route_bfs_dist           = cur->route_bfs_dist + 1; // 0x00423ee2
            own.region_route_bfs_at(write_idx) = neighbor;                // 0x00423f08
            ++write_idx;                                                  // 0x00423eff
        }
        ++read_idx; // 0x00423f16
    }

    // 0x00423f1e-0x00423f37: the SAME success check the original re-derives independently after
    // both the early-exit ("found") and the exhaustion ("not found") paths converge here.
    const bool found = (read_idx < write_idx) && (own.region_route_bfs_at(read_idx) == to_region);
    if (!found) {
        return 0; // 0x00423fa5
    }

    // 0x00423f3c-0x00423f67: success path ONLY -- reset EVERY region's route_mark to 0 first.
    for (llm_map_region *r = *v.region_list_head; r != nullptr; r = r->next) {
        r->route_mark = 0;
    }

    // 0x00423f69-0x00423f92: walk the reconstructed chain from to_region back through
    // ->route_parent (terminates at from_region, whose route_parent was seeded null), marking 1.
    for (llm_map_region *r = to_region; r != nullptr; r = r->route_parent) {
        r->route_mark = 1;
    }

    // 0x00423f94-0x00423f9c: the argument is to_region (this function's own [EBP-0x2c] slot,
    // never reassigned) -- NOT from_region. Pointer-as-int32 per the committed prototype (same
    // idiom sim_pathfind_route.cpp's group_path_step_record(step_ctx) call site uses).
    c.route_mark_shared_nodes(static_cast<int32_t>(reinterpret_cast<intptr_t>(to_region)));
    return 1;
}

// llm_map_region_flood_reachable @0x00424ee0. See the header banner for the full derivation.
int32_t region_flood_reachable(const sim_view &v, sim_store &own, int32_t query_cell,
                               int32_t start_cell) {
    // Any raw int32 cell value (query_cell/start_cell, and the queue entries derived from them) is
    // used EXACTLY as the original's full 32-bit EBX*8 addressing -- this helper reproduces that
    // via the algebraic identity (u>>8)*256 + (u&0xff) == u, so it matches even a hypothetical
    // out-of-16-bit-range input identically to the original (see uncertainties[]).
    auto cell_at = [&](uint32_t flat) -> llm_map_region_cell & {
        return own.region_cell_at(static_cast<int32_t>(flat >> 8), static_cast<int32_t>(flat & 0xFFu));
    };

    // ---- phase 1 (0x00424ee8-0x00424f3a): full 256x256 sweep, two chained WRAPPING uint8_t
    // counters -- the wraparound at 256 is the loop-termination mechanism, NOT an `i<256` bound.
    // Trip counts come from *v.map_width / *v.map_height (== "width"/"height", RID_WIDTH/RID_HEIGHT,
    // the SAME globals by address as this function's own raw reads).
    uint8_t col = 0; // BH
    for (int32_t w = *v.map_width; w != 0; --w) {
        uint8_t row = 0; // BL
        for (int32_t h = *v.map_height; h != 0; --h) {
            llm_map_region_cell  &cell   = own.region_cell_at(col, row);
            const llm_map_region *region = cell.region;
            uint32_t              value  = cell.terrain_flags & 0xFFu; // clear upper24, keep obstacle byte
            const uint32_t        flat   = (static_cast<uint32_t>(col) << 8) | row;
            if (region != nullptr && region->route_mark != 0 && v.passable[flat] != 0) {
                value |= 0xFFFFFF00u; // flood-eligible sentinel
            }
            cell.terrain_flags = value;
            ++row; // wraps at 256 via uint8_t overflow
        }
        ++col; // wraps at 256 via uint8_t overflow
    }

    // ---- phase 2 (0x00424f3a-0x0042506d): bounded flood fill from start_cell ------------------
    uint32_t read_idx  = 0; // EDI
    uint32_t write_idx = 1; // ESI

    {
        const uint32_t       seed      = static_cast<uint32_t>(start_cell);
        llm_map_region_cell &seed_cell = cell_at(seed);
        seed_cell.terrain_flags        = (seed_cell.terrain_flags & 0xFFu) | 0x100u; // byte1=1
        own.region_flood_tile_at(0)    = seed;
    }

    const uint32_t wrap_mask = *v.region_coord_wrap_mask;

    while (read_idx != write_idx) {
        const uint32_t cur     = own.region_flood_tile_at(read_idx);
        const uint32_t cur_raw = cell_at(cur).terrain_flags;
        // propagate_value = (upper24) + (low_byte << 8) -- an ARITHMETIC add (0x00424f88), not a
        // bitwise OR: a low-byte value with bit 7 set can carry into byte2. Preserved as `+`.
        const uint32_t propagate_value = (cur_raw & 0xFFFFFF00u) + ((cur_raw & 0xFFu) << 8);

        auto relax = [&](uint32_t nb_idx) {
            llm_map_region_cell &nb      = cell_at(nb_idx);
            const uint32_t       nb_raw  = nb.terrain_flags;
            const uint32_t       nb_high = nb_raw & 0xFFFFFF00u;
            if (nb_high == 0) return;               // not flood-eligible at all
            if (propagate_value >= nb_high) return; // not an improvement (unsigned compare)
            own.region_flood_tile_at(write_idx) = nb_idx;
            write_idx                           = (write_idx + 1) & 0xFFFu;
            nb.terrain_flags                    = (propagate_value & 0xFFFFFF00u) | (nb_raw & 0xFFu); // keep own low byte
        };

        // The four neighbours are computed by CHAINING byte inc/dec + the wrap-mask AND on the SAME
        // register state, exactly as the original (0x00424f8a-0x00425032) -- NOT four independent
        // deltas off `cur`. `ebx` mirrors the x86 EBX register bit-for-bit across the four blocks.
        uint32_t ebx     = cur;
        auto     inc_low = [&](int32_t delta) {
            ebx = (ebx & 0xFFFFFF00u) |
                  static_cast<uint8_t>(static_cast<int32_t>(ebx & 0xFFu) + delta);
        };
        auto inc_high = [&](int32_t delta) {
            ebx = (ebx & 0xFFFF00FFu) | (static_cast<uint32_t>(static_cast<uint8_t>(
                                             static_cast<int32_t>((ebx >> 8) & 0xFFu) + delta))
                                         << 8);
        };
        auto mask_step = [&]() { ebx &= wrap_mask; };

        inc_low(+1);
        mask_step();
        relax(ebx); // row+1

        inc_low(-2);
        mask_step();
        relax(ebx); // row-1 (from the already-masked row+1 state, chained per the original)

        inc_low(+1);
        inc_high(+1);
        mask_step();
        relax(ebx); // row restored, col+1

        inc_high(-2);
        mask_step();
        relax(ebx); // col-1 (from the already-masked col+1 state)

        read_idx = (read_idx + 1) & 0xFFFu;
    }

    // ---- result (0x00425072-0x00425091) ---------------------------------------------------------
    // 1 unless query_cell's upper-24-bits are STILL exactly the untouched-eligible sentinel
    // 0xffffff00 -- note a cell that was never eligible in phase 1 at all (upper24==0) also returns
    // 1 here, per the literal CMP; see uncertainties[].
    const uint32_t q_upper = cell_at(static_cast<uint32_t>(query_cell)).terrain_flags & 0xFFFFFF00u;
    return (q_upper != 0xFFFFFF00u) ? 1 : 0;
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

int32_t region_route_search(uint16_t start_region, int16_t target_region, uint8_t *out_path) {
    sim_state st = state();
    return detail::region_route_search(st.read, st.own, start_region, target_region, out_path);
}

int32_t region_find_route(uint32_t to_tile_idx, uint32_t from_tile_idx) {
    sim_state st = state();
    return detail::region_find_route(st.read, st.own, live_map_region_routing_calls(), to_tile_idx,
                                     from_tile_idx);
}

int32_t region_flood_reachable(int32_t query_cell, int32_t start_cell) {
    sim_state st = state();
    return detail::region_flood_reachable(st.read, st.own, query_cell, start_cell);
}


} // namespace mh::sim
