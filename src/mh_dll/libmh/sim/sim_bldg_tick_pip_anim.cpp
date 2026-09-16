//
// sim/sim_bldg_tick_pip_anim.cpp -- see sim_bldg_tick_pip_anim.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_tick_pip_anim_00479244.asm), not the Ghidra .c draft.
//
#include "sim/sim_bldg_tick_pip_anim.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to (unused: no outward calls)

namespace mh::sim {
namespace detail {

void bldg_tick_pip_anim(const sim_view &v, sim_store &store, uint16_t player, int32_t b_index) {
    // The utils_assert_stack_capacity(0x34) prologue call (0x0047924c) is the inert stack-capacity
    // probe (translator brief rule 6) -- omitted.

    building &b = store.building_at(player, b_index);

    // Building[b.building_id].pip_slot_count -- the per-building-TYPE cfg pip count, read as a raw
    // int32 at 0x0047928d (`CMP EDX, dword ptr [building_id*sizeof(cfg_building) + 0xd9ee24]`).
    const cfg_building &cb             = v.cfg_buildings[b.building_id];
    const int32_t       pip_slot_count = cb.pip_slot_count;

    // The outer for-loop's bound check (0x00479269-0x00479295) exits the WHOLE function (JMP to the
    // epilogue at 0x0047948a) once i >= pip_slot_count, matching a plain for-loop with the test at the
    // top. `i` is a byte counter in the asm (INC byte ptr, MOVZX-compared as int) -- kept uint8_t here
    // for the same zero-extending comparison the original performs.
    //
    // UNCERTAINTY: pip_level[4]/pip_frame[4]/pip_timer[4] on mh_map_object_building are fixed 4-entry
    // arrays (static_assert'd contiguous: pip_frame@0xcb, pip_timer@0xdb, pip_level@0xfb), but the
    // loop bound `pip_slot_count` comes from an INDEPENDENT per-building-type cfg value with no
    // visible clamp to 4 anywhere in this function's assembly. If any real cfg Building record's
    // pip_slot_count exceeds 4, this loop indexes past the end of these fixed-size struct members --
    // formally out-of-bounds in C++ even though the underlying bytes are contiguous with the next
    // struct field (`_pad_0x10b[4]` then `incoming_damage_tally`) and the raw address arithmetic in
    // the original would land on the exact same bytes. Reproduced faithfully (no bounds clamp added,
    // per translator brief rule 14) because clamping would be inventing a check the original does not
    // have; I could not verify the actual cfg data's pip_slot_count range without reading the cfg
    // files, which this translation pass does not do.
    for (uint8_t i = 0; (int32_t)i < pip_slot_count; ++i) {
        // 0x004792be: `CMP dword ptr [... + 0xc3d39b], 0; JLE` -- skip inactive pip slots.
        if (b.pip_level[i] <= 0) continue;

        // dVar3/dVar2 in the .c draft are the SAME read (Anim[pip_frame[i]+1].time), done twice back
        // to back with no intervening write -- a decompiler redundant-load artifact, not two distinct
        // values. Confirmed by the asm: the while loop's every comparison (0x00479366) and every
        // subtraction (0x0047943f) both read the SAME cached stack slot (EBP-0x2c), never re-loading
        // Anim[...].time from memory -- i.e. this is a SNAPSHOT taken once per pip, reused across
        // however many chain-advances the inner while performs, even though pip_frame[i] itself
        // changes inside that while. One local suffices for both uses.
        const double anim_time = v.anim_frames[b.pip_frame[i] + 1].time;

        // THE OUTER LOOP RE-READS pip_timer[i] FRESH EACH PIP, BEFORE the inner while
        // (0x004792f9-0x0047934f): elapsed = GAME_CLOCK - pip_timer[i], then pip_timer[i] = GAME_CLOCK
        // immediately, both once per pip -- not once per chain-advance. The inner while below then
        // drains this single snapshot.
        double elapsed = *v.game_clock - b.pip_timer[i];
        b.pip_timer[i] = *v.game_clock;

        // 0x00479355: `FLDZ; FCOMP elapsed; JNC exit-while` -- `while (0.0 < elapsed)`.
        while (0.0 < elapsed) {
            if (elapsed <= anim_time) {
                // LAB_00479447: chain not yet due to advance this tick -- fold the remainder back into
                // the timer and stop draining.
                b.pip_timer[i] -= elapsed;
                elapsed = 0.0;
            } else {
                // pip_frame[i] is re-read live here (not cached) because a PRIOR iteration of this
                // same while loop may have just changed it (0x00479372-0x0047939e re-derives the
                // address from the current b.pip_frame[i] each time).
                const anim_frame &cur = v.anim_frames[b.pip_frame[i] + 1];
                if (cur.next == 0) {
                    // Chain end (0x004793e1: `*(int*)(pip_level*4 + 0xc3873c)`) -- restart at the
                    // level's base frame. FIXED 2026-08-13: the folded constant is 0xc3873c, which is
                    // A_OGIEN (the game's 'fire' effect anim table, cfg_t_frame_index[4] @0xc38740),
                    // not the previously-assumed pip_level_base_frame @0xc38730 (a hex-transcription
                    // slip landed 3 entries/12 bytes early on an unrelated block). pip_level is always
                    // >=1 in this branch (gated above), so A_OGIEN[pip_level-1] is the correct 0-based
                    // index -- exact fit, no OOB, unlike the old `pip_level_base_frame[pip_level]`
                    // which read past a 4-entry array whenever pip_level>=1.
                    b.pip_frame[i] = v.pip_fire_anim_frames[b.pip_level[i] - 1];
                } else {
                    // Advance one link (0x00479436: `pip_frame[i] += Anim[pip_frame[i]+1].next`).
                    b.pip_frame[i] += cur.next;
                }
                // LAB_0047943c: `elapsed -= anim_time` -- always the ORIGINAL per-pip snapshot, common
                // to both the reset and advance sub-branches.
                elapsed -= anim_time;
            }
        }
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_tick_pip_anim(uint16_t player, int32_t b_index) {
    sim_state s = state();
    detail::bldg_tick_pip_anim(s.read, s.own, player, b_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
