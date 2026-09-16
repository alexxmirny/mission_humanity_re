//
// sim/sim_reason_to_housing_bldg.h -- llm_strat_reason_to_housing_bldg @0x0047401c (0x10a bytes).
//
// A pure READ-ONLY query over the cfg Building[] table (sim_view::cfg_buildings, already bound --
// Building[100], indexed by building::building_id). Given a "housing reason" group code (the caller,
// llm_strat_bldg_completion_dispatch's PROD_WORKING arm, passes `unit_type_group_index(active_unit_
// type)`, a value used elsewhere in that same caller as a v.text_ptrs[] index too -- so `reason` is a
// unit-type-GROUP id, not a building id) and a race, returns the index of the first housing-class
// building type whose cfg `.type` matches the (reason, race) pair's expected building-type constant,
// or 0 if `reason` is outside the four recognised codes OR the scan (index 1..99, index 0 never
// checked -- same "loop starts at 1" shape as sim_game_get_starting_unit.cpp's own cfg-table scan)
// exhausts without a match.
//
// ---- THE SWITCH (0x00474057-0x004740e9): FOUR RECOGNISED REASON CODES, EACH RACE-SPLIT -------------
// reason==0xf  (BARRACKS group) -> race==RACE_ALIEN(2): BLDG_TYPE_A_BARRAKS(7);  else BLDG_TYPE_H_BARRACKS(0x1b)
// reason==0x10 (GARAGE   group) -> race==RACE_ALIEN(2): BLDG_TYPE_A_GARAGE(8);   else BLDG_TYPE_H_GARAGE(0x1c)
// reason==0x11 (HELIPAD  group) -> race==RACE_ALIEN(2): BLDG_TYPE_A_HELIPAD(0xa);else BLDG_TYPE_H_HELIPAD(0x1e)
// reason==0x12 (AIRFIELD group) -> race==RACE_ALIEN(2): BLDG_TYPE_A_AIRFIELD(9); else BLDG_TYPE_H_AIRFIELD(0x1d)
// any other reason -> return 0 immediately (0x004740e9-0x004740f0), the scan never runs.
// The race compare is `race == 2` in every arm (not `== RACE_ALIEN` via a shared branch) -- reproduced
// as four independent ternaries below, matching the four independent CMP/JNZ pairs in the assembly
// (0x0047406e/0x0047408f/0x004740ad/0x004740cb), same "don't fold independently-computed arms into one
// shared branch" posture sim_game_get_starting_unit.cpp's own header note documents for its two arms.
//
// ---- THE SCAN (0x004740f2-0x0047412d) ----------------------------------------------------------------
// `for (i = 1; i < 100; ++i) if (Building[i].type == target) return i; return 0;` -- same bounded
// linear scan shape (index 1..99, `CMP/JL/JMP` loop-bound idiom) as sim_game_get_starting_unit.cpp's
// cfg_units[] scan and sim_unit_purge_unregistered.cpp's cfg-table walk; `100` is the literal loop
// bound read off the assembly (`CMP dword ptr [...],0x64`), not `Building[]`'s live-count global
// (G_BUILDING_COUNT_TOTAL, a DIFFERENT value read by a DIFFERENT function -- see ai_state.h's own
// comment on that global) -- reproduce the literal cap per translator-brief rule 14, not the dynamic
// count.
//
// ---- FIELDS: ALREADY NAMED / BOUND, NOT BYTE OFFSETS -------------------------------------------------
// mh_cfg_final_struct_Building.type@0x8 (uint8_t, [cfg_enum_E_BUILDING]) -- static_assert'd in
// addr/mh_structs.gen.h; sim_view::cfg_buildings already binds the whole Building[100] table
// (RID_BUILDING) read-only.
//
// ---- NAMES: EXISTING GHIDRA ENUM (rule 17a) ------------------------------------------------------
// cfg_enum_E_BUILDING already exists in Ghidra (mh_structs.gen.h's own field-comment bracket on
// `.type`); the Ghidra .c draft (tmp/decomp_sim/llm_strat_reason_to_housing_bldg_0047401c.c) renders
// all eight case constants as that enum's OWN member names (A_BARRAKS/H_BARRACKS/A_GARAGE/H_GARAGE/
// A_HELIPAD/H_HELIPAD/A_AIRFIELD/H_AIRFIELD), confirming the eight values below are not local
// invention. No generated C++ enum type exists for cfg_enum_E_BUILDING, so per the established
// per-TU precedent (sim_bldg_completion_dispatch.cpp, sim_bldg_unmap_footprint.cpp,
// sim_order_dispatch_bldg.cpp -- all re-declare the SAME BLDG_TYPE_ constants file-locally with the
// SAME values) they are re-declared here as `BLDG_TYPE_` inline constexpr rather than hoisted, same
// posture as every sibling TU. `A_BARRAKS` (missing a C) is the enum's own spelling, preserved
// verbatim -- not a transcription typo here.
//
// DECLARED NEED: `reason`'s four recognised codes (0xf/0x10/0x11/0x12) are a closed domain (a
// unit-type-group id that also doubles as a v.text_ptrs[] index at the caller,
// sim_bldg_completion_dispatch.cpp:320) but carry NO Ghidra enum and NO named constants anywhere in
// the codebase (the Ghidra .c draft itself renders them as bare hex `case 0xf:` etc, not symbolic) --
// propose an enum (or at least named constants) for this "unit-type housing group" domain once its
// full member set is known; this function only proves four of its values.
//
// CROSS-FILE OBSERVATION (not a defect in this translation -- flagged for the conductor to
// reconcile): the sole producer of `reason`, sim_unit_type_group_index.cpp, ALREADY has committed,
// named constants for these same four values -- GROUP_IDX_GROUND_VEHICLE=0xf, GROUP_IDX_INFANTRY=
// 0x10, GROUP_IDX_HELI=0x11, GROUP_IDX_PLANE=0x12 (that file's own private namespace). Composing the
// two functions literally: reason 0xf ("ground vehicle" per that file's label) selects
// BLDG_TYPE_A_BARRAKS/H_BARRACKS here, and reason 0x10 ("infantry") selects BLDG_TYPE_A_GARAGE/
// H_GARAGE -- i.e. ground vehicles land on the BARRACKS building type and infantry on GARAGE, the
// reverse of the intuitive housing pairing (and of real-world building names). Both this function's
// case values and that file's GROUP_IDX_ values are independently, solidly transcribed from their
// own assembly (this file's from the .asm switch table + the Ghidra .c draft's own cfg_enum_E_BUILDING
// member names; that file's from its own CMP-chain band boundaries) -- so this is either a genuine
// original-game quirk, or the GROUND_VEHICLE/INFANTRY *labels* in sim_unit_type_group_index.cpp's
// comment are swapped (that file's own header already flags this exact overlap as unresolved:
// "[w]hether the numeric overlap is deliberate or coincidental is not established from this function
// alone"). Not re-derived or fixed here -- surfaced for the conductor to reconcile once both are
// promoted.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_reason_to_housing_bldg @0x0047401c. See the header banner above for the full per-arm
// derivation. Pure read: touches no sim_store region and calls nothing (besides the inert Watcom
// stack probe, translator-brief rule 6, omitted).
int32_t reason_to_housing_bldg(const sim_view &v, uint32_t reason, int32_t race);

} // namespace detail

// Public wrapper. Signature matches the committed export/call/shadow shape
// (sig_llm_strat_reason_to_housing_bldg in addr/mh_export.gen.h): int32_t(uint32_t, int32_t).
int32_t reason_to_housing_bldg(uint32_t reason, int32_t race);

namespace detail {
} // namespace detail

} // namespace mh::sim
