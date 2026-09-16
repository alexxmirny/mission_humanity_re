//
// sim/sim_mine_scan_deposit_slot.cpp -- see sim_mine_scan_deposit_slot.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_mine_scan_deposit_slot_0047ab20.asm).
//
#include "sim/sim_mine_scan_deposit_slot.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const mine_scan_deposit_slot_calls &live_mine_scan_deposit_slot_calls() {
    static const mine_scan_deposit_slot_calls c = {
        MH_LIBMH_BIND(llm_map_wrap_delta_x),
        MH_LIBMH_BIND(llm_map_wrap_delta_y),
    };
    return c;
}

namespace {

// Inlines utils_math_trunc @0x004d0596 (round-toward-zero via the x87 control-word dance) around the
// FILD/FMUL @0x0047b071-0x0047b077 and the FISTP dword @0x0047b07f -- utils_math_trunc cannot be
// called through mh::call:: (MH_UNAVAILABLE__parameter_storage_not_marshallable, addr/mh_calls.gen.h:
// it takes its argument on the x87 stack, not a register/stack slot a C signature can express), so this
// reproduces its body verbatim exactly like ai/ai_mine_yield.cpp's own x87_scale_and_trunc does for the
// SAME callee -- only the final store width differs (FISTP DWORD here vs. that TU's FISTP QWORD),
// matching THIS call site's own instruction.
int32_t mine_extract_rate_trunc(int32_t extract_val, double scale) {
    return ::mh::fp::trunc_i32_mul(extract_val, scale);
}

} // namespace

namespace detail {

uint32_t mine_scan_deposit_slot(const sim_view &v, sim_store &own, uint8_t slot_index, uint32_t player,
                                int32_t building_index, const mine_scan_deposit_slot_calls &c) {
    // ---- 0x0047ab3f-0x0047ab8c: buildings[player][building_index] and its cfg TYPE record, read once.
    // No callee in this function's body writes `buildings` (wrap_delta_x/_y are proven 0-write pure
    // functions -- sim_map_wrap_delta_xy.h's own header; utils_math_trunc touches no memory besides its
    // own operand -- ai_mine_yield.cpp's header), so caching these two references (rather than the
    // original's repeated fresh-address re-derivation at every use) cannot observe a stale value.
    const building     &b         = building_of(v, player, building_index);
    const cfg_building &cb        = v.cfg_buildings[b.building_id];
    const int32_t       mine_slot = b.sub_id;

    // ---- the return value at every RET is INCIDENTAL leftover register state, not a designed result --
    // see the header's own note. Both early-exit paths return this SAME formula.
    const uint32_t raw_offset = player * 0x700u + static_cast<uint32_t>(mine_slot) * 0x38u +
                                static_cast<uint32_t>(slot_index) * 0xdu;

    mine &m    = own.mine_at(player, mine_slot);
    auto &slot = m.deposit_slot[slot_index];

    // ---- 0x0047ab99-0x0047abc1: bind resource_id = LOW BYTE of the building type's extract_id[slot]. --
    slot.resource_id          = static_cast<uint8_t>(cb.extract_id[slot_index]);
    const int32_t resource_id = slot.resource_id;

    if (resource_id == 0) { // 0x0047abd3-0x0047abfc: unbound -- no search, extract_rate=0, return.
        slot.extract_rate = 0;
        return raw_offset;
    }

    // ---- 0x0047ac01-0x0047acb3: phase-1 scan-box setup. `half_w` is the SAME field (cfg width, /2,
    // truncating) used for BOTH the column AND the row center -- see the header's WIDTH-FOR-BOTH-AXES
    // note; the row center is ALSO wrapped with width_mask, not height_mask.
    const uint32_t width_mask  = map_width_mask(v);
    const uint32_t height_mask = map_height_mask(v);
    const int32_t  half_w      = static_cast<int32_t>(cb.width) / 2; // nonneg byte, SAR-idiom == '/2'

    const uint32_t center_col = (static_cast<uint32_t>(b.x) + static_cast<uint32_t>(half_w)) & width_mask;
    const uint32_t center_row =
        (static_cast<uint32_t>(b.y) + static_cast<uint32_t>(half_w)) & width_mask; // width_mask, not a typo

    const uint32_t scan_col_start = (center_col - 10u) & width_mask;
    const uint32_t scan_row_start = (center_row - 10u) & height_mask; // the ONE site using height_mask

    // ---- 0x0047acba-0x0047ae07: the 21x21 (radius-10) box scan for the closest map::resources cell
    // with `.value[resource_id] != 0`. Ties are decided by scan order (strict `<` keeps the FIRST-found
    // minimum), matching the original's `JGE <skip>` polarity.
    int32_t  phase1_best_dist = *v.map_width + *v.map_height; // the sentinel: "no candidate yet"
    uint32_t best_col = 0, best_row = 0;

    uint32_t cur_col = scan_col_start;
    for (int32_t oc = 0; oc < 21; ++oc) {
        uint32_t cur_row = scan_row_start;
        for (int32_t ic = 0; ic < 21; ++ic) {
            const uint32_t region_idx = (cur_col / 4u) * 64u + (cur_row / 4u); // resources[col/4][row/4]
            if (v.resources[region_idx].value[resource_id] != 0) {
                // ---- |wrap_delta_x|/|wrap_delta_y|: test-call then a FRESH re-call for the value --
                // see the header's ABS-VIA-REDUNDANT-CALL note; both calls take IDENTICAL arguments,
                // never a cached register.
                const int32_t dx_probe =
                    c.wrap_delta_x(static_cast<int32_t>(cur_col), cur_row, static_cast<int32_t>(center_col));
                const int32_t dx =
                    (dx_probe < 0)
                        ? -c.wrap_delta_x(static_cast<int32_t>(cur_col), cur_row, static_cast<int32_t>(center_col))
                        : c.wrap_delta_x(static_cast<int32_t>(cur_col), cur_row, static_cast<int32_t>(center_col));
                const int32_t dy_probe = c.wrap_delta_y(static_cast<int32_t>(cur_col), cur_row,
                                                        static_cast<int32_t>(center_col), center_row);
                const int32_t dy =
                    (dy_probe < 0)
                        ? -c.wrap_delta_y(static_cast<int32_t>(cur_col), cur_row, static_cast<int32_t>(center_col),
                                          center_row)
                        : c.wrap_delta_y(static_cast<int32_t>(cur_col), cur_row, static_cast<int32_t>(center_col),
                                         center_row);
                const int32_t dist = dx + dy;
                if (dist < phase1_best_dist) {
                    phase1_best_dist = dist;
                    best_col         = cur_col;
                    best_row         = cur_row;
                }
            }
            cur_row = (cur_row + 1u) & height_mask;
        }
        cur_col = (cur_col + 1u) & width_mask;
    }

    // ---- 0x0047ae07-0x0047ae15: no candidate found anywhere in the box -> "site lost". -----------------
    const int32_t sentinel = *v.map_width + *v.map_height;
    if (sentinel <= phase1_best_dist) {
        slot.extract_rate = 0; // 0x0047b0c9
        return raw_offset;
    }

    // ---- 0x0047ae1b-0x0047ae35: freeze phase 1's find; reset the distance sentinel for phase 2. --------
    const uint32_t anchor_col       = best_col;
    const uint32_t anchor_row       = best_row;
    int32_t        phase2_best_dist = *v.map_width + *v.map_height;

    // ---- 0x0047ae3c-0x0047afaa: phase-2 scan over the building TYPE's area[w][h] footprint mask,
    // outer bound = cfg height, inner bound = cfg width (see the header's own index-order derivation).
    // Only the DISTANCE survives past this loop -- see the header's PHASE 2's DEAD STORE note for why no
    // per-cell best-position variable is modeled here.
    for (int32_t h = 0; h < static_cast<int32_t>(cb.height); ++h) {
        for (int32_t w = 0; w < static_cast<int32_t>(cb.width); ++w) {
            if (cb.area[w][h] == 0) continue; // 0x0047ae93: inactive footprint cell

            const uint32_t cand_col = (static_cast<uint32_t>(b.x) + static_cast<uint32_t>(w)) & width_mask;
            const uint32_t cand_row =
                (static_cast<uint32_t>(b.y) + static_cast<uint32_t>(h)) & width_mask; // width_mask again

            const int32_t dx_probe = c.wrap_delta_x(static_cast<int32_t>(cand_col), cand_row,
                                                    static_cast<int32_t>(anchor_col));
            const int32_t dx =
                (dx_probe < 0)
                    ? -c.wrap_delta_x(static_cast<int32_t>(cand_col), cand_row, static_cast<int32_t>(anchor_col))
                    : c.wrap_delta_x(static_cast<int32_t>(cand_col), cand_row, static_cast<int32_t>(anchor_col));
            const int32_t dy_probe = c.wrap_delta_y(static_cast<int32_t>(cand_col), cand_row,
                                                    static_cast<int32_t>(anchor_col), anchor_row);
            const int32_t dy =
                (dy_probe < 0)
                    ? -c.wrap_delta_y(static_cast<int32_t>(cand_col), cand_row, static_cast<int32_t>(anchor_col),
                                      anchor_row)
                    : c.wrap_delta_y(static_cast<int32_t>(cand_col), cand_row, static_cast<int32_t>(anchor_col),
                                     anchor_row);
            const int32_t dist = dx + dy;
            if (dist < phase2_best_dist) {
                phase2_best_dist = dist; // per-cell (cand_col,cand_row) "best" is the dead store; not modeled
            }
        }
    }

    // ---- 0x0047b00e-0x0047b048: pick the extract-rate scale from phase 2's distance. -------------------
    double scale;
    if (phase2_best_dist == 0) {
        scale = 1.0; // exact cell
    } else if (phase2_best_dist < 5) {
        scale = 0.5; // near
    } else {
        scale = 0.1; // far (0x3fb99999_9999999a bit pattern in the original -- the same literal here)
    }

    // ---- 0x0047afaa-0x0047b008: the final writes use PHASE 1's frozen anchor, never phase 2's. ---------
    slot.tile_x_q4 = static_cast<int32_t>(anchor_col) / 4;
    slot.tile_y_q4 = static_cast<int32_t>(anchor_row) / 4;

    // ---- 0x0047b048-0x0047b09e: extract_rate = trunc(cfg extract_val[slot] * scale). --------------------
    slot.extract_rate = mine_extract_rate_trunc(cb.extract_val[slot_index], scale);

    // 0x0047b0a4-0x0047b0ab: the trailing `CMP AX, PlayerSide` is dead (no branch consumes it) -- not
    // reproduced. Success-path return = the raw `player` parameter, full 32 bits (see the header note).
    return player;
}

} // namespace detail

// ---- the public wrapper -----------------------------------------------------------------------------

uint32_t mine_scan_deposit_slot(uint8_t slot_index, uint32_t player, int32_t building_index) {
    sim_state st = state();
    return detail::mine_scan_deposit_slot(st.read, st.own, slot_index, player, building_index,
                                          live_mine_scan_deposit_slot_calls());
}


} // namespace mh::sim
