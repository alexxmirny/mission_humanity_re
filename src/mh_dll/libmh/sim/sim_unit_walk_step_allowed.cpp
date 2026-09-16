//
// sim/sim_unit_walk_step_allowed.cpp -- see sim_unit_walk_step_allowed.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_walk_step_allowed_0048c79a.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_unit_walk_step_allowed.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {
namespace detail {

int32_t unit_walk_step_allowed(const sim_view &v, uint32_t player, int32_t unit_idx) {
    const unit   &u           = unit_of(v, player, unit_idx);
    const uint8_t independent = v.cfg_units[u.unit_proto_id].independent;

    // PRESERVE-BUG, found by the rig (SIM1-G5, 2026-08-22): the original's return is a
    // FULL 32-BIT EAX, but the function only ever writes the LOW BYTE of it (`MOV AL, ...` /
    // `MOV byte ptr [EBP-0x18], ...`) -- it never zero/sign-extends. EAX's upper 24 bits are whatever
    // an EARLIER address computation left there, carried through untouched. Two different address
    // computations feed it depending on which switch arm runs, both reproduced here bit-for-bit
    // (confirmed against a live rig divergence: player=1, unit_idx=1 -> original=0x00003A01,
    // matching unit_proto_id=26 -> 26*0x23f=0x3A66, masked to 0x3A00, OR 1):
    //   independent==0 or ==1: 0x0048c7ca-0x0048c7d7 computes `unit_proto_id * 0x23f` (the cfg Unit[]
    //     row address) into EAX, then `MOV AL,[EAX+0xe4a182]` overwrites ONLY the low byte with the
    //     `independent` field -- EAX's upper 24 bits are the product's, untouched by either case body.
    //   independent==2: 0x0048c806-0x0048c817 recomputes EAX as `player*0x5b04 + unit_idx*0xe9` (the
    //     units[][] row offset) for the state check -- a DIFFERENT value than the independent==0/1
    //     case, and this branch's own body never narrows it either.
    // independent>2 has NO such recomputation at all (falls straight through with neither EAX nor the
    // low byte ever written) -- see the DECLARED DIVERGENCE note below, unaffected by this fix.
    const int32_t proto_row_scratch = static_cast<int32_t>(u.unit_proto_id) * 0x23f;
    if (independent == 0) return (proto_row_scratch & ~0xff) | 1;
    if (independent == 1) return (proto_row_scratch & ~0xff) | 0;
    if (independent == 2) {
        const int32_t unit_row_scratch =
            static_cast<int32_t>(player) * 0x5b04 + unit_idx * 0xe9;
        return (unit_row_scratch & ~0xff) | ((u.state != 0x1c) ? 1 : 0);
    }

    // DECLARED DIVERGENCE (see header banner): the original reads an uninitialised stack local here,
    // not provably a fixed watermark and not reproducible. Deterministic, clearly-flagged 0 instead.
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_walk_step_allowed(uint32_t player, int32_t unit_idx) {
    sim_state st = state();
    return detail::unit_walk_step_allowed(st.read, player, unit_idx);
}


} // namespace mh::sim
