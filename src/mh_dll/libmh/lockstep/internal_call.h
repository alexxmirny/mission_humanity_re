#pragma once

#include "addr/mh_calls.gen.h"

#ifndef MH_INTERNAL_EDGES_ENTRY_ROUTED
#define MH_INTERNAL_EDGES_ENTRY_ROUTED 0
#endif

namespace mh::lockstep::detail {

// `entry` and `ours` MUST have the same type -- that is the whole point, see the header comment.
template <class F>
constexpr F internal_edge(F entry, F ours) {
#if MH_INTERNAL_EDGES_ENTRY_ROUTED
    (void)ours;
    return entry;
#else
    (void)entry;
    return ours;
#endif
}

// The standalone half of the same check: `F` is deduced from the wrapper's TYPE at the call site and
// `ours` must convert to it exactly, with no wrapper emitted.
template <class F>
constexpr F internal_edge_direct(F ours) {
    return ours;
}

} // namespace mh::lockstep::detail

// MH_INTERNAL_CALL(FN, OURS) -- yields a function pointer for a `live_*_calls()` slot.
//   FN   : the game function's name, as spelled in mh::call (mh/addr/mh_calls.gen.h)
//   OURS : our production entry point for that same function, fully qualified
//
// KEEP EACH ARM ON ONE LINE. gen_va_census.py skips lines that START with `#`, which is what stops a
// `#define`'s `mh::call::PARAM` being counted as a call site -- so a wrapped continuation line puts
// the token where the census reads it as a real site with the callee literally named `FN`. That is
// not hypothetical: it happened the moment this arm was first written across two lines, and the
// census reported `lockstep/internal_call.h:110:FN  callee absent from libmh_call_census.json`.
// .clang-format has ColumnLimit: 0, so length is not a reason to wrap it.
#ifdef MH_LIBMH_BUILD
#define MH_INTERNAL_CALL(FN, OURS) (::mh::lockstep::detail::internal_edge_direct<decltype(&::mh::call::FN)>(&(OURS)))
#else
#define MH_INTERNAL_CALL(FN, OURS) (::mh::lockstep::detail::internal_edge(&::mh::call::FN, &(OURS)))
#endif
