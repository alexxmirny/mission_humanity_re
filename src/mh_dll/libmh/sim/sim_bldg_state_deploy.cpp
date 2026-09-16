//
// sim/sim_bldg_state_deploy.cpp -- see sim_bldg_state_deploy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_deploy_anim_wait_00471239.asm, _land_activate_00471288.asm,
// _deploy_start_00471377.asm) -- see the header banner for the param_3/param_4 forwarding derivation,
// the GAME_CLOCK raw-dword-split derivation, and the notify_ui correction.
//
#include "sim/sim_bldg_state_deploy.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_land_activate_calls &live_bldg_state_land_activate_calls() {
    static const bldg_state_land_activate_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_register_online),
        MH_LIBMH_BIND(llm_strat_bldg_flush_cargo_hold),
        MH_LIBMH_BIND(llm_strat_prod_unload_cargo_manifest),
    };
    return c;
}

const bldg_state_deploy_start_calls &live_bldg_state_deploy_start_calls() {
    static const bldg_state_deploy_start_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_start_liftoff_anim_mother),
        MH_LIBMH_BIND(llm_strat_bldg_start_liftoff_anim_shuttle),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// online_state sub-phase thresholds these two handlers each test -- no backing Ghidra enum, and
// online_state's own field comment (mh_structs.gen.h) documents it as an OVERLOADED per-building-
// family counter with no single universal meaning across handlers. Named for THIS file's own two
// uses, not a general vocabulary; the exact semantic role of either value is not otherwise confirmed.
constexpr int16_t DEPLOY_ANIM_WAIT_ONLINE_STATE_ARRIVED     = 0xc; // deploy_anim_wait's gate
constexpr int16_t LAND_ACTIVATE_ONLINE_STATE_ALREADY_LANDED = 0xa; // land_activate's early-return gate

// Splits GAME_CLOCK's raw 8-byte bit pattern into the two dwords llm_strat_bldg_start_liftoff_anim_
// mother/_shuttle's ALREADY-COMMITTED prototype expects as independent uint32_t param_5/param_6 (NOT a
// reconstructed double -- see the header's derivation). memcpy, not a reinterpret_cast, matching this
// project's other bit-pattern helpers (sim_bldg_state_destroyed.cpp's frame_at()/set_frame_at()).
void split_game_clock(double clock, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &clock, sizeof(bits));
    lo = static_cast<uint32_t>(bits);
    hi = static_cast<uint32_t>(bits >> 32);
}

} // namespace

namespace detail {

void bldg_state_deploy_anim_wait(sim_store &own) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // 0x00471251-0x0047127e: the ONLY test in this function.
    if (b.online_state == DEPLOY_ANIM_WAIT_ONLINE_STATE_ARRIVED) {
        b.state = BLDG_STATE_TO_UNIT;
    } else {
        own.tick_budget() = 0.0; // _G_LLM_STRAT_TICK_BUDGET, shared with the unit-tick driver
    }
}

void bldg_state_land_activate(const sim_view &v, sim_store  &own, uint32_t /*param_1*/,
                              uint32_t /*param_2*/, uint32_t param_3, uint32_t param_4,
                              const bldg_state_land_activate_calls &c) {
    // param_1/param_2 (EAX/EDX) are dead register carriers -- see the header's derivation. Not read
    // here.
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // 0x004712a0-0x004712aa: early return, skipping everything below.
    if (b.online_state == LAND_ACTIVATE_ONLINE_STATE_ALREADY_LANDED) {
        own.tick_budget() = 0.0;
        return;
    }

    // 0x004712b0-0x004712ca: llm_strat_bldg_register_online(cur_player, cur_index, param_3, param_4,
    // GAME_CLOCK) -- MIXED convention: EAX/EDX are FRESHLY loaded with cur_player/cur_index (NOT this
    // function's own param_1/param_2), EBX/ECX are this function's OWN param_3/param_4 forwarded
    // verbatim (never touched between entry and this call), and GAME_CLOCK reaches the callee as a
    // genuine double via its own already-committed marshalling (see the header derivation).
    c.bldg_register_online(static_cast<int16_t>(*v.cur_player), static_cast<int32_t>(*v.cur_index),
                           param_3, param_4, *v.game_clock);

    // 0x004712cf-0x004712eb: cur_building->state = Building[building_id].state_transition_ids[1]'s LOW
    // WORD (RESOLVED, conductor 2026-08-22 -- named in Ghidra, addr/mh_structs.gen.h). building_id is
    // read ONCE here (no earlier read in this function) and reused for the type dispatch immediately
    // below.
    const uint16_t      bid = b.building_id;
    const cfg_building &cb  = v.cfg_buildings[bid];
    b.state                 = static_cast<uint16_t>(cb.state_transition_ids[1] & 0xffff);

    // 0x004712ef-0x00471357: mother types flush the cargo hold; shuttle types unload the cargo
    // manifest; every other type does neither -- see the header's four-way branch derivation.
    switch (cb.type) {
        case BUILDING_TYPE_A_MOTHER:
        case BUILDING_TYPE_H_MOTHER:
            c.bldg_flush_cargo_hold(static_cast<uint32_t>(*v.cur_player), static_cast<int32_t>(*v.cur_index));
            break;
        case BUILDING_TYPE_A_SHUTTLE:
        case BUILDING_TYPE_H_SHUTTLE:
            // Return value discarded -- the asm never reads EAX after this call (falls straight to the
            // shared epilogue).
            (void)c.prod_unload_cargo_manifest(static_cast<uint32_t>(*v.cur_player),
                                               static_cast<int32_t>(*v.cur_index));
            break;
        default:
            break;
    }
    // No notify_ui call anywhere in this function -- see the header's correction note.
}

void bldg_state_deploy_start(const sim_view &v, sim_store  &own, uint32_t /*param_1*/,
                             uint32_t /*param_2*/, uint32_t param_3, uint32_t param_4,
                             const bldg_state_deploy_start_calls &c) {
    // param_1/param_2 (EAX/EDX) are dead register carriers -- see the header's derivation. Not read
    // here.
    building &b = own.cur_building();

    // 0x0047138f-0x004713a4: type = Building[cur_building->building_id].type, read ONCE.
    const uint16_t bid  = b.building_id;
    const uint8_t  type = v.cfg_buildings[bid].type;

    // GAME_CLOCK's raw dwords, needed by whichever liftoff-anim branch below fires (both take the
    // SAME two-dword shape -- see the header derivation). Computed once; a pure read with no side
    // effects, so evaluating it even on the branch that ends up not using it is behaviourally
    // unobservable.
    uint32_t clock_lo = 0, clock_hi = 0;
    split_game_clock(*v.game_clock, clock_lo, clock_hi);

    // 0x004713a7-0x0047141b: the four-way CMP/JC/JBE/JZ dispatch (see the header derivation) --
    // A_MOTHER/H_MOTHER and A_SHUTTLE both start a liftoff anim and enter DEPLOY_ANIM_WAIT; everything
    // else goes straight to TO_UNIT with no call.
    switch (type) {
        case BUILDING_TYPE_A_MOTHER:
        case BUILDING_TYPE_H_MOTHER:
            // 0x004713ef-0x004713ed: param_3/param_4 forwarded verbatim (see the header derivation).
            c.bldg_start_liftoff_anim_mother(static_cast<uint32_t>(*v.cur_player),
                                             static_cast<int32_t>(*v.cur_index), param_3, param_4, clock_lo,
                                             clock_hi);
            b.state = BLDG_STATE_DEPLOY_ANIM_WAIT;
            break;
        case BUILDING_TYPE_A_SHUTTLE:
            // 0x004713c3-0x004713ed: same shape, the shuttle liftoff-anim callee.
            c.bldg_start_liftoff_anim_shuttle(static_cast<uint32_t>(*v.cur_player),
                                              static_cast<int32_t>(*v.cur_index), param_3, param_4, clock_lo,
                                              clock_hi);
            b.state = BLDG_STATE_DEPLOY_ANIM_WAIT;
            break;
        default:
            // 0x0047141b-0x00471426: no liftoff call at all.
            b.state = BLDG_STATE_TO_UNIT;
            break;
    }

    // 0x00471426-0x00471434: llm_strat_bldg_notify_ui(cur_player, cur_index), UNCONDITIONAL on every
    // path -- see the header's correction note (the batch hazard note claimed neither function calls
    // this directly; this one plainly does).
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void bldg_state_deploy_anim_wait() {
    sim_state st = state();
    detail::bldg_state_deploy_anim_wait(st.own);
}

void bldg_state_land_activate(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    sim_state st = state();
    detail::bldg_state_land_activate(st.read, st.own, param_1, param_2, param_3, param_4,
                                     live_bldg_state_land_activate_calls());
}

void bldg_state_deploy_start(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    sim_state st = state();
    detail::bldg_state_deploy_start(st.read, st.own, param_1, param_2, param_3, param_4,
                                    live_bldg_state_deploy_start_calls());
}


} // namespace mh::sim
