// libmh's host-callback table binding (LIB-ABI stage B; sim/tact split at
// LIB-IFACE-SPLIT). See host_api.h.
#include "state/host_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fp/x87.h" // CRT-X87-CPP: install_pc53 -- the PC=53 ABI guarantee (see libmh.h)

namespace {

const libmh_host_api      *g_host_api      = nullptr;
const libmh_tact_host_api *g_tact_host_api = nullptr;

// Entry names in struct order, derived from each table's generated X-macro -- the
// unbound-walks' naming source. The flat layout (COUNT function pointers, no padding) is
// compile-time asserted in hostapi_c_compile.c, which is what makes the slot walk legal.
const char *const k_entry_names[] = {
#define X(name, category, notify) #name,
    LIBMH_HOST_API_FOR_EACH(X)
#undef X
};
const char *const k_tact_entry_names[] = {
#define X(name, category, notify) #name,
    LIBMH_TACT_HOST_API_FOR_EACH(X)
#undef X
};

static_assert(sizeof(k_entry_names) / sizeof(k_entry_names[0]) == LIBMH_HOST_API_ENTRY_COUNT,
              "X-macro list disagrees with LIBMH_HOST_API_ENTRY_COUNT");
static_assert(sizeof(libmh_host_api) == LIBMH_HOST_API_ENTRY_COUNT * sizeof(void (*)(void)),
              "libmh_host_api is not a flat array of function pointers");
static_assert(sizeof(k_tact_entry_names) / sizeof(k_tact_entry_names[0]) ==
                  LIBMH_TACT_HOST_API_ENTRY_COUNT,
              "tact X-macro list disagrees with LIBMH_TACT_HOST_API_ENTRY_COUNT");
static_assert(sizeof(libmh_tact_host_api) ==
                  LIBMH_TACT_HOST_API_ENTRY_COUNT * sizeof(void (*)(void)),
              "libmh_tact_host_api is not a flat array of function pointers");

// One walk, two tables: both structs are flat pointer arrays (asserted above), so the walk
// is table-agnostic given the base, the count, and that table's own name list.
int walk_unbound(const void *table, uint32_t count, const char *const *names,
                 void (*on_unbound)(const char *name)) {
    if (table == nullptr) return -1;
    int         unbound = 0;
    const char *slots   = static_cast<const char *>(table);
    for (uint32_t i = 0; i < count; ++i) {
        void *entry;
        std::memcpy(&entry, slots + i * sizeof(void *), sizeof(entry));
        if (entry == nullptr) {
            ++unbound;
            if (on_unbound) on_unbound(names[i]);
        }
    }
    return unbound;
}

} // namespace

// CRT-X87-CPP: the PC=53 ABI guarantee. The contract prose is in libmh.h; the mechanism and the
// measured reason it must be INSTALLED rather than inherited are at mh::fp::install_pc53.
extern "C" int libmh_install_fp_precision(void) { return mh::fp::install_pc53(); }

extern "C" int libmh_set_host_api(const libmh_host_api *api, uint32_t api_version) {
    if (api_version != LIBMH_HOST_API_VERSION) return -1;
    if (api == nullptr) return -2;
#ifdef MH_LIBMH_BUILD
    // STANDALONE ONLY, and the asymmetry is the point. This is the first entry any host binds
    // through, so installing here means a host cannot forget the guarantee -- but inside mh.exe the
    // process already runs at PC=53 on every measured path, and a hosted arm that writes the control
    // word would be changing shipping behaviour to obtain something it already has. Hosted therefore
    // inherits (verified), standalone installs (guaranteed). Placed AFTER the two refusal checks so
    // a rejected handshake does not leave the FP environment altered.
    libmh_install_fp_precision();
#endif
    g_host_api = api;
    return 0;
}

extern "C" int libmh_set_tact_host_api(const libmh_tact_host_api *api, uint32_t api_version) {
    if (api_version != LIBMH_TACT_HOST_API_VERSION) return -1;
    if (api == nullptr) return -2;
    g_tact_host_api = api;
    return 0;
}

extern "C" int libmh_host_api_unbound(void (*on_unbound)(const char *name)) {
    return walk_unbound(g_host_api, LIBMH_HOST_API_ENTRY_COUNT, k_entry_names, on_unbound);
}

extern "C" int libmh_tact_host_api_unbound(void (*on_unbound)(const char *name)) {
    return walk_unbound(g_tact_host_api, LIBMH_TACT_HOST_API_ENTRY_COUNT, k_tact_entry_names,
                        on_unbound);
}

// ---- the pushed session seed (LIFT-TABLE S5, 2026-09-09) ------------------------------------
//
// llm_strat_rng_seed_wallclock_seconds left the host table here: a wall-clock read is not a service
// libmh needs pulled per session, and pulling it is what lets two multiplayer peers seed from two
// different clocks. The host pushes one number; libmh reads its own slot. Full reasoning, including
// the two declared divergences from the original, is at the declaration in libmh/include/libmh.h.
namespace {
int32_t g_session_seed = 0;
}

extern "C" void libmh_set_session_seed(int32_t wallclock_seconds) {
    g_session_seed = wallclock_seconds;
}

namespace mh::state {

const libmh_host_api *host_api_or_null() {
    return g_host_api;
}

int32_t session_seed() {
    return g_session_seed;
}

} // namespace mh::state

namespace mh {

const libmh_host_api &host() {
    if (g_host_api == nullptr) {
        // Boot-order bug: a module path ran before the host bound its table. Fail loudly and
        // immediately -- limping on through null function pointers would crash namelessly.
        std::fputs("[hostapi] FATAL: mh::host() called before libmh_set_host_api()\n", stderr);
        std::abort();
    }
    return *g_host_api;
}

const libmh_tact_host_api &tact_host() {
    if (g_tact_host_api == nullptr) {
        std::fputs("[hostapi] FATAL: mh::tact_host() called before libmh_set_tact_host_api()\n",
                   stderr);
        std::abort();
    }
    return *g_tact_host_api;
}

} // namespace mh
