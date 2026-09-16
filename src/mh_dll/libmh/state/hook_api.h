// libmh's side of the HOOK-SERVICE ABI (fork F4D-PRE). The sibling of state/host_api.h: that one
// is how module code reaches the host's CALLBACKS, this one is how it reaches the host's HOOK
// services -- entry ownership, the two entry installers, and the two determinism-harness questions.
//
// THESE FIVE ACCESSORS ARE THE ONLY WAY MODULE CODE MAY REACH THE HARNESS. A libmh module TU must
// not include hook/, seams/, effects/, shadow/ or include/mh_*_export.h at all -- guarded or not.
// Before F4D-PRE the rule was "guarded is fine" (lint_libmh_layering) and eight TUs each carried
// their own `#ifdef MH_LIBMH_BUILD` stub; the guard made the edge invisible to a link-level reader
// and duplicated the absent-answer decision eight times. check_libmh_outbound.py now enforces the
// stronger rule, in both a source arm and an object arm.
//
// UNLIKE mh::host(), NONE OF THESE ABORTS WHEN UNBOUND. Absence is a shipped configuration (a
// standalone libmh has no original to patch and no harness to ask), so each accessor answers the
// value libmh_hook.h documents for its row -- which is the value the per-TU stub it replaces gave.
// The trade is deliberate and stated: a host that forgets to bind gets degraded promotion rather
// than a named abort, so the binding is asserted by the gate and by a selftest instead of by a
// crash. mh.dll binds in MH_Core_Arm_Early (ahead of every installer); net_selftest binds in main(),
// and `net_selftest.exe hostapitest` asserts both the bound and the unbound answers.
//
// This header + hook_api.cpp live in the `state` module, i.e. INSIDE libmh.
#pragma once

#include <cstdint>

#include "../../libmh/include/libmh_hook.h"

#ifdef MH_LIBMH_BUILD
// ---- THE STANDALONE ARM (LIB-REF-SPLIT), and it exists for a MEASURED reason ------------------
//
// The LIB-REF artifact has no game image, so it has no injection harness and no host can ever bind
// a meaningful table: mh_export.gen.h's own standalone arm already refuses every install without
// generating a thunk to install. Every row's answer standalone is therefore its ABSENT answer,
// permanently -- and these inline constants are that, folded at the call site.
//
// WHY THEY ARE INLINE CONSTANTS RATHER THAN THE TABLE LOOKUP. LIB-VA0's byte ratchet
// (tools/scan_libmh_vas.py) counts ORIGINAL VAs left in the built archive, and it caught this in
// one run: with an out-of-line accessor, `entry_owner_of(mh::exp::addr_llm_strat_order_queue_dispatch)`
// in sim_order_dispatch.cpp must MATERIALISE 0x00466892 as an immediate to pass it -- a VA the
// standalone lib had never carried. The eight per-TU stubs this item deleted were each inline for
// the same reason without saying so; ONE inline arm here says it once, which is the whole shape of
// the change (one decision, not eight copies).
//
// THIS IS NOT THE GUARD THE ITEM REMOVED. That one was a harness INCLUDE -- a module TU reaching
// hook/ and include/mh_harness_export.h, which is what made the edge exist in the HOSTED build F4D
// ships. This arm reaches nothing: it is the same header, answering the same documented values,
// with no dependency on either side of it.
namespace mh::hosthook {
inline bool        bound() { return false; }
inline const char *entry_owner_of(uintptr_t) { return nullptr; }
inline bool        install_export_ok(uintptr_t, void *, const char *, uint64_t) { return false; }
inline bool        install_trampoline(uintptr_t, void *, void **, int, uint32_t, const char *, uint32_t) {
    return false;
}
inline int harness_rebind_land_players(void *) { return 0; }
inline int harness_wants_wallclock_pin() { return 0; }
} // namespace mh::hosthook
#else
namespace mh::hosthook {

// True once a host has bound a table. The gate and the selftest read it; module code does not need
// to -- every accessor below already answers correctly unbound.
bool bound();

// C9(c): who exclusively owns `entry`, or nullptr. Unbound: nullptr.
const char *entry_owner_of(uintptr_t entry);

// Install `thunk` as the game function at `target`. Unbound: false.
bool install_export_ok(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8);

// Steal the prologue and patch the entry to `detour`. `claim` is LIBMH_ENTRY_CLAIM_*.
// Unbound: false.
bool install_trampoline(uintptr_t target, void *detour, void **tramp_out, int stolen, uint32_t claim,
                        const char *who, uint32_t expect_prologue);

// C10: the harness's land_players rebind. Unbound: 0.
int harness_rebind_land_players(void *ours);

// Is the determinism harness pinning the wall clock? Unbound: 0.
int harness_wants_wallclock_pin();

} // namespace mh::hosthook
#endif // MH_LIBMH_BUILD
