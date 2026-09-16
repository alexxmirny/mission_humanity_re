//
// tact/tact_selection_clear_unless_ctrl.h -- TACT1E: clear the tactical selection unless LCtrl is
// held (the tactical mode's "Ctrl adds to selection instead of replacing it" idiom, called before
// every new selection action -- single-click select, box-select).
//
//   llm_tact_selection_clear_unless_ctrl @0x0042af6d (0x85)
//   void __watcall llm_tact_selection_clear_unless_ctrl(void)
//
// Re-derived from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_selection_clear_unless_ctrl_0042af6d.asm), not from Ghidra's .c.
//
// DERIVATION:
//
// GATE (@0x0042af85-0x0042afa9): reads the single keystate byte _G_LLM_KEY_LCTRL_HELD
// (0x00e69d2d) TWICE against two different bit masks -- 0x1 then 0x2 -- and is a no-op unless
// BOTH bits read zero:
//   local = ((KEY_LCTRL_HELD & 0x1) == 0) && ((KEY_LCTRL_HELD & 0x2) == 0) ? 1 : 0;
//   if (local == 0) return;  // either bit set -> LCtrl (in either encoding) counts as held
// Ghidra's own comment names both TEST sites `_G_LLM_KEY_LCTRL_HELD` (same address, two masks) --
// one byte packs both bit encodings of the same logical key, not two different keys.
//
// LOOP (@0x0042afaf-0x0042afe6): for i = TACT_UNIT_FIRST_SLOT (1) to TACT_UNIT_LAST_SLOT (0x80)
// inclusive, unconditionally clear bit 0 of `_G_LLM_TACT_UNITS[i].status` (`AND 0xfe`) -- same
// unchecked `tact_store::unit_at()` accessor, same slot range as
// llm_tact_select_next_unit's loop C (tact_select_next_unit.cpp).
//
// PROOF PATH: RIG / offline. The only externally-visible effect is
// `_G_LLM_TACT_UNITS[*].status` bit 0 (already tracked via `tact_store::unit_at()`) gated on the
// read-only `_G_LLM_KEY_LCTRL_HELD` keystate byte -- no float math, no RNG, no outward call (the
// opening `utils_assert_stack_capacity` is the inert prologue helper, omitted per the translator
// brief's rule 6).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_selection_clear_unless_ctrl @0x0042af6d. See the header banner for the full
// derivation. Takes the read-only view (for the LCtrl keystate gate) and the store (for the unit
// roster write) -- no outward call, so no `_calls` struct.
void selection_clear_unless_ctrl(const tact_view &v, tact_store &own);

} // namespace detail

void selection_clear_unless_ctrl();

// Declared here per the module convention; DEFINED in tact_selection_clear_unless_ctrl.cpp,
// CALLED from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
