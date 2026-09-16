//
// libmh/state/spine_exports.cpp -- FORCE AN OUT-OF-LINE BODY FOR THE PROCESS-WIDE SINGLETONS.
//
// Fork F4D. Three accessors in this tree are header-only magic statics that the whole process must
// share ONE of -- mh::state::live() (the region registry), owner_table() and owner_count() (region
// ownership). While the spine and the injection layer were one image that was automatic: the
// linker folded the COMDATs and everybody saw the same object. Split into mh.dll + libmh.dll it is
// not, so libmh.dll EXPORTS them and mh.dll reaches them across the boundary like any other
// contract row (seams/libmh_bind.cpp carries the argument and the absent answers).
//
// ---- WHY THIS FILE EXISTS AT ALL, RATHER THAN A NON-INLINE DEFINITION ---------------------------
//
// A .def can only export a symbol the image actually DEFINES, and an `inline` function that every
// call site inlines is defined nowhere. Measured: the Debug build (/Od) emitted all three as
// COMDATs and exported them fine; the Release build (/O2) inlined owner_table() and owner_count()
// at every one of their call sites, emitted no out-of-line copy, and the link failed with
// `libmh.exp : error LNK2001: unresolved external symbol` for exactly those two. A configuration
// that only fails in Release is precisely the shape worth fixing structurally.
//
// The obvious alternative -- move the bodies out of the headers -- was rejected for a measured
// reason rather than a stylistic one. F4D-PRE caught an out-of-line accessor making
// sim_order_dispatch.cpp materialise an original VA as an immediate, which LIB-VA0's byte ratchet
// (tools/scan_libmh_vas.py) reported as a new VA in the archive. These three take no arguments so
// that particular trap does not apply, but live_base() sits on paths the sim walks per unit per
// step, and turning a folded magic-static read into a call across 627 TUs is a codegen change to
// the spine for a packaging problem. Taking the address instead costs one array and changes no
// call site: every internal caller still inlines, and the out-of-line copy the linker is now
// obliged to emit returns the same function-local static (the static is its own COMDAT, shared by
// every inlined and out-of-line instance -- which is the property that makes all of this correct).
//
// EXTERNAL LINKAGE, DELIBERATELY. A `const` array at namespace scope has internal linkage in C++,
// and an unreferenced internal one is exactly what the optimiser deletes -- the F4A measurement
// trap in a new costume (an anchor eliminated by /GL, silently measuring the wrong thing). The
// `extern` declaration below makes it a real definition nothing may drop.
//
// This TU is part of the `state` module, so tools/gen_libmh_vcxproj.py picks it up for both libmh
// artifacts automatically; it is compiled into mh_nettest and libref_host too, where it is inert.
//
#include "addr/mh_regions.gen.h"
#include "state/region_owner.h"

namespace mh::state {

extern void *const k_spine_export_anchor[];
void *const        k_spine_export_anchor[] = {
    (void *)&live,
    (void *)&owner_table,
    (void *)&owner_count,
};

} // namespace mh::state
