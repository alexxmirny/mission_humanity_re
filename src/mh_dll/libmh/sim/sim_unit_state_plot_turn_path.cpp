//
// sim/sim_unit_state_plot_turn_path.cpp -- see sim_unit_state_plot_turn_path.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_state_plot_turn_path_00485d43.asm) -- no Ghidra .c draft
// was available for this function in this batch, so every field/offset/branch below is read directly
// off the raw instruction stream, per the translator brief's rule 1.
//
#include "sim/sim_unit_state_plot_turn_path.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_plot_turn_path_calls &live_unit_state_plot_turn_path_calls() {
    static const unit_state_plot_turn_path_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_path_attach_slot),
    };
    return c;
}

namespace detail {

void unit_state_plot_turn_path(const sim_view &v, sim_store &own, const unit_state_plot_turn_path_calls &c) {
    const uint16_t player = *v.cur_player; // _G_LLM_STRAT_CUR_PLAYER (0x00485d75 et al., read repeatedly)

    // 0x00485d5b-0x00485eac: scan _G_LLM_STRAT_PATH_SLOT_FLAGS[player][0..99] for the first slot whose
    // flag is 0 (free). If the loop runs out at i==100 with none free, the original falls straight to
    // the epilogue with no further side effects -- reproduced as an early return.
    int32_t slot = -1;
    for (int32_t i = 0; i < 100; ++i) {
        if (own.path_slot_flag_at(player, i) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    // 0x00485d8f-0x00485d9b: seed `heading` from the current unit's move_heading (unit+0xac). MOVZX in
    // the asm; move_heading is already uint8_t, so the int32_t widening below is zero-extending.
    int32_t heading = v.cur_unit->move_heading;

    // 0x00485d9e-0x00485e25: four chained lookups filling path-buffer entries 0..3.
    for (int32_t entry = 0; entry < 4; ++entry) {
        // 0x00485db8-0x00485dc7: heading = DIR_REMAP_TABLE[heading].step_primary -- the UPDATE happens
        // BEFORE the write below, so entry[entry].heading is always the POST-lookup value, never the
        // pre-lookup seed. See the header's OOB note: step_primary's own documented range is 0..31 (a
        // STEP index into the 40-entry DIR_STEP_OFFSET table), fed back in here as a HEADING index into
        // the 24-entry DIR_REMAP_TABLE -- the ORIGINAL's own domain mismatch, not a translation
        // artifact; the clamp two lines below is what the original uses to bound it.
        heading = v.dir_remap_table[heading].step_primary;

        // 0x00485dc7-0x00485de7: path_buffer_at(player, slot, entry).heading = (byte)heading (only the
        // low byte is stored, matching the asm's `MOV AL, byte ptr [heading]` / byte store).
        own.path_buffer_at(player, slot, entry).heading = (uint8_t)heading;

        // 0x00485ded-0x00485e25: if (heading > 23) heading = 24. The original expresses this as a
        // compiled busy-loop (reset heading to 0, then increment while <24, with an inert 3-iteration
        // inner spin) that has no side effect other than leaving heading at EXACTLY 24 regardless of by
        // how much it exceeded 23 -- reproduced here as the equivalent clamp. See the header banner and
        // this translation's uncertainties for the full derivation.
        if (heading > 23) heading = 24;
    }

    // 0x00485e27-0x00485e58: pick step_alt1 (roll==0) or step_alt2 (roll==1) from the FINAL heading's
    // row (which may be 24, one past DIR_REMAP_TABLE's declared 24 rows -- see the header note; the
    // access below is a plain pointer index, not bounds-checked, to reproduce the original's own
    // one-past-the-end read byte-for-byte) and write its low byte into entry 4. rand_below(2) ADVANCES
    // _G_LLM_STRAT_RNG_STATE -- see the header's shadow-hazard banner.
    const int32_t        roll                   = c.rand_below(2);
    const dir_remap_row &row                    = v.dir_remap_table[heading];
    const int32_t        alt                    = (roll == 0) ? row.step_alt1 : row.step_alt2;
    own.path_buffer_at(player, slot, 4).heading = (uint8_t)alt;

    // 0x00485e5e-0x00485e7b: entry 5 is the terminator sentinel.
    own.path_buffer_at(player, slot, 5).heading = 0xff;

    // 0x00485e7b-0x00485e85: unit state -> 0x2c via the ORIGINAL setter. No Ghidra enum backs this
    // unit-state domain (see sim_unit_state_predicates.cpp's identical finding on the same state/order
    // field) -- 0x2c is transcribed as the literal the asm loads, not renamed.
    c.unit_set_state(0x2c);

    // 0x00485e85-0x00485e94: cur_unit->path_cursor = 0 (dword store, matches the field's int32_t width).
    own.cur_unit().path_cursor = 0;

    // 0x00485e94-0x00485eaa: attach the found slot to the current unit. Register order at the call site
    // is EAX=player, EDX=cur_index, EBX=slot, matching llm_strat_path_attach_slot's committed
    // (param_1=EAX, param_2=EDX, param_3=EBX) prototype.
    c.path_attach_slot((int32_t)player, (int32_t)*v.cur_index, slot);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_plot_turn_path() {
    sim_state st = state();
    detail::unit_state_plot_turn_path(st.read, st.own, live_unit_state_plot_turn_path_calls());
}


} // namespace mh::sim
