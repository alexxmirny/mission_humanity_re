//
// sim/sim_bldg_init_all.cpp -- see sim_bldg_init_all.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_init_all_0045a017.asm).
//
#include "sim/sim_bldg_init_all.h"

#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE / the entry-thunk shapes
#include "addr/mh_calls.gen.h"  // the committed callable declarations MH_PROMOTED type-checks against
#include "addr/mh_rebind.gen.h"
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED_ROW
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// cfg_enum_E_BUILDING::UNDEFINED. Spelled out because the generated header renders an enum field as
// its underlying scalar; the enum's own member list (Ghidra /Manual/cfg/enum/cfg_enum_E_BUILDING)
// has UNDEFINED = 0 and no other member below 1.
inline constexpr uint8_t BUILDING_TYPE_UNDEFINED = 0u;

// 0x0045a02f / 0x0045a036: `i = 1` and `CMP i,0x64; JL`. The loop's own literals -- see the header
// for why these are NOT roster caps.
inline constexpr int32_t BLDG_TYPE_FIRST = 1;
inline constexpr int32_t BLDG_TYPE_LIMIT = 100; // 0x64, exclusive

} // namespace

// llm_strat_bldg_init_defaults is PROMOTED (MH_EXPORT_REPLACE in
// sim/hostreach/sim_batch_h_promote.cpp), so this site is the `promoted` class: hosted it expands to
// `::mh::call::llm_strat_bldg_init_defaults`, i.e. the ORIGINAL ENTRY -- exactly the token the
// original's own CALL lands on, E9'd into our body when the batch-H promotion is installed and
// running the stock body when it is not -- while the standalone build takes our own body directly and
// the site adds no mh::call:: VA dependency (state/promoted_select.h's whole argument).
//
// _ROW, NOT THE TWO-ARGUMENT FORM, and this file is where that distinction was first exercised for
// this callee. Adding the site MADE it a rebind row: gen_libmh_rebind derives its pool from the
// callees libmh actually calls, so before this file there was no MH_LIBMH_BIND_llm_strat_bldg_init_
// defaults and the two-argument `MH_PROMOTED(fn, ::mh::sim::bldg_init_defaults)` looked correct. The
// regeneration then bound the row to `::mh::sim::promoted_arm::bh_llm_strat_bldg_init_defaults` (R2
// takes the SEAM's impl), which is a DIFFERENT body from the public wrapper -- the same body plus the
// promotion seam's first-call liveness line. Naming the wrapper here would have left two standalone
// routes to one callee landing in two different bodies, which promoted_select.h's header calls a
// worse failure than the VA it removes. Taking the binder's own target by macro makes that
// impossible to get wrong.
const bldg_init_all_calls &live_bldg_init_all_calls() {
    static const bldg_init_all_calls c = {
        MH_PROMOTED_ROW(llm_strat_bldg_init_defaults),
    };
    return c;
}

namespace detail {

void bldg_init_all(const sim_view &v, const bldg_init_all_calls &c) {
    // 0x0045a02f-0x0045a03a: i = 1; loop while i < 100. Watcom compiles this jump-to-condition-check,
    // so an empty range would never enter the body -- immaterial here, the bound is a constant.
    for (int32_t i = BLDG_TYPE_FIRST; i < BLDG_TYPE_LIMIT; ++i) {
        // 0x0045a043-0x0045a051: IMUL EAX,i,0x842 / CMP byte ptr [EAX+0xd9ec88],0x0 / JZ.
        // `Building` base 0x00d9ec80 + 8 == the `type` field (header banner: the G92 check).
        if (v.cfg_buildings[i].type == BUILDING_TYPE_UNDEFINED) continue;

        // 0x0045a053-0x0045a057: MOVZX EAX,word ptr [EBP-0x18] -- the LOW 16 BITS of the loop index,
        // zero-extended into the argument register. Reproduced literally rather than passing `i`:
        // the mask is a no-op for every value this loop can produce (1..99), but it is what the
        // instruction does and the callee's committed prototype takes a uint32_t, so dropping it
        // would be a silent widening of the contract rather than a simplification.
        c.bldg_init_defaults((uint32_t)((uint16_t)i));
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_init_all() {
    const sim_view v = state().read;
    detail::bldg_init_all(v, live_bldg_init_all_calls());
}


} // namespace mh::sim
