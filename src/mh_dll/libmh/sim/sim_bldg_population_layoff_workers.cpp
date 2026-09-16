//
// sim/sim_bldg_population_layoff_workers.cpp -- see sim_bldg_population_layoff_workers.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_strat_population_layoff_workers_00491689.asm) -- the Ghidra
// .c draft was read too and, unusually for this project, agrees field-for-field once cross-checked
// against the asm's own pop_stats/building offsets (see the walk below), but the asm remains the
// authority per house rules, not the draft.
//
#include "sim/sim_bldg_population_layoff_workers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const population_layoff_workers_calls &live_population_layoff_workers_calls() {
    static const population_layoff_workers_calls gc = {
        MH_LIBMH_BIND(llm_strat_bldg_remove_workers),
        MH_LIBMH_BIND(llm_strat_bldg_notify_state_change),
    };
    return gc;
}

namespace detail {

void population_layoff_workers(const sim_view &v, sim_store &own,
                               const population_layoff_workers_calls &gc, int32_t player,
                               int32_t worker_count) {
    // 0x004916a6, and every subsequent row-address/callee use (0x004916b6, 0x004916c8, 0x004916d8,
    // 0x004916eb, 0x00491702, 0x0049172f, 0x0049174c, 0x00491773, 0x00491785, 0x00491795,
    // 0x004917b3): `player` is re-read through a 16-bit MOVZX every single time -- only the low 16
    // bits ever participate. The committed prototype still takes plain `int32_t player` (per the
    // .asm header and addr/mh_calls.gen.h's own binding); the narrowing happens INSIDE the body, not
    // at the parameter boundary.
    const uint16_t p16 = (uint16_t)player;

    // ---- phase 1 (0x004916a6-0x00491702): drain _G_LLM_STRAT_POP_STATS[p16].human directly --------
    //
    // `deficit` starts as the full request. If `.human == 0` this whole phase is skipped (0x004916b4
    // JZ) and `deficit` stays `worker_count` unchanged. Otherwise `.human` absorbs as much as it can:
    // fully, with no deficit left (`.human >= worker_count`), or entirely, with the shortfall
    // becoming the deficit (`.human < worker_count`) -- never driven negative.
    int32_t deficit = worker_count;
    if (own.population_at((uint32_t)p16).human != 0) {
        if (own.population_at((uint32_t)p16).human < worker_count) {
            // 0x004916c8-0x004916e9: human fully drained, remainder becomes the deficit.
            deficit                                = worker_count - own.population_at((uint32_t)p16).human;
            own.population_at((uint32_t)p16).human = 0;
        } else {
            // 0x004916eb-0x00491700: human absorbs the whole request, no deficit remains.
            own.population_at((uint32_t)p16).human -= worker_count;
            deficit = 0;
        }
    }

    // 0x00491702-0x0049170f: the persistent round-robin cursor, read ONCE before the loop into a
    // local -- the loop below advances only this local, writing it back to `.layoff_cursor` solely
    // on an actual removal (see below), matching the asm's single load here.
    int32_t cursor = own.population_at((uint32_t)p16).layoff_cursor;

    // ---- phase 2 (0x00491712-0x004917ab): pay the remaining deficit one worker at a time ----------
    //
    // DELIBERATELY UNBOUNDED -- see the header banner. The only exit test is `deficit != 0`; no
    // iteration cap, no re-scan for "is anyone left staffed" -- reproduced exactly.
    while (deficit != 0) {
        // 0x0049171c-0x0049172e: advance the cursor, wrapping 100->1 (slot 0 is the roster's
        // count/header row, never itself a layoff target -- same idiom
        // sim_bldg_refresh_all_buildings.cpp's occupancy walk documents for slot 0).
        ++cursor;
        if (cursor == 100) {
            cursor = 1;
        }

        // 0x0049172f-0x00491767: a slot only pays if it is OCCUPIED (building_id != 0) AND STAFFED
        // (current_workers != 0) -- re-read every iteration (translator brief rule 16: `const` on
        // sim_view is not a promise of stability, and `own.population_at` above already established
        // the same re-read discipline for pop_stats).
        const building &b = building_of(v, (uint32_t)p16, cursor);
        if (b.building_id != 0 && b.current_workers != 0) {
            // 0x0049176b-0x0049177c: lay off from this slot. EBX=count=1 is a literal in the asm
            // (`MOV EBX,0x1`), not derived from the remaining deficit.
            const uint32_t removed = gc.remove_workers(p16, (uint32_t)cursor, 1u);
            if (removed != 0) {
                // 0x0049178c-0x004917a8: only on an actual removal -- persist the cursor, debit
                // workers_employed, and pay down the deficit by the CALL's return value (always 1
                // here in practice, but the asm subtracts `removed`, not a literal 1 -- reproduced
                // that way).
                own.population_at((uint32_t)p16).layoff_cursor = cursor;
                own.population_at((uint32_t)p16).workers_employed -= (int32_t)removed;
                deficit -= (int32_t)removed;
            }
        }
        // 0x004917ab: unconditional loop-back regardless of which branch above ran (empty slot,
        // unstaffed slot, or a `removed == 0` no-op all fall through to here the same way).
    }

    // 0x004917b0-0x004917bc: called UNCONDITIONALLY, even when the loop above never ran at all (the
    // deficit was already 0 after phase 1) -- in that case `cursor` is still the value read from
    // `.layoff_cursor` BEFORE the loop, per the asm (the cursor local is only ever written back to
    // `.layoff_cursor` inside the `removed != 0` arm above, never unconditionally).
    gc.notify_state_change(p16, (uint32_t)cursor);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void population_layoff_workers(int32_t player, int32_t worker_count) {
    sim_state st = state();
    detail::population_layoff_workers(st.read, st.own, live_population_layoff_workers_calls(), player,
                                      worker_count);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
