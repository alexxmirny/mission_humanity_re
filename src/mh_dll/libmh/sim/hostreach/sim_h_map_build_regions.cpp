//
// sim/hostreach/sim_h_map_build_regions.cpp -- see sim_h_map_build_regions.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_map_build_regions_00423335.asm,
// tmp/decomp_sim/llm_map_assign_remaining_tiles_to_regions_00422a42.asm) -- both .c drafts beside them
// were cross-checked instruction-by-instruction against the raw sequence; the header banner records the
// two places they read as more uniform than the assembly actually is (the null-vs-sentinel leftover-
// tile guard, and the fixpoint counter's inc-then-selective-dec mechanism vs. its net effect).
//
#include "sim/hostreach/sim_h_map_build_regions.h"

#include "addr/mh_rebind.gen.h"                        // LIB-REBIND: the config-selected binder (MH_LIBMH_BIND)
#include "state/rebind_targets.gen.h"                  // MH_REBIND_TARGET_* declarations the macro expands to
#include "sim/libtrans/sim_lt_map_region_pool.h"       // mh::sim::get_next_block() -- translated sibling
#include "sim/sim_map_region_helpers.h"                // mh::sim::region_pick_smaller() / merge_small_regions() -- translated siblings
#include "sim/hostreach/sim_h_map_region_flood_fill.h" // wave-2 sibling: region_flood_fill
#include "sim/hostreach/sim_h_map_region_prep.h"       // wave-2 siblings: the three prep steps
#include "sim/hostreach/sim_h_map_region_seed.h"       // wave-2 sibling: seed_regions_multires

namespace mh::sim {

const map_build_regions_calls &live_map_build_regions_calls() {
    static const map_build_regions_calls c = {
        // -- WAVE 2 DISSOLVED ALL FIVE OF THESE (2026-09-10) -----------------------------------
        // An earlier draft of this block bound them to `mh::call::` with a reasoned argument that
        // they were Law-4 frontier originals which would stay original. The reasoning was sound and
        // the PREMISE was wrong, and gen_va_census is what said so: it could route these five to NO
        // OWNING ITEM, because they are in no migration ledger at all -- while their own siblings
        // llm_map_merge_small_regions and llm_map_region_pick_smaller ARE verified rows. The
        // nav-region pipeline was HALF-OWNED, and translating its entry point (this function) is
        // what made that visible. All five are now ours, so every one of these is a DIRECT sibling
        // call and the VA sites are gone.
        //
        // They stay STRUCT MEMBERS rather than becoming literal calls in the body: the struct is
        // what lets net_selftest simtest substitute a recorder, and a `detail::` body that reached
        // for a sibling's `live_*_calls()` instead is the exact defect lint_detail_calls.py gates
        // (translator-brief rule 3c). Bound to the siblings' PUBLIC wrappers, the shape
        // sim_map_apply_area.cpp established for this subsystem.
        &mh::sim::init_region_route_step_deltas,
        &mh::sim::compute_obstacle_proximity_flags,
        &mh::sim::seed_regions_multires,
        &mh::sim::compute_region_merge_threshold,
        &mh::sim::region_flood_fill,
        // -- translated siblings, bound to their public wrappers directly (behavior-identical to a
        // literal mh::sim::X(...) call; the indirection only buys the offline stub seam) --
        &mh::sim::merge_small_regions,
        &mh::sim::get_next_block,
        &mh::sim::region_pick_smaller,
    };
    return c;
}

namespace {
// The flood-fill-pending grid-cell sentinel (mh_llm_map_region::region's own field comment). Compared/
// written via uintptr_t, matching sim_map_region_helpers.cpp's own region_pick_smaller -- the value is
// not a real address a `llm_map_region *` literal could portably encode.
inline llm_map_region *region_pending_sentinel() {
    return reinterpret_cast<llm_map_region *>(static_cast<uintptr_t>(0xffffffffu));
}
} // namespace

namespace detail {

// ---- llm_map_build_regions @0x00423335 -------------------------------------------------------------
void build_regions(const sim_view &v, sim_store &own, const map_build_regions_calls &c) {
    // 0x0042334d-0x0042335e: reseed WIDTH_M/HEIGHT_M as map_width-1 / map_height-1 -- a SECOND writer
    // of this pair beyond llm_strat_planet_map_session_init (see the header banner's correction of
    // sim_state.h's own comment).
    const uint32_t width_m  = static_cast<uint32_t>(*v.map_width - 1);
    const uint32_t height_m = static_cast<uint32_t>(*v.map_height - 1);
    own.width_m_mut()       = width_m;
    own.height_m_mut()      = height_m;

    // 0x00423363-0x00423373: _G_LLM_MAP_REGION_COORD_WRAP_MASK = width_m*0x100 + height_m. The .asm
    // re-reads both globals from memory after storing them; using the locals above is bit-identical --
    // nothing else writes either global between the store and this computation (sequential, single-
    // threaded code).
    own.region_coord_wrap_mask_mut() = (width_m << 8) + height_m;

    own.region_alloc_counter() = 1; // 0x00423379: `counter` reset to 1 -- index 0 is never allocated.

    c.init_region_route_step_deltas();    // 0x00423383
    c.compute_obstacle_proximity_flags(); // 0x00423388

    own.region_list_head_mut() = nullptr; // 0x0042338d: drop the active list -- a full rebuild.

    // 0x00423397-0x0042341a: seed grid sentinels from the passable plane, x outer / y inner (matches
    // every other _G_LLM_MAP_REGION_GRID[x][y] site in this closure). 0 = impassable, never gets a
    // region; 0xffffffff = passable, pending assignment.
    for (int32_t x = 0; x < *v.map_width; ++x) {
        for (int32_t y = 0; y < *v.map_height; ++y) {
            own.region_cell_at(x, y).region =
                (v.passable[(x << 8) | y] == 0) ? nullptr : region_pending_sentinel();
        }
    }

    c.seed_regions_multires();          // 0x0042341a
    c.compute_region_merge_threshold(); // 0x0042341f
    c.merge_small_regions();            // 0x00423424 -- translated sibling, see header banner

    // 0x00423429-0x004234ae: a SECOND full sweep. The guard is `region == nullptr`, NOT the 0xffffffff
    // sentinel -- read 0x00423484-0x0042348b (`CMP dword ptr [...],0x0` / JZ) carefully; a plausible-
    // looking "still pending" reading would be wrong here (see the header banner's TWO DIFFERENT
    // LEFTOVER FALLBACKS note). A passable cell whose region is still null (never claimed by the
    // seed/merge pipeline above) gets a brand-new region -- get_next_block() zeroes the fresh node's
    // x/y/cell_count/neighbor_count itself (sim/libtrans/sim_lt_map_region_pool.h's own field-by-field
    // banner); UNLIKE assign_remaining_tiles_to_regions's own allocate branch below, this site never
    // overwrites them afterward -- no x/y/cell_count fixup, no flood_fill call. Transcribed as the
    // original has it (translator-brief rule 10 -- preserve, do not "fix" to match the sibling
    // function's more careful version).
    for (int32_t x = 0; x < *v.map_width; ++x) {
        for (int32_t y = 0; y < *v.map_height; ++y) {
            if (v.passable[(x << 8) | y] != 0 && own.region_cell_at(x, y).region == nullptr) {
                own.region_cell_at(x, y).region = c.get_next_block();
            }
        }
    }
}

// ---- llm_map_assign_remaining_tiles_to_regions @0x00422a42 ------------------------------------------
void assign_remaining_tiles_to_regions(const sim_view &v, sim_store &own, const map_build_regions_calls &c) {
    // FIXPOINT LOOP: each outer iteration is one full x*y sweep. `fresh_allocations` is reset to 0 at
    // the TOP of every sweep (0x00422a5a) and counts this pass's fresh region allocations -- see the
    // header banner's THE FIXPOINT COUNTER note for the derivation of why that net effect (not the raw
    // asm inc-then-selective-dec mechanism) is what must be reproduced.
    int32_t fresh_allocations = 0;
    do {
        fresh_allocations = 0;

        for (int32_t x = 0; x < *v.map_width; ++x) {
            for (int32_t y = 0; y < *v.map_height; ++y) {
                // 0x00422a9f-0x00422ab4: only a cell still carrying the pending sentinel is a candidate
                // this pass.
                if (own.region_cell_at(x, y).region != region_pending_sentinel()) continue;

                // 0x00422ac0-0x00422b1c: chain the four orthogonal neighbors through region_pick_smaller
                // -- same order and wrap-mask idiom as llm_map_region_apply_area
                // (sim/sim_map_apply_area.cpp): (x+1,y), (x-1,y), (x,y+1), (x,y-1).
                llm_map_region *cand = nullptr;
                cand                 = c.region_pick_smaller(cand, static_cast<int32_t>((static_cast<uint32_t>(x) + 1) & *v.width_m), y);
                cand                 = c.region_pick_smaller(cand, static_cast<int32_t>((static_cast<uint32_t>(x) - 1) & *v.width_m), y);
                cand                 = c.region_pick_smaller(cand, x, static_cast<int32_t>((static_cast<uint32_t>(y) + 1) & *v.height_m));
                cand                 = c.region_pick_smaller(cand, x, static_cast<int32_t>((static_cast<uint32_t>(y) - 1) & *v.height_m));

                if (cand != nullptr) {
                    // 0x00422b25-0x00422b45: inherit -- claim the neighbor's region, bump its cell_count.
                    // Net zero on fresh_allocations (resolved this pass) -- see header banner.
                    own.region_cell_at(x, y).region = cand;
                    cand->cell_count += 1;
                    continue;
                }

                // 0x00422b4d-0x00422bcd: no neighbor's OWN region was inheritable. Before giving up,
                // check whether any of the SAME four neighbor CELLS is itself still pending -- if so,
                // this cell seeds a brand-new region now rather than waiting on a later pass.
                const bool any_neighbor_pending =
                    own.region_cell_at(static_cast<int32_t>((static_cast<uint32_t>(x) + 1) & *v.width_m), y)
                            .region == region_pending_sentinel() ||
                    own.region_cell_at(static_cast<int32_t>((static_cast<uint32_t>(x) - 1) & *v.width_m), y)
                            .region == region_pending_sentinel() ||
                    own.region_cell_at(x, static_cast<int32_t>((static_cast<uint32_t>(y) + 1) & *v.height_m))
                            .region == region_pending_sentinel() ||
                    own.region_cell_at(x, static_cast<int32_t>((static_cast<uint32_t>(y) - 1) & *v.height_m))
                            .region == region_pending_sentinel();

                if (!any_neighbor_pending) {
                    // 0x00422bcf-0x00422bea: dead end -- no path to a region exists yet and none of the
                    // neighbors will create one this pass either. Give up permanently: 0, matching
                    // build_regions's own "0 = never gets a region" convention. Net zero on
                    // fresh_allocations (resolved this pass) -- see header banner.
                    own.region_cell_at(x, y).region = nullptr;
                    continue;
                }

                // 0x00422bef-0x00422c30: seed a brand-new region here. get_next_block() zeroes
                // cell_count/neighbor_count itself; this site then explicitly sets x/y and the flood-
                // filled cell_count (UNLIKE build_regions's own leftover-tile fallback above, which
                // leaves them zeroed -- see the header banner's TWO DIFFERENT LEFTOVER FALLBACKS note).
                llm_map_region *fresh           = c.get_next_block();
                fresh->x                        = static_cast<uint8_t>(x);
                fresh->y                        = static_cast<uint8_t>(y);
                fresh->cell_count               = c.region_flood_fill(static_cast<uint32_t>(x), static_cast<uint32_t>(y), fresh);
                own.region_cell_at(x, y).region = fresh;
                ++fresh_allocations; // the ONLY branch with a net +1 -- see header banner.
            }
        }
    } while (fresh_allocations != 0);
}

} // namespace detail


void build_regions() {
    sim_state st = state();
    detail::build_regions(st.read, st.own, live_map_build_regions_calls());
}

void assign_remaining_tiles_to_regions() {
    sim_state st = state();
    detail::assign_remaining_tiles_to_regions(st.read, st.own, live_map_build_regions_calls());
}

} // namespace mh::sim
