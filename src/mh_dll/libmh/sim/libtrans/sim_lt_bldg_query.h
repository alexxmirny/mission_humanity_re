//
// sim/libtrans/sim_lt_bldg_query.h -- the "does this building type currently host a slot whose unit
// type carries soldiers" query (LT-lib_trans batch E). One original:
//
//   llm_bldg_first_occupied_unit_slot_has_soldiers @0x0049a025 (0x8e bytes) --
//   `int __watcall llm_bldg_first_occupied_unit_slot_has_soldiers(int building_index)`, param in EAX
//   (committed prototype, addr/mh_calls.gen.h:1365 / addr/mh_export.gen.h:5866-5868).
//
// PURE LEAF, READ-ONLY over cfg data. The only CALL in the body is the inert Watcom stack-capacity
// probe (utils_assert_stack_capacity @0x004cf46f, ruled inert 2026-07-27 -- translator-brief rule 6),
// omitted here. No sim_view write, no roster access, no `<tu>_calls` struct needed -- the batch-E
// context doc confirms all 11 batch-E bodies share this shape.
//
// ---- WHAT IT ACTUALLY SCANS, AND WHY THE PARAMETER IS NOT A ROSTER INDEX -------------------------
// `building_index` is a cfg Building TYPE id, not a per-player roster slot: the only table it
// addresses is `Building[100]` (sim_view::cfg_buildings, RID_BUILDING), stride 0x842
// (`IMUL EDX,building_index,0x842` @0x0049a057) -- a cfg TABLE, already boot-loaded and read-only,
// never the per-player `map::object::building` roster sim_view::buildings binds. Ghidra's own
// parameter name ("building_index") is misleading for the same reason; this translation calls it
// `building_type_id` internally while keeping the public wrapper's parameter name matching the
// committed prototype (`building_index`), same convention sim_unit_goal_in_weapon_range.h documents
// for its own internally-renamed-but-prototype-matching parameter.
//
// For unit-type SLOT 1..99 inclusive (slot 0 is never examined -- the loop counter is seeded to 1,
// not 0, `MOV [EBP-0x1c],1` @0x0049a040; the bound is `slot < 100`, `CMP ...,0x64 ; JL` @0x0049a047):
// read `Building[building_type_id].unit_quant[slot]`'s 8-byte entry as TWO RAW int32 halves --
// qty at +0, flags at +4 -- and treat the slot as OCCUPIED iff `(flags & 0x7fffffff) != 0 ||
// qty != 0` (the +4 half, masked, is tested FIRST via `TEST ...,0x7fffffff ; JNZ` @0x0049a066/
// 0x0049a070; the +0 half is only consulted when the masked +4 half was zero, `CMP ...,0 ; JZ`
// @0x0049a072/0x0049a079). The mask deliberately drops the entry's top bit: an entry whose ONLY set
// bit is 0x80000000 in the +4 half does NOT count as occupied.
//
// On the FIRST occupied slot found, `slot` DOUBLES as the index into `Unit[100]`
// (sim_view::cfg_units, RID_UNIT, stride 0x23f -- `IMUL EAX,slot,0x23f` @0x0049a07b): the return
// value is `Unit[slot].soldier_count != 0` (1 or 0). This is NOT an any-of-100 scan -- it returns on
// the FIRST occupied slot even if that slot's unit type has soldier_count==0, even when a LATER slot
// would have qualified. If no slot 1..99 is occupied, returns 0 without ever indexing Unit[].
// Returns int32_t 0/1 only, never a count (0x0049a08b/0x0049a094/0x0049a09f). No bounds check
// anywhere on `building_type_id` against Building[100]'s extent, nor on the derived `slot` against
// Unit[100]'s extent (slot is always in [1,99] by loop construction, so the latter never actually
// escapes the table; the former is unguarded exactly as the original leaves it -- not "fixed" here).
//
// ---- GHIDRA GAP (recorded, not fixed here) ---------------------------------------------------------
// `mh_cfg_final_struct_Building.unit_quant` is typed `double[100]` at offset 0x27d
// (mh_structs.gen.h's own static_assert), which is WRONG for this reader: the asm never executes a
// single x87 instruction against these bytes, only the two raw integer TEST/CMP ops above. Four other
// already-translated readers in this tree (ai_build_sources.cpp, sim_bldg_find_idle_producer.cpp,
// sim_bldg_side_has_aircraft_producer.cpp, sim_bldg_state_prod.cpp, ai_unit_housing.cpp) DO index this
// same field as a `double` and compare `!= 0.0` / `> 0.0` -- provably bit-equivalent to the raw
// integer test for THIS entry's occupied/not-occupied question (masking the sign bit out of the high
// dword and testing both halves against zero is exactly IEEE 754's `!= 0.0` bit pattern), but routing
// a body with ZERO x87 instructions in its own disassembly through a double comparison anyway is what
// this translation avoids: the read below goes through `offsetof(cfg_building, unit_quant)` +
// `std::memcpy` at the raw byte address, never through the `.unit_quant[slot]` double-typed field.
// Do NOT "fix" this by switching to the double field -- the field's Ghidra type is the thing that is
// wrong here (see the .h banner's own field derivation from `0xd9eefd - 0xd9ec80 = 0x27d`, entry
// stride 8, second dword at +4 from `0xd9ef01`), and retyping it is conductor work (a candidate retype
// to a `{int32 qty; int32 flags;}` pair array needs a survey of the other 5 readers first -- not a
// blind change from this one call site).
//
// ---- WHY std::memcpy, NOT A DIRECT POINTER DEREFERENCE --------------------------------------------
// `offsetof(cfg_building, unit_quant)` is 0x27d (odd) and `sizeof(cfg_building)` is 0x842 (even, but
// not a multiple of 4), so `Building[building_type_id].unit_quant[slot]`'s raw byte address is NEVER
// 4-aligned for any `building_type_id`/`slot` pair (matches the raw asm operand `0xd9ef01`, itself
// 1 mod 4). `mh_cfg_final_struct_Building` is declared under `#pragma pack(push, 1)`
// (mh_structs.gen.h), so ordinary struct-member access already generates unaligned-safe code for any
// PROPERLY TYPED field -- but a `reinterpret_cast` to `const int32_t *` at an arbitrary byte offset
// and a direct dereference is a genuinely misaligned access at the language level. `std::memcpy` is
// this codebase's established idiom for exactly that read (see save_driver.cpp, lockstep/
// rx_dispatch.cpp, tact_ui_sel_panel_single_mode_tick.cpp).
//
// ---- SHADOW SITE ------------------------------------------------------------------------------------
// Manifest entry + mh_shadow.gen.h macros already exist (conductor, 2026-09-02): 0 measured writable
// regions (this is a pure query -- the committed seam for llm_bldg_first_occupied_unit_slot_has_soldiers's
// own region array is `{{"", RID_COUNT, 0u, 0u}}`), so `compare_return` is the entire verdict, same
// class as sim_unit_type_group_index.cpp's own empty-region-set site.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_bldg_first_occupied_unit_slot_has_soldiers @0x0049a025. See the header banner above for the
// full derivation. Pure read: no sim_store parameter (nothing in this closure writes anything here),
// no `<tu>_calls` (the only callee is the inert stack-capacity probe, omitted per translator-brief
// rule 6).
int32_t first_occupied_unit_slot_has_soldiers(const sim_view &v, int32_t building_type_id);

} // namespace detail

// Live wrapper: the logic applied to state().read. Parameter name matches the committed prototype
// (`building_index`, addr/mh_calls.gen.h:1365) even though the header banner above documents it as a
// building TYPE id, not a roster index -- same convention sim_unit_goal_in_weapon_range.h uses for its
// own internally-renamed parameter.
int32_t first_occupied_unit_slot_has_soldiers(int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
