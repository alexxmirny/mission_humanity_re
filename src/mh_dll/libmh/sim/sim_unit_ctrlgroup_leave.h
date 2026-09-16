//
// sim/sim_unit_ctrlgroup_leave.h -- llm_strat_unit_ctrlgroup_leave @0x0044928f (0x172 bytes),
// RI-SIM batch-A. Removes a unit from BOTH its assigned Ctrl+digit group AND the
// transient selection group 0, via two SEPARATE compaction scans with no bookkeeping shared
// between them -- unlike sim_unit_ctrlgroup_member.h's add/remove pair (which this function does
// NOT call; it inlines its own group-0 compaction rather than delegating to
// llm_strat_unit_ctrlgroup_remove_member).
//
// ---- the ambient PlayerSide global, read TWICE ------------------------------------------------
//
// Same shape as sim_unit_ctrlgroup_member.h's own pair: `PlayerSide` (sim_view::player_side) is
// read directly, not passed as a parameter, at TWO points in the .asm (0x004492aa and 0x0044935c)
// -- once to index the roster for the unit's CURRENT group id (`a2`), once more (inside the
// `a2 != 0` block only) to clear the roster field and drive the notify call. Nothing runs between
// those two reads that could plausibly change PlayerSide (the only intervening writes are to
// `_G_LLM_STRAT_CTRL_GROUPS[a2]`, a different global), so this translation re-reads it at both
// original call sites rather than hoisting a single cached value -- value-identical, matching the
// documented precedent, not a behaviour change.
//
// ---- llm_strat_unit_ctrlgroup_leave @0x0044928f ------------------------------------------------
//
// 0x004492aa-0x004492c7: `a2 = units[PlayerSide][unit_index].ctrl_group_id` (byte read,
// zero-extended). `ctrl_group_id` is uint8_t at unit+0x2f (mh_structs.gen.h, static_assert'd) --
// same field sim_unit_ctrlgroup_member.h's pair reads/writes.
//
// IF a2 != 0 (0x004492ca-0x0044938b), all of the following runs:
//   * 0x004492db-0x00449301: an UNBOUNDED linear scan of `_G_LLM_STRAT_CTRL_GROUPS[a2].unit_ids[]`
//     for `unit_index` -- there is NO comparison against `.count` anywhere in this loop (verified:
//     the only CMP inside LAB_004492db..LAB_00449301 is the unit_ids[i]==unit_index equality
//     check), unlike the group-0 scan below, which IS bound-checked. Reproduced as unbounded --
//     this is the original's own contract (it trusts the unit is present in the group it claims to
//     be a member of), not a bug to retrofit a bound onto.
//   * 0x00449308: `_G_LLM_STRAT_CTRL_GROUPS[a2].count -= 1` (DEC, BEFORE the shift below).
//   * 0x00449314-0x0044935a: shift-compact survivors down by one slot, `unit_ids[j] =
//     unit_ids[j+1]` for j in [match_index, new_count) -- a plain 16-bit MOV, no width surprise,
//     bound re-read from `.count` every iteration (post-decrement value).
//   * 0x0044935c-0x00449372: `units[PlayerSide][unit_index].ctrl_group_id = 0` -- single BYTE
//     store, second PlayerSide read (see above).
//   * 0x00449379-0x0044938b: unconditional tail call `llm_strat_order_ctrlgrp_flash_member(
//     (uint16_t)PlayerSide, (uint16_t)unit_index, (uint32_t)a2)` -- register args EAX=PlayerSide,
//     EDX=unit_index, EBX=a2, matching mh_calls.gen.h's committed signature. Runs ONLY inside this
//     `a2 != 0` block (confirmed: the CALL at 0x00449386 is unreachable from the a2==0 path, which
//     jumps straight from 0x004492ce to LAB_0044938b) -- NOT unconditional over the whole function.
//
// THEN, ALWAYS (0x0044938b-0x004493f7), regardless of whether the a2!=0 block ran or found
// anything:
//   * 0x00449392-0x004493a7: a BOUND-CHECKED linear scan of `_G_LLM_STRAT_CTRL_GROUPS[0].unit_ids[]`
//     for `unit_index`, bounded against group 0's OWN `.count` (re-read every iteration -- CMP
//     EAX,dword ptr [0x00b63be0] at 0x00449395). If the scan runs off the end without a match, the
//     function returns immediately (0x0044939d JMP 0x004493f7) -- no roster touch, no call.
//   * On a match (0x004493b8-0x004493f1): `_G_LLM_STRAT_CTRL_GROUPS[0].count -= 1` (DEC, BEFORE the
//     shift), then shift-compact survivors down by one slot, same shape as the a2 block's own shift.
//   * No roster write and no notify call anywhere in this second block -- group 0 is a transient
//     selection scratch (see sim_state.h's ctrl_groups comment), not a real Ctrl+digit assignment.
//
// THIS SECOND SCAN IS A SEPARATE, INDEPENDENT COMPACTION with NO shared bookkeeping with the
// first: it runs unconditionally, its own `local_1c`/`local_18` counters are freshly zeroed
// (0x0044938b), and it always executes even when a2==0 (nothing to do above) or when the a2!=0
// block already ran and found its match -- the unit can be removed from its assigned group AND
// from the transient selection group 0 in the SAME call, and the two removals do not affect each
// other's index arithmetic (group 0's `unit_ids[]` is disjoint storage from group a2's when
// a2 != 0).
//
// Nothing else is written: no other roster field, no other group, no return value (void).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward call (the a2!=0 block's tail notify) -----------------------------------------
//
// `llm_strat_order_ctrlgrp_flash_member` is an ALREADY-TRANSLATED member of the SIM1C order-enqueue
// module (mh::sim::detail::order_ctrlgrp_flash_member, sim_order_enqueue.h/.cpp) -- but per the
// batch brief, this TU calls it as an ORIGINAL callee via `mh::call::llm_strat_order_ctrlgrp_
// flash_member` (raw address, mh/addr/mh_calls.gen.h), NOT the mh::sim C++ body directly, indirected
// through this `calls` struct for the same reason sim_unit_ctrlgroup_member.h's own table is
// indirected: a direct `mh::call::` inside a `detail::` body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest / the offline fixture. Production
// binds this to `mh::call::llm_strat_order_ctrlgrp_flash_member` (`live_unit_ctrlgroup_leave_
// calls()`); simtest binds it to a recording stub.
struct unit_ctrlgroup_leave_calls {
    void (*order_ctrlgrp_flash_member)(uint16_t side, uint16_t unit_id,
                                       int32_t group_index); // llm_strat_order_ctrlgrp_flash_member
};

const unit_ctrlgroup_leave_calls &live_unit_ctrlgroup_leave_calls();

namespace detail {

// llm_strat_unit_ctrlgroup_leave @0x0044928f. See the header derivation above.
//
// PARAMETER: EAX=unit_index (uint, the .asm header's own type) -- the only parameter, per the
// committed `void __watcall llm_strat_unit_ctrlgroup_leave(uint unit_index)` prototype.
void unit_ctrlgroup_leave(const sim_view &v, sim_store &own, const unit_ctrlgroup_leave_calls &c,
                          uint32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state() (and live_unit_ctrlgroup_leave_calls()). Matches the
// original's committed __watcall shape (mh_export.gen.h's sig_llm_strat_unit_ctrlgroup_leave
// typedef: `void(__cdecl *)(uint32_t unit_index)`).
void unit_ctrlgroup_leave(uint32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
