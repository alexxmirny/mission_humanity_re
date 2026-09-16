//
// ai/ai_engage_scan.cpp -- see ai_engage_scan.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai_a_l1/*.asm), not from Ghidra's C: the decompile of
// llm_strat_ai_unit_scan_engage_candidates_in_range truncates target_mask to a byte (the assembly
// pushes the full dword) and the decompile of llm_strat_ai_engage_sort_candidates_by_dist hides the
// uninitialised-register bug behind an `unaff_ESI` that reads as ordinary, if odd, C -- both are
// noted at the site below.
//
#include "ai/ai_engage_scan.h"


namespace mh::ai {
namespace detail {

int32_t unit_scan_engage_candidates_in_range(const ai_view &v, const ai_calls &gc, int32_t player,
                                             int32_t unit_id, uint32_t target_mask) {
    // Both range queries are called on the SAME (player, unit_id) pair the caller handed us; the
    // original loads EAX/EDX once (player/unit_id) and reuses them unchanged across both calls.
    const int32_t sight        = (int32_t)gc.unit_get_sight((uint32_t)player, unit_id);
    const int32_t weapon_range = (int32_t)gc.unit_max_weapon_range((uint32_t)player, unit_id);

    // `CMP EAX,EBX / JLE` -- SIGNED compare. weapon_range <= sight keeps sight; only a STRICTLY
    // larger weapon_range overrides it.
    const int32_t radius = (weapon_range > sight) ? weapon_range : sight;

    // unit.x is pushed into the scanner's `x` slot and unit.y into its `y` slot -- verified against
    // the push order, not against which byte offset "looks like" x: EDX (loaded from +0x84, unit.x)
    // is param 2 (x), EBX (loaded from +0x85, unit.y) is param 3 (y), matching ai_calls'
    // (player, x, y, ring_index, target_mask).
    const unit &u = unit_of(v, (uint32_t)player, unit_id);
    // `return iVar2;` in the original (0x004ed7e9) -- propagate the scan's own result verbatim.
    return gc.scan_spiral_ring_for_engage_candidates(player, u.x, u.y, radius, target_mask);
}

namespace {

// Resolves ANY packed ref (source or target) to its (x, y) using the (ref & 0xa0) == 0 -> BUILDING
// test -- the polarity ref_is_building_by_a0 names, and the one this whole function uses (both for
// the source entity and for every scratch target). NOT the 0x40 polarity the partition helper and
// the survivability pick in ai_engage.cpp use -- see the target_ref field comment in
// mh_structs.gen.h for why both live on.
void resolve_ref_xy(const ai_view &v, uint32_t ref, int32_t index, uint8_t &out_x, uint8_t &out_y) {
    const uint32_t owner = ref_owner(ref);
    if (ref_is_building_by_a0(ref)) {
        const building &b = building_of(v, owner, index);
        out_x             = b.x;
        out_y             = b.y;
    } else {
        const unit &u = unit_of(v, owner, index);
        out_x         = u.x;
        out_y         = u.y;
    }
}

} // namespace

void engage_sort_candidates_by_dist(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                    uint32_t source_ref, int32_t source_index,
                                    int32_t inherited_sorted_flag) {
    // ---- PHASE 1: distances, unconditional --------------------------------------------------
    uint8_t source_x = 0, source_y = 0;
    resolve_ref_xy(v, source_ref, source_index, source_x, source_y);

    // UNSIGNED compare against the count (`CMP EBX,[count] / JC`), as everywhere else in this
    // module.
    for (uint32_t i = 0; i < (uint32_t)*own.engage_scratch_count; ++i) {
        engage_candidate &c        = own.engage_scratch[i];
        uint8_t           target_x = 0, target_y = 0;
        resolve_ref_xy(v, c.target_ref, c.target_index, target_x, target_y);
        // Argument order verified from the push sequence: source_x is pushed LAST (so it is the
        // FIRST argument), then source_y, then target_x, then target_y pushed FIRST (so it is the
        // LAST argument) -- i.e. (source_x, source_y, target_x, target_y). Getting this backwards
        // is invisible on a square map, which is why it is called out here rather than assumed.
        c.dist_sq = gc.toroidal_dist_sq(source_x, source_y, target_x, target_y);
    }

    // ---- PHASE 2: the bubble sort, gated by a bug the original DOES NOT KNOW IT HAS -----------
    //
    // The original's "swapped" flag lives in ESI. Tracing every instruction from this function's
    // entry (which only PUSHes the caller's ESI to save it) to the first read of ESI at 0x004d54cd
    // (`TEST ESI,ESI` / `JNZ` to the shared epilogue at 0x004d66d6): NOTHING writes ESI in between.
    // `llm_strat_toroidal_dist_sq`, called _G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT times in
    // phase 1, does not touch it either (it is callee-preserved, per the calling convention). So
    // the value tested is whatever ESI happened to hold in the CALLER when this function was
    // entered -- an accident of that call site's own register allocation, not anything this
    // function computes.
    //
    // The test at 0x004d54cd is reused as BOTH the entry gate and the per-pass exit test (a single
    // do-the-passes-until-a-clean-one loop written around one shared branch): a pass sets ESI = 1
    // at its own start and clears it to 0 the instant any swap happens; the loop only re-enters
    // when ESI == 0. So:
    //   inherited_sorted_flag != 0  ->  the test fails IMMEDIATELY, zero passes ever run. The
    //                                   scratch keeps whatever order it already had -- "sort" was
    //                                   named for what the function usually does, not what it did.
    //   inherited_sorted_flag == 0  ->  a normal bubble sort: repeat passes over
    //                                   [0, count-1) swapping adjacent entries whose dist_sq is out
    //                                   of order, until a pass makes no swap.
    // This is preserved exactly, not "fixed" to always sort -- an AI built against the buggy
    // original relies on the unsorted case wherever a caller happens to arrive with ESI != 0.
    if (inherited_sorted_flag != 0) return;

    // A SECOND LATENT BUG, transcribed rather than fixed. The pass loop's bound is
    // `MOV EAX,[count] / DEC EAX / CMP EBX,EAX / JC` -- an UNSIGNED compare against count - 1,
    // re-read on every iteration. With count == 0 that underflows to 0xFFFFFFFF and the pass walks
    // off the end of the scratch for ~4 billion iterations: a HANG, not a wrong answer. It is
    // unreachable in practice (every caller gates on count != 0 before calling), and a guard the
    // original does not have would be a wrong reimplementation -- so the unsigned form stays. It is
    // also the second reason this function is not armed as a shadow site; see tracker AI1A.
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t j = 0; j < (uint32_t)(*own.engage_scratch_count - 1); ++j) {
            engage_candidate &a = own.engage_scratch[j];
            engage_candidate &b = own.engage_scratch[j + 1];
            if (b.dist_sq < a.dist_sq) {
                const engage_candidate tmp = a;
                a                          = b;
                b                          = tmp;
                changed                    = true;
            }
        }
    }
}

void engage_partition_turret_candidates(const ai_view &v, const ai_store &own) {
    // UNSIGNED compare against the count (`CMP ECX,[count] / JC`), the same form
    // scan_target_list_for_engage_candidates uses in ai_engage.cpp: a negative count would run this
    // over essentially the whole address space rather than skipping it. Transcribed as written.
    uint32_t write_cursor = 0; // EBP in the original
    for (uint32_t read_cursor = 0; read_cursor < (uint32_t)*own.engage_scratch_count; ++read_cursor) {
        const engage_candidate &c = own.engage_scratch[read_cursor];
        // (ref & 0x40) != 0 -> BUILDING -- ref_is_building_by_40, NOT the 0xa0 polarity every other
        // consumer of this scratch uses. A class-0 ref is a building here and a unit there; see the
        // target_ref field comment in mh_structs.gen.h.
        if (!ref_is_building_by_40(c.target_ref)) continue;

        const building     &b  = building_of(v, ref_owner(c.target_ref), c.target_index);
        const cfg_building &cb = v.cfg_buildings[b.building_id];
        if (cb.type != BLDG_TYPE_A_TURRET && cb.type != BLDG_TYPE_H_TURRET) continue;

        // `CMP EBP,ECX / JZ` -- skip the swap entirely (not just make it a no-op) when the write
        // cursor has already caught up to the read cursor.
        if (write_cursor != read_cursor) {
            const engage_candidate tmp       = own.engage_scratch[write_cursor];
            own.engage_scratch[write_cursor] = own.engage_scratch[read_cursor];
            own.engage_scratch[read_cursor]  = tmp;
        }
        ++write_cursor;
    }
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t unit_scan_engage_candidates_in_range(int32_t player, int32_t unit_id, uint32_t target_mask) {
    const ai_state st = state();
    return detail::unit_scan_engage_candidates_in_range(st.read, live_calls(), player, unit_id,
                                                        target_mask);
}

// `inherited_sorted_flag` is a REQUIRED parameter here and deliberately has no default. An earlier
// version passed 0 ("always sort"), which the 2026-08-01 reviewers correctly called a latent
// divergence: at a real call site whose live ESI is non-zero the original does NOT sort, so a
// hardcoded 0 would be wrong the first time anyone wired this wrapper into the game. Nothing calls
// it today -- production reaches the ORIGINAL through mh::call::, and there is no shadow arm -- so
// the cost of forcing the decision onto a future caller is zero and the payoff is that the mistake
// cannot be made silently.
void engage_sort_candidates_by_dist(uint32_t source_ref, int32_t source_index,
                                    int32_t inherited_sorted_flag) {
    const ai_state st = state();
    detail::engage_sort_candidates_by_dist(st.read, st.own, live_calls(), source_ref, source_index,
                                           inherited_sorted_flag);
}

void engage_partition_turret_candidates() {
    const ai_state st = state();
    detail::engage_partition_turret_candidates(st.read, st.own);
}

// ---- differential-oracle arms -------------------------------------------------------------------
//
// TWO of the three get a site. engage_sort_candidates_by_dist deliberately gets NONE, and this is
// the reason:
//
// Its sort phase is gated on the caller's leftover ESI (see the long comment at the site above), so
// its input is an ambient register rather than a parameter. That defeats the oracle from BOTH ends.
// Our arm cannot see ESI at all -- there is no way to express "whatever the caller left" in C++.
// And the ORIGINAL arm cannot either: the shadow dispatcher marshals the original's arguments
// itself and calls it through a trampoline, so the ESI the original body reads is the dispatcher's,
// not the game caller's. The original arm would therefore not even reproduce its own production
// behaviour, and a green run would mean nothing while a red one would be an artifact of the
// harness. A zero-call site and a meaningless site log identically; an un-armed one does not.
//
// It is covered offline instead -- `net_selftest.exe aitest` drives detail::
// engage_sort_candidates_by_dist over heap buffers with the flag set BOTH ways, which is the one
// place both behaviours can be exercised deliberately. Recorded as its own evidence tier in
// tools/data/ai_migration.json rather than averaged into the batch's.

} // namespace mh::ai
