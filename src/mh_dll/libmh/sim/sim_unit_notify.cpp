//
// sim/sim_unit_notify.cpp -- see sim_unit_notify.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_notify_ui_00488a22.asm, tmp/decomp/llm_strat_unit_notify_status_
// 004dae0e.asm). notify_status was checked branch-by-branch against the raw CMP/JC/JBE/JZ cascade
// and matches; it is translated as-is.
//
// notify_ui's local-player arm: CORRECTED 2026-08-12 (reimpl-verify caught it). An earlier reading
// dismissed the Ghidra .c draft's `general.change_flag2 = 0` as a decompiler fabrication because the
// body has only ONE store instruction -- true, but that one instruction (0x00488a4b, `MOV dword ptr
// [...],1`) is 4 BYTES wide, and change_flag/change_flag2 are contiguous int16_t fields with no
// padding, so it sets both atomically. The .c draft's two-statement rendering was right.
//
#include "sim/sim_unit_notify.h"

#include "addr/mh_calls.gen.h"   // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"         // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_event_codes.h" // EVENT_INFO_REFRESH
#include "addr/mh_rebind.gen.h"  // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const notify_ui_calls &live_notify_ui_calls() {
    static const notify_ui_calls c = {
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_notify_ui @0x00488a22 ------------------------------------------------------
//
// Two mutually exclusive arms (the first arm JMPs straight past the second in the .asm):
//   (side == PlayerSide)  -> general.change_flag = 1 AND general.change_flag2 = 0, written together
//                             by the original's single dword store (0x00488a4b) -- see the header
//                             HAZARD note.
//   else                  -> if the pending click-select target (flags == (side|0x80) as a 16-bit
//                             value, AND id == unit_index) names this exact unit, fire INFO_REFRESH.
void notify_ui(const sim_view &v, sim_store &own, const notify_ui_calls &c, uint32_t side,
               uint32_t unit_index) {
    // 16-bit bit-pattern compare (CMP AX, word ptr PlayerSide) -- side's low 16 bits vs PlayerSide.
    if (static_cast<int16_t>(side) == *v.player_side) {
        // 0x00488a4b -- one MOV dword ptr, setting both fields at once (see header HAZARD note).
        own.change_flag()  = 1;
        own.change_flag2() = 0;
        return;
    }

    // (side & 0xffff) | 0x80, compared against the 16-bit flags word zero-extended to 32 bits --
    // matches the .asm's `OR AL,0x80` (low byte only) followed by `MOVZX EAX,AX` (which folds AH,
    // i.e. bits 8-15 of side, back in unchanged), i.e. exactly (side & 0xffff) | 0x80.
    if (static_cast<uint16_t>((side & 0xffffu) | 0x80u) == *v.click_select_target_flags &&
        own.click_select_target_id() == unit_index) {
        c.set_event(EVENT_INFO_REFRESH);
    }
}

// ---- llm_strat_unit_notify_status @0x004dae0e --------------------------------------------------
//
// No outward calls. Gated on player_data[player & 0xf].ai_enabled != 0 BEFORE the roster row is even
// computed (0x004dae1c-0x004dae3b) -- the whole function is a no-op for a non-AI-enabled player slot.
void notify_status(const sim_view &v, sim_store &own, uint32_t player, int32_t unit_idx,
                   uint32_t status_code) {
    const uint32_t p = player & 0xfu;

    if (player_of(v, p).ai_enabled == 0)
        return;

    // Roster row computed ONCE (0x004dae41-0x004dae4d) and reused for every branch below, matching
    // the .asm's own single EAX computation shared across the whole cascade.
    unit &u = own.unit_at(p, unit_idx);

    if (status_code < 100) {
        if (status_code == 0) {
            u.order_status_flags |= 0x40u;
        } else if (status_code == 1) {
            u.order_notify_status = 2;
        }
        // status_code in [2,99]: no-op.
        return;
    }
    if (status_code < 0x65) { // == 100
        u.order_notify_status = 3;
        return;
    }
    if (status_code < 0x66) { // == 101
        u.order_status_flags &= 0xbfu;
        u.order_notify_status = 6;
        return;
    }
    if (status_code < 0x67) { // == 102
        u.order_status_flags &= 0xbfu;
        u.order_notify_status = 5;
        return;
    }
    if (status_code != 0x67) // > 103: no-op
        return;
    // == 103
    u.order_status_flags &= 0xbfu;
    u.order_notify_status = 4;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void notify_ui(uint32_t side, uint32_t unit_index) {
    sim_state st = state();
    detail::notify_ui(st.read, st.own, live_notify_ui_calls(), side, unit_index);
}

void notify_status(uint32_t player, int32_t unit_index, uint32_t status_code) {
    sim_state st = state();
    detail::notify_status(st.read, st.own, player, unit_index, status_code);
}


} // namespace mh::sim
