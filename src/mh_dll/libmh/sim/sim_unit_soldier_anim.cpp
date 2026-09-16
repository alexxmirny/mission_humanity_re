//
// sim/sim_unit_soldier_anim.cpp -- see sim_unit_soldier_anim.h. Translated from the DISASSEMBLY:
//   tmp/decomp/llm_strat_unit_soldiers_start_walk_anim_004897c7.asm
//   tmp/decomp/llm_strat_unit_squad_pick_lead_soldier_in_direction_0048905e.asm
// per the translator brief -- the .c drafts in tmp/decomp/ are cited only as corroboration, every field
// access and branch below was re-walked against its own displacement/opcode.
//
#include "sim/sim_unit_soldier_anim.h"

#include <cstdlib> // std::abs -- pure integer, not an FP CRT call

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call out to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const unit_soldier_anim_calls &live_unit_soldier_anim_calls() {
    static const unit_soldier_anim_calls c = {
        MH_LIBMH_BIND(llm_strat_facing24_from_points),
        MH_PROMOTED_ROW(llm_strat_facing_step_apply) /* renamed from llm_ui_cursor_apply_anim_frame_offset 2026-09-02 */,
        MH_LIBMH_BIND(llm_math_manhattan_dist),
    };
    return c;
}

namespace {

// _DAT_00501492 (bytes 00 00 00 00 00 00 00 40 LE) == 2.0 (double). Read-only image constant, not in
// the state view per the conductor's batch context -- spelled here as a constexpr rather than added as
// a sim_view member.
inline constexpr double WALK_DURATION_STEP_SCALE = 2.0; // VA 0x00501492

} // namespace

namespace detail {

// ---- llm_strat_unit_soldiers_start_walk_anim @0x004897c7 --------------------------------------
void unit_soldiers_start_walk_anim(const sim_view &v, sim_store &own, const unit_soldier_anim_calls &c,
                                   uint32_t player, int32_t unit_index) {
    // 0x004897eb/every later "MOVZX EAX,word ptr [EBP-0x24]" re-read: only the LOW 16 BITS of the
    // stored 32-bit `player` parameter are ever read for indexing, zero-extended -- i.e. player&0xffff,
    // applied at every one of this function's roster/soldier/cfg reads, same masking convention
    // sim_unit_update_soldiers.cpp documents for the sibling chain.
    const uint32_t player_m = (uint32_t)(uint16_t)player;

    // 0x004897de-0x00489805: seed the walk cursor from the squad HEAD (unit.unit_above), read as the
    // SAME little-endian 2-byte reassembly sim_unit_update_soldiers.cpp's cur_unit->unit_above
    // derivation performs for the identical field (mh_map_object_unit::unit_above is uint8_t[2], not a
    // ushort, per addr/mh_structs.gen.h) -- not a decompiler artifact, a real byte-pair read.
    const unit &u = unit_of(v, player_m, unit_index);
    uint32_t    soldier_idx =
        uint32_t(u.unit_above[0]) | (uint32_t(u.unit_above[1]) << 8);

    // 0x00489808-0x004899de: DO-WHILE, head = soldier_idx (as seeded above, possibly 0), chained via
    // next_soldier, terminated by next_soldier==0. A squad with unit_above==0 (no soldiers recorded)
    // still runs this body once against _G_LLM_STRAT_SOLDIERS[player][0] -- docs/structs.md's
    // "used-slot count" metadata slot for record 0 -- because there is no "if soldier_idx!=0" guard in
    // the assembly; reproduced as literal control flow, matching the sibling function's own note on the
    // same shape.
    do {
        soldier &s = own.soldier_at(player_m, soldier_idx);

        // 0x00489828-0x00489887: skip this soldier's whole body unless BOTH:
        //   (a) (end_x,end_y) != (start_x,start_y) -- the two-CMP block at LAB_00489808/LAB_00489864
        //       is literally "if (end_x==start_x && end_y==start_y) skip", so entry is the negation:
        //       end_x!=start_x || end_y!=start_y (matches the .c's rendering exactly);
        //   (b) walk_duration's bit pattern is +0.0/-0.0 -- 0x00489874 TEST dword[+0x17],0x7fffffff /
        //       JNZ skip, then 0x00489880 CMP dword[+0x13],0 / JZ enter. THIS IS A RAW INTEGER TEST OF
        //       THE TWO 32-BIT HALVES, NOT AN X87 COMPARE -- there is no FLD/FCOMP/FNSTSW anywhere in
        //       this range of the .asm, unlike the energy-guard idiom sim_unit_update_soldiers.cpp
        //       documents for a DIFFERENT function. The mask (high&0x7fffffff)==0 accepts exactly the
        //       two zero bit patterns (sign bit either way, exponent+mantissa all zero); combined with
        //       low==0 that is bit-for-bit "the double's magnitude is exactly zero" -- which is exactly
        //       what IEEE `== 0.0` means (never true for a denormal, Inf, or NaN, since all of those
        //       leave some exponent/mantissa bit set). So `s.walk_duration == 0.0` reproduces this
        //       two-instruction integer test exactly; it is NOT an approximation and NOT a place where
        //       an x87 compare could have been in play, so it is not carried to uncertainties[].
        if ((s.end_x != s.start_x || s.end_y != s.start_y) && s.walk_duration == 0.0) {
            // 0x0048988e-0x004898ef: from_x/from_y/to_x/to_y loaded fresh from start_x/start_y/end_x/
            // end_y (int8_t fields, per addr/mh_structs.gen.h -- already correctly typed, no cast
            // needed here beyond what the field type already gives).
            const int8_t from_x = s.start_x;
            const int8_t from_y = s.start_y;
            const int8_t to_x   = s.end_x;
            const int8_t to_y   = s.end_y;

            // 0x004898f2-0x00489919: llm_strat_facing24_from_points(from_x,from_y,to_x,to_y) -- EAX=
            // from_x, EDX=from_y, EBX=to_x, ECX=to_y at the call site, matching the callee's committed
            // __watcall(from_x,from_y,to_x,to_y) parameter order in mh_calls.gen.h. Result -> sprite_frame.
            s.sprite_frame = (int8_t)c.facing24_from_points(from_x, from_y, to_x, to_y);

            // 0x0048992f/0x00489939: two dword stores of 0 covering walk_elapsed's whole 8 bytes (offset
            // +0x0b/+0x0f). walk_elapsed is a properly typed `double` field (addr/mh_structs.gen.h), so
            // a single typed store reproduces both dword clears bit-exactly -- chosen over two manual
            // 4-byte stores because the field's real type makes that available, unlike the era this
            // draft's `*(undefined4*)&walk_elapsed = 0` split was written against.
            s.walk_elapsed = 0.0;

            // 0x0048996c-0x00489990: dx=|to_x-from_x|, dy=|to_y-from_y| via SUB/CDQ/XOR/SUB (the
            // classic branch-free abs idiom) summed into one int -- std::abs on the int32-widened
            // (sign-extended, per the MOVSX loads above) deltas is bit-identical and is pure integer
            // arithmetic, not an FP CRT call.
            const int32_t dx   = std::abs((int32_t)to_x - (int32_t)from_x);
            const int32_t dy   = std::abs((int32_t)to_y - (int32_t)from_y);
            const int32_t dist = dx + dy;

            // 0x00489943-0x00489963: cfg_units[u.unit_proto_id].step_speed[player_m] -- EBX is built as
            // unit_proto_id*sizeof(cfg_unit) + player_m*8 (0x00489967 SHL EAX,3, i.e. *8, the stride of
            // ONE double), i.e. step_speed IS indexed BY PLAYER, one double per player slot -- confirmed
            // from the address arithmetic itself, not a decompiler artifact reading a flat array as if
            // it had a player dimension. cfg_unit::step_speed is double[9] (addr/mh_structs.gen.h),
            // unchecked here exactly as the assembly is (player_m always <8 at every real call site).
            const double step_speed = v.cfg_units[u.unit_proto_id].step_speed[player_m];

            // 0x00489995-0x004899a4: FILD dword[dist] ; FMUL double[step_speed] ; FMUL double[2.0] ;
            // FSTP double[walk_duration] -- x87 left-to-right, (dist*step_speed)*2.0, NOT
            // dist*(step_speed*2.0). C++'s `*` is left-associative, so writing the product in this exact
            // left-to-right order reproduces the x87 multiply sequence term-for-term under
            // /arch:IA32 /fp:precise (no trunc/FRNDINT anywhere in this function -- the FSTP stores the
            // full double product directly, unlike the interpolation helper in the sibling file).
            s.walk_duration = (double)dist * step_speed * WALK_DURATION_STEP_SCALE;
        }

        // 0x004899c0-0x004899d7: reload next_soldier fresh (MOVZX word, zero-extended) and loop while
        // nonzero (0x004899de JNZ back to LAB_00489808).
        soldier_idx = s.next_soldier;
    } while (soldier_idx != 0);
}

// ---- llm_strat_unit_squad_pick_lead_soldier_in_direction @0x0048905e --------------------------
uint32_t squad_pick_lead_soldier_in_direction(const sim_view &v, int32_t player, uint32_t head_soldier_idx,
                                              uint32_t heading, const unit_soldier_anim_calls &c) {
    // 0x0048907d/0x00489081: BOTH out-params are pre-seeded to 0x10 before the call -- not dead
    // initialisation, the callee (llm_ui_cursor_apply_anim_frame_offset) reads its own out-params'
    // incoming bytes for some headings (per the conductor's batch context: reproduce the pre-seed).
    // int8_t, NOT char (reimpl-verify note, 2026-08-10). The original reloads both with MOVSX
    // (0x00489093/0x00489097, again at 0x00489105/0x00489109), i.e. SIGNED. Plain `char` happens to
    // be signed on this build -- no /J or -funsigned-char anywhere in the tree -- so the two spell
    // the same code today; but that is a toolchain-flag dependency for a sign-extension the assembly
    // states outright, and the struct's own cur_x/cur_y are already int8_t in mh_structs.gen.h.
    // The callee's out-params are `char *`, hence the casts at the call.
    int8_t out_x = 0x10;
    int8_t out_y = 0x10;
    c.cursor_apply_anim_frame_offset((char *)&out_x, (char *)&out_y, (int32_t)heading);

    // player is used as a full 32-bit row multiplier here (0x0048909b IMUL EDX,[player],0xb54 reads the
    // whole stored dword, unlike start_walk_anim's MOVZX-word masking above) -- no player&0xffff mask
    // in this function's assembly, so none is applied here.
    const uint32_t row = (uint32_t)player * v.caps.soldiers;

    // 0x0048909b-0x004890c8: score the HEAD soldier first, unconditionally, before the loop even checks
    // its next_soldier.
    const soldier &head      = v.soldiers[row + head_soldier_idx];
    int32_t        best_dist = c.manhattan_dist((int32_t)head.cur_x, (int32_t)head.cur_y, (int32_t)out_x,
                                                (int32_t)out_y);
    uint32_t       current   = head_soldier_idx;
    uint32_t       best_idx  = head_soldier_idx;

    // 0x004890d7-0x00489151: a plain WHILE, not the sibling functions' do-while -- it tests the CURRENT
    // soldier's next_soldier BEFORE advancing (0x004890e4 CMP word[+0x02],0 / JZ exit), so a
    // single-soldier squad (head.next_soldier==0) never enters the loop body at all and the head's own
    // score (computed above) stands unchallenged.
    while (v.soldiers[row + current].next_soldier != 0) {
        current             = v.soldiers[row + current].next_soldier;
        const soldier &s    = v.soldiers[row + current];
        const int32_t  dist = c.manhattan_dist((int32_t)s.cur_x, (int32_t)s.cur_y, (int32_t)out_x,
                                               (int32_t)out_y);
        // 0x00489140-0x00489143: CMP EAX(dist),[best_dist] ; JLE skip -- update ONLY on a STRICTLY
        // greater distance (`best_dist < dist`), so the first soldier reaching a given maximum (the
        // head, on a tie or an all-idle tail) keeps the lead. Verified off the JLE polarity directly,
        // not inferred from the .c.
        if (best_dist < dist) {
            best_dist = dist;
            best_idx  = current;
        }
    }

    return best_idx;
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

void unit_soldiers_start_walk_anim(uint32_t player, int32_t unit_index) {
    sim_state st = state();
    detail::unit_soldiers_start_walk_anim(st.read, st.own, live_unit_soldier_anim_calls(), player,
                                          unit_index);
}

uint32_t squad_pick_lead_soldier_in_direction(int32_t player, uint32_t head_soldier_idx, uint32_t heading) {
    const sim_view v = state().read;
    return detail::squad_pick_lead_soldier_in_direction(v, player, head_soldier_idx, heading,
                                                        live_unit_soldier_anim_calls());
}


} // namespace mh::sim
