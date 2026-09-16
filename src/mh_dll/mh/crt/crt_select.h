//
// crt/crt_select.h -- which CRT a call site gets (LIB-CRT).
//
// Two macros, and the whole of the standalone-vs-hosted CRT choice goes through them. `MH_CRT` is
// the call, `MH_CRT_CMP` (below) is the qsort comparator, which reaches the original as data rather
// than as a call and so needs its own arm. The vendored bodies are crt_sprintf.h (the format family),
// crt_string.h (byte/string ops), crt_math.h (the x87 leaves), crt_heap.h (the allocator),
// crt_rand.h (the cosmetic tactical LCG) and crt_qsort.h (the sort whose tie order is contractual).
//
//
//     MH_CRT(w_sprintf__vss)     hosted     -> ::mh::call::w_sprintf__vss  (the binary's own Watcom
//                                             CRT, reached as a VA -- bit-exact and free, and the
//                                             configuration the working game keeps forever)
//                                standalone -> ::mh::crt::w_sprintf__vss   (crt/crt_sprintf.h)
//
// WHY A MACRO RATHER THAN A NAMESPACE ALIAS. The two arms are not the same KIND of name: the hosted
// arm is an `inline` function template instantiation in addr/mh_calls.gen.h whose body embeds a
// literal VA, the standalone arm is an ordinary function. A `namespace mh_crt = ...` alias would bind
// both to whichever namespace won, and the generated header is regenerated from the DB -- so an alias
// would also need the generator to know about the split. The macro keeps the choice in ONE file that
// the generator never touches.
//
// WHY THIS ALSO MOVES THE CENSUS. tools/gen_va_census.py routes a site by finding the text
// `mh::call::` in a libmh-destined module. `MH_CRT(x)` contains no such text, so a converted site
// leaves the census by construction rather than by a rule someone has to maintain -- which is the
// point: the census measures VA calls left in libmh, and after conversion there is not one here.
// The hosted build still calls the VA, and gen_libmh_calls.py's hosted measure still sees it.
//
// THE SIGNATURES MUST MATCH EXACTLY. crt_sprintf.h's shapes are written against
// addr/mh_calls.gen.h's, and `crttest` case A re-checks that by taking a function POINTER of each
// pair through one declared type -- so a regenerated header that changes a shape breaks the build
// rather than silently binding a different arity.
//
// ---- MH_CRT_CMP: the same choice for a comparator, which is NOT a call ---------------------------
//
// A qsort comparator reaches the original as DATA -- `(void *)mh::addr::<name>`, a raw VA literal
// cast to a pointer and handed to qsort. That spelling contains no `mh::call::` token, so the VA
// census could not see it and LIB-CRT's "no comparator is a VA" clause was measuring nothing. All
// four comparators the two live qsort sites use were in that state (2026-09-08).
//
// The two arms differ in KIND as well as in target, which is why this is a second macro rather than
// a reuse of MH_CRT: the hosted arm is an ADDRESS the game's own __watcall qsort calls with the
// element pointers in EAX/EDX, and the standalone arm is a `__cdecl int32_t(void*, void*)` our own
// qsort calls. Neither one can stand in for the other, and there is no shared name to select on --
// so both are named at the site:
//
//     MH_CRT_CMP(llm_strat_ai_scan_target_sort_cmp, &::mh::ai::scan_target_sort_cmp)
//
// The translated bodies live in their own DOMAIN (ai/, sim/), not here -- they are game logic that
// happens to be reached through a CRT function, and crt/ holds the C runtime.
#pragma once

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "crt/crt_heap.h"
#include "crt/crt_math.h"
#include "crt/crt_qsort.h"
#include "crt/crt_rand.h"
#include "crt/crt_sprintf.h"
#include "crt/crt_string.h"

#ifdef MH_LIBMH_BUILD
#define MH_CRT(fn)                 ::mh::crt::fn
#define MH_CRT_CMP(addr_sym, ours) ((void *)(ours))
#else
#define MH_CRT(fn)                 ::mh::call::fn
#define MH_CRT_CMP(addr_sym, ours) ((void *)::mh::addr::addr_sym)
#endif
