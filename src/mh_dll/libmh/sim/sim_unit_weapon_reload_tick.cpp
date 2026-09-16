//
// sim/sim_unit_weapon_reload_tick.cpp -- see sim_unit_weapon_reload_tick.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_weapon_reload_tick_0047de7d.asm), not from the Ghidra .c
// draft (whose two switch-case bodies were re-verified byte-for-byte identical against the raw
// jump-table targets before being merged -- see the header's derivation).
//
#include "sim/sim_unit_weapon_reload_tick.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_weapon_reload_tick(const sim_view &v, sim_store &own, uint8_t weapon_slot,
                             double delta_time, double /*game_clock_unread*/) {
    // 0x0047deba-0x0047dec9: _G_LLM_STRAT_CUR_UNIT->weapons[weapon_slot] -- ordinary
    // stride-0x13 indexing (UNIT_WEAPON_SLOTS' own stride); read AND written below, so the
    // mutable store's cur_unit() accessor.
    unit        &u = own.cur_unit();
    unit_weapon &w = u.weapons[weapon_slot];

    // 0x0047decf-0x0047dedf: Weapon[weapon_id].type -- the cfg table read that drives the switch.
    const uint8_t weapon_id = w.weapon_id;
    const uint8_t type      = v.cfg_weapons[weapon_id].type;

    // 0x0047dee4-0x0047def5: `(type - 1)` unsigned-compared against 7 then used to index an
    // 8-entry jump table with exactly two distinct targets (caseD_1, caseD_3) plus the default
    // exit for type==7 or type outside 1..8. caseD_1 (type in {1,2,5,6,8}) and caseD_3 (type in
    // {3,4}) are byte-for-byte identical code -- see the header banner's full instruction-level
    // verification -- so they are merged into one shared body here rather than duplicated or
    // split into a cross-TU helper.
    switch (type) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 8:
            // 0x0047defc/0x0047dfad: proceed only while the slot is disabled (mid-reload).
            if (w.enabled == 0) {
                // 0x0047df14-0x0047df28 / 0x0047dfc5-0x0047dfd9: UNCONDITIONAL decrement, runs
                // whenever the enabled check above passed, regardless of what the timer check below
                // finds.
                w.reload_timer -= delta_time;

                // 0x0047df2b-0x0047df42 / 0x0047dfdc-0x0047dff3: FCOMP computes ST(0)-src with
                // ST(0)=0.0 (FLDZ) and src=the just-written reload_timer; JC (taken on CF, i.e. C0)
                // fires when `0.0 < reload_timer`, so the refill body below runs only when the
                // POST-DECREMENT reload_timer is `<= 0.0` -- same idiom
                // sim_bldg_turret_reload_tick.cpp's identical guard already documents.
                if (w.reload_timer <= 0.0) {
                    // 0x0047df44-0x0047df76 / 0x0047dff5-0x0047e027: refill ONLY when ammo reads
                    // exactly 0, never topped up otherwise.
                    if (w.ammo == 0) {
                        w.ammo = v.cfg_weapons[weapon_id].ammo;
                    }
                    // 0x0047df79-0x0047dfa4 / 0x0047e02a-0x0047e055: reload_timer = 0.0 (two
                    // dword-zero stores -- Watcom's all-zero-bits idiom for an 8-byte double field,
                    // not two separate fields), then re-enable the slot.
                    w.reload_timer = 0.0;
                    w.enabled      = 1;
                }
            }
            break;
        default:
            // type==7 (jump-table slot present but targets the default exit) or type outside 1..8:
            // no effect, matching the Ghidra .c draft's absent case label for 7.
            break;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_weapon_reload_tick(uint8_t weapon_slot, double delta_time, double game_clock_unread) {
    sim_state st = state();
    detail::unit_weapon_reload_tick(st.read, st.own, weapon_slot, delta_time, game_clock_unread);
}


} // namespace mh::sim
