//
// state/promoted_select.h -- which body a PROMOTED callee's call site gets (LIB-REF-SPLIT).
//
// One macro, and the whole of the standalone-vs-hosted choice for the `promoted` class of outward
// call goes through it. It is crt/crt_select.h's sibling in shape and in reason; read that file's
// header for the argument, because this is the same argument about a different population.
//
//     MH_PROMOTED(llm_net_send_order, ::mh::orders::send_order)
//                                hosted     -> ::mh::call::llm_net_send_order  (the ORIGINAL ENTRY,
//                                              which a promoted run has already E9'd into our body --
//                                              so hosted this site is unchanged, to the token)
//                                standalone -> ::mh::orders::send_order        (our body, directly)
//
// ---- WHY THE CLASS EXISTS AT ALL ---------------------------------------------------------------
//
// A `promoted`-class site is one whose callee WE HAVE ALREADY REIMPLEMENTED AND PROMOTE. Hosted, the
// site is not really a dependency: the entry patch lands the call in our own body, which is why
// gen_va_census.py routes these 22 sites to a class whose `why` says "the site is a VA dependency
// ONLY in the standalone configuration". Standalone there is no entry to patch and no bytes at that
// address, so the same site is a dangling pointer into nothing. The endgame plan D-E3(b) states the
// resolution in one line: for an `ours-already` callee the standalone build must "flip the
// calls-struct binding to the direct C++ body", not merely tolerate the VA.
//
// ---- WHY NOT MH_LIBMH_BIND, WHICH LOOKS LIKE IT ALREADY DOES THIS ------------------------------
//
// Because it would change the HOSTED build, and this item's acceptance forbids that. MH_LIBMH_BIND
// makes its callee a REBIND ROW, and a row's hosted arm is R11-gated with
// `SHIP_REBIND_DEFAULT = 1` (seams/net_internal.h, user's call 2026-09-04) -- so converting a site
// to the binder does not leave the shipping build alone: it makes it call our body directly instead
// of reaching it through the patched entry, which for a callee whose SHIP_PROMOTE_* key is off is a
// different body altogether. LIB-REBIND-UI had to run both rig halves for exactly that reason. This
// macro's hosted arm expands to the token sequence the site already had, so there is nothing to
// re-prove on the hosted side.
//
// ---- WHY THE STANDALONE ARM STILL TYPE-CHECKS --------------------------------------------------
//
// `decltype(&::mh::call::FN)` is an unevaluated operand: it names the generated wrapper's type
// without odr-using it, so no wrapper is emitted and no VA reaches the object -- verified by
// compiling both spellings and byte-scanning the results -- while `checked<F>` still refuses a body
// whose signature has drifted from the committed original. Without that the flip would be a cast in
// all but name. (The hosted arm carries no such check, because the site has never had one: it names
// the wrapper and nothing else.)
//
// ---- WHAT THIS MEANS FOR THE CENSUS ------------------------------------------------------------
//
// `MH_PROMOTED(x, y)` contains no `mh::call::` token at the site, so a converted site leaves
// gen_va_census.py by construction -- the same mechanism, and the same deliberate property, that
// crt/crt_select.h's header describes for MH_CRT. The HOSTED measure must not lose them, so
// gen_libmh_calls.py counts MH_PROMOTED sites into its own channel beside `bound` and `vendored`,
// and the census reconciliation identity closes over that channel.
//
// ---- MH_PROMOTED_ROW: the same choice, spelled so it CANNOT disagree with the binder -----------
//
// Eleven of the twelve callees are already REBIND ROWS, and the standalone binder has a settled
// answer for each: `MH_REBIND_TARGET_<fn>`, derived from the seam by gen_libmh_rebind.py's R2 and
// type-checked against the committed original signature by one static_assert per row in
// state/rebind_verify.gen.cpp. Naming the body again here would create a second opinion about which
// body a standalone build reaches for that callee -- and two standalone routes to one callee landing
// in two different bodies is a worse failure than the VA this file removes. So the row case takes
// the binder's own target by macro rather than by transcription, and only the one callee that is not
// a row (llm_strat_order_pending_enqueue) names its body at the site.
#pragma once

#include "addr/mh_calls.gen.h"
#include "addr/mh_rebind.gen.h"       // MH_REBIND_TARGET_<fn>
#include "state/rebind_targets.gen.h" // ... and the declarations for them

namespace mh::promoted {

// `F` is deduced from the generated wrapper's TYPE at the call site; `ours` must convert to it
// exactly. Nothing here odr-uses the wrapper.
template <class F>
constexpr F checked(F ours) {
    return ours;
}

} // namespace mh::promoted

// MH_PROMOTED(FN, OURS) -- yields a callable for a promoted callee.
//   FN   : the game function's name, as spelled in mh::call (mh/addr/mh_calls.gen.h)
//   OURS : our public live wrapper for that same function, fully qualified. The PUBLIC wrapper,
//          never a promoted_arm/rebind_arm adapter -- the libmh rebind notes R1, same rule and same
//          reason (the arms carry oracle diagnostics that do not belong on a shipping path).
//
// Usable both ways, because the 22 sites need both: `&MH_PROMOTED(f, g)` for a calls-struct member
// and `MH_PROMOTED(f, g)(args)` for an inline call.
#ifdef MH_LIBMH_BUILD
#define MH_PROMOTED(FN, OURS) (*::mh::promoted::checked<decltype(&::mh::call::FN)>(&(OURS)))
#else
#define MH_PROMOTED(FN, OURS) ::mh::call::FN
#endif

// MH_PROMOTED_ROW(FN) -- the row case: the body is whatever the standalone binder binds for FN.
#define MH_PROMOTED_ROW(FN) MH_PROMOTED(FN, MH_REBIND_TARGET_##FN)
