//
// sim/sim_bldg_side_has_aircraft_producer.h -- "does this player have a building that can currently
// turn out a plane" query: walks the player's occupied building slots, and for each one scans every
// unit type for a positive production-capacity/queue entry whose cfg type is A_PLANE/H_PLANE
// (RI-SIM / SIM1D).
//
// llm_strat_bldg_side_has_aircraft_producer @0x004d8b89 (0xf4 B, `int __watcall
// llm_strat_bldg_side_has_aircraft_producer(int player)`, committed prototype -- param `player`
// storage=EAX:4). Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_side_has_aircraft_producer_004d8b89.asm), not from the Ghidra `.c`
// draft -- the draft's overall shape (outer occupied-slot walk, inner unit-type scan, two chained
// conditions) reads correctly and was used as a map, but every index expression and branch target
// below was independently re-derived from the raw SHL/SUB/IMUL/CMP opcodes per house rules.
//
// ---- THE OUTER COUNTER: buildings[player][0].index DOUBLES AS "OCCUPIED SLOTS REMAINING" --------
// 0x004d8b9b-0x004d8bb4: `MOVZX EBP, word ptr [player*0x6aa4 + 0xc3d2a0]` -- `0xc3d2a0` is
// `v.buildings[player][0].index`'s own address (struct offset 0, two bytes before `building_id` at
// offset 2, cross-checked against `0xc3d2a2` == the `building_id` base every other read in this
// function and in `_CONTEXT.md`'s confirmed identity uses). Zero-extended (word, not sign-extended)
// into EBP, which the rest of the body only ever TESTs (`TEST EBP,EBP`) and DECrements once per
// occupied slot whose unit scan came up empty (0x004d8c6e) -- i.e. it is a per-player "how many
// occupied slots remain to be checked" cursor, the same slot-0-doubles-as-a-count idiom
// sim_state.h documents for `soldiers[0].owner_unit` / the housing-roster counts. The slot cursor
// itself (`ESI`, starting at 1) is UNBOUNDED against `BUILDINGS_PER_PLAYER` -- the original stops
// only when this counter hits 0, not when the slot index reaches 100. See uncertainties in the
// translation report on why this is transcribed as-is rather than clamped.
//
// ---- THE OCCUPIED-SLOT GATE (0x004d8bc1-0x004d8bf9) ------------------------------------------------
// `EBX = player*0x6aa4` (`IMUL`-equivalent SHL/SUB chain, same row stride BUILDINGS_PER_PLAYER's
// comment already cites) + `EAX = slot*0x111` (row stride, same SHL/ADD chain shape as the sibling
// `llm_strat_bldg_find_idle_producer_for_unit` per `_CONTEXT.md`), then
// `CMP word ptr [EBX+EAX+0xc3d2a2],0` -- `v.buildings[player][slot].building_id != 0`. An empty slot
// (JZ) skips straight to `LAB_004d8c6f` (slot++, no decrement of the remaining-count); an occupied
// slot falls into the unit-type scan.
//
// ---- THE INNER SCAN, unit = 1 .. cfg_unit_sec->total (0x004d8bfb-0x004d8c66) ----------------------
// Recomputes `building_id` at the same address (`MOVZX`, this read is the zero-extended one the `.c`
// calls `(uint)*(ushort*)...`), then for each candidate unit id:
//   `IMUL EDX,EAX,0x842` (`sizeof(cfg_final_struct_Building)`, static_assert'd) + `FLDZ`/`FCOMP
//   double ptr [EDX+unit*8+0xd9eefd]`/`FNSTSW`/`SAHF`/`JNC` -- `0.0 <
//   v.cfg_buildings[building_id].unit_quant[unit]` (CONFIRMED identity, `_CONTEXT.md`: `0xd9eefd` ==
//   `cfg_buildings`'s region base `0xd9ec80` + `unit_quant`'s struct offset `0x27d`,
//   static_assert'd). `JNC` (0.0 >= value, i.e. NOT `0.0 < value`) skips straight to the loop
//   increment; only a strictly positive `unit_quant[unit]` continues to the type check.
//   `CMP dword ptr [unit*0x23f+0xe4a176],0x11` / `...,0x12` -- `Unit[unit].type == A_PLANE ||
//   == H_PLANE`. Address cross-check: `Unit`'s region base is `0x00e4a098` (mh_addrs.gen.h) +
//   `cfg_final_struct_Unit::type`'s struct offset `0xde` (static_assert'd) + one more `0x23f`
//   stride (the `Unit[1].type` Ghidra comment anchors the FIRST index, i.e. the displayed constant
//   `0xe4a176` already bakes in `+1*0x23f`) == `0xe4a176` for `unit==1`, matching exactly. This is
//   `v.cfg_units[unit].type` -- the SAME `cfg_units` array `sim_view` already exposes, no new
//   binding needed. `UNIT_TYPE_A_PLANE`/`UNIT_TYPE_H_PLANE` are the project's own established
//   constants (`sim/sim_order_enqueue.h`), reused here rather than re-declared, matching every
//   other sim TU that branches on this pair.
//   A match (either CMP) jumps to `LAB_004d8c5b: MOV EAX,1; JMP 0x004d8b82` -- return 1 immediately,
//   short-circuiting the whole outer walk.
// The loop bound itself, `CMP EBX,dword ptr [0x00e6049c]; JBE <loop>` -- `0x00e6049c` is
// `G_UNIT_COUNT_TOTAL` (mh_addrs.gen.h's own comment: `cfg::static::struct::Unit` (label `UNIT`
// @0xe5f638) `.total`), i.e. `v.cfg_unit_sec->total` -- see the DECLARED NEED below, `sim_view` has
// no binding for this struct yet (ai/ai_state.h's `cfg_unit_section` alias is the precedented
// sibling; `sim_view::cfg_building_sec` is this module's own precedent for the analogous Building
// counterpart).
//
// ---- EXHAUSTING THE INNER SCAN WITHOUT A MATCH (0x004d8c65-0x004d8c6f) ----------------------------
// `INC EBX` (unit++) / `CMP EBX,[G_UNIT_COUNT_TOTAL]; JBE <loop>` (unsigned, INCLUSIVE per
// `mh_addrs.gen.h`'s own comment on this global) -- falling out of the loop `DEC`s the outer
// remaining-count (EBP) exactly once, for the occupied slot just fully scanned; `INC ESI` (slot++)
// always runs next regardless of whether the slot was occupied.
//
// ---- THE OUTER TERMINATION AND THE TWO EXIT JUMPS (0x004d8c70-0x004d8c78) --------------------------
// `TEST EBP,EBP; JNZ LAB_004d8bc1` -- continue while slots remain; on EBP==0, `JMP 0x004d8b80`.
// BOTH exit paths in this function jump to addresses BELOW the function's own recorded entry
// (0x004d8b89) -- `0x004d8b80` (the EBP==0 / "return 0" path) and `0x004d8b82` (the explicit
// `MOV EAX,1` / "return 1" path, exactly 2 bytes later). Neither address is inside the exported
// `.asm` (the export starts at the entry symbol), so the actual bytes at 0x004d8b80-0x004d8b88 are
// NOT directly observed here -- but the inference that `0x004d8b80` is a 2-byte `XOR EAX,EAX`
// falling straight into a SHARED `POP*6; RET` tail at `0x004d8b82` is very strong: the committed
// prototype is a real `int __watcall(...)` (not the phantom-EDX:EAX `undefined8` the "Watcom fake
// return" trap produces on an UNCOMMITTED convention), the two targets
// are exactly 2 bytes apart (the size of `XOR r32,r32`), one of them is reached ONLY after an
// explicit `MOV EAX,1`, and the Ghidra `.c` draft's own independently-derived pseudocode places
// `return 0` / `return 1` at precisely these two control-flow points.
//
// RESOLVED by reimpl-verify (2026-08-13): a live read-memory capture at 0x004d8b80 confirmed the
// exact 9 bytes -- `31 C0 5D 5F 5E 5A 59 5B C3` = `XOR EAX,EAX; POP EBP; POP EDI; POP ESI; POP EDX;
// POP ECX; POP EBX; RET` -- a shared epilogue with two entry points, register-restore-and-return
// only, no global read/write, no side channel. The pop order is the exact mirror of this function's
// own prologue push order (0x004d8b93-0x004d8b98: PUSH EBX,ECX,EDX,ESI,EDI,EBP). The inference above
// was correct; this note is kept for the record rather than deleted.
//
// ---- DECLARED NEED: sim_view has no cfg_static_struct_Unit binding yet -----------------------------
// `ai/ai_state.h` binds this region already as `cfg_unit_section` (`mh::game::mh_cfg_static_struct_Unit`,
// `.total` == `G_UNIT_COUNT_TOTAL`); `sim_view` has the analogous Building-side binding
// (`cfg_building_sec`, `mh_cfg_static_struct_Building`) but nothing for Unit yet. This file is
// written AS IF `sim_view::cfg_unit_sec` (same type/RID as ai_state.h's `cfg_unit_section`) already
// exists, matching the precedent `sim_bldg_footprint_is_clear.h` set for a not-yet-landed shadow
// manifest entry -- it will not compile until the conductor adds the member (read-only, no writer
// anywhere in the sim closure: `.total` is boot-loaded cfg data).
//
// ---- WHAT THIS DOES NOT TOUCH -----------------------------------------------------------------------
// No outward calls besides the inert Watcom stack-capacity probe (translator-brief rule 6) -- pure
// query, writes nothing. Touches x87 floats (one FCOMP), otherwise pure integer indexing.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_side_has_aircraft_producer @0x004d8b89. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. Returns 1 (has an aircraft
// producer) or 0.
int32_t bldg_side_has_aircraft_producer(const sim_view &v, int32_t player);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (`int __watcall llm_strat_bldg_side_has_aircraft_producer(int player)`) exactly.
int32_t bldg_side_has_aircraft_producer(int32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
