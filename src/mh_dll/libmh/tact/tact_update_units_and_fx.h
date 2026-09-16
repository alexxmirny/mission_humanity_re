//
// tact/tact_update_units_and_fx.h -- TACT1C: the per-frame roster+FX pump. Called once per tactical
// frame from the frontier `llm_tact_frame` (roster_write_via: llm_tact_unit_weapons_tick,
// llm_tact_fx_update_projectile per tools/data/tact_migration.json).
//
//   llm_tact_update_units_and_fx @0x0042aff2 (0xad)
//   void __watcall llm_tact_update_units_and_fx(void)  -- committed prototype, no parameters
//
// SHAPE: two independent, back-to-back sequential scans, no shared state between them beyond the
// move-path-cache reset each scan touches (see step 2/4 below). Neither scan reads or writes the
// other's array.
//
// PROOF PATH: OFFLINE (tacttest) -- re-derived and RE-VERIFIED directly against the raw opcodes in
// tmp/decomp_tact/llm_tact_update_units_and_fx_0042aff2.asm (see the CORRECTION note below). This function
// makes NO outward calls of its own other than the two already-translated migration siblings below,
// invoked through their PUBLIC wrappers (translator-brief.md 3b's in-manifest-sibling exception --
// see this header's own note above the calls, and the .cpp) -- so there is no `_calls` struct here.
// The opening `CALL utils_assert_stack_capacity` is the standard inert CRT prologue (translator-brief
// rule 6) and is omitted.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_update_units_and_fx @0x0042aff2.
//
//  0. @0x0042b00a: MOVE_PATH_CACHE_VALID = 0 (first of two writes -- see step 4; both PRESERVED
//     literally, redundant in the original, per this item's own handoff).
//
//  1. @0x0042b014-0x0042b064: SCAN 1, unit slots [TACT_UNIT_FIRST_SLOT, TACT_UNIT_LAST_SLOT] = [1,
//     0x80] inclusive (the tactical roster's own documented 1-based convention, tact_state.h's
//     TACT_UNIT_FIRST_SLOT/TACT_UNIT_LAST_SLOT).
//
//  2. @0x0042b02e: MOVE_PATH_CACHE_VALID = 0 -- SECOND write, inside the loop body, executed on
//     EVERY iteration regardless of the type gate below. Redundant with step 0 in the original;
//     PRESERVE both stores literally (translator-brief rule: preserve reset-then-fill / redundant
//     writes -- do not dedupe).
//
//  3. @0x0042b03f-0x0042b056: the call gate. Re-derived directly from the two CMP/Jcc pairs (not
//     re-derived from the prior handoff's prose, which read as an OR of two boundary EQUALITIES and
//     does not match the opcodes):
//       CMP units[i].type, 0   ; JBE skip   -> skip (no call) when type == 0 (unsigned byte, so
//                                              "<= 0" is exactly "== 0")
//       CMP units[i].type, 0x80 ; JC call   -> call only when type < 0x80 (unsigned)
//     The two conditions are evaluated in SEQUENCE and both gate the SAME single CALL instruction
//     (0x0042b05d) -- there is exactly one call site in the assembly, not two -- so the call
//     condition is the CONJUNCTION `type > 0 && type < 0x80`, transcribed as one combined `if`
//     guarding one call statement (never as two separate call sites, one per comparison).
//
//  4. @0x0042b05a-0x0042b05d: if the gate holds, `unit_weapons_tick(i)` -- the ALREADY-TRANSLATED
//     TACT1C sibling, called through its PUBLIC wrapper `mh::tact::unit_weapons_tick(int32_t)`
//     (translator-brief 3b's in-manifest-sibling exception: this is a same-domain, already-proven
//     migration member, not a `mh::call::` real-VA marshal).
//
//  5. @0x0042b064-0x0042b095: SCAN 2, fx-pool slots [0, TACT_FX_POOL_SLOTS) = [0, 0x400). Gate:
//     `CMP fx_pool[i].fx_type, 0 ; JBE skip` -> call `fx_update_projectile(i)` iff `fx_type != 0`
//     (matches `mh_llm_tact_fx::fx_type`'s own Ghidra comment: "0 = free slot"). Called through its
//     PUBLIC wrapper `mh::tact::fx_update_projectile(int32_t)`, same rationale as step 4.
//
void update_units_and_fx(const tact_view &v, tact_store &own);

} // namespace detail

void update_units_and_fx();

// Declared here per the module convention; DEFINED in tact_update_units_and_fx.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
