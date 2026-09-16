#include "sim/sim_unit_update_soldiers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const unit_update_soldiers_calls &live_unit_update_soldiers_calls() {
    static const unit_update_soldiers_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
    };
    return c;
}

namespace {

// llm_strat_unit_state member this function tests. Value from sim_order_enqueue.h's own "Ghidra enum
// dump 2026-08-08, get-data-type-by-string llm_strat_unit_state" (STOP_TO_DEFAULT=0x01) -- duplicated
// locally rather than shared by #include, matching sim_unit_idle_state.cpp's and sim_order_enqueue.h's
// own precedent of NOT sharing these constants across sibling sim/ files.
inline constexpr uint16_t UNIT_STATE_STOP_TO_DEFAULT = 0x01;

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// two call sites (0x0047e883, 0x0047e90b) are ORDINARY calls to it, not a compiler-inlined copy of its
// body the way ai_group_muster_pick.cpp's weapon_power_add_and_trunc shows -- same situation
// ai_opponent_relations.cpp's trunc_float_to_int32 documents, reproduced here as the identical
// instruction sequence rather than reached through mh::call or substituted with std::trunc.
//
// Takes the (end-start) delta and the elapsed/duration fraction, forms the product in x87 exactly as
// the two call sites do (FILD dword / FMUL qword), truncates toward zero via the same temporary
// control-word swap utils_math_trunc's own body performs, and stores with a 32-bit FISTP -- NOT the
// 64-bit FISTP the sibling precedents (ai_mine_yield.cpp / ai_group_muster_pick.cpp /
// ai_opponent_relations.cpp) use: read directly off the opcode byte at 0x0047e888/0x0047e910
// (`db5dcc`/`db5dcc` -> 0xDB ModRM reg=3 == FISTP m32int, not the 0xDF/7 FISTP m64int those three call
// sites use), so `r` here is int32_t, not int64_t.
//
// The caller then narrows to the LOW BYTE and sign-extends it back (0x0047e88b-0x0047e894 /
// 0x0047e913-0x0047e91c: MOV AL,byte / MOV byte,AL / MOV AL,byte / MOVSX EBX,AL, identically at both
// call sites) before adding it to a start_x/start_y int8_t field -- folded into this helper's return
// rather than repeated at both call sites, since it is byte-for-byte identical both times.
int32_t trunc_axis_delta(int32_t delta, double frac) {
    return ::mh::fp::trunc_i32_mul(delta, frac);
}

} // namespace

namespace detail {

void unit_update_soldiers(const sim_view &v, sim_store &own, const unit_update_soldiers_calls &c) {
    // 0x0047e63d-0x0047e645 (FLDZ/FCOMP energy/FNSTSW/SAHF/JNC): the WHOLE function is skipped, with
    // NO RNG draw, on an ORDERED energy<=0.0. Unordered (NaN energy) does NOT skip -- CF=1 either way
    // (ST0<src ordered, OR unordered) -- same FCOMP/JNC idiom sim_unit_passive_engage.cpp's header
    // derives for the identical shape; `!(energy <= 0.0)` reproduces it exactly (IEEE `<=` is false on
    // NaN, so the negation is true, matching CF=1's "don't skip").
    if (!(v.cur_unit->energy <= 0.0)) {
        const uint32_t player = (uint32_t)*v.cur_player;

        // 0x0047e64b-0x0047e653: the walk cursor seeds from the squad HEAD, a plain little-endian word
        // read of unit_above (offsets 0/1 reassembled -- same field sim_unit_passive_engage.cpp's
        // roster walk reassembles for a COUNT on a different record; here it is a SOLDIER SLOT index).
        uint32_t soldier_idx =
            uint32_t(v.cur_unit->unit_above[0]) | (uint32_t(v.cur_unit->unit_above[1]) << 8);

        // 0x0047e656-0x0047e660: drawn UNCONDITIONALLY, every call that passes the energy guard, no
        // matter what `state` turns out to be below.
        const int32_t roll_100 = c.rand_below(100);

        // 0x0047e663-0x0047e6ae: with (state==STOP_TO_DEFAULT) && (roll_100<1) [floor(roll_100==0), a
        // ~1% chance] -- pick ONE random soldier SLOT (0-based walk position) and flag the HEAD soldier
        // idle_wander_flag=1 (the head, NOT the picked slot -- soldier_idx has not moved yet here).
        bool    picked     = false;
        int32_t picked_idx = 0; // only meaningful when picked == true
        if (v.cur_unit->state == UNIT_STATE_STOP_TO_DEFAULT && roll_100 < 1) {
            picked_idx                                           = c.rand_below(v.cfg_units[v.cur_unit->unit_proto_id].soldier_count);
            picked                                               = true;
            own.soldier_at(player, soldier_idx).idle_wander_flag = 1;
        }

        // 0x0047e6b5-0x0047ea52: the linked-list walk, head = soldier_idx (as seeded above, possibly
        // 0), chained via next_soldier, terminated by next_soldier == 0. UNCONDITIONAL do-while: a
        // squad with unit_above==0 (no soldiers) still runs this body ONCE against
        // _G_LLM_STRAT_SOLDIERS[player][0] -- docs/structs.md's "used-slot count" metadata slot for
        // record 0 -- because there is no "if (soldier_idx != 0) skip the whole loop" guard in the
        // assembly; reproduced as literal control flow, not guarded away (see the .h banner).
        int32_t visit_index = 0; // 0-based count of soldiers visited so far, vs. picked_idx
        for (;;) {
            soldier &s = own.soldier_at(player, soldier_idx);

            // 0x0047e6c8-0x0047e6db: TEST/JNZ on the upper 31 bits (sign masked off) then CMP/JZ on
            // the lower 32 -- an integer bit test for "walk_duration's pattern is +0.0 or -0.0", which
            // is bit-exactly what IEEE `== 0.0` means (it also treats -0.0 == +0.0 as true, and is
            // false for every nonzero pattern including NaN) -- so the plain double comparison below
            // reproduces the two-instruction bit test exactly, not an approximation of it.
            if (s.walk_duration == 0.0) {
                // 0x0047e95b-0x0047ea25: only the ONE soldier at the picked slot animates; every other
                // idle soldier in the walk does nothing this tick (both the sprite update AND the
                // anim_change_count bump are skipped for them).
                if (picked && picked_idx == visit_index) {
                    // 0x0047e981-0x0047e99e: sprite_frame += (rand_below(3)-1)*3, one of {-3,0,3}. The
                    // add happens at full width in EAX/EBX; only the low byte is stored back
                    // (0x0047e99e MOV byte, not the full register) -- `int8_t(...)` reproduces that
                    // truncation.
                    const int32_t nudge = c.rand_below(3) - 1;
                    s.sprite_frame      = int8_t((int32_t)s.sprite_frame + nudge * 3);
                    // 0x0047e9a4-0x0047ea0b: clamp/wrap into [1, 0x15] -- mutually exclusive (the
                    // "<1" branch JMPs straight past the ">0x15" check at 0x0047e9da).
                    if (s.sprite_frame < 1)
                        s.sprite_frame = 0x15;
                    else if (s.sprite_frame > 0x15)
                        s.sprite_frame = 1;
                    // 0x0047ea12-0x0047ea25: unconditional once this branch is taken at all, whether
                    // or not either clamp fired.
                    ++s.anim_change_count;
                }
            } else {
                // 0x0047e6e1-0x0047e700: advance elapsed by the tick budget, unconditionally.
                s.walk_elapsed = *v.tick_budget + s.walk_elapsed;

                // 0x0047e72c-0x0047e73b (FLD elapsed / FCOMP duration / FNSTSW / SAHF / JC): finalize
                // iff elapsed>=duration ORDERED. Unlike the energy guard above, this needs NO negation
                // trick: IEEE `>=` is already false whenever either operand is NaN, so a plain
                // `elapsed >= duration` already means "ordered AND elapsed>=duration" -- exactly CF=0's
                // condition (JC, taken on ST0<src-or-unordered, goes to the INTERPOLATE arm instead;
                // see the derivation in the header banner and the plate/history note on this file).
                if (s.walk_elapsed >= s.walk_duration) {
                    // 0x0047e741-0x0047e800: start=end, cur=start (x then y), walk_duration=0.0 (two
                    // dword stores covering the whole 8-byte double, matching the bit-test discriminant
                    // above).
                    s.start_x       = s.end_x;
                    s.cur_x         = s.start_x;
                    s.start_y       = s.end_y;
                    s.cur_y         = s.start_y;
                    s.walk_duration = 0.0;
                } else {
                    // 0x0047e80f-0x0047e950: frac = elapsed/duration (computed once, reused for both
                    // axes); cur_x/cur_y = start + trunc((end-start)*frac), narrowed to a byte (see
                    // trunc_axis_delta above).
                    const double frac = s.walk_elapsed / s.walk_duration;
                    s.cur_x           = int8_t((int32_t)s.start_x +
                                               trunc_axis_delta((int32_t)s.end_x - (int32_t)s.start_x, frac));
                    s.cur_y           = int8_t((int32_t)s.start_y +
                                               trunc_axis_delta((int32_t)s.end_y - (int32_t)s.start_y, frac));
                }
            }

            // 0x0047ea2b-0x0047ea52: advance to next_soldier (MOVZX word, zero-extended); loop back
            // unless it is 0.
            ++visit_index;
            const uint16_t next = s.next_soldier;
            soldier_idx         = next;
            if (next == 0)
                break;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_update_soldiers() {
    sim_state st = state();
    detail::unit_update_soldiers(st.read, st.own, live_unit_update_soldiers_calls());
}


} // namespace mh::sim
