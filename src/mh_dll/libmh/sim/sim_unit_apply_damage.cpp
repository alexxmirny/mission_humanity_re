//
// sim/sim_unit_apply_damage.cpp -- see sim_unit_apply_damage.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_apply_damage_0047e380.asm), cross-checked against the Ghidra .c draft and
// against mh_structs.gen.h's static_asserts for every field offset touched.
//
#include "sim/sim_unit_apply_damage.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"        // CRT-X87: the shared x87 truncation helpers
#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::sim {

const unit_apply_damage_calls &live_unit_apply_damage_calls() {
    static const unit_apply_damage_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_ai_notify_object_removed),
        MH_LIBMH_BIND(llm_strat_unit_state_die_explode),
        MH_LIBMH_BIND(llm_strat_unit_soldier_remove_last),
        MH_LIBMH_BIND(llm_map_bldg_footprint_set_passable),
        MH_LIBMH_BIND(llm_strat_unit_update_damage_smoke),
        MH_LIBMH_BIND(llm_strat_unit_notify_ui),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), 2 occurrences
// in this function (origin_x and origin_y of the DEPLOY_TO_BUILDING footprint lookup). Value-for-value
// C's truncating `/ 32` -- see sim_bldg_state_destroyed.cpp's fine_to_tile() for the verification;
// re-derived locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// mh_cfg_final_struct_Building::anim is `cfg_t_frame_index[12]` flattened to uint8_t[48]
// (addr/mh_structs.gen.h) -- same flattening sim_bldg_state_destroyed.cpp's / sim_bldg_reset_
// construction_anim.cpp's frame_at()/set_frame_at() already document; reused here in the same shape
// (file-local, not shared cross-TU, per those files' own "write only your own new files" reasoning).
// Only the getter is needed here -- this function never writes Building::anim.
int32_t frame_at(const uint8_t (&anim)[48], int32_t slot) {
    int32_t value;
    std::memcpy(&value, &anim[slot * 4], sizeof(value));
    return value;
}

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only). This function's one call site (0x0047e3f3) is
// an ORDINARY call to it, not compiler-inlined -- reproduced here as the identical instruction sequence,
// matching sim_bldg_state_destroyed.cpp's debris_intensity() / sim_unit_refund.cpp's refund_amount()
// precedent for this exact callee. FISTP width verified at THIS site (opcode `db 5d e8` -> 0xDB ModRM
// 0x5D -> reg field 3 -> 0xDB /3 == FISTP m32int), same 32-bit form those precedents use.
int32_t truncate_toward_zero(double value) {
    return ::mh::fp::trunc_i32(value);
}

// The soldier-strip loop's continue-condition (0x0047e578-0x0047e5c2): `FILD soldier_count / FDIVR
// energy_max / FSUBR energy_max / FCOMP cur_energy`, i.e. `energy_max - energy_max/soldier_count`
// computed ENTIRELY on the x87 stack (no intermediate FSTP to a 64-bit double before the FCOMP) --
// reproduced as raw asm rather than a C++ `double threshold = ...;` expression to avoid a 64-bit
// intermediate-rounding step the original never performs, matching sim_bldg_state_destroyed.cpp's
// debris_intensity() precedent for a chained x87 computation feeding a compare. Returns true iff the
// loop should CONTINUE (i.e. iff NOT(threshold < cur_energy), matching the original's `JC -> exit`
// being the negation of "continue").
//
// CRT-X87-CPP (2026-09-11): the avoid-the-intermediate-rounding reasoning above was TESTED, not just
// argued, and it HOLDS -- fptest case C1 pins the refusal. `threshold >= cur_energy` is the right
// flag reading, and it still differs from the assembly on 44 of 1584 swept inputs at PC=64 and on 16
// at PC=53 (the PC=53 ones are exponent range, not precision: the x87 register's 15-bit exponent has
// no control-word field that narrows it). Stays asm.
bool soldier_strip_should_continue(int32_t soldier_count, double energy_max, double cur_energy) {
    return ::mh::fp::soldier_strip_should_continue(soldier_count, energy_max, cur_energy);
}

} // namespace

namespace detail {

void unit_apply_damage(const sim_view &v, sim_store &own, const unit_apply_damage_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced, read+write throughout

    // ---- early-out: DIE_EXPLODE/CORPSE_FOW_DECAY skip EVERYTHING (0x0047e39d-0x0047e3b5) -----------
    // Not merely "skip the state transition" -- the tail's damage-smoke/notify_ui refresh does not run
    // either. See the header banner.
    if (u.state == UNIT_STATE_DIE_EXPLODE || u.state == UNIT_STATE_CORPSE_FOW_DECAY) {
        return;
    }

    // ---- energy -= pending_damage; pending_damage = 0.0 (0x0047e3b5-0x0047e3dc) --------------------
    u.energy -= u.pending_damage;
    u.pending_damage = 0.0; // double field, two dword-zero stores in the asm -- Watcom's usual idiom

    // ---- death test: energy<=0.0 OR trunc(energy)<0, short-circuit like the original's JNC/JGE pair
    // (0x0047e3e1-0x0047e403) -- truncate_toward_zero is only reached when energy>0.0, same as the asm.
    const bool lethal = (u.energy <= 0.0) || (truncate_toward_zero(u.energy) < 0);

    if (lethal) {
        u.energy = 0.0; // (0x0047e405-0x0047e411)

        // ---- HQ energy credit (0x0047e418-0x0047e437): the OWNING player's OWN unit slot 0 (HQ/
        // mothership), a DIFFERENT slot than `u` whenever cur_index != 0. DECLARED NEED: see the header
        // banner -- v.unit_death_hq_energy_credit does not exist in sim_view yet.
        own.unit_at(*v.cur_player, 0).energy += *v.unit_death_hq_energy_credit;

        // ---- DEPLOY_TO_BUILDING footprint-unmap (0x0047e43c-0x0047e514) --------------------------
        // Computed ONCE and reused for both axes + the call's third argument -- see the header banner
        // on why this is provably equivalent to the asm's two-and-a-half-times re-derivation.
        if (u.state == UNIT_STATE_DEPLOY_TO_BUILDING) {
            const int32_t            proto_id  = u.unit_proto_id;
            const int32_t            equiv_bid = v.cfg_units[proto_id].equivalent;
            const int32_t            anim1     = frame_at(v.cfg_buildings[equiv_bid].anim, 1);
            const int32_t            sprite_id = v.anim_frames[anim1 + 1].sprite_id;
            const sprite_meta_entry &sm        = v.sprite_meta[sprite_id];

            const uint32_t tile_x =
                map_width_mask(v) & (static_cast<uint32_t>(u.x) - static_cast<uint32_t>(fine_to_tile(sm.origin_x)));
            const uint32_t tile_y =
                map_height_mask(v) & (static_cast<uint32_t>(u.y) - static_cast<uint32_t>(fine_to_tile(sm.origin_y)));

            c.bldg_footprint_set_passable(static_cast<int32_t>(tile_x), static_cast<int32_t>(tile_y), equiv_bid);
        }

        c.unit_set_state(UNIT_STATE_DIE_EXPLODE);
        c.ai_notify_object_removed(static_cast<uint32_t>(*v.cur_player) | OBJECT_REMOVED_FLAG_UNIT,
                                   static_cast<uint32_t>(*v.cur_index), /*hard_remove=*/0);
        c.unit_state_die_explode();
    } else if (v.cfg_units[unit_of(v, *v.cur_player, *v.cur_index).unit_proto_id].soldier_count > 0) {
        // ---- soldier-strip loop (0x0047e546-0x0047e5e0) -------------------------------------------
        // The GATE above reads unit_proto_id via the ROSTER (unit_of), a LITERALLY DIFFERENT expression
        // from the loop body below, which reads it via the cur_unit POINTER (`u`) throughout -- see the
        // header banner on why both are preserved as written rather than assumed interchangeable.
        while (true) {
            const uint16_t  proto_id = u.unit_proto_id; // re-read every iteration -- see header note
            const cfg_unit &cb       = v.cfg_units[proto_id];
            if (!soldier_strip_should_continue(cb.soldier_count, cb.energy, u.energy)) break;
            u.unit_proto_id -= 1; // DEC -- literal, see header note; NOT a soldier-count decrement
            c.unit_soldier_remove_last(*v.cur_player, *v.cur_index);
        }
    }

    // ---- tail: refresh damage-smoke fx + HUD/info-panel (0x0047e5e2-0x0047e608) --------------------
    c.unit_update_damage_smoke(*v.cur_player, *v.cur_index);
    c.unit_notify_ui(*v.cur_player, *v.cur_index);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_apply_damage() {
    sim_state st = state();
    detail::unit_apply_damage(st.read, st.own, live_unit_apply_damage_calls());
}


} // namespace mh::sim
