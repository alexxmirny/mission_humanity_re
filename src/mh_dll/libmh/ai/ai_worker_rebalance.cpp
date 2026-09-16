//
// ai/ai_worker_rebalance.cpp -- see ai_worker_rebalance.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_rebalance_building_workers_004e3bc2.asm), not from Ghidra's C: the
// decompile's `ROUND(fVar7)` after a `floor()`/`utils::math::trunc()` pair is a Watcom-artifact
// spelling of a single `(int)floor(x)`, and it never names the two roster accumulators' cfg columns
// (worker_count vs builder_count) at all -- it just indexes through a raw offset into `Building`.
//
#include "ai/ai_worker_rebalance.h"

#include <cmath>

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

namespace {

// Building.state values the original tests by literal, transcribed from the Ghidra plate's
// CONSTRUCTION/DISMANTLING/UPGRADING labels (mh_map_object_building.state carries no C++ enum type
// yet -- same file-scope-constant discipline as ai_bldg_repair.cpp). This function never tests the
// fourth sibling state (CHARGE_STEP, 0x6a) at all -- only these three.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;

inline bool bldg_state_locked(uint16_t state) {
    return state == BLDG_STATE_CONSTRUCTION || state == BLDG_STATE_UPGRADING ||
           state == BLDG_STATE_DISMANTLING;
}

// Phase 0 (0x004e3beb-0x004e3bf7) and Phase 3's want-computation (0x004e3fb7-0x004e3fdb) both build
// their FP operand the same way: FILD an int (zero/one operands), FMUL by a value already resident on
// the FP stack or read from memory, then FSTP once to a `double` before handing it to the CRT
// `floor`/`trunc` pair. The multiply therefore runs at 80-bit extended precision and is rounded to a
// double exactly ONCE, at the FSTP -- computing `(double)diff * (double)ratio` in plain C++ would let
// the compiler round the float ratio to double FIRST (widening 0.2f's binary error before multiplying)
// and then round the product a second time, which is not bit-identical. Hence the FP work stays here,
// in the original's own instruction order; only the (already-integral) floor result crosses into the
// CRT.
//
// `floor` @0x004daadb is immediately followed by `trunc` @0x004d0596 in the original at both call
// sites. `floor` already yields an integral double, so `trunc` is a no-op on its result -- the pair
// is the original's own float-to-int idiom and means exactly `(int)floor(x)`. Both are real CRT
// functions (not `ai_calls` members, per the batch context), so they are called directly here rather
// than being stubbed or routed through the calls table.
int32_t x87_floor_diff_times_ratio(int32_t diff, const float *ratio) {
    return ::mh::fp::x87_floor_diff_times_ratio(diff, ratio);
}

int32_t x87_floor_worker_count_times_util(int32_t worker_count, double util) {
    return ::mh::fp::x87_floor_worker_count_times_util(worker_count, util);
}

// Phase 2's divide (0x004e3d68) is UNGUARDED: `total` (demand + locked) can be zero, giving +-Inf
// (avail != 0) or NaN (avail == 0, a 0/0). The two clamps that follow read the x87 STATUS WORD
// through FNSTSW/SAHF and branch on it directly, which is why this whole sequence has to stay in
// asm rather than become two C++ `if` statements over the already-divided double:
//
//   Clamp 1 (max_fuck_ratio > util -> flush to 0.0) uses JBE, which tests CF||ZF. Unordered (NaN)
//   sets BOTH, so JBE is taken (flush SKIPPED) on NaN -- and `max_fuck_ratio > NaN` is also false in
//   C++ (IEEE `>` is false whenever an operand is NaN), so this clamp agrees with naive C++ on every
//   input, including NaN and both infinities.
//
//   Clamp 2 (1.0 < util -> flush to 1.0) uses JNC, which tests ONLY CF (= C0). Unordered ALSO sets
//   C0, so JNC is NOT taken (flush FIRES) on NaN too -- but `1.0 < NaN` is false in C++ and would NOT
//   flush. This is a real divergence from the naive translation: on a genuine 0/0 (avail == 0 and
//   total == 0), the hardware clamp forces util to exactly 1.0, while `if (1.0 < util) util = 1.0;`
//   in C++ would leave it as NaN. See uncertainties[] for the case-by-case verdict this produces.
//
// Because the two clamps run in the ORIGINAL's sequence (clamp 2 reads whatever clamp 1 left in
// memory, not the pre-clamp-1 value), both flushes have to stay in that order -- splitting them
// would require assuming max_fuck_ratio < 1.0, which nothing enforces.
//
// CRT-X87-CPP (2026-09-11): THIS ONE IS C++ NOW, and the clamp-2 divergence above is why the
// spelling is `!(1.0 >= util)` rather than the naive `1.0 < util` -- the negation is true on
// unordered, so the NaN IS flushed to 1.0 exactly as the hardware does. Proven, not argued: fptest
// case C4 compares the header against this assembly BY BITS over 1089 inputs at both precision
// settings, 0 differences, with the naive spelling kept as the negative arm. The "has to stay in
// asm" reading above was measured wrong; the ordering constraint it identifies is real and is
// preserved by writing the two clamps as two sequential `if`s over one variable.
double x87_labor_utilization(int32_t avail, int32_t total, const float *max_fuck_ratio) {
    return ::mh::fp::x87_labor_utilization(avail, total, max_fuck_ratio);
}

// Phase 4's final gate, 0x004e408c-0x004e40a5. `pop_total != 0` is already guaranteed by the caller's
// guard immediately above this call, so the divide here is never 0/0 or x/0 -- no NaN/Inf edge exists
// at this call site (unlike Phase 2's).
//
// CRT-X87-CPP (2026-09-11): THIS ONE IS C++ NOW. The single-rounding worry this comment used to
// carry -- "FCOMPP compares ST(0) directly against the 80-bit division result, which a
// `(double)housing_prev / (double)pop_total` would round first" -- is REAL in general and MEASURED
// NOT TO BITE HERE, because both operands are int32 and the threshold is a float: the 53-vs-64-bit
// gap is ~2^-64 relative and the coarsest thing that could land in it is ~2^-61 away. fptest case
// C3, 1089 inputs, both precision settings, 0 differences, with a two-sided width arm. The same
// worry applied to the two floor helpers above STILL STANDS -- they are untested and stay asm.
bool x87_housing_short(int32_t housing_prev, int32_t pop_total, const float *extra_space) {
    return ::mh::fp::x87_housing_short(housing_prev, pop_total, extra_space);
}

} // namespace

int32_t rebalance_building_workers(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player) {
    const pop_stats &pop = v.pop[player];
    player_data     &wp  = own.players[player]; // the ONE field we write: ai_labor_utilization

    // ---- Phase 0: the idle reserve, 0x004e3bd7-0x004e3c30 ----
    int32_t reserve = x87_floor_diff_times_ratio(pop.pop_total - pop.human_in_field, v.unemployed_ratio);
    if (reserve < *v.unemployed_min) reserve = *v.unemployed_min; // SIGNED (JGE @0x004e3c10)
    if (pop.pop_total == pop.housing_prev) reserve = 0;           // @0x004e3c23-0x004e3c2b

    // ---- Phase 1: two accumulators over the roster, 0x004e3c32-0x004e3d3e ----
    // The COUNT-DRIVEN roster-walk skeleton (see the batch context): `i` starts at 1 (slot 0 is the
    // header), `remaining` seeds from buildings[player][0].index reinterpreted as its raw ushort bit
    // pattern, an empty slot advances `i` without consuming `remaining`, and `i` is never bounded.
    int32_t locked = 0;
    int32_t demand = 0;
    {
        int32_t  i         = 1;
        uint32_t remaining = (uint16_t)building_of(v, player, 0).index;
        while (remaining != 0) {
            const building &b = building_of(v, player, i);
            if (b.building_id == 0) {
                ++i;
                continue;
            }
            --remaining;
            if (gc.bldg_is_alive((int32_t)player, i) != 0) {
                if (bldg_state_locked(b.state)) {
                    // Adjacent cfg columns: builder_count is +0x25c (0xd9eedc), NOT worker_count's
                    // +0x258 (0xd9eed8) -- confirmed against addr/mh_structs.gen.h's static_asserts.
                    locked += v.cfg_buildings[b.building_id].builder_count;
                } else if (gc.bldg_uses_workers(player, i) != 0 &&
                           gc.is_worker_priority_candidate((int32_t)player, i) != 0) {
                    demand += v.cfg_buildings[b.building_id].worker_count;
                }
            }
            ++i;
        }
    }

    // ---- Phase 2: the utilisation ratio, 0x004e3d44-0x004e3de0 ----
    const int32_t avail     = pop.pop_total - pop.human_in_field - reserve;
    const int32_t total     = demand + locked; // can be 0 -- see x87_labor_utilization's header
    wp.ai_labor_utilization = x87_labor_utilization(avail, total, v.max_fuck_ratio);

    // ---- Phase 3: issue the orders, a SECOND roster walk, 0x004e3de2-0x004e4039 ----
    {
        int32_t  i         = 1;
        uint32_t remaining = (uint16_t)building_of(v, player, 0).index;
        while (remaining != 0) {
            const building &b = building_of(v, player, i);
            if (b.building_id == 0) {
                ++i;
                continue;
            }
            --remaining;
            if (gc.bldg_is_alive((int32_t)player, i) != 0) {
                // Step 1 (0x004e3eaa-0x004e3e83): activate a not-yet-staffed, non-busy building. This
                // does NOT change `state` -- the order is queued, not applied -- so step 2 re-tests
                // `b.state`/`b.built_flags` fresh through the same live reference rather than reusing a
                // cached verdict, matching the original's two independent memory reads.
                if (!bldg_state_locked(b.state) && (b.built_flags & 2) == 0) {
                    gc.bldg_order_activate_enqueue(player, i);
                }

                // Step 2: the wanted worker count.
                int32_t want;
                bool    have_want = true;
                if (bldg_state_locked(b.state)) {
                    // The zero test at 0x004e3ee1-0x004e3ef6 is `(hi & 0x7fffffff) == 0 && lo == 0` --
                    // a sign-blind +-0.0 test. That bit pattern is +0.0 or -0.0 and NOTHING else (no
                    // NaN/Inf shares it, since the mask only clears the sign bit), so it is exactly
                    // equivalent to a plain `== 0.0` in IEEE double semantics -- the same equivalence
                    // ai_shortage_gate.cpp's `holdings == 0.0` comment already establishes for this
                    // project. Written as `== 0.0` here, not `!= 0.0`, per the batch context's
                    // instruction to reproduce the test rather than its negation.
                    want = (wp.ai_labor_utilization == 0.0)
                               ? 0
                               : v.cfg_buildings[b.building_id].builder_count;
                } else if (gc.bldg_uses_workers(player, i) == 0) {
                    have_want = false; // skip this building entirely -- no step 3 at all
                } else if (gc.is_worker_priority_candidate((int32_t)player, i) == 0) {
                    want = 0; // the predicate's own (zero) return value, per the original's spelling
                } else {
                    want = x87_floor_worker_count_times_util(
                        v.cfg_buildings[b.building_id].worker_count, wp.ai_labor_utilization);
                }

                // Step 3 (0x004e3f68/0x004e401c): one CMP feeds both the assign and unassign branches
                // in the original -- cur < want -> assign; cur == want -> nothing; cur > want ->
                // unassign. Both original branch sites (the locked-state CMP+JGE+JLE pair and the
                // worker-priority CMP+JL+JLE pair) resolve to this same three-way outcome, so one
                // shared comparison here reproduces both.
                if (have_want) {
                    const int32_t cur = b.current_workers;
                    if (cur < want) {
                        gc.bldg_order_assign_workers_enqueue((uint16_t)player, (uint16_t)i,
                                                             (uint32_t)(want - cur));
                    } else if (cur > want) {
                        gc.bldg_order_unassign_workers_enqueue((uint16_t)player, (uint16_t)i,
                                                               (uint32_t)(cur - want));
                    }
                }
            }
            ++i;
        }
    }

    // ---- Phase 4: the return value, 0x004e403f-0x004e40a9 -- a DECISION ("housing is short"), not a
    // status code. `wp.ai_labor_utilization` can never be NaN or infinite here: both Phase 2 clamps
    // collapse every non-finite/out-of-range result into exactly 0.0 or exactly 1.0 (see
    // x87_labor_utilization's header), so the naive C++ comparisons below are safe -- the NaN-sensitive
    // JBE/JNC divergence that forced x87 in Phase 2 cannot recur here.
    if (1.0 <= wp.ai_labor_utilization) return 0;              // @0x004e405f
    if (pop.pop_total == 0 && pop.housing_prev == 0) return 1; // @0x004e4076
    if (pop.pop_total == 0) return 0;                          // @0x004e408a
    return x87_housing_short(pop.housing_prev, pop.pop_total, v.extra_space) ? 1 : 0;
    // The original's tail `JMP 0x004e792e` is a shared Watcom epilogue, not a call -- it means `return`.
}

} // namespace detail

int32_t rebalance_building_workers(uint32_t player) {
    const ai_state st = state();
    return detail::rebalance_building_workers(st.read, st.own, live_calls(), player);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// NOTHING IS STUBBED, per the batch context: the three predicates (bldg_is_alive, bldg_uses_workers,
// is_worker_priority_candidate) are pure reads, and the three order enqueues (activate / assign /
// unassign) all reach llm_strat_order_enqueue DIRECTLY -- the IMMEDIATE lane, never order_schedule --
// so their whole write-set is _G_LLM_STRAT_ORDER_QUEUE, _G_LLM_STRAT_ORDER_QUEUE_COUNT and, for the
// two worker-count ones, _G_LLM_STRAT_ORDER_SCRATCH_ARGS. Every one of those is declared by this
// site, so the restore between the two arms undoes the duplicate immediate-lane write.

} // namespace mh::ai
