//
// sim/sim_bldg_state_reset_idle.cpp -- see sim_bldg_state_reset_idle.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_state_default_reset_004711c3.asm,
// _idle_noop_00471203.asm, _idle_activate_00472415.asm); the Ghidra .c drafts for these three agree
// with the assembly (unlike sim_bldg_state_destroyed.cpp's draft), so both were read side by side.
//
#include "sim/sim_bldg_state_reset_idle.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_reset_idle_calls &live_bldg_state_reset_idle_calls() {
    static const bldg_state_reset_idle_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace detail {

void bldg_state_default_reset(const sim_view &v, sim_store &own, const bldg_state_reset_idle_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // 0x004711db-0x004711e0: state = IDLE_ACTIVATE (1).
    b.state = BLDG_STATE_IDLE_ACTIVATE;

    // 0x004711e6-0x004711f4: llm_strat_bldg_notify_ui(cur_player, cur_index) -- EAX=cur_player,
    // EDX=cur_index at the call site, matching the committed (uint16_t player, uint32_t b_Index)
    // prototype (addr/mh_calls.gen.h).
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

void bldg_state_idle_noop(sim_store &own) {
    // 0x0047121b-0x0047122f: the whole 8-byte _G_LLM_STRAT_TICK_BUDGET double zeroed via two dword
    // stores -- same idiom sim_unit_state_budget_noop.cpp documents/reproduces for its own pair, and
    // the SAME scratch double llm_strat_bldg_state_destroyed's own tail zeroes.
    own.tick_budget() = 0.0;
}

void bldg_state_idle_activate(const sim_view &v, sim_store &own, const bldg_state_reset_idle_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // 0x0047242d-0x00472432: online_state = 1.
    b.online_state = 1;

    // 0x00472438-0x0047244d: type = Building[cur_building->building_id].type (cfg_enum_E_BUILDING,
    // uint8_t, offset 0x8, IMUL 0x842 row stride off cfg_buildings' base 0xd9ec80 -- same
    // stride/base sim_bldg_state_destroyed.cpp already cross-checked).
    const uint8_t type = v.cfg_buildings[b.building_id].type;

    // 0x00472450-0x00472462: the exact CMP/JC/JBE/JZ four-way branch, transcribed literally rather
    // than collapsed to the equivalent-looking `type == A_TURRET || type == H_TURRET` the Ghidra .c
    // draft renders as a single boolean -- see the header's hazard note.
    uint16_t next_state;
    if (type < BUILDING_TYPE_A_TURRET) {
        // JC taken (0x00472454): type < A_TURRET.
        next_state = BLDG_STATE_IDLE_NOOP_88;
    } else if (type <= BUILDING_TYPE_A_TURRET) {
        // JC not taken, JBE taken (0x0047245a): type == A_TURRET (JC already proved type >= A_TURRET).
        next_state = BLDG_STATE_TURRET_SCAN;
    } else if (type == BUILDING_TYPE_H_TURRET) {
        // JBE not taken, JZ taken (0x00472460): type == H_TURRET.
        next_state = BLDG_STATE_TURRET_SCAN;
    } else {
        // JZ not taken (0x00472462): type > A_TURRET and type != H_TURRET.
        next_state = BLDG_STATE_IDLE_NOOP_88;
    }
    b.state = next_state;

    // 0x0047247e-0x0047248c: llm_strat_bldg_notify_ui(cur_player, cur_index) -- same call shape as
    // bldg_state_default_reset above.
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));

    // 0x00472491-0x004724a5: tick_budget = 0.0 -- same tail as bldg_state_idle_noop above.
    own.tick_budget() = 0.0;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void bldg_state_default_reset() {
    sim_state st = state();
    detail::bldg_state_default_reset(st.read, st.own, live_bldg_state_reset_idle_calls());
}

void bldg_state_idle_noop() {
    sim_state st = state();
    detail::bldg_state_idle_noop(st.own);
}

void bldg_state_idle_activate() {
    sim_state st = state();
    detail::bldg_state_idle_activate(st.read, st.own, live_bldg_state_reset_idle_calls());
}


} // namespace mh::sim
