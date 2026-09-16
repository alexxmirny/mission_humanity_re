// seams/libmh_hook_host.cpp -- mh.dll's implementation of the HOOK-SERVICE ABI (fork F4D-PRE).
//
// Five forwarders and a bind. This is the whole of what libmh's outbound edge became: before this
// item the 627-TU roster reached mh::hook:: and MH_Harness_ directly from eight of its TUs, each
// carrying its own `#ifdef MH_LIBMH_BUILD` absent-answer stub; now it reaches them through one
// table whose rows are declared once (libmh/include/libmh_hook.h) and filled once, here.
//
// WHY THE FORWARDERS ARE WRITTEN OUT RATHER THAN THE ADDRESSES TAKEN DIRECTLY. Three of the five
// have C++ signatures the C table cannot spell -- `bool`, a defaulted argument, and
// `mh::hook::entry_claim` -- so each needs a one-line adapter anyway. Writing all five the same way
// keeps the table's shape uniform and puts the two static_asserts that pin the claim encoding next
// to the one row that uses it.
//
// NOT a libmh TU: it lives in seams/, which gen_libmh_vcxproj.py does not scan, and it names
// hook/ headers freely because it IS the harness side of the boundary.
#include "include/mh_libmh_hook_bind.h"

#include "hook/detour.h"               // install_trampoline, entry_claim
#include "hook/export.h"               // install_export_ok
#include "hook/promoted.h"             // entry_owner_of
#include "include/mh_harness_export.h" // MH_Harness_RebindLandPlayers / _WantsWallclockPin
#include "state/hook_api.h"            // libmh_set_hook_api -- libmh's side of the same contract

namespace {

// The claim encoding crosses the boundary as a uint32_t (libmh_hook.h says why). Pin it here: a
// renumbering of the enum fails to COMPILE instead of silently turning an `exclusive` claim into a
// `rebind` one, which is the difference between a refused install and a promoted body.
static_assert(static_cast<uint32_t>(mh::hook::entry_claim::exclusive) == LIBMH_ENTRY_CLAIM_EXCLUSIVE,
              "LIBMH_ENTRY_CLAIM_EXCLUSIVE no longer matches mh::hook::entry_claim::exclusive");
static_assert(static_cast<uint32_t>(mh::hook::entry_claim::rebind) == LIBMH_ENTRY_CLAIM_REBIND,
              "LIBMH_ENTRY_CLAIM_REBIND no longer matches mh::hook::entry_claim::rebind");

const char *fwd_entry_owner_of(uintptr_t entry) {
    return mh::hook::entry_owner_of(entry);
}

int fwd_install_export_ok(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8) {
    return mh::hook::install_export_ok(target, thunk, name, expect_entry8) ? 1 : 0;
}

int fwd_install_trampoline(uintptr_t target, void *detour, void **tramp_out, int stolen,
                           uint32_t claim, const char *who, uint32_t expect_prologue) {
    return mh::hook::install_trampoline(target, detour, tramp_out, stolen,
                                        static_cast<mh::hook::entry_claim>(claim), who,
                                        expect_prologue)
               ? 1
               : 0;
}

int fwd_harness_rebind_land_players(void *ours) {
    return MH_Harness_RebindLandPlayers(ours);
}

int fwd_harness_wants_wallclock_pin(void) {
    return MH_Harness_WantsWallclockPin();
}

// Designated initialisers would be the readable form, but the struct is asserted to be a FLAT array
// of function pointers (state/hook_api.cpp) and positional init is what makes a missing row a
// compile-time shape error rather than a silently null slot.
const libmh_hook_api k_table = {
    fwd_entry_owner_of,
    fwd_install_export_ok,
    fwd_install_trampoline,
    fwd_harness_rebind_land_players,
    fwd_harness_wants_wallclock_pin,
};

} // namespace

namespace mh::hostapi {

const libmh_hook_api &mhdll_hook_table() {
    return k_table;
}

} // namespace mh::hostapi

extern "C" int MH_LibMH_BindHookApi(void) {
    return libmh_set_hook_api(&mh::hostapi::mhdll_hook_table(), LIBMH_HOOK_API_VERSION);
}
