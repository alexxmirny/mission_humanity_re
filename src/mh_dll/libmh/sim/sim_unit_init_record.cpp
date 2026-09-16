//
// sim/sim_unit_init_record.cpp -- see sim_unit_init_record.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_init_record_004619b0.asm), cross-checked field-by-field against Ghidra's
// own .c draft: unlike several SIM1A siblings, this draft's control flow and every field/offset it
// names checked out exactly against the raw IMUL/ADD/MOV address arithmetic (struct offsets verified
// against mh_structs.gen.h's static_asserts one at a time -- see the header hazards for the two places
// that are easy to misread anyway: the double `unit_above` write and the slot-0 scratch pair).
// Callees are indirected through init_record_calls (see the header) so the body stays testable by
// net_selftest.exe simtest, matching every other multi-callee TU in this batch.
//
#include "sim/sim_unit_init_record.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h"        // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const init_record_calls &live_init_record_calls() {
    static const init_record_calls c = {
        MH_CRT(utils_fill_data),
        MH_PROMOTED_ROW(llm_strat_squad_placement_offset_lookup) /* renamed from llm_ui_cursor_lookup_offset_pair 2026-09-02 */,
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

void init_record(const sim_view &v, sim_store &own, const init_record_calls &c, int32_t unit_idx,
                 uint32_t unit_proto_id, uint32_t player) {
    // `player` is read back via a 16-bit load at every address computation in the original -- see the
    // header note. `unit_idx`/`unit_proto_id` are read back full-width throughout (only narrowed on
    // write, into fields that are themselves 2 bytes wide), so no truncation is applied to them.
    const uint16_t player16 = (uint16_t)player;

    // ---- (1) bulk zero-fill, ALWAYS, via the ORIGINAL callee (0x004619cf-0x004619f0) -- see HAZARD 1
    unit &u = own.unit_at(player16, unit_idx);
    c.fill_data(&u, sizeof(unit), 0);

    // ---- (2a) self-index stamp, ALWAYS, right after the fill (0x004619f5-0x00461a0b) -- HAZARD 2 ----
    // unit_above is `uint8_t[2]`, not a scalar (mh_structs.gen.h) -- split little-endian, same idiom
    // sim_unit_remove_from_map.cpp / sim_unit_soldier_chain.cpp use for the identical field.
    u.unit_above[0] = (uint8_t)((uint32_t)unit_idx & 0xffu);
    u.unit_above[1] = (uint8_t)(((uint32_t)unit_idx >> 8) & 0xffu);

    // ---- (2b) the rest of the record's own fields, ALWAYS (0x00461a12-0x00461afe) ------------------
    const cfg_unit &proto = v.cfg_units[unit_proto_id];

    u.unit_proto_id  = (uint16_t)unit_proto_id;
    u.move_microstep = 0x1f;
    u.energy         = proto.energy; // straight double copy, no arithmetic
    u.facing_target  = 1;
    u.facing_current = 1;
    // GAME_CLOCK read once and reused for both fields: the original re-reads the same global twice
    // (0x00461ab9 and 0x00461ad8), but nothing writes it in between, so a single read is
    // value-identical -- same "safe reuse" reasoning sim_unit_on_destroyed.cpp applies to
    // `unit_proto_id`.
    const double now = *v.game_clock;
    u.activity_clock = now;
    u.rotation_clock = now;
    u.path_slot_id   = 0xff;

    // ---- (3) weapon-slot loop, ALWAYS, 4 iterations (0x00461afe-0x00461c25) -------------------------
    // cfg_unit::weapons is a flat (id, enabled) byte-pair array -- read only via the sanctioned
    // accessors (sim_state.h), never by hand-indexing the flattened uint8_t[8].
    for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS; ++slot) {
        // weapon_id re-read once and reused for both cfg_weapons lookups below: the original re-reads
        // Unit[proto].weapons[slot].id from cfg memory THREE times (once for the write into
        // weapons[slot].weapon_id, once for the ammo lookup, once for the pocket lookup) -- nothing
        // writes cfg data in between, so caching the one value is value-identical.
        const uint8_t weapon_id = cfg_unit_weapon_id(proto, slot);

        u.weapons[slot].weapon_id = weapon_id;
        u.weapons[slot].enabled   = cfg_unit_weapon_enabled(proto, slot);
        u.weapons[slot].enabled_2 = u.weapons[slot].enabled; // derived from the write just above
        u.weapons[slot].ammo      = v.cfg_weapons[weapon_id].ammo;
        u.weapons[slot].pocket    = v.cfg_weapons[weapon_id].pocket;
    }

    // ---- (4) crew-link loop, if this unit type carries crew (0x00461c25-0x00461e2f) -- HAZARD 3 -----
    const int32_t soldier_count = proto.soldier_count;
    if (soldier_count != 0) {
        int32_t remaining        = soldier_count; // decrements per successful link; loop exit test
        int32_t prev_linked_slot = 0;             // becomes each new link's next_soldier; final value
                                                  // overwrites unit_above below (HAZARD 2b)

        for (int32_t slot = 1; slot < 100 && remaining != 0; ++slot) {
            soldier &s = own.soldier_at(player16, slot);
            if (s.owner_unit != 0) continue; // occupied -- skip, do NOT decrement `remaining`

            s.owner_unit    = (int16_t)unit_idx;
            s.next_soldier  = (uint16_t)prev_linked_slot;
            s.walk_duration = 0.0; // asm zeroes both dwords of the 8-byte field; 0.0 is all-zero
                                   // bits, so this is bit-identical
            s.sprite_frame      = 1;
            s.idle_wander_flag  = 0;
            s.anim_change_count = 0;

            // PURE QUERY (addr/mh_effects.gen.h: disposition::pure) -- see the header HAZARD 3 note.
            // Register order EAX/EDX/EBX/ECX = table_col/table_row/out_a/out_b: table_col is the
            // ORIGINAL constant soldier_count, table_row is the decrementing `remaining`, out_a/out_b
            // are &start_x/&start_y (offsets 7/8 in the 0x1d-byte soldier record, confirmed against
            // the asm's own address arithmetic).
            c.cursor_lookup_offset_pair(soldier_count, remaining,
                                        reinterpret_cast<char *>(&s.start_x),
                                        reinterpret_cast<char *>(&s.start_y));
            s.end_x = s.start_x;
            s.cur_x = s.end_x;
            s.end_y = s.start_y;
            s.cur_y = s.end_y;

            // record[0]'s owner_unit doubles as the roster's used-slot count -- same slot-0 dual-use
            // sim_unit_remove_from_map.cpp's unlink loop decrements.
            own.soldier_at(player16, 0).owner_unit += 1;

            prev_linked_slot = slot;
            --remaining;
        }

        // HAZARD 2b: unconditional once soldier_count != 0 (reached whether or not any slot was
        // actually free to link -- prev_linked_slot stays 0 in that case, matching the asm exactly).
        u.unit_above[0] = (uint8_t)((uint32_t)prev_linked_slot & 0xffu);
        u.unit_above[1] = (uint8_t)(((uint32_t)prev_linked_slot >> 8) & 0xffu);
    }

    // ---- (5) player-wide slot-0 scratch bump, ALWAYS/UNCONDITIONALLY (0x00461e2f-0x00461e58) -------
    // HAZARD 4: these are NOT the new unit's own fields -- they always index slot 0, the roster's
    // reserved per-player header row. Mirrored by llm_strat_unit_teardown's own slot-0 decrements
    // (not reimplemented here).
    unit &u0 = own.unit_at(player16, 0);
    u0.energy += 1.0; // real x87 FADD in the original (FLD1; FADD; FSTP)
    {
        const uint32_t bumped = (uint32_t(u0.unit_above[0]) | (uint32_t(u0.unit_above[1]) << 8)) + 1u;
        u0.unit_above[0]      = (uint8_t)(bumped & 0xffu);
        u0.unit_above[1]      = (uint8_t)((bumped >> 8) & 0xffu);
    }

    // ---- (6) player-profile bookkeeping + refresh, ALWAYS (0x00461e58-0x00461e96) -------------------
    player_profile &prof = own.profile_at((int32_t)player16);
    prof.units_alive[*v.planet_index] += 1;
    prof.units_built_total[*v.planet_index] += 1;

    c.set_event(MAP_OBJECTS_REFRESH);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_init_record(int32_t unit_idx, uint32_t unit_proto_id, uint32_t player) {
    sim_state st = state();
    detail::init_record(st.read, st.own, live_init_record_calls(), unit_idx, unit_proto_id, player);
}


} // namespace mh::sim
