//
// sim/sim_unit_state_corpse_fow_decay.cpp -- see sim_unit_state_corpse_fow_decay.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_unit_state_corpse_fow_decay_004822dc.asm) -- see the header's
// banner for the bit-trick loop-condition resolution and the DAT_ constant citations.
//
#include "sim/sim_unit_state_corpse_fow_decay.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_corpse_fow_decay_calls &live_unit_state_corpse_fow_decay_calls() {
    static const unit_state_corpse_fow_decay_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_free_slot),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
    };
    return c;
}

namespace detail {

void unit_state_corpse_fow_decay(const sim_view &v, sim_store &own,
                                 const unit_state_corpse_fow_decay_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced, read+write -- same resolved
                              // pointer as *v.cur_player/*v.cur_index (see sim_view::cur_unit's
                              // comment); the asm re-derives this address fresh at almost every
                              // field access (MOV EAX,[CUR_UNIT] before each byte/dword touch) --
                              // reused here as one local per this codebase's established
                              // simplification (see sim_unit_state_die_explode.cpp's identical note).

    // ---- 0x004822f4-0x00482300: top-level dispatch on move_microstep (int32_t @0xa2, a full DWORD
    // test -- see the header banner on why this is NOT the byte-width read the call-argument sites
    // use further down). --------------------------------------------------------------------------
    if (u.move_microstep == 0) {
        // ---- 0x004823fc-0x00482447: SINGLE-STEP arm ---------------------------------------------
        if (own.tick_budget() < CORPSE_FOW_DECAY_SINGLE_STEP_THRESHOLD) { // 0x004823fc-0x0048240b
            u.activity_clock -= own.tick_budget();                        // 0x00482422-0x00482430
        } else {
            c.unit_free_slot(static_cast<uint32_t>(*v.cur_player),
                             static_cast<int32_t>(*v.cur_index)); // 0x0048240d-0x0048241b
        }
        own.tick_budget() = 0.0; // 0x00482433-0x0048243d, both dwords
    } else {
        // ---- 0x00482306-0x004823f5: RING-DRAIN arm -- see the header banner for the bit-trick
        // loop-condition derivation. Faithful form: `tick_budget != 0.0 && move_microstep != 0`. ---
        while (own.tick_budget() != 0.0 && u.move_microstep != 0) {
            if (own.tick_budget() < CORPSE_FOW_DECAY_RING_BUDGET_THRESHOLD) { // 0x0048232e-0x0048233d
                u.activity_clock -= own.tick_budget();                        // 0x004823d0-0x004823de
                own.tick_budget() = 0.0;                                      // 0x004823e1-0x004823eb
            } else {
                // 0x00482343-0x0048236e: fow_remove_sight with the PRE-decrement microstep as the
                // sight radius (byte-truncated, matching the asm's own MOVZX byte read of the field).
                c.fow_remove_sight(static_cast<uint32_t>(*v.cur_player), u.x, u.y,
                                   static_cast<uint8_t>(u.move_microstep));

                u.move_microstep -= 1; // 0x00482373-0x0048237e

                if (u.move_microstep != 0) { // 0x00482383-0x0048238a
                    // 0x0048238c-0x004823b7: map_fow_UpdateFoWPlus with the POST-decrement microstep.
                    c.map_fow_UpdateFoWPlus(static_cast<uint32_t>(*v.cur_player), u.x, u.y,
                                            static_cast<uint8_t>(u.move_microstep));
                }

                // 0x004823bc-0x004823c8: budget += _DAT_0050144a (== budget - 5.0, since the DAT_ is
                // committed with its actual negative sign -- see the header's constant citation).
                own.tick_budget() += CORPSE_FOW_DECAY_RING_BUDGET_ADD_BACK;
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_corpse_fow_decay() {
    sim_state st = state();
    detail::unit_state_corpse_fow_decay(st.read, st.own, live_unit_state_corpse_fow_decay_calls());
}


} // namespace mh::sim
