//
// sim/libtrans/sim_lt_prod_transfer.h -- lib_trans batch E. One original:
//
//   llm_strat_prod_transfer_progress @0x00490368 (0xfa B) -- double __watcall(int32_t slot)
//
// Journey-completion fraction [0,1] of a shuttle transfer sitting in `slot` of the LOCAL PLAYER's
// (PlayerSide's) prod-shuttle-slot table. Callers use the result to interpolate the shuttle icon's
// star-map position (0x004c5ade, 0x0048f0cf) -- a per-frame UI query, not a sim state mutator.
//
// ---- HAZARD (1): PLAYER IS NOT A PARAMETER -- THIS IS PEER-DEPENDENT BY CONSTRUCTION -------------
// The body reads the LOCAL player straight out of `PlayerSide` (sim_view::player_side,
// `MOVZX EAX, word ptr [0x00e58354]` @0x00490383) rather than taking it as an argument. Two peers
// running this with the same `slot` argument can legitimately return different values if their
// `PlayerSide` differs -- that is fine for its only real callers (star-map UI interpolation, which
// is inherently per-viewer), but it means:
//   * this function MUST NOT be called from a lockstep-deterministic path (it would desync trivially
//     -- not because the reimplementation is wrong, but because the ORIGINAL already isn't peer-
//     agnostic);
//   * a shadow-arm differential is only meaningful when the two arms run under the SAME local
//     PlayerSide -- which every shadow call already satisfies (it patches the live game process, one
//     PlayerSide at a time), so this is a property to be aware of, not a defect to fix here.
//
// ---- HAZARD (2): PlayerSide IS READ UNSIGNED -----------------------------------------------------
// `MOVZX`, not `MOVSX` -- PlayerSide is a signed int16_t in sim_view (`const int16_t *player_side`),
// but the original zero-extends it to 32 bits before the index multiply. Cast through `uint16_t`
// first (matching detail::prod_transfer_progress below) so a hypothetical negative PlayerSide widens
// the same (wrong-looking but original) way instead of sign-extending.
//
// ---- HAZARD (3): INDEX DERIVATION (translator-brief rule 2b) --------------------------------------
// All four raw address computations in the asm are `player*0x1f18 + slot*0x31c + 0xbd215c` (the
// region base, `sim_view::prod_shuttle_slots` @ RID_PROD_SHUTTLE_SLOTS). 0x1f18 / 0x31c ==
// PROD_SHUTTLE_SLOTS_PER_PLAYER (10) exactly, and 0x31c == sizeof(prod_shuttle_slot), so
// `player * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot` is the same byte address reached via array
// indexing. The four field operands used, verified by subtracting the same 0xbd215c base:
//   0xbd2172 - 0xbd215c = 0x16 = type_ref_id   (word)
//   0xbd2176 - 0xbd215c = 0x1a = status        (word)
//   0xbd217c - 0xbd215c = 0x20 = travel_duration      (qword/double)
//   0xbd2184 - 0xbd215c = 0x28 = travel_duration_copy (qword/double)
// All four already match mh_llm_prod_shuttle_slot's committed field offsets -- no retype needed.
// The original re-derives the slot base FOUR separate times (once per field read); this translation
// resolves the record reference once and reuses it, which the batch context confirms is an
// optimisation difference only, not a semantic one.
//
// ---- HAZARD (4): THE GUARD IS A 16-BIT COMPARE -----------------------------------------------------
// `CMP word ptr [...],0x0` @0x0049039d and `CMP word ptr [...],0xc8` @0x004903b7 -- both 16-bit
// compares (type_ref_id and status are `uint16_t`/`int16_t` fields), not int32. Free/never-bound
// (type_ref_id == 0) or not-in-transit (status != 0xc8/200) both fall to the same -1.0 result.
//
// ---- HAZARD (5): THE ZERO/NAN TEST -- SEE THE .cpp's journey_copy_is_arrived() -----------------
// `FLDZ; FCOMP copy; FNSTSW AX; SAHF; JNC` @0x004903d7-0x004903e2. Fully derived in the .cpp next to
// the helper it produced; short version: JNC fires exactly when the compare is ORDERED and
// `0.0 >= copy`, which is bit-for-bit what a plain IEEE-754 relational operator already computes
// (C++ relational ops against a NaN operand are defined to evaluate false) -- so `copy <= 0.0`
// reproduces this branch exactly, NaN included. See uncertainties[] in the translation report: this
// disagrees with a "NaN also takes the 1.0 arm" reading of this branch.
//
// ---- HAZARD (6): THE TWO CONSTANT RESULTS ARE DWORD-IMMEDIATE BIT PATTERNS ------------------------
// 1.0 as {0, 0x3ff00000} (0x0049042b/0x00490432) and -1.0 as {0, 0xbff00000} (0x0049043b/0x00490442)
// -- plain `return 1.0;` / `return -1.0;` compiles to the identical IEEE-754 bit pattern, so this is
// a byte-identical equivalence, not an approximation.
//
// ---- HAZARD (7): ARITHMETIC ORDER IS FIXED ---------------------------------------------------------
// `FLD copy; FSUB duration; FDIV copy` @0x00490404-0x00490420, i.e. `(copy - duration) / copy`. Do
// not rearrange (e.g. to `1.0 - duration/copy`) -- x87 rounding at each step is observable.
//
// ---- HAZARD (8): RETURN CONVENTION -----------------------------------------------------------------
// Result returned in ST0 (`FLD` @0x00490455, not MM0) -- the manifest's plate already records the
// 2026-07-27 P0-CALLS ST0-vs-MM0 prototype correction; `mh_calls.gen.h:1224` already returns
// `double`. No action needed here.
//
// ---- WHAT THIS DOES NOT TOUCH ----------------------------------------------------------------------
// Read-only: `sim_view::prod_shuttle_slots` and `sim_view::player_side`. No write of any kind, no
// outward call besides the inert Watcom stack-capacity probe (translator-brief rule 6). Shadow
// manifest already carries an entry with an EMPTY region set (`mh_shadow_regions_
// llm_strat_prod_transfer_progress[1] = {{"", RID_COUNT, 0u, 0u}}`) -- the differential can only ever
// compare the return value, which is exactly right for a pure query. No `<tu>_calls` struct needed
// (batch context #5: every batch-E body's only callee is the inert stack probe).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_prod_transfer_progress @0x00490368. See the header banner above for the full hazard
// derivation. `player` is read out of `v.player_side` inside this function, NOT passed in by the
// caller (hazard 1) -- `slot` is the only real parameter, matching the original's __watcall(EAX=slot)
// shape.
double prod_transfer_progress(const sim_view &v, int32_t slot);

} // namespace detail

// Public wrapper. Matches the committed prototype exactly: double(int32_t slot) __watcall,
// addr::exp::sig_llm_strat_prod_transfer_progress.
double prod_transfer_progress(int32_t slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
