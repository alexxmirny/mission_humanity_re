// state/host_in.h -- libmh's side of the INBOUND (host -> libmh) ABI (LIB-REF-IN;
// The LIB-REF plan section 3.5).
//
// The C contract itself is libmh/include/libmh_host_in.h (hand-authored) plus
// libmh_host_in.gen.h (derived: order ids, entry ids, the version). This header carries only the
// two things a C header must not: the module-internal declarations, and the SELFTEST SEAM.
//
// WHY THERE IS A SELFTEST SEAM AT ALL. The done_when asks for a trap-by-name arm -- proof that an
// entry with no implementation is caught BY NAME rather than read as a working entry that did
// nothing. In this direction every entry is an ordinary linked C function, so it can never BE
// null in a shipped build; the reachable-hole state is the DISPATCH TABLE the surface builds at
// open time (the order thunks and the entry impl slots). So the suite punches a hole in that
// runtime table, exactly as hostapitest nulls one slot of the host-callback struct, and asserts
// the walk names it and the call traps with it. The seam writes the runtime copy only -- it
// cannot touch the generated tables, and libmh_in_open rebuilds from them.
#pragma once

#include <cstdint>

#include "../../libmh/include/libmh_host_in.h"

namespace mh::libmh_in {

// The gate every entry body passes through: false means refuse (the surface is not open, or this
// entry's runtime slot is holed -- either way the trap has already named it). Emits the per-entry
// one-shot "call #1" liveness line on the way through, so a run can be asked WHICH entries it
// entered, the same reason sim_hostreach_promote logs per row.
bool enter(uint32_t entry_id, bool &fired);

// ---- the selftest seam (hostintest) --------------------------------------------------------
//
// Null the runtime slot for one entry / one order id, so the next call into it takes the unbound
// path. `restore_all()` puts both tables back from the generated data. NOT declared in the C
// header: a host has no business holing its own surface.
void test_unbind_entry(uint32_t entry_id);
void test_unbind_order(uint32_t order_id);
void test_restore_all();

// The last name the trap printed, and how many times it has fired since the last reset. The trap
// is not fatal (unlike hostapi's REQUIRED-entry trap) because an inbound entry's caller is the
// HOST, and refusing it with a named code is a better contract than killing the host's process.
const char *last_trap();
int         trap_count();
void        trap_reset();

} // namespace mh::libmh_in
