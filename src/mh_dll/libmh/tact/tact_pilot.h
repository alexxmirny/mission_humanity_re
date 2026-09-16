//
// tact/tact_pilot.h -- TACT0's pilot slice: four functions, one per facet of the interface.
//
//   llm_tact_quantize_facing_dir       @0x0042d0cf (0x58)  batch B layer 1 -- NO state at all
//   llm_tact_ui_mouse_in_rect          @0x00435909 (0x69)  batch E layer 3 -- tact_view only
//   llm_tact_units_reset_hp_for_active @0x0042ad7f (0x5e)  batch C layer 1 -- tact_store (own arena)
//   llm_tact_move_path_preview_clear   @0x0042ea8a (0x6f)  batch B layer 3 -- planes() (shared)
//
// CHOSEN TO EXERCISE THE INTERFACE, NOT TO MIGRATE A BATCH -- SIM0's posture exactly. Each is the
// smallest member that uses one facet, and between them they use every facet once: a body that
// needs no state proves the interface is not mandatory ceremony; a const-view reader proves W1's
// read path; an arena write proves W2's accessor; and the plane write is the one TACT0 exists for,
// the first write through `mode_planes` from the tactical side.
//
// ONE IS NOW ARMABLE AND NONE IS PROMOTED (TACT-RIG, 2026-08-25). `quantize_facing_dir` has a
// shadow site, and it has one to demonstrate the tactical RIG rather than to record an evidence
// tier: it is the member that touches no state at all, so what an armed run proves is that a
// tactical vehicle reaches GAME_MODE 6 and a site armed there records a NON-ZERO call count. The
// other three stay unarmed, and `tacttest`'s per-function cases remain what asserts all four.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_quantize_facing_dir @0x0042d0cf.
//
// Folds a 24-notch compass heading into `notch_span`-sized buckets. Takes NO view and NO store: it
// touches no global, and saying so with an empty parameter list is more honest than threading a
// state handle through for symmetry.
//
// THE ARITHMETIC IS SIGNED AND THE ORIGINAL'S ROUNDING IS PRESERVED. `notch_span / 2` and the final
// `/ notch_span` are IDIV (0x0042d0f4, 0x0042d11c), which truncates toward zero -- not a shift, and
// not floor. The `-1` before the final divide is the original's, not a fencepost of ours.
//
// NOTE THE WRAP IS ONE-SIDED: only values ABOVE 0x18 are folded back, and by exactly 0x18. A
// centred value of exactly 0x18 is left alone, and a negative facing_dir is not wrapped at all.
// Both are the original's behaviour (JLE at 0x0042d108), reproduced rather than tidied.
int32_t quantize_facing_dir(int32_t notch_span, int32_t facing_dir);

// llm_tact_ui_mouse_in_rect @0x00435909. Strict-inside hit test of the tactical HUD's latched
// cursor against [x0,x1) x [y0,y1). Reads through the CONST view -- this is W1's read path in one
// function.
int32_t ui_mouse_in_rect(const tact_view &v, int32_t rect_x0, int32_t rect_y0, int32_t rect_x1,
                         int32_t rect_y1);

// llm_tact_units_reset_hp_for_active @0x0042ad7f. Zeroes `.hp` for every roster slot whose type is
// below 0x80, i.e. every unit not already marked despawned (llm_tact_unit_despawn marks by ADDING
// 0x80 to the type, so a despawned unit's HP is deliberately left alone).
//
// THE BOUNDS ARE THE ORIGINAL'S AND BOTH ENDS MATTER: `for (i = 1; i < 0x81; ++i)` -- slot 0 is
// skipped and slot 0x80 IS included, over a 129-record array. An off-by-one in either direction
// would be invisible on a fixture that seeded every slot identically, which is why the fixture
// seeds slot 0 and slot 0x80 with values the assertions look at specifically.
void units_reset_hp_for_active(tact_store &own);

// llm_tact_move_path_preview_clear @0x0042ea8a. Zero-fills the move-path preview overlay byte over
// the 128x128 tactical sub-block, before a group's order path is redrawn.
//
// THIS IS THE FUNCTION TACT0 IS ABOUT. It writes a plane the tactical mission does not own, through
// the one binding both modes share. The byte it clears is `tile_object::unit[1]` -- the SECOND of
// the two-readings pair, which in strategic mode is a unit's PLAYER field. Clearing it wholesale is
// only safe because the modes are exclusive in time, which is precisely the invariant no compiler
// can carry and which `mode_planes` documents rather than pretends to enforce.
void move_path_preview_clear(mh::state::mode_planes &planes);

} // namespace detail

// Live wrappers: the logic applied to state(). Signatures match the originals' __watcall shape.
int32_t quantize_facing_dir(int32_t notch_span, int32_t facing_dir);
int32_t ui_mouse_in_rect(int32_t rect_x0, int32_t rect_y0, int32_t rect_x1, int32_t rect_y1);
void    units_reset_hp_for_active();
void    move_path_preview_clear();

// The domain's shadow install point is mh::tact::install_shadow, declared in tact_state.h and
// defined in tact_state.cpp -- the state pair owns arm orchestration for every tactical site,
// including this file's quantize_facing_dir (installed via this per-TU wrapper, whose generated
// installer is TU-local).

} // namespace mh::tact
