// libmh's side of the HOOK-SERVICE ABI (fork F4D-PRE). See state/hook_api.h.
//
// Deliberately tiny and deliberately branchy-per-call rather than defaulting the slots to stub
// functions at construction: a null slot is the ONE representation of "no host answered for this
// row", and libmh_hook_api_unbound() has to be able to see it. Pre-filling with stubs would make
// the unbound walk report a complete table and turn the gap this file exists to surface into
// something only a reader could find.
#include "state/hook_api.h"

// ---- THE STANDALONE ARM (LIB-REF-SPLIT) ------------------------------------------------------
//
// Under MH_LIBMH_BUILD the six accessors are inline constants in the header -- see the long note
// there and the LIB-VA0 measurement behind it -- so this file has nothing to define. The C
// entry points still exist, because the symbol has to resolve for any host that links the archive
// and because /WHOLEARCHIVE would otherwise fail on an object with no definitions at all; what
// changes is the ANSWER. A standalone host that tries to bind a table is REFUSED by name (-3)
// rather than accepted into a table nothing will ever read: there is no game image to patch, so a
// bind that returned 0 would be the quiet lie this project keeps rediscovering.
#ifdef MH_LIBMH_BUILD

extern "C" int libmh_set_hook_api(const libmh_hook_api *, uint32_t) {
    return -3; // no injection harness exists in a standalone libmh
}

extern "C" int libmh_hook_api_unbound(void (*)(const char *)) {
    return -1; // the documented "no table is bound at all", and none can be
}

#else

#include <cstring>

namespace {

const libmh_hook_api *g_hook_api = nullptr;

// Row names in struct order, from the one X-macro -- the unbound walk's naming source, exactly as
// host_api.cpp derives its own. The flat layout (COUNT function pointers, no padding) is asserted
// below, which is what makes the slot walk legal.
const char *const k_row_names[] = {
#define X(name) #name,
    LIBMH_HOOK_API_FOR_EACH(X)
#undef X
};

static_assert(sizeof(k_row_names) / sizeof(k_row_names[0]) == LIBMH_HOOK_API_ENTRY_COUNT,
              "X-macro list disagrees with LIBMH_HOOK_API_ENTRY_COUNT");
static_assert(sizeof(libmh_hook_api) == LIBMH_HOOK_API_ENTRY_COUNT * sizeof(void (*)(void)),
              "libmh_hook_api is not a flat array of function pointers");

} // namespace

extern "C" int libmh_set_hook_api(const libmh_hook_api *api, uint32_t api_version) {
    if (api_version != LIBMH_HOOK_API_VERSION) return -1;
    if (api == nullptr) return -2;
    g_hook_api = api;
    return 0;
}

extern "C" int libmh_hook_api_unbound(void (*on_unbound)(const char *name)) {
    if (g_hook_api == nullptr) return -1;
    int         unbound = 0;
    const char *slots   = reinterpret_cast<const char *>(g_hook_api);
    for (uint32_t i = 0; i < LIBMH_HOOK_API_ENTRY_COUNT; ++i) {
        void *entry;
        std::memcpy(&entry, slots + i * sizeof(void *), sizeof(entry));
        if (entry == nullptr) {
            ++unbound;
            if (on_unbound) on_unbound(k_row_names[i]);
        }
    }
    return unbound;
}

namespace mh::hosthook {

bool bound() {
    return g_hook_api != nullptr;
}

const char *entry_owner_of(uintptr_t entry) {
    if (g_hook_api == nullptr || g_hook_api->entry_owner_of == nullptr) return nullptr;
    return g_hook_api->entry_owner_of(entry);
}

bool install_export_ok(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8) {
    if (g_hook_api == nullptr || g_hook_api->install_export_ok == nullptr) return false;
    return g_hook_api->install_export_ok(target, thunk, name, expect_entry8) != 0;
}

bool install_trampoline(uintptr_t target, void *detour, void **tramp_out, int stolen, uint32_t claim,
                        const char *who, uint32_t expect_prologue) {
    if (g_hook_api == nullptr || g_hook_api->install_trampoline == nullptr) return false;
    return g_hook_api->install_trampoline(target, detour, tramp_out, stolen, claim, who,
                                          expect_prologue) != 0;
}

int harness_rebind_land_players(void *ours) {
    if (g_hook_api == nullptr || g_hook_api->harness_rebind_land_players == nullptr) return 0;
    return g_hook_api->harness_rebind_land_players(ours);
}

int harness_wants_wallclock_pin() {
    if (g_hook_api == nullptr || g_hook_api->harness_wants_wallclock_pin == nullptr) return 0;
    return g_hook_api->harness_wants_wallclock_pin();
}

} // namespace mh::hosthook

#endif // MH_LIBMH_BUILD
