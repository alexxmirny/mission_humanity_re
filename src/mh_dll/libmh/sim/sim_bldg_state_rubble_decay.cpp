//
// sim/sim_bldg_state_rubble_decay.cpp -- see sim_bldg_state_rubble_decay.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_state_rubble_sight_decay_004738a3.asm), cross-checked
// against the Ghidra .c draft (tmp/decomp_sim/*.c) -- the draft agrees with the assembly on every
// branch and both loop-exit shapes.
//
#include "sim/sim_bldg_state_rubble_decay.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_rubble_decay_calls &live_bldg_state_rubble_decay_calls() {
    static const bldg_state_rubble_decay_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_free_record),
        MH_LIBMH_BIND(llm_strat_sight_remove_circle),
        MH_LIBMH_BIND(llm_strat_sight_add_circle),
    };
    return c;
}

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// `anim[48]` is Ghidra's flattening of the ORIGINAL cfg_t_frame_index[12] (int32_t[12]); slot 0 is
// repurposed here as the pending sight-circle countdown, a raw int32_t -- see the header's
// ANIM[0]-AS-COUNTDOWN note. SAME shape as sim_map_create_building.cpp's own copy (copied verbatim
// per this project's per-TU convention -- see that file for the precedent).
inline void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value);
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

} // namespace

namespace detail {

// ---- FP-COMPARISON NOTE (all three branches below) ------------------------------------------------
// All three floating comparisons in this function are x87 FCOMP/FNSTSW/SAHF/JC(C)/JNC pairs, which
// treat an UNORDERED result (either operand NaN) as CF=1 -- i.e. as if the "less-than" side of the
// comparison held. A naive C++ `<`/`<=` evaluates false for NaN under IEEE 754 and would silently pick
// the OTHER branch on a NaN input. Each comparison below is restated as a negated `>=`/`<=` so NaN
// forces the same arm the hardware would take, while remaining identical to the naive form for every
// ordered (non-NaN) input -- same technique sim_bldg_state_charge.cpp's bldg_state_charge_step()
// applies to its own step<=0.0/tick_budget<step pair. Not confirmed reachable with real cfg/runtime
// data, but Law 2 preserves the original's behaviour regardless of whether it is.
void bldg_state_rubble_sight_decay(const sim_view &v, sim_store &own, const bldg_state_rubble_decay_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    if (load_u32_le(&b.anim[0]) == 0) {
        // ---- ARM A: the post-rubble cleanup wait (0x004739cb-0x00473a16) ---------------------------
        // NaN-safe form (see the FP-COMPARISON note below): the original's FCOMP/FNSTSW/SAHF/JC pair
        // treats an UNORDERED comparison (either operand NaN) as "jump taken", i.e. as if
        // tick_budget < rubble_cleanup_period were true. A naive `<` evaluates false for NaN under
        // IEEE 754, which would silently swap this branch for the free_record one. Restated as negated
        // `>=` so NaN forces this branch exactly as the hardware does, identical to the naive form for
        // every ordered (non-NaN) input.
        if (!(own.tick_budget() >= *v.rubble_cleanup_period)) {
            b.last_tick_time -= own.tick_budget();
        } else {
            c.bldg_free_record(*v.cur_player, *v.cur_index);
        }
        // Both branches converge on the SAME unconditional reset (0x00473a02/0x00473a0c) before the
        // shared epilogue -- unlike ARM B below, which does NOT always reset tick_budget.
        own.tick_budget() = 0.0;
    } else {
        // ---- ARM B: the sight-circle decay loop (0x004738cd-0x004739c9) -----------------------------
        // Condition re-checked live at the top of every iteration (both operands re-read fresh, per
        // translator-brief rule 16 -- anim[0] in particular via the CUR_BUILDING pointer each pass).
        // On exit-by-condition (either operand going false) this falls STRAIGHT to the epilogue
        // WITHOUT resetting tick_budget -- see the header note; do not add a trailing reset here.
        // NaN-safe form of the original's FLDZ/FCOMP/JNC pair -- see the FP-COMPARISON note below:
        // `!(tick_budget <= 0.0)` matches the hardware's "continue on unordered" for the same reason
        // the two comparisons inside the loop body do.
        while (!(own.tick_budget() <= 0.0) && load_u32_le(&b.anim[0]) != 0) {
            // NaN-safe form (see the FP-COMPARISON note below).
            if (!(own.tick_budget() >= *v.rubble_sight_decay_period)) {
                // Not enough budget for a full decay step (0x0047399f-0x004739b0): carry the
                // shortfall into last_tick_time, drain the budget, loop back to re-check.
                b.last_tick_time -= own.tick_budget();
                own.tick_budget() = 0.0;
            } else {
                // A full decay step (0x00473902-0x0047399d): consume one period's worth of budget,
                // peel off one sight-circle radius step.
                own.tick_budget() += *v.rubble_sight_decay_period_neg; // i.e. -= the period

                const uint32_t old_radius = load_u32_le(&b.anim[0]); // PRE-decrement radius
                c.sight_remove_circle(*v.cur_player, b.x, b.y, b.building_id,
                                      static_cast<uint8_t>(old_radius));

                const uint32_t new_radius = old_radius - 1;
                store_u32_le(&b.anim[0], new_radius);
                if (new_radius != 0) {
                    // Skipped entirely when the decrement lands on 0 (0x00473963 JZ) -- POST-decrement
                    // radius, confirmed by the asm re-reading [cur_building+0x93] AFTER the DEC.
                    c.sight_add_circle(*v.cur_player, b.x, b.y, b.building_id,
                                       static_cast<uint8_t>(new_radius));
                }
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------

void bldg_state_rubble_sight_decay() {
    sim_state st = state();
    detail::bldg_state_rubble_sight_decay(st.read, st.own, live_bldg_state_rubble_decay_calls());
}


} // namespace mh::sim
