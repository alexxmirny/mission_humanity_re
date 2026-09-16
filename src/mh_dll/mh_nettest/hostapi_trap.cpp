//
// hostapi_trap.cpp -- the selftest host table's TRAP, and nothing else.
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

#include <cstdio>
#include <cstdlib>

namespace {

int  g_trap_count = 0;
char g_last_trap[128];
int  g_trap_capture = 0;

} // namespace

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
