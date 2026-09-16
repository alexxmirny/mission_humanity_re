//
// ai/ai_attack_candidates.cpp -- see ai_attack_candidates.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_attack_candidate_add_004ec7d9.asm), not from Ghidra's C: the decompile
// re-materialises the raw (pre-square) llm_strat_unit_max_weapon_range result as a real store into
// `weapon_range_sq` before immediately overwriting it with the square -- a dead store the assembly
// makes but no caller can ever observe, so only the square is written here.
//
#include "ai/ai_attack_candidates.h"


namespace mh::ai {
namespace detail {

void attack_candidate_add(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player,
                          int32_t unit_index) {
    // First write (0x004ec7e9..0x004ec7f1): the record at the CURRENT count gets its unit_index.
    attack_candidate &rec0 = own.attack_candidates[(uint32_t)*own.attack_candidate_count];
    rec0.unit_index        = (uint16_t)unit_index;

    // unit_max_weapon_range's first argument is `player`, not a unit reference: at the call site EAX
    // still holds this function's own `player` argument (ECX, moved back into EAX right before the
    // call) and EDX still holds `unit_index` (never overwritten since it was copied to EBX earlier).
    // Same (player, unit_index) pattern ai_engage_scan.cpp's unit_scan_engage_candidates_in_range
    // uses over the same callee.
    const uint32_t range = gc.unit_max_weapon_range((uint32_t)player, unit_index);

    // The original RE-READS the count from memory here (0x004ec801) instead of reusing the register
    // it loaded before the call. Reproduced faithfully by re-indexing through the count pointer a
    // second time rather than caching `rec0`. It resolves to the identical slot: unit_max_weapon_range
    // is a pure query over the rosters/cfg tables and has no path to change this scratch's count, so
    // the two reads can never disagree -- but the reload is preserved on principle rather than assumed
    // away.
    attack_candidate &rec = own.attack_candidates[(uint32_t)*own.attack_candidate_count];
    // The raw `range` store the assembly makes into weapon_range_sq (0x004ec809) is DEAD -- the very
    // next instruction (0x004ec80f/0x004ec812) squares it and overwrites the same slot, and nothing
    // reads the slot in between. Only the square, the sole observable value, is written.
    rec.weapon_range_sq = range * range;

    // x, y: units[player][unit_index].x / .y (map::object::unit, byte fields at +0x84/+0x85),
    // zero-extended (MOVZX) into the record's uint16_t columns. Same record slot as the write above --
    // the original does not re-read the count a third time, it reuses the EAX it already has.
    const unit &u = unit_of(v, (uint32_t)player, unit_index);
    rec.x         = u.x;
    rec.y         = u.y;

    // score / dist_sq are DELIBERATELY untouched: llm_strat_ai_active_unit_tick, the sole caller,
    // initialises them itself. Writing either here would be a divergence.

    ++*own.attack_candidate_count;
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------

void attack_candidate_add(int32_t player, int32_t unit_index) {
    const ai_state st = state();
    detail::attack_candidate_add(st.read, st.own, live_calls(), player, unit_index);
}

// ---- differential-oracle arm ---------------------------------------------------------------------
//
// Runs on mh::ai::shadow_calls(). The one callee, unit_max_weapon_range, is a pure read over the
// rosters/cfg tables (ai_state.h's shadow_calls() classification lists bldg_find_by_ai_build_and_type
// and the weapon predicates in the same "pure, REAL" tier), and this function's own writes land only
// inside the attack-candidate scratch and its count -- both declared by this site. So nothing here
// escapes and the call runs for real.

} // namespace mh::ai
