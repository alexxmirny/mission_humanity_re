//
// orders/issue/issue_promote.h -- PROMOTION install glue for the order-issue domain (RI-ORDERS /
// O4-P, 2026-09-01).
//
// Installs the 62 rows of tools/data/orders_issue_migration.json carrying `"state": "verified"`
// (37 T1 + 25 T2) as the LIVE implementation, one MH_EXPORT_REPLACE seam per row -- the same
// whole-domain-closure shape order_queue.cpp's O3 promotion uses for the container, applied here to
// this domain's 62-row set. Nothing here is a third flavour of the shadow seam: a shadow site runs
// the ORIGINAL, restores what it wrote, runs OURS, and compares; a promoted entry has no second arm
// at all -- ours simply IS the function for the rest of the run (see issue_promote.cpp's own banner
// for the per-row adapter shape and the anti-vacuity liveness log).
//
// THE 53 `"state": "dead"` ROWS ARE EXCLUDED, not merely unlisted. O4-PREP's three-tool concurrence
// (find-cross-references / find-constant-uses / the whole-image raw-pointer scan) found each of them a TOTAL
// ORPHAN -- zero call/JMP refs, zero immediate uses of the entry VA, and the entry address occurs
// nowhere in initialized memory (no dispatch/callback table, aligned or packed). Installing a live
// seam over an unreachable original would prove nothing at runtime and would just be one more ABI
// contract this domain has to maintain forever (Law 4) for no observable benefit.
//
// GATED by ONE ini switch for the whole domain, `[promote] orders_issue=1` -- the unit of
// replacement is the call-tree closure, not a function at a time, so promoting half of it would
// leave our own code calling originals our other half has already replaced. `default_on` is passed
// in by the caller (the seams layer's `[promote]` ship-default block) rather than read from a header
// here, for the same layering reason order_queue.cpp's install_promotion documents: this module must
// not include a seams header.
//
// PER-ROW ESCAPE HATCH: `[promote_skip] <original function name>=1` in the same ini pulls exactly
// ONE row back to the stock original without touching the other 61 -- the cheap red-ladder flip for
// isolating a single bad row, rather than re-deriving a safe partial closure by hand.
//
#pragma once

namespace mh::orders::issue {

// True once at least one of the 62 seams below has been installed successfully.
bool promotion_active();

// Installs the domain's 62 live seams, honouring `[promote_skip]` per row. Returns the number
// actually installed (0 = off, the default = the stock path byte for byte, and also the one-flag
// rollback).
int install_promotion(int default_on);

} // namespace mh::orders::issue
