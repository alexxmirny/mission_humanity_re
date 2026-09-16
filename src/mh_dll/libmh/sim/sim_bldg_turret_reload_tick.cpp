//
// sim/sim_bldg_turret_reload_tick.cpp -- see sim_bldg_turret_reload_tick.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_turret_reload_tick_0047c60b.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_bldg_turret_reload_tick.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void bldg_turret_reload_tick(const sim_view &v, sim_store &own, uint16_t player, uint32_t building_id,
                             double dt) {
    // 0x0047c628-0x0047c63b: buildings[player][building_id].sub_id -- ordinary building_of()
    // indexing (BUILDINGS_PER_PLAYER*sizeof(building) / sizeof(building)); read-only, so the const
    // view.
    const building &b      = building_of(v, player, (int32_t)building_id);
    const uint8_t   sub_id = b.sub_id;

    // 0x0047c649-0x0047c65c onward: turrets[player][sub_id] -- ordinary turret_at() indexing
    // (TURRETS_PER_PLAYER*sizeof(turret) / sizeof(turret)); this function both reads and writes it,
    // so the mutable store.
    turret &t = own.turret_at(player, sub_id);

    // Check 1 (0x0047c66f-0x0047c676): proceed only while the turret is actively counting down.
    if (t.reload_ready_flag != 0) return;

    // Check 2 (0x0047c68c-0x0047c695): UNCONDITIONAL decrement -- runs whenever check 1 passed,
    // regardless of what check 3 finds. FP comparison note: this is an x87 store (FSTP to memory)
    // immediately followed by an FLDZ/FCOMP reload of the SAME memory location, so comparing the
    // freshly-computed `double` against 0.0 here reproduces the store-then-reload rounding exactly
    // under this TU's /arch:IA32 /fp:precise build (see sim_state.h's FP-landmine note); flagged in
    // uncertainties per the translator brief's blanket FP-comparison rule.
    t.reload_timer -= dt;

    // Check 3 (0x0047c6ab-0x0047c6b6): FCOMP computes ST(0)-src with ST(0)=0.0 (FLDZ) and
    // src=reload_timer; JC (taken on CF, i.e. C0) fires when 0.0 < reload_timer, so the refill body
    // below runs only when the POST-DECREMENT reload_timer is <= 0.0.
    if (t.reload_timer > 0.0) return;

    // Refill body (0x0047c6c8-0x0047c72f), all three guards passed.
    //
    // weapon_id is read HERE (inside the arm that actually uses it) rather than at the original's
    // unconditional early read (0x0047c655-0x0047c65c, into a dead local whenever an earlier guard
    // failed) -- a plain field read with no side effect, so relocating it changes nothing observable.
    if (t.ammo == 0) {
        // 0x0047c6d1-0x0047c6ee: ammo = Weapon[weapon_id].ammo. Refilled ONLY when ammo reads
        // exactly 0, never topped up otherwise.
        t.ammo = v.cfg_weapons[t.weapon_id].ammo;
    }
    // 0x0047c704/0x0047c70e: reload_timer = 0.0. The asm stores this as TWO consecutive dword-zero
    // writes (0.0's bit pattern is all-zero) rather than an FSTP -- the struct's single 8-byte double
    // field, not two fields; see mh_structs.gen.h's reload_timer comment.
    t.reload_timer = 0.0;
    // 0x0047c728: mark the turret ready.
    t.reload_ready_flag = 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_turret_reload_tick(uint16_t player, uint32_t building_id, double dt) {
    sim_state st = state();
    detail::bldg_turret_reload_tick(st.read, st.own, player, building_id, dt);
}


} // namespace mh::sim
