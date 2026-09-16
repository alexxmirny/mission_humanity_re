//
// sim_unit_state_plot_turn_path_selftest.cpp -- `simtest` offline oracle for
// llm_strat_unit_state_plot_turn_path (sim/sim_unit_state_plot_turn_path.h/.cpp, RI-SIM / SIM1-G2).
//
#include "sim/sim_unit_state_plot_turn_path.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int     rand_below_calls = 0;
    int32_t last_upper_bound = -1;
    int32_t rand_ret         = 0;

    int      set_state_calls = 0;
    uint16_t last_new_state  = 0;

    int     attach_calls  = 0;
    int32_t last_player   = -1;
    int32_t last_unit_idx = -1;
    int32_t last_slot     = -1;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_state_plot_turn_path_calls &recording_calls() {
    static const unit_state_plot_turn_path_calls c = {
        [](int32_t upper_bound) -> int32_t {
            ++g_log.rand_below_calls;
            g_log.last_upper_bound = upper_bound;
            return g_log.rand_ret;
        },
        [](uint16_t new_state) -> void {
            ++g_log.set_state_calls;
            g_log.last_new_state = new_state;
        },
        [](int32_t player, int32_t unit_index, int32_t slot_id) -> void {
            ++g_log.attach_calls;
            g_log.last_player   = player;
            g_log.last_unit_idx = unit_index;
            g_log.last_slot     = slot_id;
        },
    };
    return c;
}

} // namespace

void run_unit_state_plot_turn_path_tests() {
    constexpr uint16_t PLAYER = 1;
    constexpr uint16_t INDEX  = 4;

    // ---- (a) no free path slot (all 100 flags busy): early return, NOTHING else happens. -----------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = INDEX;
        fx.cur_unit_ptr    = &fx.units[(size_t)PLAYER * UNITS_PER_PLAYER + INDEX];
        sim_view  v        = fx.view();
        sim_store own      = fx.store();
        for (int32_t i = 0; i < 100; ++i) own.path_slot_flag_at(PLAYER, i) = 1;
        own.cur_unit().path_cursor = 77; // must NOT change

        detail::unit_state_plot_turn_path(v, own, recording_calls());

        ck_eq(g_log.rand_below_calls, 0, "plot_turn_path: no free slot -> rand_below never called");
        ck_eq(g_log.set_state_calls, 0, "plot_turn_path: no free slot -> unit_set_state never called");
        ck_eq(g_log.attach_calls, 0, "plot_turn_path: no free slot -> path_attach_slot never called");
        ck_eq((int32_t)own.cur_unit().path_cursor, 77, "plot_turn_path: no free slot -> path_cursor untouched");
    }

    // ---- (b) a free slot exists (not the first index, to prove the FIRST free one is chosen): full
    // walk of the 4-entry remap chain (no clamp needed), entry 4's alt pick, entry 5 sentinel,
    // unit_set_state(0x2c), path_cursor reset to 0, path_attach_slot(player, cur_index, slot). ------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = INDEX;
        fx.cur_unit_ptr    = &fx.units[(size_t)PLAYER * UNITS_PER_PLAYER + INDEX];

        // A local remap table (fixture leaves dir_remap_table null by default, same convention as
        // sim_tile_neighbor_reverse_dir_selftest.cpp): a simple +1 chain from the seed heading, all
        // staying well under the >23 clamp threshold, so the loop's output is exactly predictable.
        // Sized 25 (not 24) so the header-documented one-past-the-end read at the FINAL heading (up
        // to 24) lands in real, controlled memory instead of true UB.
        std::vector<dir_remap_row> local_remap(25);
        for (int32_t i = 0; i < 25; ++i) {
            local_remap[(size_t)i].step_primary = i + 1;    // heading -> heading+1 each lookup
            local_remap[(size_t)i].step_alt1    = 1000 + i; // distinct per-row sentinel, roll==0
            local_remap[(size_t)i].step_alt2    = 2000 + i; // distinct per-row sentinel, roll==1
        }

        sim_view v        = fx.view();
        v.dir_remap_table = local_remap.data();
        sim_store own     = fx.store();

        // Free slots at 0 and 2 are busy; slot 1 is free and must be the one chosen (not slot 3, the
        // NEXT free one after it, and not slot 0/2 which are busy).
        own.path_slot_flag_at(PLAYER, 0) = 1;
        own.path_slot_flag_at(PLAYER, 1) = 0; // FREE -- expected pick
        own.path_slot_flag_at(PLAYER, 2) = 1;
        own.path_slot_flag_at(PLAYER, 3) = 0; // also free, must NOT be picked (1 comes first)

        own.cur_unit().move_heading = 2;  // seed heading
        own.cur_unit().path_cursor  = 55; // must become 0
        g_log.rand_ret              = 0;  // roll==0 -> step_alt1

        detail::unit_state_plot_turn_path(v, own, recording_calls());

        // Hand-derived chain: seed=2 -> lookup(2).step_primary=3 (entry0=3) -> lookup(3).step_primary=4
        // (entry1=4) -> lookup(4).step_primary=5 (entry2=5) -> lookup(5).step_primary=6 (entry3=6).
        // None exceed 23, so no clamp fires; final heading=6 for the entry-4 alt pick.
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 1, 0).heading, 3u, "plot_turn_path: entry0 heading");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 1, 1).heading, 4u, "plot_turn_path: entry1 heading");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 1, 2).heading, 5u, "plot_turn_path: entry2 heading");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 1, 3).heading, 6u, "plot_turn_path: entry3 heading");
        ck_eq(g_log.rand_below_calls, 1, "plot_turn_path: rand_below called exactly once");
        ck_eq(g_log.last_upper_bound, 2, "plot_turn_path: rand_below(2)");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 1, 4).heading, (uint32_t)(uint8_t)(1000 + 6),
              "plot_turn_path: entry4 = dir_remap_table[final_heading].step_alt1 (roll==0), low byte "
              "only -- entry4's store is a byte truncation of the row's int32_t step_alt1");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 1, 5).heading, 0xffu, "plot_turn_path: entry5 = 0xff sentinel");
        ck_eq(g_log.set_state_calls, 1, "plot_turn_path: unit_set_state called exactly once");
        ck_eq((uint32_t)g_log.last_new_state, 0x2cu, "plot_turn_path: unit_set_state(0x2c)");
        ck_eq((int32_t)own.cur_unit().path_cursor, 0, "plot_turn_path: path_cursor reset to 0");
        ck_eq(g_log.attach_calls, 1, "plot_turn_path: path_attach_slot called exactly once");
        ck_eq(g_log.last_player, (int32_t)PLAYER, "plot_turn_path: path_attach_slot player arg");
        ck_eq(g_log.last_unit_idx, (int32_t)INDEX, "plot_turn_path: path_attach_slot unit_index = cur_index");
        ck_eq(g_log.last_slot, 1, "plot_turn_path: path_attach_slot slot = the chosen free slot (1)");
    }

    // ---- (c) roll==1 picks step_alt2 instead of step_alt1 (same chain as (b), only the roll differs).
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = INDEX;
        fx.cur_unit_ptr    = &fx.units[(size_t)PLAYER * UNITS_PER_PLAYER + INDEX];

        std::vector<dir_remap_row> local_remap(25);
        for (int32_t i = 0; i < 25; ++i) {
            local_remap[(size_t)i].step_primary = i + 1;
            local_remap[(size_t)i].step_alt1    = 1000 + i;
            local_remap[(size_t)i].step_alt2    = 2000 + i;
        }
        sim_view v                       = fx.view();
        v.dir_remap_table                = local_remap.data();
        sim_store own                    = fx.store();
        own.path_slot_flag_at(PLAYER, 0) = 0;
        own.cur_unit().move_heading      = 2;
        g_log.rand_ret                   = 1; // roll==1 -> step_alt2

        detail::unit_state_plot_turn_path(v, own, recording_calls());

        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 0, 4).heading, (uint32_t)(uint8_t)(2000 + 6),
              "plot_turn_path: entry4 = dir_remap_table[final_heading].step_alt2 (roll==1), low byte only");
    }

    // ---- (d) the >23 clamp: a chain step that jumps heading past 23 clamps to EXACTLY 24 for the
    // NEXT lookup, and the entry-4 alt is read from row 24 (the header-documented one-past-the-end
    // read of DIR_REMAP_TABLE's declared 24 rows, reproduced deliberately). ------------------------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = INDEX;
        fx.cur_unit_ptr    = &fx.units[(size_t)PLAYER * UNITS_PER_PLAYER + INDEX];

        std::vector<dir_remap_row> local_remap(25);
        for (int32_t i = 0; i < 25; ++i) {
            local_remap[(size_t)i].step_primary = i; // default: identity (overridden below at row 0)
            local_remap[(size_t)i].step_alt1    = 1000 + i;
            local_remap[(size_t)i].step_alt2    = 2000 + i;
        }
        // seed heading 0 -> lookup(0).step_primary = 30 (jumps past 23) -> clamped to 24 for entry0's
        // STORED value is 30 itself (the clamp applies to the NEXT iteration's input, not to what was
        // just stored -- see the header's derivation) -- entry0 stores 30, then heading becomes 24.
        local_remap[0].step_primary = 30;
        // lookup(24).step_primary = 5 -> entry1 stores 5, heading stays 5 (no clamp, <=23).
        local_remap[24].step_primary = 5;
        // lookup(5).step_primary = 6 -> entry2 stores 6, heading stays 6 (no clamp).
        local_remap[5].step_primary = 6;
        // lookup(6).step_primary = 50 -> entry3 stores 50 (fits in a byte), then clamps to 24 for
        // the FINAL heading used by entry4's alt lookup below.
        local_remap[6].step_primary = 50;

        sim_view v                       = fx.view();
        v.dir_remap_table                = local_remap.data();
        sim_store own                    = fx.store();
        own.path_slot_flag_at(PLAYER, 0) = 0;
        own.cur_unit().move_heading      = 0;
        g_log.rand_ret                   = 0;

        detail::unit_state_plot_turn_path(v, own, recording_calls());

        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 0, 0).heading, 30u,
              "plot_turn_path: entry0 stores the RAW post-lookup value (30), the clamp has not fired yet");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 0, 1).heading, 5u,
              "plot_turn_path: entry1's lookup used the CLAMPED heading (24), reading row 24 -> step_primary=5");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 0, 2).heading, 6u, "plot_turn_path: entry2 heading");
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 0, 3).heading, 50u,
              "plot_turn_path: entry3 stores the RAW post-lookup value (50), the clamp has not fired yet");
        // 50 > 23 -> heading clamps to 24 AFTER entry3's store -> entry4 reads ROW 24's step_alt1
        // (1000+24=1024, truncated to a byte: 1024 & 0xff = 0).
        ck_eq((uint32_t)own.path_buffer_at(PLAYER, 0, 4).heading, (uint32_t)(uint8_t)(1000 + 24),
              "plot_turn_path: entry4 reads the CLAMPED (24) row's step_alt1 -- the header-documented "
              "one-past-DIR_REMAP_TABLE's-declared-24-rows read");
    }
}

} // namespace mh::sim::test
