//
// sim/sim_storage_find_home_for_unit.h -- storage-slot picker for a produced/recruited unit (RI-SIM /
// SIM1D, batch D).
//
//   llm_strat_storage_find_home_for_unit @0x00463328 (0x386 bytes)
//   `int __mh_watcall_ebx_volatile llm_strat_storage_find_home_for_unit(uint player, uint unit_type,
//   int probe_slot)` per the .asm header (player=EAX, unit_type=EDX, probe_slot=EBX) -- matches
//   mh_calls.gen.h's `llm_strat_storage_find_home_for_unit(uint32_t player, uint32_t unit_type,
//   int32_t probe_slot)`. Translated from the DISASSEMBLY
//   (tmp/decomp_sim/llm_strat_storage_find_home_for_unit_00463328.asm), NOT from the Ghidra .c draft:
//   the draft's local variable numbering does not match the real EBP offsets this file cites (its
//   "local_1c"/"local_18"/"local_20" names are Ghidra's own renumbering, not the raw stack slots), and
//   every literal/threshold/callee below was independently re-walked against the CMP/JC/JBE opcodes.
//
// PARAMETER MASKING -- every read of `player`/`unit_type` in the body goes through MOVZX WORD (e.g.
// 0x00463347, 0x00463364), so both are truncated to their low 16 bits at every use. Masked ONCE here
// (`p`/`ut`) mirroring sim_prod_shuttle_complete.cpp's established fix for the same raw-export-thunk
// hazard (a caller supplying garbage in the upper 16 bits is not hypothetical -- the committed export
// thunk does a bare PUSH, no clearing).
//
// ---- TWO CALL SHAPES, BY BRANCH (probe_slot == 0 vs != 0) --------------------------------------
//
// `probe_slot == 0` selects the ROUND-ROBIN search (0x00463364-0x004636b1); `probe_slot != 0`
// selects a PROBE of that one specific slot (0x004635cf-0x004636aa) -- CMP dword[EBP-0x20],0 / JNZ
// at 0x0046335a/0x0046335e. The two branches call TWO DIFFERENT original acceptance-check helpers:
//   * round-robin: `llm_bldg_storage_accepts_unit_type(Building[building_id].type,
//     Unit[unit_type].type)` @0x00497ac4 -- looks up BOTH cfg TYPE fields itself.
//   * probe: `llm_strat_storage_type_accepts_unit(building_id, unit_type)` @0x00497b91 -- takes the
//     raw ids and does its own cfg lookup internally.
// Both are pure queries (no write, no other outward call) and are marshalled via mh_calls.gen.h. Per
// sim_bldg_defense_cost.h's precedent (a single, provably-pure callee is STILL indirected through a
// `_calls` struct, never called via `mh::call::` directly from `detail::`), both are bound through
// `storage_find_home_for_unit_calls` so `detail::storage_find_home_for_unit` stays drivable by
// `net_selftest.exe simtest` over heap buffers.
//
// ---- THE ROUND-ROBIN SEARCH ---------------------------------------------------------------------
//
// (1) 0x0046336e-0x00463406: classify `Unit[unit_type].type` (a full DWORD read, `uint32_t`) into one
//     of four persistent per-class cursors on `_G_LLM_STRAT_UNIT_HOUSING_STATS[player]`
//     (`own.unit_housing_at`), OR return 1 immediately for a fixed sub-range, OR leave the cursor
//     variable UNTOUCHED (see the uncertainty below) -- read directly off the CMP/JC/JBE chain, in
//     the SAME shape sim_unit_housing_count.cpp's own ladder over the identical field already
//     documents (four of five thresholds -- 0xf/0x1/0xa/0x10/0x12 -- are bit-identical to that
//     function's own five-way ladder; the difference is what happens past 0x12):
//       type == UNIT_TYPE_UNDEFINED (0)                        -> cursor left untouched (uncertainty)
//       1 <= type <  UNIT_TYPE_A_WALKER (0xb)                  -> rr_cursor_soldiers
//       UNIT_TYPE_A_WALKER <= type <  UNIT_TYPE_A_HELI (0xf)   -> rr_cursor_vehicles
//       UNIT_TYPE_A_HELI   <= type <  UNIT_TYPE_A_PLANE (0x11) -> rr_cursor_helis
//       UNIT_TYPE_A_PLANE  <= type <  UNIT_TYPE_A_HELI_MOTHER (0x13) -> rr_cursor_planes
//       UNIT_TYPE_A_HELI_MOTHER <= type <= UNIT_TYPE_H_HELI_CARGO (0x18) -> `return 1` immediately
//         (0x004633fa) -- the six heli-mother/heli-shuttle/heli-cargo variants (both race sides) never
//         search storage at all; the caller treats the literal 1 as "home found", not a real slot id.
//       type > UNIT_TYPE_H_HELI_CARGO (0x18)                   -> cursor left untouched (uncertainty)
//     UNIT_TYPE_A_WALKER/A_HELI/A_PLANE/A_HELI_MOTHER/H_HELI_CARGO come from sim_unit_type_predicates.h
//     / sim_order_enqueue.h (already pinned there); UNIT_TYPE_UNDEFINED from
//     sim_unit_type_predicates.h.
//
// (2) 0x00463406-0x00463433: the scan. `remaining` (EBP-0x1c) seeds from
//     `storage_of(v,p,0).b_index` (0x00463351-0x00463357) and DECREMENTS once per OCCUPIED slot
//     examined (0x00463453), regardless of accept/reject outcome -- `scan_budget` (EBP-0x24) is a
//     SEPARATE loop counter incremented once per ITERATION (0x0046341e), also regardless of outcome.
//     The loop exits when EITHER reaches its own bound (`scan_budget < 0x19` @0x00463410-adjacent,
//     `remaining != 0` @0x00463413-0x00463417) -- see the FUNCTION-SPECIFIC HAZARD note below on why
//     these two 0x18/0x19 pairs (the persistent-cursor wrap and the scan budget) are DIFFERENT
//     constants that must not be unified.
//
//     `storage_of(v,p,0).b_index` being read as a scan budget -- rather than "slot 0's owning
//     building" -- is itself an UNCERTAINTY (see below): slot 0 is never a round-robin target (the
//     cursor only ever lives in [1,24]), so this reads as a plausible reuse of that slot's leading
//     field as a per-player "how many occupied storage slots do I have" cache, but no writer for it
//     was traced in this closure (out of scope for a single-function translation) and the field is
//     otherwise documented as `b_index` (docs/structs.md / mh_structs.gen.h) with no such alternate
//     meaning recorded anywhere else in the project.
//
// (3) 0x00463423-0x00463433: EVERY iteration advances the cursor FIRST (`cursor += 1`, wrap
//     `if (cursor > 0x18) cursor = 1`) THEN tests `storage_of(v,p,cursor).b_index` -- an empty slot
//     (`b_index == 0`) costs an iteration but not a `remaining` decrement (0x00463446-0x0046344d JZ
//     straight past the DEC). An occupied slot decrements `remaining`, then chains FOUR conditions
//     (all must hold to accept): `llm_bldg_storage_accepts_unit_type(...) != 0`,
//     `building.built_flags == BUILT_FLAGS_OPERATIONAL(3)`, `building.online_state != 0`,
//     `slot.occupancy < 0x32(50)` (matches `mh_map_object_unit_storage::occupancy`'s own field
//     comment, "occupancy weight... gated <50" -- no new named constant needed). On acceptance, a
//     SECOND classification of `Unit[unit_type].type` (0x0046353b-0x004635ba, a fresh DWORD re-read,
//     not a cached reuse of the first) decides WHICH persistent cursor field to persist the accepted
//     slot into -- same four-way split as (1) but WITHOUT the "return 1" arm (impossible to reach
//     here: any type in [0x13,0x18] already returned at (1)) and without a separate UNDEFINED
//     sub-branch beyond "no write":
//       type2 <  UNIT_TYPE_A_WALKER (0xb)  -> rr_cursor_soldiers, UNLESS type2 == UNDEFINED (no write)
//       type2 <  UNIT_TYPE_A_HELI (0xf)    -> rr_cursor_vehicles
//       type2 <  UNIT_TYPE_A_PLANE (0x11)  -> rr_cursor_helis
//       type2 <  UNIT_TYPE_A_HELI_MOTHER (0x13) -> rr_cursor_planes
//       type2 >= UNIT_TYPE_A_HELI_MOTHER (0x13) -> no write (0x00463566's unconditional JMP past every
//         assignment) -- the ONLY way to reach this arm is the same type>0x18 uncertainty as (1),
//         since [0x13,0x18] cannot reach the loop body at all.
//     The function then returns `cursor` (the accepted slot id) directly -- NOT via the `remaining`/
//     `scan_budget` exit path.
//
// If the loop exits via either bound without an accept, the function returns 0 (0x004636aa).
//
// ---- THE PROBE BRANCH (probe_slot != 0) -----------------------------------------------------------
//
// 0x004635cf-0x004636aa: tests exactly `storage_of(v,p,probe_slot)` -- no cursor read, no cursor
// write, no `remaining`/`scan_budget` bookkeeping at all. Same four-condition accept chain as (3)
// above (accepts-check, built_flags, online_state, occupancy<50), calling
// `llm_strat_storage_type_accepts_unit` instead. Returns `probe_slot` on accept, 0 otherwise.
//
// ---- FUNCTION-SPECIFIC HAZARD: PRESERVE THE SPLIT ROUND-ROBIN BUG, DO NOT UNIFY --------------------
//
// The hardcoded-limits survey "The split round-robin": the PERSISTENT per-class cursor wrap
// (`if (cursor > 0x18) cursor = 1`, 0x00463426-0x0046342c) and the LOCAL scan-budget bound
// (`scan_budget < 0x19`, 0x0046340d-0x00463411) are two INDEPENDENTLY hardcoded 24/25 constants that
// happen to agree in vanilla but drifted apart in the 2026-07-07 grand-build regression (budget left
// at 25 while the wrap was cap-raised to 127) -- storage buildings "behind" the cursor became
// unreachable and produced units were silently discarded. Reproduced here as TWO SEPARATE literals
// (`0x18`/`0x19` for the wrap, `0x19` for the budget) rather than one shared named constant, exactly
// per the task brief: "do NOT unify them or 'fix' the mismatch -- translate exactly as written, this
// is a documented preserve_bug candidate, not a translation error to correct."
//
#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h"        // BUILT_FLAGS_OPERATIONAL, UNIT_TYPE_A_HELI/H_HELI/
                                          // A_PLANE/H_PLANE/A_HELI_CARGO/H_HELI_CARGO -- already
                                          // pinned there (SIM1C), reused rather than re-declared.
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_UNDEFINED/A_HELI_MOTHER/H_HELI_MOTHER/
                                          // A_HELI_SHUTTLE/H_HELI_SHUTTLE/A_WALKER/H_WALKER/
                                          // H_GROUND/A_GROUND -- already pinned there (SIM1A).
#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls, one per branch -- see the header banner on why both go through a `_calls`
// struct rather than `mh::call::` directly (sim_bldg_defense_cost.h precedent).
struct storage_find_home_for_unit_calls {
    // llm_bldg_storage_accepts_unit_type @0x00497ac4 -- round-robin branch.
    int32_t (*bldg_storage_accepts_unit_type)(int16_t building_type, uint16_t unit_type);
    // llm_strat_storage_type_accepts_unit @0x00497b91 -- probe branch.
    int32_t (*storage_type_accepts_unit)(uint32_t building_index, uint16_t unit_index);
};

const storage_find_home_for_unit_calls &live_storage_find_home_for_unit_calls();

// The logic over an explicit view + store + calls table, matching every other sim TU's split.
namespace detail {

// llm_strat_storage_find_home_for_unit @0x00463328. See the header banner for the full derivation.
// `probe_slot == 0` selects the round-robin search (which reads AND writes
// `_G_LLM_STRAT_UNIT_HOUSING_STATS[player].rr_cursor_*`, hence the mutable `own`); `probe_slot != 0`
// only reads. Returns the accepted storage slot id (1..24, or the probed slot), the sentinel 1 for
// the heli-mother/-shuttle/-cargo short-circuit, or 0 for "no home".
int32_t storage_find_home_for_unit(const sim_view &v, sim_store &own,
                                   const storage_find_home_for_unit_calls &c, uint32_t player,
                                   uint32_t unit_type, int32_t probe_slot);

} // namespace detail

// Public wrapper. Signature matches the committed export/call/shadow shapes
// (sig_llm_strat_storage_find_home_for_unit in addr/mh_export.gen.h).
int32_t storage_find_home_for_unit(uint32_t player, uint32_t unit_type, int32_t probe_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
