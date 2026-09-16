#include <array>
#include <vector>

#include "sim/sim_bldg_instant_construct.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-default, non-symmetric seeds -- no two of these share a value, so a swapped-
// argument translation (x<->y, player<->building_type, etc.) cannot pass by accident.
constexpr int32_t  PLAYER        = 3;
constexpr int32_t  BUILDING_TYPE = 37;
constexpr int32_t  X             = 111;
constexpr int32_t  Y             = 222;
constexpr uint16_t INVENTION     = 45;

// ---- recorder --------------------------------------------------------------------------------
// Captureless lambdas convert to plain function pointers, same shape as
// sim_prod_completion_selftest.cpp's g_pc/g_da/g_sa -- one file-scope recorder feeds the whole
// `bldg_instant_construct_calls` stub table.
struct bic_recorder {
    int32_t                             footprint_n = 0;
    std::vector<std::array<int32_t, 4>> footprint_args; // (x, y, building_type, viewer)

    int32_t                             handle_progress_n = 0;
    std::vector<std::array<int32_t, 2>> handle_progress_args; // (player, invention)

    int32_t                             construct_finalize_n = 0;
    std::vector<std::array<int64_t, 6>> construct_finalize_args; // (param_1, y_b, player, param_4, x_b,
                                                                 // building_id) -- int64_t so
                                                                 // uint32_t param_1==0xfffffffe widens
                                                                 // as the unsigned value, not sign-
                                                                 // extended garbage.
    // control knobs
    int32_t footprint_ret          = 1; // non-zero == "clear" (not blocked)
    int32_t construct_finalize_ret = 1; // non-zero == "got a real roster slot"

    void reset() { *this = bic_recorder{}; }
};
bic_recorder g_bic;

const bldg_instant_construct_calls &rec_bic_calls() {
    static const bldg_instant_construct_calls c = {
        [](int32_t x, int32_t y, int32_t building_type, uint32_t viewer) -> int32_t {
            g_bic.footprint_n++;
            g_bic.footprint_args.push_back({x, y, building_type, (int32_t)viewer});
            return g_bic.footprint_ret;
        },
        [](uint16_t player, uint16_t inv) {
            g_bic.handle_progress_n++;
            g_bic.handle_progress_args.push_back({(int32_t)player, (int32_t)inv});
        },
        [](uint32_t param_1, int32_t y_b, uint16_t player, char param_4, uint32_t x_b,
           uint32_t building_id) -> int32_t {
            g_bic.construct_finalize_n++;
            g_bic.construct_finalize_args.push_back({(int64_t)param_1, (int64_t)y_b, (int64_t)player,
                                                     (int64_t)param_4, (int64_t)x_b,
                                                     (int64_t)building_id});
            return g_bic.construct_finalize_ret;
        },
    };
    return c;
}

// ==== step 1: the placement-check gate ==========================================================

void test_footprint_blocked_returns_zero_no_further_calls() {
    sim_fixture f;
    g_bic.reset();
    g_bic.footprint_ret           = 0;     // blocked
    f.b(PLAYER, 0).cycle_progress = 999.5; // sentinel -- nothing in this function should touch it

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck_eq((uint32_t)r, 0u, "blocked footprint -> whole body skipped, returns 0 (0x00462dc6 JZ)");
    ck_eq((uint32_t)g_bic.footprint_n, 1u, "blocked footprint -> footprint_is_clear called exactly once");
    ck_eq((uint32_t)g_bic.handle_progress_n, 0u, "blocked footprint -> handle_progress never reached");
    ck_eq((uint32_t)g_bic.construct_finalize_n, 0u,
          "blocked footprint -> construct_finalize never reached");
    ck_eq_d(f.b(PLAYER, 0).cycle_progress, 999.5, "blocked footprint -> no cycle_progress write anywhere");
}

void test_footprint_call_arguments_match_register_setup() {
    sim_fixture f;
    g_bic.reset();
    g_bic.construct_finalize_ret = 0; // irrelevant to this test, keep the rest cheap

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck(g_bic.footprint_args.size() == 1 && g_bic.footprint_args[0][0] == X &&
           g_bic.footprint_args[0][1] == Y && g_bic.footprint_args[0][2] == BUILDING_TYPE &&
           g_bic.footprint_args[0][3] == 8,
       "footprint_is_clear(x, y, building_type, viewer=8) -- viewer HARDCODED to the omniscient/"
       "server-side value 8, not any real player's fog (0x00462db1-0x00462dc6)");
}

// ==== step 2: the auto-unlock-invention branch ===================================================

void test_invention_already_available_skips_handle_progress() {
    sim_fixture f;
    g_bic.reset();
    f.cfg_buildings[BUILDING_TYPE].invention                      = INVENTION;
    f.progress[PLAYER * PROGRESS_ROW_COUNT + INVENTION].available = 1; // already unlocked
    g_bic.construct_finalize_ret                                  = 9; // some real slot

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck_eq((uint32_t)g_bic.handle_progress_n, 0u,
          "progress[player][invention].available != 0 -> handle_progress NOT called (0x00462ded JNZ)");
    ck_eq((uint32_t)r, 9u,
          "auto-unlock skipped -> step 3 still runs unconditionally, returns construct_finalize's result");
}

void test_invention_unavailable_calls_handle_progress_with_correct_args() {
    sim_fixture f;
    g_bic.reset();
    f.cfg_buildings[BUILDING_TYPE].invention                      = INVENTION;
    f.progress[PLAYER * PROGRESS_ROW_COUNT + INVENTION].available = 0; // locked
    g_bic.construct_finalize_ret                                  = 1;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck_eq((uint32_t)g_bic.handle_progress_n, 1u,
          "progress[player][invention].available == 0 -> handle_progress called exactly once "
          "(0x00462def-0x00462e01)");
    ck(g_bic.handle_progress_args.size() == 1 && g_bic.handle_progress_args[0][0] == PLAYER &&
           g_bic.handle_progress_args[0][1] == INVENTION,
       "handle_progress(player, invention) -- the SECOND, independently-recomputed invention read "
       "(0x00462def-0x00462dfd) agrees with the first (cfg_buildings[building_type].invention)");
}

// ==== step 3: the unconditional finalize + cycle_progress seed ==================================

void test_construct_finalize_call_argument_order_and_types() {
    sim_fixture f;
    g_bic.reset();
    g_bic.construct_finalize_ret = 1;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck(g_bic.construct_finalize_args.size() == 1 &&
           g_bic.construct_finalize_args[0][0] == (int64_t)0xfffffffeu &&
           g_bic.construct_finalize_args[0][1] == Y && g_bic.construct_finalize_args[0][2] == PLAYER &&
           g_bic.construct_finalize_args[0][3] == 1 && g_bic.construct_finalize_args[0][4] == X &&
           g_bic.construct_finalize_args[0][5] == BUILDING_TYPE,
       "construct_finalize(param_1=0xfffffffe, y_b=y, player, param_4=1, x_b=x, building_id="
       "building_type) -- matches llm_bldg_construct_finalize's OWN already-committed prototype "
       "(0x00462e0b-0x00462e24), not the Ghidra .c draft's guessed argument order");
}

void test_construct_finalize_fails_returns_zero_no_cycle_progress_write() {
    sim_fixture f;
    g_bic.reset();
    g_bic.construct_finalize_ret  = 0;       // failure -- no real roster slot
    f.b(PLAYER, 0).cycle_progress = 12345.5; // sentinel: a failing result is numerically 0, so this
                                             // also proves the write is skipped ENTIRELY, not just
                                             // redirected to index 0

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck_eq((uint32_t)r, 0u, "construct_finalize failure -> function returns 0 (0x00462e2b JZ)");
    ck_eq_d(f.b(PLAYER, 0).cycle_progress, 12345.5,
            "construct_finalize failure -> cycle_progress write skipped entirely (step 3b guarded on "
            "result != 0)");
}

void test_construct_finalize_success_seeds_cycle_progress() {
    sim_fixture f;
    g_bic.reset();
    constexpr int32_t RESULT_SLOT               = 21; // distinct, non-zero, non-default roster slot
    g_bic.construct_finalize_ret                = RESULT_SLOT;
    f.cfg_buildings[BUILDING_TYPE].build_time_2 = 55.25; // distinct double, not a round/default value

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::bldg_instant_construct(v, own, rec_bic_calls(), PLAYER, BUILDING_TYPE, X, Y);

    ck_eq((uint32_t)r, (uint32_t)RESULT_SLOT,
          "construct_finalize success -> function returns its roster slot verbatim");
    ck_eq_d(f.b(PLAYER, RESULT_SLOT).cycle_progress, 55.25 - 3.0,
            "success -> buildings[player][result].cycle_progress = Building[building_type]."
            "build_time_2 + _DAT_0050114c, the literal double constant -3.0 (0x00462e2d-0x00462e50)");
}

} // namespace

void run_bldg_instant_construct_tests() {
    test_footprint_blocked_returns_zero_no_further_calls();
    test_footprint_call_arguments_match_register_setup();

    test_invention_already_available_skips_handle_progress();
    test_invention_unavailable_calls_handle_progress_with_correct_args();

    test_construct_finalize_call_argument_order_and_types();
    test_construct_finalize_fails_returns_zero_no_cycle_progress_write();
    test_construct_finalize_success_seeds_cycle_progress();
}

} // namespace mh::sim::test
