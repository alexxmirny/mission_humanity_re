//
// sim_debug_roll_random_selftest.cpp -- `simtest` cases for llm_debug_roll_random (sim/
// sim_debug_roll_random.{h,cpp}).
//
#include "sim/sim_debug_roll_random.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Own tiny recorder -- the module's calls struct is two members, not worth the generated-stub
// machinery sim_dispatch_*_selftest.cpp uses for the 75-callee dispatcher.
struct log_t {
    int     rand_below_calls  = 0;
    int32_t last_upper_bound  = 0;
    int32_t rand_below_return = 7;

    int     sprintf_calls = 0;
    void   *last_dst      = nullptr;
    double  last_a0       = 0.0;
    int32_t last_a1       = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const debug_roll_random_calls &recording_calls() {
    static const debug_roll_random_calls c = {
        [](int32_t upper_bound) -> int32_t {
            ++g_log.rand_below_calls;
            g_log.last_upper_bound = upper_bound;
            return g_log.rand_below_return;
        },
        [](void *dst, const wchar_t *, double a0, int32_t a1) -> int32_t {
            ++g_log.sprintf_calls;
            g_log.last_dst = dst;
            g_log.last_a0  = a0;
            g_log.last_a1  = a1;
            return 0;
        },
    };
    return c;
}

// The whole function: roll llm_rand_below(100), then format the result with the game clock into the
// sim's text scratch buffer. No branches, so one case pins every argument.
void test_debug_roll_random() {
    sim_fixture f;
    sim_store   own = f.store();
    f.game_clock    = 123.5;
    g_log.reset();
    g_log.rand_below_return = 42;

    detail::debug_roll_random(f.view(), own, recording_calls());

    ck(g_log.rand_below_calls == 1 && g_log.last_upper_bound == 100,
       "debug_roll_random: llm_rand_below(100), called once");
    ck(g_log.sprintf_calls == 1, "debug_roll_random: w_sprintf__vdi called once");
    ck(g_log.last_dst == (void *)f.text_scratch.data(),
       "debug_roll_random: formats into the sim's OWN text_scratch, not a fresh buffer");
    ck_eq_d(g_log.last_a0, 123.5, "debug_roll_random: passes the game clock as the double vararg");
    ck(g_log.last_a1 == 42, "debug_roll_random: passes the roll as the trailing int vararg");
}

} // namespace

void run_debug_roll_random_tests() { test_debug_roll_random(); }

} // namespace mh::sim::test
