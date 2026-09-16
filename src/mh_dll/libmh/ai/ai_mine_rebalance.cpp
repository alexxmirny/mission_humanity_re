//
// ai/ai_mine_rebalance.cpp -- see ai_mine_rebalance.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_mine_portfolio_rebalance_004e3418.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h and addr/mh_addrs.gen.h. The player row
// stride 27300 = 100 * 0x111 is built by SHL 3 / SUB / SHL 2 / SHL 4 / SUB / SHL 6 / ADD at
// 0x004e3458-0x004e3469, and the slot stride 273 = 0x111 by SHL 4 / ADD / SHL 4 / ADD @0x004e3499:
//   0xc3d2a0 = map::g::buildings[0][0]        (+0x0 index, +0x2 building_id, +0xd state,
//                                              +0xc3 x, +0xc4 y -- all four checked against
//                                              mh_map_object_building's static_asserts)
//   0xd9ec88 = cfg Building + 0x8             = Building[].type
//   0x101310c / 0x101318c / 0x101330c         = the three scratch tables (4 / 12 / 32 byte rows)
//   0x01013040 = _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT
//   0x006693ac = _G_LLM_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_mine_rebalance.h"


namespace mh::ai {
namespace detail {

namespace {

// The retire sequence, identical in both passes: enqueue the restart order, take the mine's estimate
// back out of the running portfolio, flush the pending training entries, clear the roster slot.
// Pass 2 at 0x004e36da-0x004e3721, pass 3 at 0x004e37fa-0x004e383c -- the same five steps in the same
// order, differing only in which row index they use and in what they do afterwards.
void retire_mine(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                 int32_t row, int32_t *out_yield) {
    const int32_t roster = v.mine_roster_index[row]; // 0x004e36da / 0x004e37fd
    // MOVZX word: the player is truncated to 16 bits for THIS call (0x004e36e1 / 0x004e3804) and
    // passed as a full dword to the flush two steps later (0x004e370e / 0x004e3831).
    gc.bldg_order_restart_construction_enqueue((uint16_t)player, roster);

    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) // 0x004e36f1 / 0x004e3814
        out_yield[r] -= v.mine_yield_estimate[row].by_resource[r];

    gc.queue_flush_unit_train_entries((int32_t)player); // 0x004e3711 / 0x004e3834
    own.mine_roster_index[row] = 0;                     // 0x004e3716 / 0x004e383c
}

// out_yield[0] = sum(out_yield[1..4]). Emitted FOUR times in the original -- after pass 1
// (0x004e3584), after pass 2 (0x004e3731), and at the shared exit reached by every early return
// (0x004e3853) -- so it is one helper here rather than four copies.
void resum_total(int32_t *out_yield) {
    out_yield[0] = 0;
    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) out_yield[0] += out_yield[r];
}

} // namespace

mine_rebalance_report mine_portfolio_rebalance(const ai_view &v, const ai_store &own,
                                               const ai_calls &gc, uint32_t player,
                                               int32_t *out_yield) {
    mine_rebalance_report rep{};

    // 0x004e3433: r = 0..4 INCLUSIVE. The estimator's own init loop starts at 1; this one does not.
    for (int32_t r = 0; r <= RESOURCE_ID_LAST; ++r) out_yield[r] = 0;

    int32_t collected = 0; // [EBP-0x14]
    int32_t live      = 0; // [EBP-0x20]

    // ---- pass 1: collect, 0x004e347e-0x004e3574 ----
    // The BUDGET is buildings[player][0].index read as an UNSIGNED word (MOVZX @0x004e346e) and the
    // loop head tests it `> 0` UNSIGNED (CMP + JA @0x004e3570). An empty slot does not spend it.
    uint32_t budget = (uint32_t)(uint16_t)building_of(v, player, 0).index;
    for (int32_t slot = 1; budget > 0; ++slot) {
        const building &b = building_of(v, player, slot);
        if (b.building_id == 0) continue; // JZ straight to the INC, budget untouched (0x004e34a7)

        const uint8_t type = v.cfg_buildings[b.building_id].type; // 0x004e34c2
        if (type == BLDG_TYPE_A_MINE || type == BLDG_TYPE_H_MINE) {
            if (collected >= MINE_SCRATCH_ROWS) ++rep.overrun_rows; // see the header -- not a clamp

            own.mine_roster_index[collected] = slot; // 0x004e34db
            // The two out-pointers are row `collected` of the parallel tables (0x004e34e2 /
            // 0x004e34ee), and the tile is the BUILDING's own x/y, read as unsigned bytes.
            // committed llm_strat_ai_calc_mine_yield_estimate's 5th param is int32_t *; the real
            // payload is mh::ai::mine_quality (near/mid/far ring counts).
            gc.calc_mine_yield_estimate(
                b.building_id, (uint32_t)b.x, (uint32_t)b.y,
                &own.mine_yield_estimate[collected].by_resource[0],
                reinterpret_cast<int32_t *>(&own.mine_quality_hist[collected]));

            for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) // 0x004e3548
                out_yield[r] += v.mine_yield_estimate[collected].by_resource[r];

            ++collected; // 0x004e3566
            ++live;      // 0x004e3569
        }
        --budget; // 0x004e356c -- reached by BOTH the mine and the non-mine arm
    }
    rep.collected = collected;
    rep.live      = live;

    // 0x004e357e: nothing collected -> return with only the five zeros written.
    if (collected == 0) return rep;

    resum_total(out_yield); // 0x004e3584
    // The empty loop at 0x004e359e-0x004e35a6 goes here in the original. It is dead; see the header.

    // ---- the two gates, 0x004e35a8-0x004e35fc ----
    rep.min_count = *v.mine_rebalance_min_count;
    if ((uint32_t)live < (uint32_t)rep.min_count) { // CMP + JC @0x004e35ab, UNSIGNED
        rep.below_min_count = 1;
        resum_total(out_yield); // the shared exit at 0x004e3853
        return rep;
    }
    // Bound is LIVE, not COLLECTED (CMP ESI,[EBP-0x20] @0x004e35f9) -- equal here, see the header.
    for (int32_t i = 0; i < live; ++i) {
        const int32_t roster = v.mine_roster_index[i];                            // 0x004e35bb
        if (building_of(v, player, roster).state == BLDG_STATE_RESTART_PENDING) { // 0x004e35e9
            rep.hit_state_gate = 1;
            resum_total(out_yield);
            return rep;
        }
    }

    // ---- pass 2: retire every underperformer, 0x004e35fe-0x004e372b ----
    for (int32_t i = 0; i < collected; ++i) {
        uint32_t share_acc = 0; // [EBP-0x18]

        own.mine_yield_estimate[i].by_resource[0] = 0; // 0x004e3618
        for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) {
            const int32_t est = v.mine_yield_estimate[i].by_resource[r];
            own.mine_yield_estimate[i].by_resource[0] += est;                // 0x004e3639
            if (out_yield[r] != 0)                                           // 0x004e3644
                share_acc += (uint32_t)(est * 100) / (uint32_t)out_yield[r]; // IMUL 0x64 + unsigned DIV
        }

        const mine_quality &q = v.mine_quality_hist[i];
        // 0x004e3665-0x004e3687, in the original's order: 5*mid first, then +10*near, then +far.
        const uint32_t score = (uint32_t)(MINE_QUALITY_NEAR_MUL * q.near_count +
                                          MINE_QUALITY_MID_MUL * q.mid_count + q.far_count);

        // FIVE KEEP TESTS, ANY ONE OF WHICH KEEPS THE MINE. All UNSIGNED (JNC at 0x004e3692,
        // 0x004e3697, 0x004e36a4, 0x004e36b1, 0x004e36be; the last arm's JC @0x004e36cb is the only
        // jump to the DROP label).
        bool keep = false;
        if (share_acc >= (uint32_t)*v.mine_low_share_pct_threshold) keep = true;
        else if (score >= MINE_QUALITY_SCORE_KEEP_MIN) keep = true;
        else {
            for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) {
                const uint32_t twice = (uint32_t)v.mine_yield_estimate[i].by_resource[r] * 2u;
                if (twice >= (uint32_t)out_yield[r]) {
                    keep = true;
                    break;
                }
            }
        }
        if (keep) continue; // 0x004e36d8

        retire_mine(v, own, gc, player, i, out_yield);
        ++rep.retired_pass2; // 0x004e3721
        --live;              // 0x004e3724
    }

    resum_total(out_yield);                 // 0x004e3731
    if (rep.retired_pass2 != 0) return rep; // JNZ to the shared exit @0x004e374f

    // ---- pass 3: retire one mine that produces none of the scarcest resource ----
    rep.reached_pass3 = 1;

    // 0x004e3755-0x004e378f. w[0] is the SUM and lives in the slot the four are indexed off
    // ([EBP-0x50], read back as [EBP + b*4 - 0x50] @0x004e379c).
    uint32_t w[RESOURCE_ID_LAST + 1] = {};
    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r)
        w[r] = (uint32_t)(out_yield[r] * (int32_t)MINE_REWEIGHT_MUL) / MINE_REWEIGHT_DIV[r];
    w[0] = w[1] + w[2] + w[3] + w[4];

    for (int32_t b = RESOURCE_ID_FIRST; b <= RESOURCE_ID_LAST; ++b) {
        // "under about an eighth": CMP (w[b] << 3), (w[0] + 1) / JNC @0x004e37a7, UNSIGNED. Note the
        // +1 makes the test strict-ish rather than lenient, and an ALL-ZERO portfolio DOES select
        // resource 1 (0 << 3 == 0, w[0] + 1 == 1, and 0 >= 1 is false so the continue is not taken)
        // -- it then finds every surviving mine's by_resource[1] to be zero and retires the smallest.
        // Kept exactly as written; the wrap at w[0] == 0xffffffff is the original's too.
        if ((w[b] << MINE_SCARCE_SHIFT) >= w[0] + 1) continue;

        rep.scarce_res = b;

        uint32_t best_total = MINE_VICTIM_TOTAL_SENTINEL; // 0x004e37af
        int32_t  victim     = -1;                         // 0x004e37b4
        for (int32_t i = 0; i < collected; ++i) {         // CMP ESI,[EBP-0x14] @0x004e37ef
            if (v.mine_roster_index[i] == 0) continue;    // already retired (0x004e37bf)
            const mine_yield &y = v.mine_yield_estimate[i];
            if (best_total <= (uint32_t)y.by_resource[0]) continue; // JBE @0x004e37d4
            if (y.by_resource[b] != 0) continue;                    // JNZ @0x004e37e3 -- it makes some
            victim     = i;                                         // 0x004e37e5
            best_total = (uint32_t)y.by_resource[0];                // 0x004e37e8
        }
        if (victim == -1) continue; // JZ @0x004e37f8 -- try the next resource

        retire_mine(v, own, gc, player, victim, out_yield);
        ++rep.retired_pass3;
        break; // JMP straight to the shared exit @0x004e3847 -- at most one mine per call
    }

    resum_total(out_yield); // 0x004e3853
    return rep;
}

} // namespace detail

void mine_portfolio_rebalance(uint32_t player, int32_t *out_yield) {
    const ai_state st = state();
    (void)detail::mine_portfolio_rebalance(st.read, st.own, live_calls(), player,
                                           out_yield);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// NOTHING IS STUBBED, and the region set is what makes that safe. The three callees:
//   * llm_strat_ai_calc_mine_yield_estimate writes only through the two out-pointers this body aims
//     at rows of _G_LLM_STRAT_AI_MINE_YIELD_ESTIMATE and _G_LLM_STRAT_AI_MINE_QUALITY_HIST, both
//     declared. Stubbing it would be the WORST option available rather than the safe one: every
//     retire decision below is scored off the table it fills, so a stubbed arm would compare against
//     the ORIGINAL arm's numbers and manufacture a divergence rather than silence one.
//   * llm_strat_bldg_order_restart_construction_enqueue reaches llm_strat_order_enqueue DIRECTLY --
//     never llm_strat_order_scratch_reset / _set_field, so _G_LLM_STRAT_ORDER_SCRATCH_ARGS is NOT on
//     this path, and never llm_strat_order_schedule, so nothing goes on the wire. Two regions:
//     _G_LLM_STRAT_ORDER_QUEUE and _G_LLM_STRAT_ORDER_QUEUE_COUNT.
//   * llm_strat_ai_queue_flush_unit_train_entries writes player_data only.
// Plus _G_LLM_STRAT_AI_MINE_ROSTER_INDEX, which this body owns outright, and
// _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT, which it only READS -- the caller wrote it.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE, and three of the four shapes are the LIKELY ones:
//   * `collected == 0` -- the player owns no mine. Five zeros written, nothing else exercised.
//   * `below_min == calls` -- the argument-channel threshold was never cleared, so neither retire
//     pass ran. This is what an early skirmish start produces: min_count is 2*(four score
//     counters)+1 and a young base has few mines.
//   * `gate == calls` -- a mine was already in state 0x6b on every call.
//   * `p3 == 0 && p2 == 0` with pass 3 REACHED still proves the re-weighting and the victim search
//     ran; `pass3 == 0` alone does not distinguish "no resource was scarce" from "pass 3 never ran",
//     which is why `reached3` is counted separately.
// Read the ai_agg line before the divergence count. A run in which `retired == 0` has not exercised
// the order-enqueue path, the flush call, or the roster-slot clear -- i.e. everything this function
// actually DOES to the game, as opposed to what it computes.

} // namespace mh::ai
