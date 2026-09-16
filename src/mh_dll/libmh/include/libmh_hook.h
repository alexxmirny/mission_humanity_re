/* libmh_hook.h -- the HOOK-SERVICE ABI: libmh's whole outbound edge, in one named table.
 * Fork F4D-PRE (docs/dll-split.md). NOT generated: five rows, each
 * hand-justified, and a generator over five rows would hide the justification rather than derive it.
 *
 * WHAT THIS REPLACES. Before F4D-PRE the migrated modules reached the injection harness DIRECTLY:
 * eight module TUs carried `#include "hook/..."` / `#include "include/mh_harness_export.h"` inside
 * `#ifndef MH_LIBMH_BUILD`, each with its own hand-written `#else` stub answering a per-TU absent
 * value. Measured at the item's own HEAD (dumpbin over the 627 roster objects of the HOSTED build,
 * src/mh_dll/mh/Debug): FIVE unresolved project externals, and they are these five rows --
 *
 *     ?entry_owner_of@hook@mh@@YAPBDI@Z                    6 TUs
 *     ?install_export_ok@hook@mh@@YA_NIPAXPBD_K@Z         22 TUs (reached only through the
 *                                                          MH_EXPORT_REPLACE macro -- invisible to
 *                                                          any source scan, which is why the gate
 *                                                          that guards this file reads OBJECTS)
 *     ?install_trampoline@hook@mh@@YA_NIPAXPAPAXHW4entry_claim@12@PBDI@Z   1 TU
 *     _MH_Harness_RebindLandPlayers                        1 TU
 *     _MH_Harness_WantsWallclockPin                        1 TU
 *
 * THE SHAPE IS F4B's, WITH THE ARROW REVERSED. mh_net.dll is a table mh.dll fills from a module it
 * loaded; this is a table the HOST fills for a module it hosts -- the same X-macro expanded several
 * ways, the same version handshake, the same unbound walk, and the same rule that every row states
 * what it answers when nothing is bound. It is the third such table beside libmh_host_api (the sim
 * host callbacks) and libmh_host_in (the inbound surface), and it is deliberately SEPARATE from
 * them: those two are generated from the call ledger and carry original-game callees; these five
 * are HARNESS services, which no ledger row describes.
 *
 * WHAT F4D FLIPS, AND WHAT IT DOES NOT. Today libmh is compiled into mh.dll, so the binding is an
 * ordinary same-image call: mh.dll fills the table in MH_Core_Arm_Early, beside the two host-api
 * binds it already does. At F4D, libmh becomes a DLL and `libmh_set_hook_api` becomes one more row
 * of its export contract -- mh.dll resolves it with GetProcAddress and calls it with the same
 * struct. NOT ONE CALL SITE IN THE 627-TU ROSTER CHANGES, because no call site names the host: they
 * all go through libmh/state/hook_api.h. That property is the whole point of landing this before F4D
 * rather than inside it.
 *
 * ABSENCE IS A CONFIGURATION, NOT A FAILURE, and that is the one way this table differs from
 * libmh_host_api, whose accessor aborts when unbound. A standalone libmh (LIB-REF, and any future
 * embedding host) has no original binary to patch and no determinism harness to ask; each row below
 * therefore names the answer it gives unbound, and every one of those answers is the value the
 * per-TU `#ifdef MH_LIBMH_BUILD` stub this file replaces was already giving. So the behaviour of a
 * standalone build is unchanged BY CONSTRUCTION rather than by re-derivation.
 */
#ifndef LIBMH_HOOK_H
#define LIBMH_HOOK_H

#include <stdint.h>

/* Bumped when a row is added, removed, or its signature changes. libmh_set_hook_api refuses a
 * value it was not compiled with, so a host built against an older header is rejected by name
 * rather than calling through a struct whose layout it disagrees about. */
#define LIBMH_HOOK_API_VERSION     0xF4D00001u
#define LIBMH_HOOK_API_ENTRY_COUNT 5u

/* The ONE list. Expanded three ways -- the struct below, the name table in state/hook_api.cpp
 * (the unbound walk's naming source), and tools/check_libmh_outbound.py's parse -- so a row cannot
 * exist in one expansion and be missing from another. */
#define LIBMH_HOOK_API_FOR_EACH(X) \
    X(entry_owner_of)              \
    X(install_export_ok)           \
    X(install_trampoline)          \
    X(harness_rebind_land_players) \
    X(harness_wants_wallclock_pin)

/* mh::hook::entry_claim, mirrored into the C surface. The enum itself lives in hook/detour.h,
 * which module code must not include -- that is the edge this file exists to cut -- so the two
 * values cross as a uint32_t and seams/libmh_hook_host.cpp static_asserts that each constant still
 * equals its enumerator. A renumbering therefore fails to COMPILE rather than silently promoting a
 * detour that should have been refused. */
#define LIBMH_ENTRY_CLAIM_EXCLUSIVE 0u
#define LIBMH_ENTRY_CLAIM_REBIND    1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libmh_hook_api {
    /* Who OWNS this entry, or null when nothing holds it exclusively (C9(c)). Module code asks in
     * one place only: the refusal path of a [promote] installer, to say "a detour holds this entry"
     * instead of the flat "entry guard mismatch", which needs a different action from the reader.
     * UNBOUND ANSWER: null -- "unowned", so the refusal degrades to the flat form. That is exactly
     * what the six per-TU stubs answered. */
    const char *(*entry_owner_of)(uintptr_t entry);

    /* Install `thunk` AS the game function at `target`, guarded by its expected first eight entry
     * bytes; non-zero on success, zero on a refusal the HOST has already logged. Reached only
     * through MH_EXPORT_REPLACE (addr/mh_export.gen.h), which is why 22 roster TUs depend on it and
     * none of them names it.
     * UNBOUND ANSWER: 0 -- refuse. A host with no original binary cannot patch an entry, and the
     * standalone arm of the macro already returns false without even calling this. */
    int (*install_export_ok)(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8);

    /* Steal `stolen` prologue bytes into an executable thunk published at *tramp_out and patch the
     * entry to `detour`; non-zero on success. `claim` is LIBMH_ENTRY_CLAIM_*; `who` names the
     * installer in the interlock's registry; `expect_prologue` 0 means the caller compared the entry
     * bytes itself. One module site: save_live.cpp's verify-mode oracle, which needs the ORIGINAL to
     * stay reachable and so cannot use the plain entry patch.
     * UNBOUND ANSWER: 0 -- refuse, for the same reason as install_export_ok. */
    int (*install_trampoline)(uintptr_t target, void *detour, void **tramp_out, int stolen,
                              uint32_t claim, const char *who, uint32_t expect_prologue);

    /* C10: ask the determinism harness to rebind its llm_strat_land_players_on_planet detour's
     * fall-through to `ours`; non-zero if it did. The harness owns that entry when it arms, so a
     * second entry patch would be the losing half of an instrument race (the C4/C6 protocol).
     * UNBOUND ANSWER: 0 -- "no harness rebound it", which sends resid_promote down its ordinary
     * install path. The per-TU stub answered 0. */
    int (*harness_rebind_land_players)(void *ours);

    /* Non-zero when the determinism harness is pinning the wall clock, i.e. when a promoted
     * time_GetCurrentTime must yield the entry to the harness's own pin rather than take it.
     * UNBOUND ANSWER: 0 -- no harness exists, so there is no pin to yield to. */
    int (*harness_wants_wallclock_pin)(void);
} libmh_hook_api;

/* Hand the table to libmh. 0 = bound, -1 = version mismatch, -2 = null table; on failure the
 * previously bound table (if any) is KEPT, so a rejected handshake cannot leave the surface
 * half-bound. Idempotent: binding twice with the same table is a no-op re-store.
 *
 * -3 = THIS BUILD HAS NO INJECTION HARNESS, and only a STANDALONE libmh (MH_LIBMH_BUILD) ever
 * answers it. There is no game image there, so no table could be honoured; every accessor is an
 * inline constant answering the absent value documented above. Returning -3 rather than 0 keeps a
 * standalone embedder from believing it installed something. */
int libmh_set_hook_api(const libmh_hook_api *api, uint32_t api_version);

/* Walk the bound table and call on_unbound(name) per NULL slot; returns the NULL count, or -1 when
 * no table is bound at all. -1 is DISTINCT from 0 on purpose: "nobody bound anything" and "a host
 * bound a complete table" are different facts, and collapsing them is the conflation F3B spent an
 * item undoing one level up. on_unbound may be null (count only). */
int libmh_hook_api_unbound(void (*on_unbound)(const char *name));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBMH_HOOK_H */
