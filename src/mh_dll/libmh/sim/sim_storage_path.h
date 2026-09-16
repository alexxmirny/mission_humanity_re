//
// sim/sim_storage_path.h -- the storage exit/approach path-setup pair (RI-SIM / SIM1-G3 second
// slice):
//
//   llm_strat_storage_setup_exit_path     @0x00495cd3 (0x248 B)
//   void __mh_watcall_ebx_volatile llm_strat_storage_setup_exit_path(uint player, int unit_index,
//       byte exit_x, byte exit_y, int path_slot, int storage_slot)
//   (EAX=player, EDX=unit_index, BL=exit_x, CL=exit_y, Stack[0x4]=path_slot, Stack[0x8]=storage_slot
//   -- the .asm header is the authority; both stack slots are FULL dwords, never MOVZX'd)
//
//   llm_strat_storage_setup_approach_path @0x00495b6f (0x164 B)
//   void __mh_watcall_ebx_volatile llm_strat_storage_setup_approach_path(uint player, int
//       unit_index, int path_slot, int storage_slot)
//   (EAX=player, EDX=unit_index, EBX=path_slot, ECX=storage_slot -- ALL register params, no stack
//   args at all; a DIFFERENT parameter list from its sibling, not just a dropped exit_x/exit_y)
//
// Translated from the DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_setup_{exit,approach}_path_*.asm)
// address-by-address, NOT from any exported Ghidra `.c` draft. `player` is reloaded via a 16-bit
// MOVZX at every use in BOTH functions (never a plain 32-bit reload) -- narrowed once here, same
// convention sim_storage_can.cpp's trio already established. `unit_index`/`path_slot`/`storage_slot`
// are read as FULL 32-bit dwords throughout both bodies -- no narrowing needed for those three.
//
// ---- THESE ARE ALMOST-PARALLEL SIBLINGS, NOT ONE BODY WITH A DROPPED PARAMETER ------------------
// Both: (1) resolve storage_of(player,storage_slot).b_index -> building_of(...).building_id: this is
// EXACTLY the same lookup sim_storage_can.cpp's trio performs, reusing the existing storage_of()/
// building_of() free functions -- (2) read a per-building-type "door route" field of the existing
// Building[] cfg struct (see the RESOLVED section below) and copy it byte-for-byte into this player's
// path buffer slot until a 0xff
// terminator or PATH_WAYPOINTS_PER_SLOT (300) entries, writing ONLY the `.heading` field of each
// mh_llm_strat_path_waypoint entry (`.run_length` is never touched by either function -- reproduce
// that omission, do not zero-initialise it) -- (3) call the ORIGINAL llm_strat_path_attach_slot to
// bind the slot -- (4) reset units[player][unit_index].path_cursor to 0.
//
// They differ in more than the obvious extra (exit_x, exit_y) parameter pair:
//   * setup_exit_path ALSO sets move_microstep=0x1f, computes move_heading as
//     (route_table[building_id][0] % 24) (an IDIV -- see below), looks up
//     move_microsteps[move_heading][move_microstep].facing and stores it into facing_target, and
//     writes exit_x/exit_y into x/y. setup_approach_path does NONE of this -- its move_heading is
//     the RAW route-table byte (no modulo), and it never touches facing_target/x/y at all.
//   * setup_exit_path's copy loop is a PRE-test while (checks route_table[cursor] against 0xff and
//     cursor<300 BEFORE copying -- can copy zero entries). setup_approach_path's copy loop is a
//     POST-test do-while (always copies entry 0 first, THEN checks the NEXT entry) -- a genuine
//     structural difference, not a stylistic one. Reproduce each literally from its own .asm.
//   * setup_approach_path reads units[player][unit_index].x and .y into locals at the very top
//     (0x00495b90-0x00495bc7) that are NEVER referenced again anywhere in the remaining 0x164-byte
//     body (confirmed by scanning every instruction) -- a provably dead pair of reads (no side
//     effects; nothing else touches those two locals). Omitted here; see the translation report's
//     uncertainties for why this is safe to drop rather than transcribe as inert local variables.
//
// The `CALL utils_assert_stack_capacity` prologue in both (0x00495cdb / 0x00495b77) is the standard
// inert probe (translator-brief rule 6) -- omitted.
//
// ---- ONE MORE OVERLAPPING-LOOKUP DETAIL: facing_current, NOT A DEAD "y" WRITE (RESOLVED) --------
// setup_exit_path's move_microsteps[...].facing lookup (0x00495dbe-0x00495e31) is stored TWICE in the
// assembly: once into facing_target (0x00495dd7) and once more at 0x00495e31, at address
// [EDX+0xdd8c75]. The translator's first reading called the second store `y` and, since `y` is
// unconditionally overwritten by the real exit_y four instructions later (0x00495e69), treated it as
// a dead store and dropped it. RESOLVED (conductor + reimpl-verify, SIM1-G3, 2026-08-21):
// this was wrong -- 0xdd8c75 is units_base+0x2d = facing_current (mh_structs.gen.h's static_assert),
// a DIFFERENT field from `y` (+0x85, address 0xdd8ccd). The two never alias, so the exit_y write does
// NOT overwrite this one; facing_current is a real, permanent write, set to the SAME table value as
// facing_target (move_heading/move_microstep are unchanged between the two lookups). Fixed: `u.
// facing_current = u.facing_target;` immediately after the facing_target assignment.
//
// ---- FIELDS: OFFSETS CONFIRMED AGAINST mh_structs.gen.h's static_asserts -------------------------
// mh_map_object_unit: x@0x84, y@0x85 (byte), facing_target@0x2c (byte), move_microstep@0xa2 (int32,
// 0..0x1f), move_heading@0xac (byte), path_cursor@0xa6 (int32), path_slot_id@0xaa (byte, written by
// llm_strat_path_attach_slot, not here). mh_map_object_building: building_id@0x2 (int16_t).
// mh_map_object_unit_storage: b_index@0x0 (int32_t). mh_llm_strat_move_microstep (the
// _G_LLM_STRAT_MOVE_MICROSTEPS entry, already bound as sim_view::move_microsteps): x_off@0x0,
// y_off@0x1, facing@0x2 (byte, "heading 1..0x18" per its own field comment -- matches facing_target's
// 24-way domain). mh_llm_strat_path_waypoint (sim_view::path_buffers / sim_store::path_buffer_at()):
// heading@0x0, run_length@0x1 (both byte).
//
// NOTE ON "facing fields" -- the batch driver's hazard note describes setup_exit_path as writing
// "units[].facing fields dd8ccc/dd8ccd from exit_x/exit_y". The struct's own static_asserts say
// otherwise: 0xdd8ccc/0xdd8ccd are `x`/`y` (offsets +0x84/+0x85), NOT `facing_target`/`facing_current`
// (+0x2c/+0x2d) -- verified independently by walking the byte offsets from THREE different known
// fields (move_heading@0xdd8cf4, move_microstep@0xdd8cea, path_cursor@0xdd8cee) back to a common unit
// base and cross-checking every resulting offset against mh_structs.gen.h's static_asserts, all of
// which matched exactly. This translation follows the struct (the authority per the translator
// brief), not the hazard note's paraphrase; flagged in the translation report rather than silently
// resolved either way.
//
// ---- CALLEES (ORIGINAL, indirected through a `_calls` struct like every other sim/ TU) -----------
// Both: llm_strat_path_attach_slot (0x00496a7b) -- called (player, unit_index, path_slot), matching
// its committed prototype exactly. It is ALREADY a migrated function in this codebase
// (sim_path_attach_slot.cpp/.h, SIM1-G2) but every existing caller in this codebase
// (sim_path_alloc_slot.cpp, sim_path_make_single_step.cpp, sim_path_solver.cpp,
// sim_unit_state_plot_turn_path.cpp) calls it via `mh::call::llm_strat_path_attach_slot` -- the
// ORIGINAL machine code, not `mh::sim::detail::path_attach_slot` -- and this file follows that same,
// consistent precedent rather than inventing a new convention.
//
// ---- RESOLVED (conductor, SIM1-G3, 2026-08-21): TWO FIELDS OF THE EXISTING Building
// CFG STRUCT, NOT NEW STANDALONE ARRAYS -----------------------------------------------------------
// The translator's OPEN QUESTION above is settled: 0x00d9f496 and 0x00d9f4a0 are NOT independent
// regions. `get-data` at 0x00d9f496 resolves inside `cfg_final_struct_Building[100]` (the existing,
// already-typed `Building` global at 0x00d9ec80, size 211400 = 100 * 2114) -- confirming the row
// stride the translation derived from the IMUL literal (0x842 = 2114 = sizeof(cfg_final_struct_
// Building)) IS the struct's own per-entry size, not a coincidence. Struct-relative offsets:
// 0x00d9f496 - 0x00d9ec80 = 0x816 (2070), 0x00d9f4a0 - 0x00d9ec80 = 0x820 (2080) -- exactly the "10
// bytes apart" the translator measured. 0x816 fell inside the struct's existing, already-known
// undifferentiated 146-byte pad (offset 0x78e-0x81f); 0x820 was the single already-named `facing`
// byte (per-building-type orientation, read by 5 OTHER already-migrated functions:
// llm_strat_locate_active_port, llm_strat_storage_get_approach_tile, llm_strat_unit_group_step_ground,
// llm_strat_unit_group_step_plane, llm_strat_prod_spawn_arrived_unit -- a 5th consumer the translator's
// own grep did not surface) immediately followed by 13 more pad bytes (reserved_0x821).
//
// Applied via the annotation applier (struct is NOT packed -- isPackingEnabled=false, confirmed
// before editing per the packed-struct corruption trap -- replaceAtOffset is
// safe here, struct total size unchanged at 2114 both before and after):
//   byte[10] door_exit_route      @ +0x816  (was the tail of the 146-byte pad)
//   byte[14] door_approach_route  @ +0x820  (absorbs the former `facing` byte + reserved_0x821[13];
//                                            door_approach_route[0] is byte-identical to old `facing`)
// The 5 existing `.facing` readers were updated to `.door_approach_route[0]` (a pure syntactic change,
// same value). This TU reads both fields through `v.cfg_buildings[building_id].door_exit_route` /
// `.door_approach_route` directly -- no new sim_view member was needed, since cfg_buildings was
// already a bound accessor. See tools/data/ghidra_findings.json 2026-08-21-1329-5 for the full
// derivation + apply record.
//
// Both fields are declared with their PROVEN-safe minimum size (10 / 14 bytes) -- less than the full
// 300-entry (PATH_WAYPOINTS_PER_SLOT) bound either loop can walk. The .cpp reads both through a raw
// `const uint8_t*` into the field's own storage rather than fixed-array indexing, so an unterminated
// route past the declared length reads on into the immediately-adjacent field (door_exit_route ->
// door_approach_route, contiguous at +0x816..+0x82d with no gap; door_approach_route -> `equivalent`
// and beyond, and past the whole 2114-byte record into the NEXT building_id's record) -- the same flat,
// unbounded `Building[] + building_id*sizeof(record) + offset` addressing the ORIGINAL performs. No
// real cfg-authored route is expected to run this long (see the OPEN QUESTION resolution above), so
// this only matters for Law 2 preservation, not expected real-game behaviour.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the one outward call both functions share, indirected for testability (see sim_storage_can.h's
// identical rationale -- a direct mh::call:: inside detail:: reaches the live game image). ----
struct storage_path_attach_calls {
    void (*path_attach_slot)(int32_t player, int32_t unit_idx, int32_t slot); // @0x00496a7b
};

const storage_path_attach_calls &live_storage_path_attach_calls();

namespace detail {

// llm_strat_storage_setup_exit_path @0x00495cd3. See the header banner above for the full derivation.
void storage_setup_exit_path(const sim_view &v, sim_store &own, const storage_path_attach_calls &c,
                             uint32_t player, int32_t unit_index, uint8_t exit_x, uint8_t exit_y,
                             int32_t path_slot, int32_t storage_slot);

// llm_strat_storage_setup_approach_path @0x00495b6f. See the header banner above for the full
// derivation.
void storage_setup_approach_path(const sim_view &v, sim_store &own, const storage_path_attach_calls &c,
                                 uint32_t player, int32_t unit_index, int32_t path_slot,
                                 int32_t storage_slot);

} // namespace detail

// Live wrappers: the logic applied to state(). Match the committed prototypes
// (sig_llm_strat_storage_setup_{exit,approach}_path in addr/mh_export.gen.h / addr/mh_calls.gen.h)
// exactly.

void storage_setup_exit_path(uint32_t player, int32_t unit_index, uint8_t exit_x, uint8_t exit_y,
                             int32_t path_slot, int32_t storage_slot);
void storage_setup_approach_path(uint32_t player, int32_t unit_index, int32_t path_slot,
                                 int32_t storage_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
