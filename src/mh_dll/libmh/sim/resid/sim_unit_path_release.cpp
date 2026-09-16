//
// sim/resid/sim_unit_path_release.cpp -- see sim_unit_path_release.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_unit_path_encode_directions_0044955f.asm and
// tmp/decomp_sim_resid/llm_strat_unit_release_path_and_targets_004888f4.asm), the Ghidra .c drafts
// beside each being drafts only.
//
#include "sim/resid/sim_unit_path_release.h"

#include "addr/mh_calls.gen.h"  // typed callables for the two frontier originals unit_release_path_and_targets calls OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_release_path_and_targets_calls &live_unit_release_path_and_targets_calls() {
    static const unit_release_path_and_targets_calls c = {
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_path_encode_directions @0x0044955f -----------------------------------------
void unit_path_encode_directions(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_idx) {
    // 0x0044957c-0x00449596: path_slot_id = units[player][unit_idx].path_slot_id (byte, 0-99, or
    // 0xff = none; zero-extended -- see the header's PRESERVE-BUG note: this function does NOT check
    // for 0xff, so an unassigned path yields an out-of-range slot_id, reproduced literally).
    const int32_t slot_id =
        v.units[static_cast<uint32_t>(player) * v.caps.units + unit_idx].path_slot_id;

    // 0x00449599-0x004495f0: walk the path buffer from cursor 0. At LAB_004495a0/0x004495ba the
    // loop tests `.heading != 0`; false (sentinel reached) exits the loop (JMP 0x004495c3), true
    // falls through to LAB_004495cd's `.run_length <<= 5` (SHL @0x004495e7), then LAB_004495c5
    // increments the cursor (0x004495c8) and repeats. See the header banner for why this is a
    // high-bits-first pack of run_length's low 3 bits (no OR with heading anywhere in this body --
    // the whole job here is the shift) and why an odd-length path needs no special case (one
    // waypoint per iteration, purely sentinel-driven).
    for (int32_t cursor = 0;; ++cursor) {
        path_waypoint &wp = own.path_buffer_at(player, slot_id, cursor);
        if (wp.heading == 0) break;                               // 0x004495ba CMP / 0x004495c1 JNZ / 0x004495c3 JMP-out
        wp.run_length = static_cast<uint8_t>(wp.run_length << 5); // 0x004495e7
    }
}

// ---- llm_strat_unit_release_path_and_targets @0x004888f4 ---------------------------------------
void unit_release_path_and_targets(sim_store &own, const unit_release_path_and_targets_calls &c,
                                   uint16_t player, int32_t unit_idx, uint32_t *out_cleared_pair) {
    unit &u = own.unit_at(player, unit_idx);

    // 0x00488926-0x00488936: GUARDED path-slot release. path_slot_id==0xff means "no path assigned"
    // -- the free call only fires when a real slot is held. Reproducing this as an unconditional call
    // would free a slot the original never touched, and a state comparison would not catch it unless
    // path_slot_id already happened to be unassigned.
    if (u.path_slot_id != 0xffu) {
        c.path_free_slot(player, unit_idx); // 0x00488936
    }

    // 0x0048894e-0x00488998: GUARDED PRIMARY target release (mode 1), only when target_ref != 0.
    // Order within the guard: target_release_ref call FIRST (0x00488964), target_ref cleared SECOND
    // (0x0048897c), target_index cleared THIRD (0x00488998) -- all three gated by the one guard, so
    // none of them fires when target_ref already reads 0.
    if (u.target_ref != 0) {
        c.target_release_ref(player, unit_idx, 1); // 0x00488964
        u.target_ref   = 0;                        // 0x0048897c
        u.target_index = 0;                        // 0x00488998
    }

    // 0x004889b4-0x004889fe: GUARDED SECONDARY target release (mode 3), identical shape against
    // target2_ref/target2_index, only when target2_ref != 0.
    if (u.target2_ref != 0) {
        c.target_release_ref(player, unit_idx, 3); // 0x004889ca
        u.target2_ref   = 0;                       // 0x004889e2
        u.target2_index = 0;                       // 0x004889fe
    }

    // 0x00488a0a/0x00488a13: UNCONDITIONAL -- runs regardless of any guard above.
    out_cleared_pair[0] = 0;
    out_cleared_pair[1] = 0;
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

void unit_path_encode_directions(uint16_t player, int32_t unit_idx) {
    sim_state st = state();
    detail::unit_path_encode_directions(st.read, st.own, player, unit_idx);
}

void unit_release_path_and_targets(uint16_t player, int32_t unit_idx, uint32_t *out_cleared_pair) {
    sim_state st = state();
    detail::unit_release_path_and_targets(st.own, live_unit_release_path_and_targets_calls(), player,
                                          unit_idx, out_cleared_pair);
}

} // namespace mh::sim
