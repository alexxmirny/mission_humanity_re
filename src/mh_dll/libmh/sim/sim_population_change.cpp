//
// sim/sim_population_change.cpp -- see sim_population_change.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_check_population_change_0043fe1e.asm) -- the .c draft's field-vs-comparison
// reading was independently re-derived from the raw instruction sequence (base addresses, not field
// names) and cross-checked against addr/mh_structs.gen.h's mh_llm_strat_pop_stats layout, since the
// .c's own field labels are not a source of truth here. It happens to agree with the draft (see the
// header comment banner below); it is not a rubber stamp.
//
#include "sim/sim_population_change.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const population_change_calls &live_population_change_calls() {
    static const population_change_calls c = {
        MH_LIBMH_BIND(llm_strat_population_add),
        MH_LIBMH_BIND(llm_strat_population_remove),
    };
    return c;
}

namespace detail {

// ---- field-derivation banner ------------------------------------------------------------------
// The asm indexes TWO base addresses with the same `player*0x34` row offset: 0x00c3a370 and
// 0x00c3a380 (0x0043fe39-0x0043fe47 etc.). _G_LLM_STRAT_POP_STATS's own base is 0x00c3a370
// (addr/mh_shadow.gen.h's region declarations agree), stride 0x34 -- so 0x00c3a370 is
// pop_stats[player].pop_total (struct offset 0) and 0x00c3a380 is pop_stats[player].housing_prev
// (struct offset 0x10), per mh_structs.gen.h's field order (pop_total, workers_employed,
// human_in_field, human, housing_prev, ...). Every comparison in the body loads the 0x00c3a380
// (housing_prev) value into the LEFT-hand register and compares it against the 0x00c3a370
// (pop_total) value -- i.e. every CMP/Jcc below reads as "housing_prev <op> pop_total", not the
// other way around.
// ------------------------------------------------------------------------------------------------

void check_population_change(const sim_view &v, sim_store &own, const population_change_calls &c,
                             uint32_t player) {
    (void)own; // this function only reads sim state; all mutation happens inside the two callees.

    const pop_stats &ps = v.population[player];

    // 0x0043fe36-0x0043fe4d: if housing_prev == pop_total, nothing to do at all (skip straight to
    // RET). Re-read further down is the SAME unchanged values -- no call happens between any of the
    // three loads in this function, so folding them into one `ps` read is behaviorally identical to
    // the original's three separate re-derivations of `player*0x34`.
    if (ps.housing_prev != ps.pop_total) {
        // 0x0043fe4f-0x0043fe70: housing_prev > pop_total (JLE-not-taken, i.e. NOT housing_prev <=
        // pop_total) -- population is below the housing cap, grow it. Player is passed truncated to
        // uint16_t (0x0043fe67 `MOVZX EAX, word ptr [EBP-0x18]`), matching population_add's own
        // committed uint16_t parameter.
        if (ps.pop_total < ps.housing_prev) {
            c.population_add((uint16_t)player, 0);
        }
        // 0x0043fe72-0x0043fe93 (LAB_0043fe72, reached only when housing_prev <= pop_total):
        // housing_prev < pop_total (JGE-not-taken) -- population exceeds the housing cap, shrink it.
        // Same 16-bit-word load at the call site (0x0043fe8a, identical instruction to the add
        // branch's 0x0043fe67) even though population_remove's own committed parameter is a full
        // uint32_t -- the VALUE passed is still (uint32_t)(uint16_t)player, reproduced explicitly
        // here rather than passing `player` straight through.
        else if (ps.housing_prev < ps.pop_total) {
            c.population_remove((uint32_t)(uint16_t)player, 0);
        }
        // housing_prev == pop_total is excluded by the outer guard, so no third arm is reachable --
        // matches the asm exactly (both inner branches are guarded, no `else` fallback exists).
    }
}

} // namespace detail

void check_population_change(uint32_t player) {
    sim_state st = state();
    detail::check_population_change(st.read, st.own, live_population_change_calls(), player);
}


} // namespace mh::sim
