//
// sim/sim_unit_type_group_index.h -- llm_strat_unit_type_group_index @0x00492b60 (0x8d bytes).
// A pure classification lookup: Unit[unit_id].type -> a 4-way "unit type group" index. NOT a
// per-instance unit accessor -- `unit_id` here indexes the cfg_units PROTOTYPE table directly
// (v.cfg_units[unit_id]), not units[player][index]/unit_of() the way most sim functions read a
// live unit instance. Writes nothing; the only CALL in the body is the inert Watcom stack probe
// (translator brief rule 6).
//
// THE FOUR BANDS, read directly off the CMP immediates at 0x00492b8b-0x00492bd4, and every one of
// them a real cfg_enum_E_UNIT_TYPE band boundary (the enum's full member list was closed as a
// declared need by sim_weapon_damage_calc.h/.cpp on 2026-08-16 against the live DTM; reproduced here
// only as far as this function needs it -- UNDEFINED=0, A_INFANTRY_1..5=0x1-0x5,
// H_INFANTRY_1..5=0x6-0xa, A_WALKER=0xb, H_WALKER=0xc, H_GROUND=0xd, A_GROUND=0xe, A_HELI=0xf,
// H_HELI=0x10, A_PLANE=0x11, H_PLANE=0x12, ...):
//   type == UNDEFINED (0)                 -> 0    (no group)
//   0 < type <= H_INFANTRY_5 (0xa)         -> 0x10 (infantry)
//   H_INFANTRY_5 < type < A_HELI (0xf)     -> 0xf  (ground vehicle: A_WALKER..A_GROUND)
//   A_HELI (0xf) <= type <= H_HELI (0x10)  -> 0x11 (heli)
//   H_HELI < type <= H_PLANE (0x12)        -> 0x12 (plane)
//   type > H_PLANE (0x12)                  -> 0    (no group / mothership and beyond)
// The outer CMP against 0x18 at 0x00492b9d/0x00492ba1 is DEAD CODE -- both the JBE-taken edge and
// the unconditional fallthrough at 0x00492ba3 land on the identical LAB_00492bdd (result 0), so it
// changes nothing observable; see the .cpp for the literal address citations and the uncertainties
// entry flagging this as a control-flow simplification, not a re-derivation.
//
// THE RETURN VALUES ARE READ AS LITERALS, NOT AS cfg_enum_E_UNIT_TYPE MEMBERS, despite the numeric
// overlap on the heli/plane pair (0xf/0x10/0x11/0x12). The sole caller,
// llm_strat_bldg_completion_dispatch (tmp/decomp_sim/llm_strat_bldg_completion_dispatch_004795dd.c,
// 0x004795dd), uses this function's return value BOTH as a direct G_TEXT_PTRS[] index (a
// unit-type-group display string) and as llm_strat_reason_to_housing_bldg()'s first argument (a
// housing-building category selector) -- a small, purpose-built ID space, not a re-export of the CFG
// enum. Whether the numeric overlap is deliberate or coincidental is not established from this
// function alone (see uncertainties in the translator's structured report).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_type_group_index @0x00492b60. `unit_id` indexes v.cfg_units[] DIRECTLY (a cfg
// prototype id) -- reads only cfg_units[unit_id].type, writes nothing.
int32_t unit_type_group_index(const sim_view &v, int32_t unit_id);

} // namespace detail

// Public wrapper. Signature matches the committed export/call/shadow shape
// (sig_llm_strat_unit_type_group_index in addr/mh_export.gen.h): int32_t(__cdecl*)(int32_t unit_id).
int32_t unit_type_group_index(int32_t unit_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
