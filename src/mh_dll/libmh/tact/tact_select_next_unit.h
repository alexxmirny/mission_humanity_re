//
// tact/tact_select_next_unit.h -- TACT1E: round-robin re-selection of the "active" tactical unit.
//
//   llm_tact_select_next_unit @0x0042edb8 (0x161)
//   void __watcall llm_tact_select_next_unit(void)
//
// Re-derived from the DISASSEMBLY (tmp/decomp_tact/llm_tact_select_next_unit_0042edb8.asm), not
// from Ghidra's .c, and cross-checked against the tracker's own derivation of the same shape
// (TACT1E progress, re-verified against the raw opcodes rather than trusted).
//
// GATE (@0x0042edde-0x0042ede9): no-op unless `active_unit_count() != active_unit_count_cached()`
// -- NOTE these are `tact_store::active_unit_count()`/`active_unit_count_cached()`
// (RID_TACT_ACTIVE_UNIT_COUNT/RID_TACT_ACTIVE_UNIT_COUNT_CACHED), a DIFFERENT pair of globals from
// `tact_store::unit_active_count()` (RID_TACT_UNIT_ACTIVE_COUNT) used elsewhere in this module --
// see tact_state.h's own comment on the accessor. Both `candidate`/`target` locals are initialised
// to 1 BEFORE this gate is checked, but nothing else runs when the gate fails (the panel-refresh
// call at the end is only reached on the not-equal path).
//
// DERIVATION, once the gate passes:
//
// LOOP A (@0x0042edef-0x0042ee36): for i = 1..TACT_UNIT_LAST_SLOT (128) inclusive, if
// `(unit[i].status & 1) == 1 && unit[i].owner == 0`, set `candidate = i`. NO EARLY BREAK -- the scan
// runs to the end even after a match, so `candidate` ends up the LAST such unit, not the first.
// `candidate` defaults to 1 (its initial value) if no slot ever matches.
//
// LOOP B (@0x0042ee36-0x0042eea1): starts at `i = candidate + 1`, budget-limited by a SEPARATE
// counter that runs 0..TACT_UNIT_LAST_SLOT inclusive (129 = 0x81 budget-consuming steps at most):
//   - `unit[i].type == 0` (an empty slot, i.e. "one past the end of the populated array"): wrap
//     `i` back to 1 and re-check the SAME budget value again -- wrapping does NOT consume a step.
//   - else if `(unit[i].status & 1) == 1` (already selected): fall through to advance (skip).
//   - else if `unit[i].owner == 0`: `target = i`, and break out of loop B ENTIRELY (no further
//     advance -- this is the only path that changes `target` from its initial value of 1).
//   - else (owner != 0): fall through to advance.
//   - "advance" = `++i; ++budget;` then re-check the budget bound.
//   REAL OOB HAZARD, present in the ORIGINAL game too: `candidate` can reach TACT_UNIT_LAST_SLOT
//   (128), so loop B's first read can be `unit_at(129)` -- one slot past the declared
//   TACT_UNIT_SLOTS (129, valid indices 0..128) region. Reproduced literally via the same unchecked
//   `tact_store::unit_at()` accessor every other writer in this module uses (unit_despawn,
//   unit_spawn, ...) -- not a bound this translation may add.
//
// LOOP C (@0x0042eea1-0x0042eeeb): for i = 1..TACT_UNIT_LAST_SLOT (128) inclusive, unconditionally
// clear bit 0 of `status` on every unit that currently has it set. Runs regardless of how loop B
// ended (budget exhausted OR an early break on success) -- both paths land here.
//
// STAMP (@0x0042eeed-0x0042ef0a): `target`'s status bit 0 is set UNCONDITIONALLY, even if loop B
// never found a real candidate and `target` is still its initial value of 1
// (PRESERVE-BUG -- do not add a "only stamp if a unit was actually found" guard the original
// lacks). Then `llm_tact_selection_panel_refresh()` is called, unconditionally on this
// (gate-passed) path -- routed through the calls-struct per Law 3b since it is a frontier callee,
// not a migration member.
//
// PROOF PATH: RIG. The only externally-visible effects are (a) `_G_LLM_TACT_UNITS[*].status` bit 0
// (already tracked via `tact_store::unit_at()`) and (b) the single, argumentless, presentation-only
// `llm_tact_selection_panel_refresh()` call -- no float math, no RNG, no roster mutation.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call, indirected for offline testability (same shape as unit_despawn_calls /
// unit_destroy_calls' identically-named member).
struct select_next_unit_calls {
    void (*selection_panel_refresh)(); // llm_tact_selection_panel_refresh @0x00434af7
};

const select_next_unit_calls &live_select_next_unit_calls();

namespace detail {

// llm_tact_select_next_unit @0x0042edb8. See the header banner for the full derivation. Takes only
// `own` -- every field this function touches (the unit roster, the active-unit-count pair) is
// read-modify-write through the STORE; nothing here needs the read-only view.
void select_next_unit(tact_store &own, const select_next_unit_calls &c);

} // namespace detail

void select_next_unit();

// Declared here per the module convention; DEFINED in tact_select_next_unit.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
