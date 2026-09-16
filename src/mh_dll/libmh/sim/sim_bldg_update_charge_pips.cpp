//
// sim/sim_bldg_update_charge_pips.cpp -- see sim_bldg_update_charge_pips.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_update_charge_pips_00478eb4.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_bldg_update_charge_pips.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::sim {

const bldg_update_charge_pips_calls &live_bldg_update_charge_pips_calls() {
    static const bldg_update_charge_pips_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
    };
    return c;
}

namespace {

// DAT_00501382 -- read-only image `double` constant at 0x00501382, the sole multiplier on this
// function's target-pip-count numerator (`pip_slot_count * DAT_00501382 * (energy_max - energy_cur)`).
//
// RESOLVED BY THE CONDUCTOR (2026-08-13): raw bytes at 0x00501382 read via ReVA read-memory are
// `00 00 00 00 00 00 10 40` (little-endian) -- IEEE754 double 4.0 exactly. Matches
// sim_unit_refund.cpp's `ENERGY_RATIO_SCALE` precedent for how a resolved image constant is recorded.
inline constexpr double PIP_TARGET_SCALE = 4.0; // DAT_00501382

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x00478f64) is an ORDINARY call to it, not a compiler-inlined copy of its body -- same
// situation sim_unit_refund.cpp's refund_amount() documents, reproduced here as the identical
// instruction sequence rather than reached through mh::call or substituted with std::trunc.
//
// Forms the WHOLE numerator/denominator chain ENTIRELY IN THE X87 REGISTER, matching the asm's own
// FLD/FSUB/FILD/FMUL/FMULP/FDIV sequence exactly (0x00478f04-0x00478f5e) rather than translating each
// step to a separate C++ `double` statement: the x87 stack keeps the intermediate product as an 80-bit
// float10 across the whole chain, and only the FINAL FDIV result is ever forced down to 64 bits (by the
// FISTP's own conversion, after truncation). Sequentially-rounded C++ `double` arithmetic would not
// reproduce this. `energy_max` is read from memory TWICE in the original (0x00478f04 and again at
// 0x00478f5e, both `cfg_buildings[bid].energy`) -- passed here as a single C++ parameter instead of two
// separate reads, which is behaviourally identical because `cfg_buildings` has no writer anywhere in
// the sim closure (sim_view's own comment: boot-loaded, frozen).
int32_t target_pip_count(double energy_max, double energy_cur, int32_t pip_slot_count, double scale) {
    return ::mh::fp::target_pip_count(energy_max, energy_cur, pip_slot_count, scale);
}

} // namespace

namespace detail {

void bldg_update_charge_pips(const sim_view &v, sim_store &own, const bldg_update_charge_pips_calls &c,
                             uint16_t player, uint32_t building_id) {
    // 0x00478ecb-0x00478ee4: `buildings[player][building_id]` -- ordinary building_at() indexing
    // (BUILDINGS_PER_PLAYER*sizeof(building) == 0x6aa4 / sizeof(building) == 0x111). This function
    // both reads and writes this record throughout (pip_active_count/pip_level/pip_frame/pip_timer),
    // so the mutable store. NOTE: `building_id` here is the PARAMETER (the slot index), distinct from
    // `b.building_id`, the record's OWN field (the cfg TYPE id, named `bid` below) -- see the header
    // banner's addressing note.
    building &b = own.building_at(player, (int32_t)building_id);

    // `cfg_buildings[bid]` -- bid = b.building_id (0x00478ee4, MOVZX word). Read-only cfg TYPE record
    // (sizeof(cfg_building) == 0x842, IMUL constant read off the asm); no writer anywhere in the sim
    // closure, so the const view.
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // 0x00478f04-0x00478f69: target_count computed ONCE, before either while loop below -- a single
    // FISTP, not a per-iteration re-evaluation (see target_pip_count()'s own derivation and the header
    // banner). `energy` on both sides is the HP-like construction-charge stat, not the POWER resource.
    const int32_t target_count =
        target_pip_count(cb.energy, b.energy, cb.pip_slot_count, PIP_TARGET_SCALE);

    // UNCERTAINTY: `slot = llm_rand_below(cb.pip_slot_count)` (both loops below) picks in
    // [0, cb.pip_slot_count), but pip_level/pip_frame/pip_timer are FIXED 4-entry arrays on the
    // building instance (static_assert'd contiguous). The asm has NO clamp of pip_slot_count to 4
    // anywhere in this function -- same hazard sim_bldg_tick_pip_anim.h documents for its sibling's
    // outer loop bound (same cfg field, same lack of a visible clamp). If any real cfg Building record's
    // pip_slot_count exceeds 4, `slot` can land >=4 and every pip_level[slot]/pip_frame[slot]/
    // pip_timer[slot] access below is out-of-bounds. Reproduced faithfully (no bounds clamp added, per
    // translator brief rule 14 -- this codebase resets/exhausts rather than rejects, and inventing a
    // clamp here would be a behaviour change); not verified against the actual cfg data.

    // ---- grow loop (0x00478f6c-0x004790e2): light pips until pip_active_count reaches target_count --
    while (b.pip_active_count < target_count) {
        int32_t slot;
        if (cb.pip_slot_count < 2) {
            // 0x00478fae-0x0047900b: pip_slot_count <= 1 -- no point rerolling among one slot.
            slot = 0;
        } else {
            // 0x00478fb7-0x00479009: reroll while the picked slot is already at max level (>3, i.e.
            // >=4 per the asm's `CMP ...,4; JGE reroll`).
            do {
                slot = c.rand_below(cb.pip_slot_count);
            } while (b.pip_level[slot] > 3);
        }
        // 0x0047902d/0x00479046: INC pip_level[slot], INC pip_active_count.
        ++b.pip_level[slot];
        ++b.pip_active_count;
        // 0x0047905f-0x00479091: pip_frame[slot] = table[pip_level[slot]], read using the
        // POST-increment level. The raw folded table address (0xc3873c) is A_OGIEN, now bound as
        // `pip_fire_anim_frames` -- the old Ghidra .c draft's `_G_LLM_UNKNOWN_00C38730[level+3]`
        // indexes the SAME bytes 3 entries/12 bytes earlier in an unrelated block (see
        // sim_bldg_tick_pip_anim.h's identical derivation for its sibling function, fixed 2026-08-13).
        b.pip_frame[slot] = v.pip_fire_anim_frames[b.pip_level[slot] - 1];
        // 0x004790b2-0x004790dc: first-light stamp only (post-increment level == 1).
        if (b.pip_level[slot] == 1) {
            b.pip_timer[slot] = *v.game_clock;
        }
    }

    // ---- shrink loop (0x004790e7-0x00479236): darken pips until pip_active_count reaches
    // target_count. Symmetric to the grow loop, no pip_timer write on this side. ----
    while (target_count < b.pip_active_count) {
        int32_t slot;
        if (cb.pip_slot_count < 2) {
            // 0x00479129-0x00479186: pip_slot_count <= 1.
            slot = 0;
        } else {
            // 0x00479132-0x00479184: reroll while the picked slot is already inactive (level<1, i.e.
            // level==0 per the asm's `CMP ...,1; JL reroll`).
            do {
                slot = c.rand_below(cb.pip_slot_count);
            } while (b.pip_level[slot] < 1);
        }
        // 0x004791a8/0x004791c1: DEC pip_level[slot], DEC pip_active_count.
        --b.pip_level[slot];
        --b.pip_active_count;
        // 0x004791e2-0x00479230: re-stamp pip_frame[slot] from the SAME table using the POST-decrement
        // level, but ONLY IF it did not just hit 0 (`CMP ...,0; JZ skip-stamp`).
        if (b.pip_level[slot] != 0) {
            b.pip_frame[slot] = v.pip_fire_anim_frames[b.pip_level[slot] - 1];
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_update_charge_pips(uint16_t player, uint32_t building_id) {
    sim_state st = state();
    detail::bldg_update_charge_pips(st.read, st.own, live_bldg_update_charge_pips_calls(), player,
                                    building_id);
}


} // namespace mh::sim
