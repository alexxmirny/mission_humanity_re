//
// hostapi_trap.cpp -- named required-entry traps and the executable-owned participant authority.
//
// WHY IT IS ITS OWN TU (fork F5I S2). The generated selftest host tables
// (mh_hostapi_selftest.gen.cpp) give every REQUIRED entry a stub that calls mh_hostapi_trap(name)
// before returning a default, so a required entry reached with no real host impl is caught BY NAME
// rather than answering a silent zero. Both selftest executables compile those tables -- and so
// both need this -- but the file the trap used to live in, hostapi_selftest.cpp, also carries
// `run_hostapitest`, which binds mh.dll's REAL hook table through MH_LibMH_BindHookApi(). That
// symbol is defined in mh/seams/libmh_hook_host.cpp, which is mh.dll's own code and in no roster,
// so compiling that whole TU into libmh_selftest.exe would not link.
//
// The alternative was a second copy of the policy below in the other exe's main, which is the
// shape F5I exists to remove: the FATAL-on-uncaptured-trap rule is a decision, and a decision with
// two implementations has two behaviours the first time one of them is edited. So the decision
// lives here once, hostapi_selftest.cpp keeps the SUITE that asserts it, and both images compile
// this file from the directory that owns it.
//
#include "hostapi_selftest_support.h"
#include "../mh_common/participant_boundary.h"
#include "addr/mh_regions.gen.h"
#include <cstring>

#include <cstdio>
#include <cstdlib>

namespace {

int  g_trap_count = 0;
char g_last_trap[128];
int  g_trap_capture = 0;


mh::participant_boundary::authority g_participant;
mh::participant_boundary::view      participant_world(void *) {
    using namespace mh::state;
    if (!is_bound(RID_STRAT_GAME_CLOCK) || !is_bound(RID_STRAT_SIM_STEP_INTERVAL) ||
        !is_bound(RID_GAME_SESSION_MODE) || !live_base(RID_STRAT_GAME_CLOCK) ||
        !live_base(RID_STRAT_SIM_STEP_INTERVAL) || !live_base(RID_GAME_SESSION_MODE) ||
        live_size(RID_STRAT_GAME_CLOCK) < sizeof(double) ||
        live_size(RID_STRAT_SIM_STEP_INTERVAL) < sizeof(double) || live_size(RID_GAME_SESSION_MODE) < 1)
        return {0, 0, false, false};
    return {*ptr<const double>(RID_STRAT_GAME_CLOCK), *ptr<const double>(RID_STRAT_SIM_STEP_INTERVAL),
                 *ptr<const uint8_t>(RID_GAME_SESSION_MODE) == 3, true};
}
} // namespace

void mh_hostapi_participant_initialize() {
    static const bool initialized = g_participant.initialize(participant_world, nullptr);
    if (!initialized) std::abort();
}
uint64_t mh_hostapi_participant_body_boundary(uint32_t op, uint64_t argument) {
    return g_participant.boundary(op, argument);
}
int32_t mh_hostapi_participant_removal_admit(const void *data, uint32_t size) {
    using namespace mh::participant_boundary;
    if (!data || size != sizeof(request)) return int32_t(admission::invalid);
    request r;
    std::memcpy(&r, data, sizeof(r));
    return int32_t(g_participant.admit(r));
}

void mh_hostapi_trap(const char *name) {
    ++g_trap_count;
    snprintf(g_last_trap, sizeof(g_last_trap), "%s", name);
    printf("[hostapi] REQUIRED entry %s called with no host impl\n", name);
    if (!g_trap_capture) {
        // Fatal outside hostapitest: a suite that needs a required entry must mock it (its
        // own calls struct) or the selftest host must grow a real impl -- never limp on a
        // silent default. The exit IS the "test that names it".
        printf("[hostapi] FATAL: required host entry reached in a selftest run -- failing\n");
        fflush(stdout);
        exit(1);
    }
}

void mh_hostapi_trap_set_capture(int on) {
    g_trap_capture = on;
}

int mh_hostapi_trap_count() {
    return g_trap_count;
}

const char *mh_hostapi_last_trap() {
    return g_last_trap;
}

void mh_hostapi_trap_reset() {
    g_trap_count   = 0;
    g_last_trap[0] = '\0';
}
