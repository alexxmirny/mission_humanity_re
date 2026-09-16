//
// sim_tile_neighbor_reverse_dir_selftest.cpp -- `simtest` offline oracle for
// llm_strat_tile_neighbor_reverse_dir (sim/sim_tile_neighbor_reverse_dir.h/.cpp, RI-SIM / SIM1F).
//
// PURE apart from two read-only lookup tables (dir_remap_table, dir_step_offsets) and the map's
// width/height masks -- no `_calls` struct, no outward call besides the inert assert prologue.
//
// sim_fixture leaves `sim_view::dir_remap_table` NULL by default (see sim_fixture::view()'s own
// comment: "no offline case reads it ... verified via its live-view shadow arm, not offline" -- true
// when that comment was written, no longer true now that THIS file exists). sim_view's members are
// public and the struct is returned BY VALUE, so this file builds its own local remap table and
// overrides that one field on its own copy of the view; `dir_step_offsets` is still the shared
// fixture buffer (sim_fixture::reset() zeroes it, this file seeds it).
//
// EXPECTED VALUES computed by an INDEPENDENT re-derivation of the header's own formula
// (`(uint32_t)(tile - off) & mask`, SUB binding before AND -- see the header's C-precedence note),
// not by calling into detail::tile_neighbor_reverse_dir.
//
#include "sim/sim_tile_neighbor_reverse_dir.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// sim_view::dir_remap_table's documented extent (SIM1F comment in sim_state.h).
constexpr int32_t N_DIRS = 24;

} // namespace

void run_tile_neighbor_reverse_dir_tests() {
    sim_fixture fx;
    fx.reset();

    // A local remap table: step_primary[i] = i (identity), so dir_remap_table[dir].step_primary
    // indexes dir_step_offsets[dir] directly below -- makes the per-direction expected value easy to
    // state without a second indirection table to keep straight.
    std::vector<dir_remap_row> local_remap((size_t)N_DIRS);
    for (int32_t i = 0; i < N_DIRS; ++i) {
        local_remap[(size_t)i].step_primary = i;
        // step_alt1..3 are not read by this function; sentinel values so a translation bug that
        // accidentally read one of them instead would very likely diverge from the expected values
        // below (they are never set to match dir_step_offsets).
        local_remap[(size_t)i].step_alt1 = -1;
        local_remap[(size_t)i].step_alt2 = -1;
        local_remap[(size_t)i].step_alt3 = -1;
    }
    // Distinct, sign-crossing (dx,dy) per step: dx runs 300 down to 300-23*27=-321, dy runs -40 up to
    // 19*23-40=397 -- both slopes different from each other and from dx's, so a dx/dy swap disagrees
    // with the per-index expectation below, and the sign crossing exercises both the "already in
    // range" and the "needs the unsigned wrap" side of the SUB-then-AND mask across the enumeration.
    for (int32_t i = 0; i < N_DIRS; ++i) {
        fx.dir_step_offsets[(size_t)i].dx = 300 - i * 27;
        fx.dir_step_offsets[(size_t)i].dy = i * 19 - 40;
    }

    sim_view v        = fx.view();
    v.dir_remap_table = local_remap.data();

    const int32_t  tile_x = 50, tile_y = 17;   // distinct, non-zero, non-symmetric
    const uint32_t wmask = map_width_mask(v);  // geom.width_mask, 0xff per sim_fixture::reset()
    const uint32_t hmask = map_height_mask(v); // geom.height_mask, 0x3f per sim_fixture::reset()

    // Worked example for dir=0 (documented by hand, then re-checked by the loop below): off = {dx:300,
    // dy:-40}. out_x = (uint32_t)(50-300) & 0xff = (uint32_t)(-250) & 0xff = 0xFFFFFF06 & 0xFF = 6.
    // out_y = (uint32_t)(17-(-40)) & 0x3f = 57 & 0x3f = 57 (57 < 64, unchanged by the mask).
    {
        uint32_t out_x = 0xdeadbeefu, out_y = 0xdeadbeefu;
        detail::tile_neighbor_reverse_dir(v, tile_x, tile_y, /*dir_index*/ 0, &out_x, &out_y);
        ck_eq(out_x, 6u, "tile_neighbor_reverse_dir dir=0: out_x = 6 (hand-derived)");
        ck_eq(out_y, 57u, "tile_neighbor_reverse_dir dir=0: out_y = 57 (hand-derived)");
    }

    // Enumerate ALL 24 input directions, asserting each mapped output against the independently
    // re-derived formula.
    for (int32_t dir = 0; dir < N_DIRS; ++dir) {
        uint32_t out_x = 0xdeadbeefu, out_y = 0xdeadbeefu;
        detail::tile_neighbor_reverse_dir(v, tile_x, tile_y, dir, &out_x, &out_y);

        const int32_t  off_dx = fx.dir_step_offsets[(size_t)dir].dx;
        const int32_t  off_dy = fx.dir_step_offsets[(size_t)dir].dy;
        const uint32_t want_x = (uint32_t)(tile_x - off_dx) & wmask;
        const uint32_t want_y = (uint32_t)(tile_y - off_dy) & hmask;

        char what_x[96], what_y[96];
        snprintf(what_x, sizeof what_x, "tile_neighbor_reverse_dir dir=%d: out_x", dir);
        snprintf(what_y, sizeof what_y, "tile_neighbor_reverse_dir dir=%d: out_y", dir);
        ck_eq(out_x, want_x, what_x);
        ck_eq(out_y, want_y, what_y);
    }

    // ---- MUTATION NOTES ------------------------------------------------------------------------
    // 1. Swapping the two-level indirection (reading step_alt1 instead of step_primary) diverges
    //    from every expected value in the sweep, since the alt fields are all -1 sentinels here.
    // 2. Reordering SUB and AND (`tile - (off & mask)` instead of `(tile - off) & mask`) disagrees
    //    for the entries in the sweep whose |off| exceeds the mask (most of them, by construction).
    // 3. Swapping dx/dy between the x and y outputs diverges across the whole sweep (the dx and dy
    //    slopes above are different functions of `i`).
    // 4. Swapping the width_mask/height_mask constants (0xff <-> 0x3f) diverges for dir=0's own
    //    hand-derived out_x (6, unaffected by either mask since 6 < 0x3f already -- so use the loop's
    //    broader sweep, not just the worked example, to catch this one: several out_y entries land in
    //    [0x40, 0xff] where the 0x3f mask actually bites and 0xff would not).
}

} // namespace mh::sim::test
