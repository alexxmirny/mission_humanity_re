//
// sim/sim_bldg_state_mine_scan.cpp -- see sim_bldg_state_mine_scan.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_mine_scan_deposits_00474384.asm), cross-checked against the
// Ghidra .c draft (tmp/decomp_sim/llm_strat_bldg_state_mine_scan_deposits_00474384.c), which agrees
// with the assembly.
//
#include "sim/sim_bldg_state_mine_scan.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_mine_scan_calls &live_bldg_state_mine_scan_calls() {
    static const bldg_state_mine_scan_calls c = {
        MH_LIBMH_BIND(llm_strat_mine_scan_deposit_slot),
    };
    return c;
}

namespace {

// The fixed 4-iteration loop bound (0x004743a3: `CMP dword ptr [...],0x4`) -- a literal in the
// assembly, not read from any declared array-length symbol. See the header's own note on why this is
// NOT generalized to the callee's deposit_slot[4] array length.
inline constexpr int32_t MINE_DEPOSIT_SLOT_SCAN_COUNT = 4;

} // namespace

namespace detail {

void bldg_state_mine_scan_deposits(const sim_view &v, sim_store &own, const bldg_state_mine_scan_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, write-only here

    // ---- for (slot = 0; slot < 4; slot++) mine_scan_deposit_slot(slot, cur_player, cur_index) ------
    // (0x0047439c-0x004743cc). Register order EAX/EDX/EBX = slot/cur_player/cur_index, matching
    // llm_strat_mine_scan_deposit_slot's already-committed (slot_index, player, building_index)
    // prototype exactly -- see the header's ARGUMENT ORDER note.
    for (int32_t slot = 0; slot < MINE_DEPOSIT_SLOT_SCAN_COUNT; ++slot) {
        c.mine_scan_deposit_slot(static_cast<uint8_t>(slot), *v.cur_player, static_cast<int32_t>(*v.cur_index));
    }

    // ---- unconditional trailing state store (0x004743cc-0x004743d7) --------------------------------
    // Runs regardless of the loop (there is no branch around it) -- not gated on anything the loop
    // produced. No tick_budget touch, no notify_ui call: this function makes no other effect.
    b.state = BLDG_STATE_MINE_CHECK_DEPOSITS;
}

} // namespace detail

// ---- the public wrapper -----------------------------------------------------------------------------

void bldg_state_mine_scan_deposits() {
    sim_state st = state();
    detail::bldg_state_mine_scan_deposits(st.read, st.own, live_bldg_state_mine_scan_calls());
}


} // namespace mh::sim
