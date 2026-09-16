//
// sim/sim_unit_target_tracking.cpp -- see sim_unit_target_tracking.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_target_tick_0047e065.asm,
// tmp/decomp/llm_strat_unit_update_target_tracking_00448ee1.asm,
// tmp/decomp/llm_strat_unit_update_target2_tracking_00449062.asm), not from Ghidra's C drafts --
// all three drafts fold register-reuse artifacts into locals named `unused_ecx`/`unused_ebx` that
// look like real parameters; every value below was re-derived from the raw register writes.
//
// ---- DECLARED NEED: this TU will not compile until sim_view grows cur_player/cur_index -- see the
// header's full note (mirrors sim_unit_passive_engage.cpp's move for its own missing accessor).
//
#include "sim/sim_unit_target_tracking.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_target_tracking_calls &live_unit_target_tracking_calls() {
    static const unit_target_tracking_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_weapon_pixel_distance_ratio),
        MH_LIBMH_BIND(llm_strat_unit_predict_coords_after_delay),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_target_class),
        MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        MH_LIBMH_BIND(llm_strat_unit_fire_at_target2_if_aimed),
    };
    return c;
}

namespace detail {

// ---- PRIMARY target (llm_strat_unit_update_target_tracking @0x00448ee1) --------------------------
//
// No aliveness gate: unlike the target2 sibling below, this one is unconditional -- fetch the
// target's fine coords, optionally lead/predict for a guided weapon, store, always return 1.
int32_t unit_update_target_tracking(const sim_view &v, sim_store &own,
                                    const unit_target_tracking_calls &c, uint32_t player,
                                    int32_t unit_idx) {
    const unit   &u         = unit_of(v, player, unit_idx);
    const uint8_t weapon_id = u.weapons[u.selected_weapon].weapon_id; // 0x00448ee1-0x00448f37

    // 0x00448f3a-0x00448f7c: get_coords(target_ref&0xf, (ushort)target_index, &fine_x, &fine_y).
    int32_t target_fine_x = 0, target_fine_y = 0;
    c.unit_get_coords((uint16_t)(u.target_ref & 0xf), (int32_t)(uint16_t)u.target_index,
                      &target_fine_x, &target_fine_y);

    // 0x00448f81-0x00448f8f: Weapon[weapon_id].homing == 2 (cfg_t_homing_type; no Ghidra enum
    // member names this value today -- see declared_needs[]).
    if (v.cfg_weapons[weapon_id].homing == 2) {
        // 0x00448f95-0x00448fa2: get_coords(player, unit_idx, &own_fine_x, &own_fine_y) -- the
        // FIRER's own position this time, not the target's.
        int32_t own_fine_x = 0, own_fine_y = 0;
        c.unit_get_coords((uint16_t)player, unit_idx, &own_fine_x, &own_fine_y);

        // 0x00448fa7-0x00448fbc: weapon_pixel_distance_ratio(weapon_id, own_x, own_y, target_x,
        // target_y) -- x87 float10 return, /arch:IA32 /fp:precise.
        double dist_ratio = c.weapon_pixel_distance_ratio((int32_t)weapon_id, own_fine_x, own_fine_y,
                                                          target_fine_x, target_fine_y);

        // 0x00448fbf-0x00449012: predict_coords_after_delay(target_ref&0xf, (ushort)target_index,
        // unused_ebx, unused_ecx, GAME_CLOCK+dist_ratio, &fine_x, &fine_y). unused_ebx/unused_ecx are
        // genuine Watcom register-reuse leftovers, NOT meaningful arguments -- see the uncertainty
        // note on unused_ecx's provenance. unused_ebx is solid: EBX is freshly loaded with
        // player*0x5b04 (the unit row-stride multiply, an address-arithmetic byproduct) at
        // 0x00448ff4 and never touched again before the call.
        uint32_t unused_ebx = (uint16_t)player * 0x5b04u;
        // unused_ecx: ECX is never explicitly reassigned between the weapon_pixel_distance_ratio
        // call (which last set it to target_fine_x as that call's x2 argument, 0x00448fab) and this
        // call -- so whatever value survives the pixel_distance_ratio call is what goes in. Ghidra's
        // own decompile names it `unused_ecx = local_1c` (== target_fine_x at that point), which
        // matches the raw asm IF llm_strat_weapon_pixel_distance_ratio leaves ECX unclobbered. Not
        // independently re-verified against that callee's own body -- see uncertainties[].
        uint32_t unused_ecx = (uint32_t)target_fine_x;
        double   time_delta = *v.game_clock + dist_ratio;
        // target_fine_x/y stay int32_t (matching unit_get_coords' fill above and self.target_fine_x/y's
        // int32_t struct field below); unit_predict_coords_after_delay's committed out-params are
        // uint32_t * (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast at this minority site.
        c.unit_predict_coords_after_delay((uint32_t)(u.target_ref & 0xf),
                                          (int32_t)(uint16_t)u.target_index, unused_ebx, unused_ecx,
                                          time_delta, reinterpret_cast<uint32_t *>(&target_fine_x),
                                          reinterpret_cast<uint32_t *>(&target_fine_y));
    }

    // 0x00449017-0x00449061: store, return 1.
    unit &self         = own.unit_at(player, unit_idx);
    self.target_fine_x = target_fine_x;
    self.target_fine_y = target_fine_y;
    return 1;
}

// ---- SECONDARY target (llm_strat_unit_update_target2_tracking @0x00449062) -----------------------
//
// Same shape as the primary above, PLUS its own aliveness gate on the target's energy.
int32_t unit_update_target2_tracking(const sim_view &v, sim_store &own,
                                     const unit_target_tracking_calls &c, uint32_t player,
                                     int32_t unit_idx) {
    const unit   &u         = unit_of(v, player, unit_idx);
    const uint8_t weapon_id = u.weapons[u.selected_weapon].weapon_id; // 0x00449062-0x004490b8

    // 0x004490bb-0x00449110: FLDZ/FCOMP/FNSTSW/SAHF/JC on the target's energy -- ORDERED `0.0 <
    // energy`, ALIVE (JC) branch taken on true OR on unordered (NaN energy). Writing the dead
    // condition as plain IEEE `energy <= 0.0` reproduces this exactly: `<=` is already false for
    // NaN, so a NaN-energy target lands in the alive/else branch below, matching JC firing on
    // unordered -- same idiom sim_unit_passive_engage.cpp documents for this FCOMP/JC shape (there
    // written as the equivalent `!(energy <= 0.0)` alive-gate; here written as the dead-gate since
    // this function has an explicit dead branch).
    const unit &target = unit_of(v, (uint32_t)(u.target2_ref & 0xf), (int32_t)(uint16_t)u.target2_index);
    if (target.energy <= 0.0) {
        // 0x00449112-0x00449162: release (mode 3), clear BOTH target2_ref and target2_index (unlike
        // target_tick's own release path below, which clears only target2_ref), return 0.
        c.target_release_ref(player, unit_idx, 3);
        unit &self         = own.unit_at(player, unit_idx);
        self.target2_ref   = 0;
        self.target2_index = 0;
        return 0;
    }

    // 0x00449167-0x004491a9: get_coords(target2_ref&0xf, (ushort)target2_index, &fine_x, &fine_y).
    int32_t target2_fine_x = 0, target2_fine_y = 0;
    c.unit_get_coords((uint16_t)(u.target2_ref & 0xf), (int32_t)(uint16_t)u.target2_index,
                      &target2_fine_x, &target2_fine_y);

    // 0x004491ae-0x004491bc: same homing==2 gate as the primary sibling.
    if (v.cfg_weapons[weapon_id].homing == 2) {
        int32_t own_fine_x = 0, own_fine_y = 0;
        c.unit_get_coords((uint16_t)player, unit_idx, &own_fine_x, &own_fine_y);
        double dist_ratio = c.weapon_pixel_distance_ratio((int32_t)weapon_id, own_fine_x, own_fine_y,
                                                          target2_fine_x, target2_fine_y);
        // Same register-provenance shape as the primary sibling: unused_ebx solid (fresh
        // player*0x5b04 at 0x00449221, untouched to the call), unused_ecx carried from
        // target2_fine_x via the same "never reassigned between the two calls" argument -- same
        // uncertainty about llm_strat_weapon_pixel_distance_ratio's own ECX behavior.
        uint32_t unused_ebx = (uint16_t)player * 0x5b04u;
        uint32_t unused_ecx = (uint32_t)target2_fine_x;
        double   time_delta = *v.game_clock + dist_ratio;
        // Same int32_t-local/uint32_t*-out-param cast as the primary sibling above (TACT1-P C6,
        // 2026-09-04).
        c.unit_predict_coords_after_delay((uint32_t)(u.target2_ref & 0xf),
                                          (int32_t)(uint16_t)u.target2_index, unused_ebx, unused_ecx,
                                          time_delta, reinterpret_cast<uint32_t *>(&target2_fine_x),
                                          reinterpret_cast<uint32_t *>(&target2_fine_y));
    }

    // 0x00449244-0x0044927c: store, return 1.
    unit &self          = own.unit_at(player, unit_idx);
    self.target2_fine_x = target2_fine_x;
    self.target2_fine_y = target2_fine_y;
    return 1;
}

// ---- the driver (llm_strat_unit_target_tick @0x0047e065) ------------------------------------------
//
// Globals-only entry point -- CUR_PLAYER/CUR_INDEX are read only to pass to the callees below (the
// original's own args at 0x0047e122-0x0047e130 etc.), NOT to address the current unit's own record:
// every field access on "the current unit" in the ORIGINAL goes through _G_LLM_STRAT_CUR_UNIT
// directly (verified at 0x0047e07d, 0x0047e08b, 0x0047e0a5, 0x0047e0c6, 0x0047e0d4, 0x0047e0ee,
// 0x0047e114, 0x0047e135, 0x0047e141, 0x0047e153, 0x0047e172, 0x0047e1c9/0x0047e1ce), so "u" binds
// via *v.cur_unit here -- matching sim_unit_update_rotation.cpp/sim_unit_update_soldiers.cpp's own
// choice for the identical triad, rather than assuming CUR_UNIT == &units[CUR_PLAYER][CUR_INDEX]
// (reimpl-verify 2026-08-10: an earlier draft derived "u" via unit_of(v, player, index) instead,
// which both those siblings had already flagged as an assumption not provable from one function's
// own bytes). "u" is a live reference and is re-read after the (conditional) call to
// unit_update_target2_tracking below, since that call writes through the SAME live memory (rule 16:
// const means read-only, not immutable) -- this mirrors the raw asm, which reloads CUR_UNIT's fields
// fresh at LAB_0047e135 after the call.
void unit_target_tick(const sim_view &v, sim_store &own, const unit_target_tracking_calls &c) {
    const uint32_t player = (uint32_t)*v.cur_player;
    const int32_t  index  = (int32_t)*v.cur_index;
    const unit    &u      = *v.cur_unit;

    // 0x0047e07d-0x0047e10d: alive = (target2_ref&0x40 && !(building.energy<=0.0)) ||
    // (target2_ref&0xa0 && !(unit.energy<=0.0)) -- short-circuit OR, building clause evaluated
    // first, exactly as the raw JC/fallthrough targets order it. Same ORDERED/NaN-is-alive FCOMP/JC
    // idiom as unit_update_target2_tracking's own gate above.
    bool alive = false;
    if ((u.target2_ref & 0x40) != 0) {
        const building &b = building_of(v, (uint32_t)(u.target2_ref & 0xf),
                                        (int32_t)(uint16_t)u.target2_index);
        if (!(b.energy <= 0.0)) alive = true;
    }
    if (!alive && (u.target2_ref & 0xa0) != 0) {
        const unit &tu = unit_of(v, (uint32_t)(u.target2_ref & 0xf), (int32_t)(uint16_t)u.target2_index);
        if (!(tu.energy <= 0.0)) alive = true;
    }

    if (alive) {
        // 0x0047e114-0x0047e130: only a UNIT target2 (0xa0 bit) re-tracks; a building target2 does
        // not. LOCAL call, same TU -- not through `c`/mh::call:: (translator-brief instruction: both
        // functions are defined in this batch).
        if ((u.target2_ref & 0xa0) != 0) unit_update_target2_tracking(v, own, c, player, index);

        // 0x0047e135-0x0047e1a6: re-read u's fields (may have changed above), classify, convert
        // target2's fine coords to tile coords via TRUNCATING signed /32 (the SAR/SHL/SBB/SAR
        // sequence at 0x0047e159-0x0047e18c is the standard shift-form of IDIV-equivalent truncating
        // division by a power of two -- C++'s `/` on int32_t already truncates toward zero
        // identically, so plain division reproduces it exactly; same idiom
        // sim_unit_state_predicates.cpp's fine_to_tile() documents, re-derived locally per the
        // translator brief rather than shared across TUs), then test weapon range.
        int32_t target_class_result =
            c.target_class((uint32_t)(uint16_t)u.target2_ref, (int32_t)(uint16_t)u.target2_index);
        int32_t  tile_y = u.target2_fine_y / 32;
        int32_t  tile_x = u.target2_fine_x / 32;
        uint32_t in_range =
            c.unit_in_weapon_range((int32_t)player, index, tile_x, tile_y, target_class_result);
        if (in_range != 0) {
            // 0x0047e1aa-0x0047e1af
            c.unit_fire_at_target2_if_aimed();
            return;
        }
        // in_range == 0: falls through to the release path below, same as the not-alive case.
    }

    // 0x0047e1b1-0x0047e1d7: release (mode 3), clear target2_ref ONLY (target2_index is left as-is
    // here -- unlike unit_update_target2_tracking's own dead branch, which clears both; preserve the
    // asymmetry, do not "fix" it).
    c.target_release_ref(player, index, 3);
    own.cur_unit().target2_ref = 0;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

int32_t unit_update_target_tracking(uint32_t player, int32_t unit_idx) {
    sim_state st = state();
    return detail::unit_update_target_tracking(st.read, st.own, live_unit_target_tracking_calls(),
                                               player, unit_idx);
}

int32_t unit_update_target2_tracking(uint32_t player, int32_t unit_idx) {
    sim_state st = state();
    return detail::unit_update_target2_tracking(st.read, st.own, live_unit_target_tracking_calls(),
                                                player, unit_idx);
}

void unit_target_tick() {
    sim_state st = state();
    detail::unit_target_tick(st.read, st.own, live_unit_target_tracking_calls());
}


} // namespace mh::sim
