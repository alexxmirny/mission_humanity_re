//
// sim/sim_unit_type_predicates.h -- six small unit-classification helpers over the packed
// owner|kind ref: five "is this unit of movement/AI class X" predicates plus one unrelated
// accessor that happens to share their address neighborhood and their index arithmetic.
//
//   llm_strat_unit_is_aircraft          @0x004d45c0 (0x98)  -- plane/heli/heli-mother/cargo/shuttle
//   map_unit_IsAiPlane                  @0x004d4658 (0x4c)  -- A_PLANE/H_PLANE only
//   map_unit_IsAiHeli                   @0x004d46a4 (0x4c)  -- A_HELI/H_HELI only
//   map_unit_IsAiGround                 @0x004d4760 (0x5e)  -- A_GROUND/H_GROUND/A_WALKER/H_WALKER
//   map_unit_IsAiSoldier                @0x004d47be (0x51)  -- cfg ai_unit category == SOLDIER
//   llm_strat_unit_get_ai_group_index   @0x004d48fa (0x1f)  -- accessor for unit::ai_group_index
//
// THE FIRST FIVE TAKE A PACKED REF, NOT A PLAYER INDEX. Every one of them reads its first argument
// exactly like sim_state.h's ref_owner()/REF_OWNER_MASK convention: low nibble = owning player, and
// bit 0x40 set means "this is a building, not a unit" -- all five return false (0) immediately in
// that case without touching the roster at all. The Ghidra-committed prototypes and the .c draft
// both name this argument `player`, which is what it is NOT; see REF_BUILDING_BIT below and every
// detail:: signature, which name it `unit_ref` instead.
//
// THE SIXTH DOES NOT. llm_strat_unit_get_ai_group_index's first argument is a PLAIN player index --
// no `AND EAX,0xf`, no `TEST AL,0x40` anywhere in its 0x1f-byte body (0x004d48fa-0x004d4918). It is
// grouped in this file only because it sits in the same address run and shares the same
// `player*0x5b04 + index*0xe9 + base` row/column arithmetic as the five predicates' unit lookup --
// not because it shares their packed-ref convention. Do not add a mask to it.
//
// TWO DIFFERENT CFG FIELDS, BOTH READ THROUGH THE SAME unit_proto_id INDIRECTION:
//   * the first four predicates read cfg_unit::type (a movement/airframe class, cfg_enum_E_UNIT_TYPE)
//   * IsAiSoldier reads cfg_unit::ai_unit (an AI category, cfg_enum_ai_E_UNIT) -- a DIFFERENT field,
//     not a different reading of the same one.
// Both are reached as `v.cfg_units[unit_of(v, owner, index).unit_proto_id].<field>` -- Unit[] is
// indexed by unit_proto_id, a cfg PROTOTYPE table lookup, never by the unit's own roster index.
//
// THE UNIT_TYPE_*/AI_UNIT_SOLDIER CONSTANTS BELOW ARE READ OFF GHIDRA'S OWN DECOMPILE, not invented.
// Ghidra already renders every CMP immediate in these five bodies as a cfg_enum_E_UNIT_TYPE /
// cfg_enum_ai_E_UNIT member name (A_PLANE, H_HELI_CARGO, SOLDIER, ...) in the .c drafts -- i.e. the
// enum already exists in the DTM, and per the naming-convention rule 17a these are that enum's own
// member names, not local invention. Three of the ten UNIT_TYPE_* values were independently pinned
// already (mh::ai::ai_state.h: UNIT_TYPE_A_HELI_MOTHER=0x13, UNIT_TYPE_H_HELI_MOTHER=0x14,
// UNIT_TYPE_H_HELI_CARGO=0x18, from a DIFFERENT function's disassembly) and agree exactly with the
// values re-derived here, which is the cross-check that the term-order-to-CMP-order mapping below is
// sound. See sim_unit_type_predicates.cpp for the full derivation and the one place it is genuinely
// inferential (recorded as an uncertainty, not silently assumed).
//
// NO GENERATED C++ ENUM EXISTS FOR EITHER cfg_enum_E_UNIT_TYPE or cfg_enum_ai_E_UNIT
// (mh_structs.gen.h types both fields as bare uint32_t with only a bracket comment naming the
// Ghidra enum) -- declared as a need rather than worked around.
//
#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // shares this file's UNIT_TYPE_A_HELI/H_HELI/A_PLANE/H_PLANE/
                                   // A_HELI_CARGO/H_HELI_CARGO -- same real cfg_enum_E_UNIT_TYPE
                                   // members, already named there (SIM1C, 2026-08-08); NOT
                                   // redeclared here to avoid the C2374/C2086 redefinition that
                                   // produced when both TUs first tried to name the same values.
#include "sim/sim_state.h"

namespace mh::sim {

// The building/unit selector these five predicates test (`TEST AL,0x40` in every one of them,
// e.g. 0x004d45ca/0x004d4662/0x004d46ae/0x004d476a/0x004d47cb). Set => the ref names a BUILDING and
// every predicate below returns 0 without reading the roster at all. sim_state.h has no sim-side
// equivalent of mh::ai::ai_state.h's `ref_is_building_by_40` (only REF_OWNER_MASK/ref_owner()) --
// declared as a need; named locally here rather than worked around with a bare `0x40` at five call
// sites.
inline constexpr uint32_t REF_BUILDING_BIT = 0x40u;

// ---- cfg_enum_E_UNIT_TYPE members read by is_aircraft/is_plane/is_heli/is_ground -----------------
// Values + comparison order read off the five bodies' own CMP/JZ chains; see the .cpp for the
// per-function derivation and address citations. UNIT_TYPE_A_HELI/H_HELI/A_PLANE/H_PLANE/
// A_HELI_CARGO/H_HELI_CARGO come from sim_order_enqueue.h (included above); only the members that
// header does not already carry are declared here.
// cfg_enum_E_UNIT_TYPE member 0 -- Ghidra's own decompile renders it "UNDEFINED" (docs/structs.md's
// llm_tutorial_step_op.operands note: "0(UNDEFINED)-terminated list of building/unit type codes"),
// i.e. the enum's own member name, not a local invention (naming-convention rule 17a).
//
// HOISTED HERE 2026-08-10 (SIM1A). It previously lived as a private `inline constexpr`
// copy in sim_unit_recruit.h, whose comment argued the per-file copies were deliberately not
// consolidated. That argument does not survive a THIRD user: sim_unit_housing_count.h needed the
// same member, declared its own copy citing that same comment, and reimpl_probe.cpp -- which
// includes every sim header -- then had two definitions of one name in one translation unit
// (C2374/C2086). `inline` does not rescue that; two definitions in ONE TU is an error regardless.
// This header is already the shared cfg_enum_E_UNIT_TYPE vocabulary, so the member belongs here.
inline constexpr uint32_t UNIT_TYPE_UNDEFINED      = 0x0u;
inline constexpr uint32_t UNIT_TYPE_A_HELI_MOTHER  = 0x13u; // agrees with mh::ai::ai_state.h's pin
inline constexpr uint32_t UNIT_TYPE_H_HELI_MOTHER  = 0x14u; // agrees with mh::ai::ai_state.h's pin
inline constexpr uint32_t UNIT_TYPE_A_HELI_SHUTTLE = 0x15u;
inline constexpr uint32_t UNIT_TYPE_H_HELI_SHUTTLE = 0x16u;
inline constexpr uint32_t UNIT_TYPE_A_WALKER       = 0xbu;
inline constexpr uint32_t UNIT_TYPE_H_WALKER       = 0xcu;
inline constexpr uint32_t UNIT_TYPE_H_GROUND       = 0xdu;
inline constexpr uint32_t UNIT_TYPE_A_GROUND       = 0xeu;

// ---- cfg_enum_ai_E_UNIT member read by is_soldier -------------------------------------------------
// `Unit[proto].ai_unit == SOLDIER` -- the single CMP at 0x004d47f7 (`CMP dword ptr [...],0x1`).
inline constexpr uint32_t AI_UNIT_SOLDIER = 0x1u;

// The logic over an EXPLICIT state, matching every other sim TU: net_selftest.exe simtest drives
// these over heap buffers, no game and no rig. The six public wrappers below are these applied to
// state().read.
namespace detail {

// llm_strat_unit_is_aircraft @0x004d45c0. True iff unit_ref names a UNIT (bit 0x40 clear) whose
// Unit[proto].type is one of the ten values above (plane, heli, heli-mother, heli-cargo,
// heli-shuttle -- both race variants of each).
int32_t is_aircraft(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

// map_unit_IsAiPlane @0x004d4658. True iff unit_ref is a unit whose type is A_PLANE/H_PLANE.
int32_t is_plane(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

// map_unit_IsAiHeli @0x004d46a4. True iff unit_ref is a unit whose type is A_HELI/H_HELI.
int32_t is_heli(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

// map_unit_IsAiGround @0x004d4760. True iff unit_ref is a unit whose type is
// A_GROUND/H_GROUND/A_WALKER/H_WALKER (the ground movement class).
int32_t is_ground(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

// map_unit_IsAiSoldier @0x004d47be. True iff unit_ref is a unit whose Unit[proto].ai_unit is
// SOLDIER. Reads a DIFFERENT cfg field (ai_unit, not type) from the four functions above.
int32_t is_soldier(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

// llm_strat_unit_get_ai_group_index @0x004d48fa. `player` is a PLAIN, UNMASKED player index -- see
// the file header. Returns unit_of(v, player, unit_index).ai_group_index, zero-extended.
uint32_t get_ai_group_index(const sim_view &v, int32_t player, int32_t unit_index);

} // namespace detail

// Public wrappers. Signatures match the committed export/call/shadow shapes
// (sig_llm_strat_unit_is_aircraft, sig_map_unit_IsAiPlane, sig_map_unit_IsAiHeli,
// sig_map_unit_IsAiGround, sig_map_unit_IsAiSoldier, sig_llm_strat_unit_get_ai_group_index in
// addr/mh_export.gen.h) -- note map_unit_IsAiGround's first parameter is uint16_t (its committed
// storage is AX:2), the other four packed-ref wrappers take uint32_t.
int32_t  is_aircraft(uint32_t unit_ref, int32_t unit_index);
int32_t  is_plane(uint32_t unit_ref, uint32_t unit_index);
int32_t  is_heli(uint32_t unit_ref, uint32_t unit_index);
int32_t  is_ground(uint16_t unit_ref, uint32_t unit_index);
int32_t  is_soldier(uint32_t unit_ref, uint32_t unit_index);
uint32_t get_ai_group_index(int32_t player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
