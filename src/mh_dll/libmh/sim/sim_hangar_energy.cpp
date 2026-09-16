//
// sim/sim_hangar_energy.cpp -- see sim_hangar_energy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_hangar_any_unit_needs_energy_0047b0db.asm,
// _hangar_recharge_pulse_0047b1be.asm), cross-checked against the Ghidra .c drafts (tmp/decomp_sim/*.c)
// -- both agree branch-for-branch with the assembly.
//
#include "sim/sim_hangar_energy.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const hangar_recharge_pulse_calls &live_hangar_recharge_pulse_calls() {
    static const hangar_recharge_pulse_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_try_pay_action_cost),
        MH_LIBMH_BIND(llm_strat_unit_update_damage_smoke),
    };
    return c;
}

namespace detail {

int32_t hangar_any_unit_needs_energy(const sim_view &v, int32_t player, int32_t index) {
    const uint32_t p = static_cast<uint32_t>(player);

    // 0x0047b0f8-0x0047b112: sub_id = buildings[player][index].sub_id (the hangar's STORAGE SLOT, not
    // a unit/roster index); loop cursor starts at 0.
    const building     &b = building_of(v, p, index);
    const unit_storage &s = storage_of(v, p, b.sub_id);

    for (int32_t i = 0; i < s.docked_count; ++i) {
        const int32_t   unit_idx = s.docked_units[i];
        const unit     &u        = unit_of(v, p, unit_idx);
        const cfg_unit &cu       = v.cfg_units[u.unit_proto_id];

        // 0x0047b18f-0x0047b1a9: FLD energy / FCOMP max / JNC <continue-loop>. JNC fires (this unit is
        // already full, ordered `energy >= max`) -> move on to the next docked unit without setting
        // the found flag. See the header's FP-COMPARE note: this is a faithful direct transcription
        // for every input, ordered or unordered, because IEEE-754 `>=` is also false on any NaN
        // operand, matching x87's CF=1-on-unordered (JNC not taken) case.
        if (u.energy >= cu.energy) {
            continue;
        }

        // 0x0047b1a0-0x0047b1a7: fallthrough (JNC not taken) -> found=1, return immediately.
        return 1;
    }

    // 0x0047b1ab-0x0047b1b2: docked_count<=0 short-circuits here too (CMP/JL/JMP at 0x0047b12c-0x0047b134),
    // same as running the loop to exhaustion without a match.
    return 0;
}

void hangar_recharge_pulse(const sim_view &v, sim_store &own, uint32_t player, int32_t index,
                           const hangar_recharge_pulse_calls &c) {
    // 0x0047b1db-0x0047b1f5: sub_id = buildings[player][index].sub_id; loop cursor starts at 0. Same
    // addressing as hangar_any_unit_needs_energy above -- re-derived independently per this TU's own
    // read, not shared, matching every other sibling pair in this closure.
    const building     &b = building_of(v, player, index);
    const unit_storage &s = storage_of(v, player, b.sub_id);

    for (int32_t i = 0; i < s.docked_count; ++i) {
        const int32_t unit_idx = s.docked_units[i];

        // ---- gate (0x0047b272-0x0047b281): already-full check, same FP-compare posture as
        // hangar_any_unit_needs_energy's own gate (see the header note) -- direct, faithful for NaN too.
        {
            const unit     &u_read = unit_of(v, player, unit_idx);
            const cfg_unit &cu     = v.cfg_units[u_read.unit_proto_id];
            if (u_read.energy >= cu.energy) {
                continue; // already full: no payment attempt, no write, no calls
            }
        }

        // ---- payment gate (0x0047b287-0x0047b295): nonzero result = payment failed, skip this unit
        // entirely (no partial charge, no damage-smoke call).
        const int32_t pay_result = c.unit_try_pay_action_cost(player, unit_idx);
        if (pay_result != 0) {
            continue;
        }

        // ---- the pulse (0x0047b29b-0x0047b2d4): energy += cfg_units[proto].energy_2 (the recharge
        // RATE field, distinct from `.energy` itself). The original re-derives the unit address and
        // re-reads unit_proto_id AFTER the payment call rather than reusing the gate's values
        // (0x0047b29b/0x0047b2ab both recompute the row+stride address, 0x0047b2bb re-does the
        // MOVZX-then-IMUL proto lookup) -- `u`/`cu` below are a fresh reference/read for the same
        // reason, not a cached carry-over from the gate above.
        unit           &u  = own.unit_at(player, unit_idx);
        const cfg_unit &cu = v.cfg_units[u.unit_proto_id];
        u.energy           = cu.energy_2 + u.energy;

        // ---- the clamp (0x0047b2ea-0x0047b34b): FLD energy(updated) / FCOMP max / JBE <skip-clamp>.
        // Fallthrough (do clamp) means `energy > max`, ORDERED-STRICT -- see the header's FP-COMPARE
        // note: `cu.energy < u.energy` is a faithful direct transcription (false on NaN, matching
        // JBE's unordered-fires-skip case).
        if (cu.energy < u.energy) {
            u.energy = cu.energy;
        }

        // ---- unconditional on this success path only (0x0047b351-0x0047b358) -------------------------
        c.unit_update_damage_smoke(player, unit_idx);
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

int32_t hangar_any_unit_needs_energy(int32_t player, int32_t index) {
    const sim_view v = state().read;
    return detail::hangar_any_unit_needs_energy(v, player, index);
}

void hangar_recharge_pulse(uint32_t player, int32_t index) {
    sim_state st = state();
    detail::hangar_recharge_pulse(st.read, st.own, player, index, live_hangar_recharge_pulse_calls());
}


} // namespace mh::sim
