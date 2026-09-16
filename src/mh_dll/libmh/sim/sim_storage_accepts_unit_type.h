//
// sim/sim_storage_accepts_unit_type.h -- llm_bldg_storage_accepts_unit_type (RI-SIM / SIM1D third
// slice, gap-close batch). Translated from the DISASSEMBLY
// (tmp/decomp/llm_bldg_storage_accepts_unit_type_00497ac4.asm), not from the Ghidra .c draft -- the
// draft happened to read identically to the assembly on this one (verified branch-by-branch, not
// assumed), so nothing here diverges from it.
//
// llm_bldg_storage_accepts_unit_type @0x00497ac4 (0xc3 B), committed prototype `int32_t __watcall
// llm_bldg_storage_accepts_unit_type(int16_t building_type, uint16_t unit_type)`
// (addr/mh_export.gen.h's sig_llm_bldg_storage_accepts_unit_type / addr/mh_calls.gen.h's
// trampoline, both already committed independently of this translation).
//
// PURE PREDICATE: two scalar params in, one int32_t out, no globals, no calls (other than the inert
// opening `CALL utils_assert_stack_capacity`, per translator brief rule 6 -- not reproduced). Already
// a callee inside this same batch's llm_strat_storage_find_home_for_unit closure
// (sim/sim_storage_find_home_for_unit.h), which dll_shadow_manifest.json's own closure note already
// calls "a pure query with no writes" -- this file is that same verdict, now for the callee's own
// direct translation rather than an inference about it.
//
// ---- THE FOUR-BUCKET RANGE DISPATCH (0x00497ae8-0x00497b7f) ---------------------------------------
//
// unit_type is dispatched into four half-open UNSIGNED ranges via a chain of `CMP word ptr ...`/
// `JC`/`JBE` (word-sized, matching the committed uint16_t param -- unsigned comparisons throughout,
// no MOVSX anywhere in this body):
//
//   unit_type in [1, 0xa]   -> true iff building_type == 8    || building_type == 0x1c
//   unit_type in [0xb, 0xe] -> true iff building_type == 7    || building_type == 0x1b
//   unit_type in [0xf,0x10] -> true iff building_type == 0xa  || building_type == 0x1e
//   unit_type in [0x11,0x12]-> true iff building_type == 9    || building_type == 0x1d
//   unit_type == 0, or unit_type >= 0x13                      -> false
//
// The unit_type==0 exclusion is its own branch, not folded into the range test: LAB_00497b19
// (0x00497b19-0x00497b20) tests `unit_type >= 1` (JNC) BEFORE reaching the building_type==8||0x1c
// check, and falls straight to the shared false-return tail (0x00497b7f) when unit_type is exactly
// 0. The other three buckets have no such carve-out -- their lower bound (0xb, 0xf, 0x11) already
// excludes 0.
//
// The building_type CMPs (0x00497b25 etc.) compare the raw `short` param for bitwise equality only
// (JZ/JNZ) -- signedness is irrelevant to an equality test, and none of the compared literals (7-10,
// 0x1b-0x1e) are negative as a `short` regardless.
//
// Each building_type pair looks like {base_id, base_id + 0x14} EXCEPT the unit_type in [0xf,0x10]
// bucket, whose pair is {0xa, 0x1e} -- a +0x14 delta too (0x1e - 0xa == 0x14), so all four pairs are
// consistent with a base/faction-offset-variant scheme; not asserted as a named enum split (see
// declared_needs) since no cfg_enum_E_BUILDING_TYPE (or similar) member names surfaced for these ten
// literals in the .c draft the way sim_unit_type_predicates.h's cfg_enum_E_UNIT_TYPE members did.
//
// No floats, no globals, no callees (beyond the inert stack-probe) anywhere in this function's body.
//
#pragma once
#include <cstdint>

namespace mh::sim {

// The logic over its two scalar params, matching every other pure-predicate sim TU: no sim_view/
// sim_store parameter at all, because this function reads no game state (same shape as
// sim_facing24_from_points.h's detail::, minus even the one outward call that one has).
namespace detail {

// llm_bldg_storage_accepts_unit_type @0x00497ac4. See the header banner above for the full
// four-bucket derivation.
int32_t storage_accepts_unit_type(int16_t building_type, uint16_t unit_type);

} // namespace detail

// Live wrapper: detail:: applied directly (no state to fetch). Matches the committed
// sig_llm_bldg_storage_accepts_unit_type shape exactly.
int32_t storage_accepts_unit_type(int16_t building_type, uint16_t unit_type);

namespace detail {
} // namespace detail

} // namespace mh::sim
