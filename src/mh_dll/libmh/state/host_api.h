// libmh's side of the host-callback ABI (LIB-ABI stage B; the endgame plan D-E3;
// sim/tact split at LIB-IFACE-SPLIT, 2026-09-10).
//
// The HOST (mh.dll's thunk-backed binder, the selftest host, later LIB-REF/Godot) fills the
// TWO generated tables -- sim (libmh_host_api) and tact (libmh_tact_host_api) -- and hands
// them in through libmh_set_host_api() / libmh_set_tact_host_api() before the sim steps.
// An entry both sides call exists in BOTH tables (duplication is the design; a host may
// even bind it differently per table). Module code reaches the host ONLY through the
// accessors below -- mh::host() for non-tact modules, mh::tact_host() for tact/ (the
// generator fails on a disagreement) -- never through mh::call:: for a
// host-callback-classed callee (stage C's lint enforces that), and never through the
// harness-side binder header (include/mh_hostapi_bind.h).
//
// This header + host_api.cpp live in the `state` module, i.e. INSIDE libmh: they know
// nothing about where the tables' entries point.
#pragma once

#include "../../libmh/include/libmh.h" // the C facade -- MH_VFS_READ/MH_VFS_WRITE live there
#include "../../libmh/include/libmh_host_api.gen.h"
#include "../../libmh/include/libmh_tact_host_api.gen.h"

extern "C" {
// Declared in libmh/include/libmh.h (the C facade); defined in host_api.cpp.
// Returns 0 on success, -1 on a version mismatch, -2 on a null table; on failure the
// previously bound table (if any) is kept. Each table has its own version constant --
// LIBMH_HOST_API_VERSION / LIBMH_TACT_HOST_API_VERSION -- and its own handshake.
int libmh_set_host_api(const libmh_host_api *api, uint32_t api_version);
int libmh_set_tact_host_api(const libmh_tact_host_api *api, uint32_t api_version);
// Walks the bound table and calls on_unbound(name) for every NULL entry; returns the count
// of NULL entries, or -1 if no table is bound at all. on_unbound may be null (count only).
// One walk per table -- a gap is named by the table that carries it.
int libmh_host_api_unbound(void (*on_unbound)(const char *name));
int libmh_tact_host_api_unbound(void (*on_unbound)(const char *name));
// The pushed session seed (LIFT-TABLE S5) -- declared in libmh/include/libmh.h, defined in
// host_api.cpp. Lives beside the table binding because it is the same kind of thing: a value the
// host hands libmh once, before the sim runs.
void libmh_set_session_seed(int32_t wallclock_seconds);
}

namespace mh::state {

// The bound table, or null before libmh_set_host_api() succeeds. Callers on a path that can
// legitimately run before binding must null-check; the sim spine may assume it is bound
// (the arm-time startup check refuses to arm on a failed bind).
const libmh_host_api *host_api_or_null();

// The wall-clock seconds the host pushed through libmh_set_session_seed(), or 0. Read by
// sim/resid/sim_planet_session_begin.cpp where the original called out to
// llm_strat_rng_seed_wallclock_seconds -- see that entry point's banner in libmh.h for why it is a
// pushed parameter and not a pulled service.
int32_t session_seed();

} // namespace mh::state

namespace mh {

// The bound host tables, fail-fast: each aborts with a named message if no host has bound
// it. Both hosts bind both tables before any module code runs (mh.dll at arm time,
// net_selftest in main), so a hit here is a boot-order bug, not a recoverable condition.
// These are THE way module code reaches a host-callback-classed callee -- mh::call:: for
// one is a lint error (stage C; gen_libmh_calls) -- and the accessor is table-selecting:
// host() is the sim table (non-tact modules), tact_host() the tact table (tact/ only;
// gen_libmh_hostapi fails an accessor used from the wrong module).
const libmh_host_api      &host();
const libmh_tact_host_api &tact_host();

} // namespace mh
