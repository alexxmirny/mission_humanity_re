// Support surface for the host-callback selftest host (LIB-ABI stages B+D; sim/tact split
// at LIB-IFACE-SPLIT).
//
// mh_hostapi_selftest_table() / mh_hostapi_selftest_tact_table() are the GENERATED tables
// (mh_hostapi_selftest.gen.cpp): notify entries are return-default no-ops, required entries
// call mh_hostapi_trap(name) before returning a default -- a required entry reached without
// a real host impl is caught BY NAME, never silently (the done_when's "a test that names
// it"). A shared entry uses the same stub in both tables.
//
// The trap always prints and counts; run_hostapitest() reads the capture to assert the
// naming, and the suite driver treats a nonzero trap count at suite end as a failure.
#pragma once

#include "../libmh/include/libmh_host_api.gen.h"
#include "../libmh/include/libmh_tact_host_api.gen.h"

const libmh_host_api      &mh_hostapi_selftest_table();
const libmh_tact_host_api &mh_hostapi_selftest_tact_table();

// Prints "[hostapi] REQUIRED entry <name> called with no host impl", records it below.
// OUTSIDE capture mode the trap is FATAL (exit 1) -- a suite that reaches a required entry
// without a real host impl fails at that moment, named. hostapitest turns capture on to
// assert the naming without dying.
void mh_hostapi_trap(const char *name);
void mh_hostapi_trap_set_capture(int on);

// Capture state for assertions: total trap count since start (or last reset) + last name.
int         mh_hostapi_trap_count();
const char *mh_hostapi_last_trap(); // "" until the first trap
void        mh_hostapi_trap_reset();

int run_hostapitest();
