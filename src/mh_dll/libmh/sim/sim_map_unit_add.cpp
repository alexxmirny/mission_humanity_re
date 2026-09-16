//
// sim/sim_map_unit_add.cpp -- see sim_map_unit_add.h. Translated from the DISASSEMBLY
// (tmp/decomp/map_unit_Add_00461e9e.asm), cross-checked field-by-field against Ghidra's own .c draft
// (tmp/decomp/map_unit_Add_00461e9e.c) and against the address arithmetic of the sibling
// llm_strat_unit_init_record (sim_unit_init_record.cpp), whose shape this function shares for every
// field group except the two called out in the header (HAZARD A's table-lookup facing init, and the
// absent crew-link step / self-index stamp).
//
#include "sim/sim_map_unit_add.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const map_unit_add_calls &live_map_unit_add_calls() {
    static const map_unit_add_calls c = {
        MH_CRT(utils_fill_data),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

void unit_add(const sim_view &v, sim_store &own, const map_unit_add_calls &c, uint32_t unit,
              int32_t unit_proto, uint16_t player) {
    // `player` is read back via a 16-bit load at every address computation in the original -- see the
    // header note. `unit`/`unit_proto` are read back full-width throughout, matching
    // sim_unit_init_record.cpp's identical parameter-width note.
    const uint16_t player16 = player;

    // ---- (1) bulk zero-fill, ALWAYS, via the ORIGINAL callee (0x00461ebd-0x00461ee3) ----------------
    // `auto` here (not the `unit` type alias by name): the parameter above is itself named `unit`
    // (matching the committed prototype's parameter name), which shadows the `mh::sim::unit` type
    // alias inside this function's body.
    auto &u = own.unit_at(player16, static_cast<int32_t>(unit));
    c.fill_data(&u, sizeof(u), 0);

    // ---- (2) the record's own fields, ALWAYS (0x00461ee3-0x004620ee) --------------------------------
    const cfg_unit &proto = v.cfg_units[unit_proto];

    u.unit_proto_id  = static_cast<uint16_t>(unit_proto);
    u.energy         = proto.energy; // straight double copy, no arithmetic
    u.order          = MAP_UNIT_ADD_STATE_PARKED;
    u.state          = MAP_UNIT_ADD_STATE_PARKED;
    u.elevation      = proto.elevation_2;
    u.move_heading   = 1;
    u.move_microstep = 0x1f;

    // HAZARD A: BOTH facing fields come from the SAME move-microstep table entry (unlike
    // sim_unit_init_record.cpp's literal `= 1`). The .asm re-derives the lookup twice, independently,
    // from the SAME (move_heading, move_microstep) pair just written above -- nothing writes either
    // field or the table in between, so one lookup reused for both destinations is value-identical.
    const uint8_t initial_facing =
        v.move_microsteps[static_cast<uint32_t>(u.move_heading) * MICROSTEPS_PER_HEADING +
                          static_cast<uint32_t>(u.move_microstep)]
            .facing;
    u.facing_target  = initial_facing;
    u.facing_current = initial_facing;

    // GAME_CLOCK read once and reused for both fields: the original re-reads the same global twice,
    // but nothing writes it in between, matching sim_unit_init_record.cpp's identical "safe reuse".
    const double now = *v.game_clock;
    u.activity_clock = now;
    u.rotation_clock = now;
    u.path_slot_id   = 0xff;

    // HAZARD B: the .asm writes this as two raw 32-bit dword stores (low=0, high=0x3ff00000), the
    // split IEEE-754 bit pattern for exactly 1.0 -- see the header note. Reproduced as a plain
    // assignment, not a bit-twiddle.
    u.move_step_speed_scale = 1.0;

    // ---- (3) weapon-slot loop, ALWAYS, 4 iterations (0x004620ee-0x00462210) -------------------------
    // IDENTICAL shape to sim_unit_init_record.cpp's own step 3 -- see that file for the derivation of
    // why caching weapon_id once (rather than the original's three separate cfg-memory re-reads) is
    // value-identical.
    for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS; ++slot) {
        const uint8_t weapon_id = cfg_unit_weapon_id(proto, slot);

        u.weapons[slot].weapon_id = weapon_id;
        u.weapons[slot].enabled   = cfg_unit_weapon_enabled(proto, slot);
        u.weapons[slot].enabled_2 = u.weapons[slot].enabled; // derived from the write just above
        u.weapons[slot].ammo      = v.cfg_weapons[weapon_id].ammo;
        u.weapons[slot].pocket    = v.cfg_weapons[weapon_id].pocket;
    }

    // NO crew-link step here -- see the header's step-4 note: this function's .asm has no
    // soldier_count read and no _G_LLM_STRAT_SOLDIERS write anywhere in its body.

    // ---- (4) player-wide slot-0 scratch bump, ALWAYS/UNCONDITIONALLY (0x00462215-0x0046223e) -------
    // Same slot-0 convention sim_unit_init_record.cpp's own HAZARD 4 block uses (energy float half +
    // unit_above 16-bit half, split little-endian since unit_above is `uint8_t[2]`, not a scalar).
    auto &u0 = own.unit_at(player16, 0);
    u0.energy += 1.0; // real x87 FADD in the original (FLD1; FADD; FSTP)
    {
        const uint32_t bumped = (uint32_t(u0.unit_above[0]) | (uint32_t(u0.unit_above[1]) << 8)) + 1u;
        u0.unit_above[0]      = static_cast<uint8_t>(bumped & 0xffu);
        u0.unit_above[1]      = static_cast<uint8_t>((bumped >> 8) & 0xffu);
    }

    // ---- (5) player-profile bookkeeping + refresh, ALWAYS (0x0046223e-0x00462277) -------------------
    player_profile &prof = own.profile_at(static_cast<int32_t>(player16));
    prof.units_alive[*v.planet_index] += 1;
    prof.units_built_total[*v.planet_index] += 1;

    c.set_event(MAP_UNIT_ADD_MAP_OBJECTS_REFRESH);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_add(uint32_t unit, int32_t unit_proto, uint16_t player) {
    sim_state st = state();
    detail::unit_add(st.read, st.own, live_map_unit_add_calls(), unit, unit_proto, player);
}


} // namespace mh::sim
