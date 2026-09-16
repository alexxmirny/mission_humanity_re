//
// sim/sim_prod_spawn_arrived_unit.cpp -- see sim_prod_spawn_arrived_unit.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_prod_spawn_arrived_unit_0048f31e.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_prod_spawn_arrived_unit.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_spawn_arrived_unit_calls &live_prod_spawn_arrived_unit_calls() {
    static const prod_spawn_arrived_unit_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_create),
        mh::state::evt::snd_play_at,
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
        MH_LIBMH_BIND(llm_strat_unit_order_exit_storage_auto),
    };
    return c;
}

namespace {

// llm_strat_unit_state members this function writes/tests. Declared LOCALLY rather than shared,
// matching this codebase's established per-TU convention for these constants (e.g.
// sim_storage_launch_parked_to_orbit.cpp's own UNIT_STATE_PARKED/_ASCEND_TO_ORBIT, sim_unit_create.cpp's
// own UNIT_STATE_IDLE_SCATTER) -- NOT folded into a shared header. Values agree exactly with
// sim_order_dispatch.cpp's UNIT_STATE_DESCEND_CRUISE and sim_order_enqueue.h's UNIT_STATE_IDLE_SCATTER,
// both already independently pinned from their own disassembly.
constexpr uint16_t UNIT_STATE_DESCEND_CRUISE = 0x30; // 0x0048f46a
constexpr uint16_t UNIT_STATE_IDLE_SCATTER   = 0x13; // 0x0048f486

// cfg_enum_E_UNIT_TYPE members this function tests (0x0048f4cb-0x0048f4fb). Values agree exactly with
// sim_order_enqueue.h's UNIT_TYPE_A_HELI_MOTHER/UNIT_TYPE_H_HELI_MOTHER (already pinned there and in
// mh::ai::ai_state.h from independent disassembly) -- declared locally here per the same per-TU
// convention as the UNIT_STATE_* pair above, not shared, since this TU has no other reason to include
// sim_order_enqueue.h.
constexpr uint32_t UNIT_TYPE_A_HELI_MOTHER = 0x13;
constexpr uint32_t UNIT_TYPE_H_HELI_MOTHER = 0x14;

// _G_LLM_PROD_SHUTTLE_SLOTS[player][slot].status values, named off the struct field's own comment
// (addr/mh_structs.gen.h's mh_llm_prod_shuttle_slot::status): 0xc9 = "arrived, ready to spawn", 0xcc =
// "unit spawned/delivered". Declared locally for the same reason as the pair above.
constexpr int16_t SHUTTLE_SLOT_STATUS_ARRIVED   = 0xc9;
constexpr int16_t SHUTTLE_SLOT_STATUS_DELIVERED = 0xcc;

} // namespace

namespace detail {

uint32_t prod_spawn_arrived_unit(const sim_view &v, sim_store &own,
                                 const prod_spawn_arrived_unit_calls &c, uint16_t player,
                                 uint32_t slot, uint32_t x, uint32_t y, int32_t storage_idx) {
    // The committed storage for `slot` is a full 32-bit register/stack slot (`uint param_2`), but
    // EVERY read of it inside the body (0x0048f349, 0x0048f36a, 0x0048f391, ...) is a 16-bit
    // `MOVZX EAX, word ptr [...]` -- the upper 16 bits of whatever the caller passes are never
    // examined anywhere in this function, matching the .c draft's own `(param_2 & 0xffff)` at every
    // use site. `x`/`y` (a2/param_4) get no such treatment -- every one of their reads is a full
    // 32-bit `MOV`. Reproduced by masking once here (translator-brief rule 7: width is semantic)
    // rather than at every site.
    slot = slot & 0xffffu;

    const uint32_t slot_index = static_cast<uint32_t>(player) * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot;

    // 0x0048f33f-0x0048f3a5: the gate. Three ANDed, short-circuited conditions -- see the header
    // banner for the field-offset cross-check. origin_planet is compared ZERO-extended (MOVZX), not
    // sign-extended, matching the assembly's own `(uint16_t)origin_planet == G_PLANET_INDEX`.
    {
        const prod_shuttle_slot &slot_rec = v.prod_shuttle_slots[slot_index];
        const bool               ready =
            slot_rec.status == SHUTTLE_SLOT_STATUS_ARRIVED &&
            static_cast<int32_t>(static_cast<uint16_t>(slot_rec.origin_planet)) == *v.planet_index &&
            slot_rec.type_ref_id != 0;
        if (!ready) {
            return 0; // 0x0048f3a7
        }
    }

    own.prod_shuttle_slot_at(player, static_cast<int32_t>(slot)).is_heli_mother_pending = 0; // 0x0048f3c9

    // 0x0048f3d3-0x0048f400: create the unit. The 5th (stack) argument is a literal 2, NOT `true`/1 --
    // see the header banner's derivation. Reproduced literally per translator-brief rule 1.
    const uint16_t type_ref_id = v.prod_shuttle_slots[slot_index].type_ref_id;
    const uint32_t new_unit_id = c.unit_create(x, y, type_ref_id, player, static_cast<uint8_t>(2));

    if (new_unit_id == 0) {
        // ---- FAILURE PATH: 0x0048f6c2-0x0048f794 -------------------------------------------------
        // No room to place the unit -- play the explosion FX/sound at a fallback pixel position
        // instead. `general.bw_mask`/`general.bh_mask` are the PIXEL-space wrap masks (struct offsets
        // 0/4), NOT the tile-space width_mask/height_mask (offsets 8/0x20) that sim_state.h's
        // map_width_mask()/map_height_mask() helpers read -- see the header banner's address
        // cross-check. Computed from the ORIGINAL x/y parameters, not anything from the create call.
        const uint32_t fail_x = v.geom->bw_mask & (x * 0x20 + 0x10); // 0x0048f6c2-0x0048f6d3
        const uint32_t fail_y = v.geom->bh_mask & (y * 0x20 + 0x10); // 0x0048f6d6-0x0048f6e7

        if (*v.sim_active != 0) { // 0x0048f6ea
            // 0x0048f6f3-0x0048f717: floor-divide-toward-zero by 32, reproduced as the exact
            // SAR/SHL/SBB shift form per translator-brief rule 8 (differs from a plain `>>5` or `/32`
            // on negative inputs; fail_x/fail_y are non-negative in practice since they are masked,
            // but the original computes it this way regardless).
            const auto floor_div32_trunc = [](uint32_t val) -> int32_t {
                const int32_t signed_val = static_cast<int32_t>(val);
                const int32_t sign       = signed_val >> 31; // SAR ...,0x1f (0 or -1)
                const int32_t borrow     = ((sign << 4) < 0) ? 1 : 0;
                return (signed_val - sign * 0x20 - borrow) >> 5;
            };
            const int32_t tile_row = floor_div32_trunc(fail_y); // 0x0048f6f3-0x0048f704
            const int32_t tile_col = floor_div32_trunc(fail_x); // 0x0048f706-0x0048f714

            // The original offscreen_snd_volume(0x0048f719)+snd_play(0x0048f743) pair, fused into
            // ONE position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that
            // exact pair synchronously at emit. The 0x0048f736 fresh type_ref_id re-read sat
            // BETWEEN the pair in the original; it is a pure load of a slot nothing in between
            // writes, so reading it just before the emit is value-identical.
            const uint16_t sound_type_ref_id = v.prod_shuttle_slots[slot_index].type_ref_id;
            c.snd_play_at(v.cfg_units[sound_type_ref_id].sound_explo, tile_col, tile_row);
        }

        const uint16_t anim_type_ref_id = v.prod_shuttle_slots[slot_index].type_ref_id; // 0x0048f776: fresh re-read
        c.fx_anim_spawn(fail_x, fail_y, static_cast<uint32_t>(v.cfg_units[anim_type_ref_id].anim_explo),
                        *v.game_clock, 1); // 0x0048f776-0x0048f78f

        return 0; // 0x0048f794: local_18 = local_1c (still 0)
    }

    // ---- SUCCESS PATH: 0x0048f412-0x0048f6bd -----------------------------------------------------
    unit &u = own.unit_at(player, static_cast<int32_t>(new_unit_id));

    u.elevation    = v.cfg_units[u.unit_proto_id].elevation + 300; // 0x0048f412-0x0048f451
    u.state        = UNIT_STATE_DESCEND_CRUISE;                    // 0x0048f457-0x0048f46a
    u.order        = UNIT_STATE_IDLE_SCATTER;                      // 0x0048f473-0x0048f486
    u.shuttle_slot = static_cast<uint8_t>(slot);                   // 0x0048f48f-0x0048f4a5

    // 0x0048f4ab-0x0048f4fb: re-read unit_proto_id fresh (unchanged by the writes above) to classify
    // the newly-created unit's cfg TYPE.
    const uint32_t proto_type = v.cfg_units[u.unit_proto_id].type;
    if (proto_type == UNIT_TYPE_A_HELI_MOTHER || proto_type == UNIT_TYPE_H_HELI_MOTHER) {
        player_profile &profile = own.profile_at(player);
        if (profile.primary_mother_unit[*v.planet_index] == 0 &&
            profile.primary_mother_bldg[*v.planet_index] == 0) {                              // 0x0048f4fd-0x0048f537
            profile.primary_mother_unit[*v.planet_index] = static_cast<int32_t>(new_unit_id); // 0x0048f53b-0x0048f553
        }
    }

    own.prod_shuttle_slot_at(player, static_cast<int32_t>(slot)).status =
        SHUTTLE_SLOT_STATUS_DELIVERED; // 0x0048f559-0x0048f56f

    if (storage_idx != 0) { // 0x0048f578-0x0048f57c
        // 0x0048f582-0x0048f6b8: launched out of a storage building -- orient the unit toward the
        // storage's exit heading and issue the auto exit-storage order.
        const int32_t  b_index     = storage_of(v, player, storage_idx).b_index;
        const uint16_t building_id = building_of(v, player, b_index).building_id;

        // cfg_final_struct_Building::facing @0x820 (conductor-added this slice; see sim_state.h's
        // dir_step_offset comment).
        const uint8_t facing_byte = v.cfg_buildings[building_id].door_approach_route[0];

        u.move_heading = facing_byte; // 0x0048f5ab-0x0048f5d1
        const uint8_t facing =
            v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing;
        // 0x0048f5d7-0x0048f631: the assembly recomputes this identical [move_heading][move_microstep]
        // lookup a second time for facing_current rather than reusing the first result -- nothing
        // writes move_heading/move_microstep in between, so the two computations are provably
        // identical; computed once here.
        u.facing_target  = facing; // 0x0048f631-0x0048f62b
        u.facing_current = facing; // 0x0048f631-0x0048f685 (second, redundant recomputation in the original)

        u.home_storage_slot = static_cast<uint8_t>(storage_idx); // 0x0048f68b-0x0048f6a1

        c.unit_order_exit_storage_auto(player, new_unit_id, storage_idx, x, y); // 0x0048f6a7-0x0048f6b8
    }

    return new_unit_id; // 0x0048f794/0x0048f79a
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t prod_spawn_arrived_unit(uint16_t player, uint32_t slot, uint32_t x, uint32_t y,
                                 int32_t storage_idx) {
    sim_state st = state();
    return detail::prod_spawn_arrived_unit(st.read, st.own, live_prod_spawn_arrived_unit_calls(),
                                           player, slot, x, y, storage_idx);
}


} // namespace mh::sim
