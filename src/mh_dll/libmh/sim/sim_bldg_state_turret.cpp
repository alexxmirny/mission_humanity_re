//
// sim/sim_bldg_state_turret.cpp -- see sim_bldg_state_turret.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_turret_scan_00471ab1.asm,
// _turret_attack_00471e53.asm), not from the Ghidra .c drafts (though those drafts were checked
// against every branch below and agree with the assembly throughout -- see the header banner).
//
#include "sim/sim_bldg_state_turret.h"

#include <cstdint>
#include <cstring>
#include <limits>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_turret_scan_calls &live_bldg_state_turret_scan_calls() {
    static const bldg_state_turret_scan_calls c = {
        MH_LIBMH_BIND(llm_strat_turret_acquire_target),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

const bldg_state_turret_attack_calls &live_bldg_state_turret_attack_calls() {
    static const bldg_state_turret_attack_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dist_out_of_range),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(llm_strat_turret_fire),
    };
    return c;
}

namespace {

// This file's own literal operands, sourced from the Ghidra .c drafts' resolved `llm_strat_bldg_state`
// enum member names (rule 17a: existing enum, not yet a real C++ type). Same VALUES already used
// independently in sim_bldg_state_reset_idle.h (TURRET_SCAN, plain `mh::sim`-scope) and
// sim_order_dispatch_bldg.cpp (TURRET_ATTACK, this same anonymous-namespace shape) -- kept internal
// linkage here (unlike sim_bldg_state_reset_idle.h's copy) so no TU that ever included both headers
// could hit a redefinition.
constexpr uint16_t BLDG_STATE_TURRET_SCAN   = 0x7a; // Ghidra: TURRET_SCAN
constexpr uint16_t BLDG_STATE_TURRET_ATTACK = 0x7b; // Ghidra: TURRET_ATTACK

// RESOLVED (conductor, 2026-08-22): read-memory confirmed DAT_005012e4 = 0.5 (hex 00 00 00 00 00 00 E0
// 3F). Named+typed in Ghidra as _G_LLM_STRAT_TURRET_ATTACK_INTERVAL_SCALE (EN v307).
constexpr double TURRET_ATTACK_INTERVAL_SCALE = 0.5; // _G_LLM_STRAT_TURRET_ATTACK_INTERVAL_SCALE, was DAT_005012e4

// mh_map_object_building::anim / mh_cfg_final_struct_Building::anim are both `cfg_t_frame_index[12]`
// flattened to uint8_t[48] (addr/mh_structs.gen.h) -- same flattening sim_bldg_state_destroyed.cpp's
// frame_at()/set_frame_at() document and work around; re-derived file-locally here per this project's
// per-TU convention (not shared cross-TU).
int32_t frame_at(const uint8_t (&anim)[48], int32_t slot) {
    int32_t value;
    std::memcpy(&value, &anim[slot * 4], sizeof(value));
    return value;
}
void set_frame_at(uint8_t (&anim)[48], int32_t slot, int32_t value) {
    std::memcpy(&anim[slot * 4], &value, sizeof(value));
}

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), the SAME
// truncating-`/32` idiom sim_order_enqueue.cpp's fine_to_tile() / sim_bldg_state_destroyed.cpp's own
// copy verify; re-derived locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// llm_strat_turret_fire's param_6/param_7 are the raw dword-pair bit pattern of cur_building's
// last_tick_time double (0x004723d2/0x004723d5: PUSH cur_building[+0x9] (high dword) then PUSH
// cur_building[+0x5] (low dword) -- see the header's param-marshalling derivation), passed as two
// plain uint32_t registers/stack slots rather than a reconstructed double (matching this callee's
// already-committed uint32_t/uint32_t signature in mh_calls.gen.h). memcpy, not a reinterpret_cast,
// matching sim_bldg_state_deploy.cpp's split_game_clock() precedent for the identical shape.
void split_double(double value, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    lo = static_cast<uint32_t>(bits);
    hi = static_cast<uint32_t>(bits >> 32);
}

} // namespace

namespace detail {

void bldg_state_turret_scan(const sim_view &v, sim_store &own, const bldg_state_turret_scan_calls &c) {
    building           &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t      bid = b.building_id;      // read ONCE, reused for both cfg lookups below
    const cfg_building &cb  = v.cfg_buildings[bid];

    // DECLARED NEED (see the header banner): Building[bid].per_shot_cost, this turret's per-shot/
    // per-attempt time cost. turret_scan uses it UNMULTIPLIED, unlike turret_attack below.
    const double per_shot_cost = cb.per_shot_cost;

    const uint8_t sub_id = b.sub_id;                             // 0x00471b1b, already named (mh_map_object_building+0xc6)
    turret       &t      = own.turret_at(*v.cur_player, sub_id); // turrets[cur_player][sub_id]

    // 0x00471aea/0x00471af1-0x00471b13: has-pending-damage GATE, an FP comparison (see the header's
    // blanket FP-comparison note). This is a loop-PERSISTENT local, not reset per pass -- confirmed by
    // tracing every read/write of [EBP-0x18] across the whole do-while: it is cleared at most once
    // (0x00471d82) and stays cleared for every subsequent pass of THIS call.
    bool has_pending_damage = b.pending_damage > 0.0;
    bool found_target       = false;

    // 0x00471b25-0x00471dd0: `do { ... } while (tick_budget > 0.0 && !found_target)` -- bottom-tested,
    // always runs at least once (the loop-exit FCOMP sits at 0x00471dbf, after the body).
    do {
        // 0x00471b31/0x00471d9a-0x00471db5: insufficient-budget arm -- bank the whole remainder into
        // last_tick_time and zero tick_budget, same shape every sibling in this family uses.
        if (own.tick_budget() < per_shot_cost) {
            b.last_tick_time -= own.tick_budget();
            own.tick_budget() = 0.0;
        } else {
            // ---- idle-scan sweep bookkeeping (0x00471b37-0x00471bf9) ----
            if (t.aim_step_dir == 1) {
                t.aim_heading++;
                if (t.aim_heading == 0x19) t.aim_heading = 1;
            } else {
                t.aim_heading--;
                if (t.aim_heading == 0) t.aim_heading = 0x18;
            }

            // 0x00471bf9-0x00471c25: UNCONDITIONAL decrement, then the mod-5 throttle / pending-damage
            // override (0x00471c25-0x00471c49).
            t.acquire_retry_counter--;
            if (t.acquire_retry_counter % 5 == 0 || has_pending_damage) {
                uint32_t       target_ref = 0, target_slot = 0;
                const uint32_t found = c.turret_acquire_target(
                    *v.cur_player, static_cast<int32_t>(*v.cur_index), &target_ref, &target_slot);
                if (found == 0) {
                    // 0x00471cc9-0x00471d80: acquire miss.
                    if (t.acquire_retry_counter == 0) {
                        // Full timeout: flip sweep direction, re-seed both scratch counters from the
                        // mod-40-plus-30 scalar (0x00471cfc-0x00471d80).
                        t.aim_step_dir          = -t.aim_step_dir;
                        t.acquire_retry_seed    = (t.acquire_retry_seed % 40) + 30;
                        t.acquire_retry_counter = t.acquire_retry_seed;
                    } else {
                        // 0x00471d82: clears has_pending_damage -- persists into later passes, see
                        // the comment on its declaration above.
                        has_pending_damage = false;
                    }
                } else {
                    // 0x00471c66-0x00471cbd: acquire hit.
                    t.counter_ref         = static_cast<uint16_t>(target_ref);
                    t.counter_target_slot = static_cast<int32_t>(target_slot);
                    b.state               = BLDG_STATE_TURRET_ATTACK;
                    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
                    found_target = true;
                }
            }
            own.tick_budget() -= per_shot_cost;
        }
    } while (own.tick_budget() > 0.0 && !found_target);

    // 0x00471dd6-0x00471e49: shared animation-frame tail (see the header derivation) -- IDENTICAL
    // computation to turret_attack's own tail below.
    //
    // THE WRITE INDEX IS sprite_quantity - 1 AND THE READ INDEX IS sprite_quantity. The asymmetry is
    // real, it is the whole content of this line, and it has now been got wrong in both directions:
    //
    //   0x00471e30: MOVZX EAX,byte ptr [EAX + 0xd9ee03]   load sprite_quantity
    //   0x00471e42: DEC EBX                              the VALUE's -1 (aim_heading - 1)
    //   0x00471e43: MOV dword ptr [EAX + 0x8f],EBX        the STORE
    //
    // `anim` is at +0x93 in mh_map_object_building, and the store's displacement is 0x8f == 0x93 - 4.
    // So BOTH decrements are present: `DEC EBX` is the value's, and the address's is FOLDED INTO THE
    // DISPLACEMENT. A 2026-08-22 reimpl-verify pass changed this line from `- 1` to plain
    // sprite_quantity on the reasoning that "the only DEC decrements the value, never the address" --
    // true about the DEC and false about the address, because an index offset folded into a
    // displacement leaves no arithmetic instruction to find. Do not re-derive this from the
    // instruction mnemonics alone; check the displacement against the field offset.
    //
    // CAUGHT BY RUNTIME, not by review (SIM-DEEP-DIV): the 30k A/B diverged at step 3215 in
    // `buildings`, and the raw region dump named 3 bytes in ONE turret's anim[] -- original
    // [590,0,...], ours [589,590,...]. Same value, one slot too high, slot 0 left stale.
    //
    // sprite_quantity is >= 1 for every building that reaches this tail (the observed turret has 1).
    // Were it 0 the original would store at +0x8f, i.e. into anim_dur[11]'s last 4 bytes; this
    // expression reproduces that address rather than guarding it, which is the faithful choice.
    set_frame_at(b.anim, cb.sprite_quantity - 1,
                 frame_at(cb.anim, cb.sprite_quantity) + t.aim_heading - 1);
}

void bldg_state_turret_attack(const sim_view &v, sim_store &own, const bldg_state_turret_attack_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // 0x00471e72-0x00471e86: the efficiency==0.0 BIT-PATTERN short-circuit -- see the header's
    // derivation on why a plain `== 0.0` reproduces this exactly rather than "upgrading" it.
    if (b.efficiency == 0.0) {
        own.tick_budget() = 0.0;
        return;
    }

    const uint16_t      bid = b.building_id;
    const cfg_building &cb  = v.cfg_buildings[bid];
    // Building[bid].per_shot_cost * TURRET_ATTACK_INTERVAL_SCALE (RESOLVED, see the header banner) --
    // turret_attack's per-shot cost carries a multiplier turret_scan's own copy does not.
    const double per_shot_cost = cb.per_shot_cost * TURRET_ATTACK_INTERVAL_SCALE;

    const uint8_t sub_id = b.sub_id;
    turret       &t      = own.turret_at(*v.cur_player, sub_id);

    // 0x00471edd: iStack_4c's binding -- read ONCE before the tracking loop, written back only once
    // after it (matching the asm's own local-copy-then-writeback shape, unlike turret_scan's direct
    // per-pass memory mutation above -- both are behaviourally equivalent here since nothing else
    // touches this field mid-function; kept faithful to which shape each function's asm actually uses).
    int32_t aim_heading = t.aim_heading;

    // 0x00471ee6-0x00471efa: this building's own fine coordinates.
    int32_t own_fine_x = 0, own_fine_y = 0;
    c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &own_fine_x, &own_fine_y);

    // 0x00471eff-0x00471f83: the counter-target reference, read ONCE.
    const uint16_t target_owner   = t.counter_ref & 0xf;
    const bool     target_is_unit = (t.counter_ref & 0x80) != 0; // see the header's bit-test derivation
    const int32_t  target_slot    = t.counter_target_slot;
    uint8_t        fire_kind      = 1; // uStack_2c default; ==2 only on the UNIT/elevated-target arm

    int32_t target_fine_x = 0, target_fine_y = 0;
    int32_t target_elevation = 0;

    if (!target_is_unit) {
        // ---- BUILDING counter-target (0x00472121-0x0047211c) ----
        const building &target_b = building_of(v, target_owner, target_slot);
        if (target_b.building_id == 0 || target_b.energy <= 0.0) {
            // Target lost -- notify_ui uses cur_player/cur_index, NOT the target's own owner/slot.
            b.state = BLDG_STATE_TURRET_SCAN;
            c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
            t.acquire_retry_counter = 1;
            return;
        }
        c.bldg_get_coords(target_owner, target_slot, &target_fine_x, &target_fine_y);
        target_elevation = 0;
    } else {
        // ---- UNIT counter-target (0x00471f9a-0x004720f6) ----
        const unit &target_u = unit_of(v, target_owner, target_slot);
        if (target_u.unit_proto_id == 0 || target_u.energy <= 0.0) {
            b.state = BLDG_STATE_TURRET_SCAN;
            c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
            t.acquire_retry_counter = 1;
            return;
        }
        c.unit_get_coords(target_owner, target_slot, &target_fine_x, &target_fine_y);

        const uint32_t out_of_range = c.dist_out_of_range(
            0, t.attack_range + 1, fine_to_tile(own_fine_x), fine_to_tile(own_fine_y),
            fine_to_tile(target_fine_x), fine_to_tile(target_fine_y));
        if (out_of_range != 0) {
            b.state = BLDG_STATE_TURRET_SCAN;
            c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
            t.acquire_retry_counter = 1;
            return;
        }
        target_elevation = target_u.elevation;
        if (target_elevation != 0) fire_kind = 2;
    }

    // 0x004721bc-0x004721cd: bearing from this building to the (now-resolved) target, computed ONCE.
    const int32_t target_bearing =
        c.dir_from_to(own_fine_x, own_fine_y, target_fine_x, target_fine_y);

    int32_t turn_dir = 1; // uStack_44 default; kept even if the loop below runs zero passes

    // 0x004721d7-0x004722e9: `while (tick_budget > 0.0) { ... }` -- top-tested, may run ZERO passes
    // (unlike turret_scan's do-while above).
    while (own.tick_budget() > 0.0) {
        if (own.tick_budget() < per_shot_cost) {
            b.last_tick_time -= own.tick_budget();
            own.tick_budget() = 0.0;
        } else {
            own.tick_budget() -= per_shot_cost;
            if (aim_heading != target_bearing) {
                // 0x00472209-0x00472269: the turn-direction decision. Verified via full truth-table
                // cross-check against the raw CMP/JLE/JG/JL chain to reduce to exactly this combined
                // condition -- see the header's derivation (the Ghidra .c draft's `0xd`/`-0xb`
                // thresholds are the strict-`<` equivalents of these `<=0xc`/`<=-0xc` ones, not a
                // discrepancy).
                const int32_t diff = aim_heading - target_bearing;
                if ((diff <= 12 || aim_heading <= target_bearing) &&
                    (diff <= -12 || target_bearing <= aim_heading)) {
                    turn_dir = -1;
                    aim_heading--;
                    if (aim_heading < 1) aim_heading += 24;
                } else {
                    turn_dir = 1;
                    aim_heading++;
                    if (aim_heading > 24) aim_heading -= 24;
                }
            }
        }
        // 0x00472290-0x004722e9: the "close enough" tolerance break, evaluated every pass regardless
        // of which arm above ran (using that pass's -- possibly unchanged -- aim_heading).
        const int32_t d1 = aim_heading - target_bearing;
        const int32_t d2 = target_bearing - aim_heading;
        if ((aim_heading <= target_bearing || d1 < 3 || d1 > 0x15) &&
            (target_bearing <= aim_heading || d2 < 3 || d2 > 0x15))
            break;
    }

    // 0x0047230a-0x00472331: stored UNCONDITIONALLY, even when the loop above ran zero passes.
    t.aim_step_dir = turn_dir;
    t.aim_heading  = aim_heading;

    // 0x00472337-0x0047238e: shared animation-frame tail -- IDENTICAL computation to turret_scan's own
    // tail above, including the sprite_quantity-1 WRITE / sprite_quantity READ asymmetry; the long
    // derivation lives there. This copy's store is 0x0047238e's `MOV dword ptr [EAX + 0x8f],EBX`, the
    // same 0x93-4 displacement, and the DEC EBX @0x0047238d is likewise the VALUE's -1.
    //
    // The 2026-08-22 reimpl-verify pass "independently re-traced" this second copy and reached the
    // same wrong answer as it did for the first. Two independent derivations agreeing is what made it
    // convincing -- and both were reading for a DEC on the address register, so agreement only meant
    // the same blind spot twice.
    set_frame_at(b.anim, cb.sprite_quantity - 1,
                 frame_at(cb.anim, cb.sprite_quantity) + aim_heading - 1);

    // 0x00472394-0x0047240b: the final fire gate.
    if (own.tick_budget() > 0.0) {
        if (t.reload_ready_flag == 0) {
            own.tick_budget() = 0.0;
        } else {
            uint32_t last_tick_time_lo = 0, last_tick_time_hi = 0;
            split_double(b.last_tick_time, last_tick_time_lo, last_tick_time_hi);
            c.turret_fire(*v.cur_player, static_cast<uint32_t>(*v.cur_index), target_fine_x,
                          target_fine_y, target_elevation, last_tick_time_lo, last_tick_time_hi,
                          fire_kind);
        }
    }
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void bldg_state_turret_scan() {
    sim_state st = state();
    detail::bldg_state_turret_scan(st.read, st.own, live_bldg_state_turret_scan_calls());
}

void bldg_state_turret_attack() {
    sim_state st = state();
    detail::bldg_state_turret_attack(st.read, st.own, live_bldg_state_turret_attack_calls());
}


} // namespace mh::sim
