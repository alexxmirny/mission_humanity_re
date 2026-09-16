//
// sim/libtrans/sim_lt_rng_draws.h -- llm_rand_below @0x00499f49 and llm_rand_below_fx @0x00499f84
// (LT1A batch A, unit 1 of 3 -- see tmp/prep_lib_trans/context_A.md). Two BYTE-IDENTICAL 0x3b-byte
// __watcall(EAX=n) -> EAX leaf wrappers over the strategic-sim PRNG step, differing in exactly one
// immediate: the channel argument they hand to the shared callee.
//
// ---- Both delegate to the SAME already-translated callee, llm_strat_rng_next @0x004b4d01 ----
// (mh::sim::detail::rng_next, sim/sim_rng_next.h -- disasm-verified, shadow-armed, SIM1F). Do NOT
// re-derive the ROR16(state+0x9248,3) recurrence here: one state (_G_LLM_STRAT_RNG_STATE via
// own.rng_state_at), one implementation (rng_next already owns it). This unit is a pure binding.
//
// ---- __cdecl, args pushed right-to-left (confirmed by the ADD ESP,0xc caller cleanup) ----
// llm_rand_below   @0x00499f49: PUSH EAX(=upper_bound) @0x00499f67, PUSH 0 @0x00499f68,
//                                PUSH 0 @0x00499f6a, CALL 0x004b4d01, ADD ESP,0xc @0x00499f71.
//   Right-to-left push order means the LAST push (closest to the return address, 0x00499f6a) is the
//   FIRST argument of `int __cdecl llm_strat_rng_next(int channel, int lo, int hi)`: channel=0. The
//   middle push (0x00499f68) is lo=0. The first push (0x00499f67, EAX) is hi=upper_bound.
//   => rng_next(own, /*channel=*/0, /*lo=*/0, /*hi=*/upper_bound).
// llm_rand_below_fx @0x00499f84: identical frame, PUSH EAX(=upper_bound) @0x00499fa2, PUSH 0
//                                 @0x00499fa3, PUSH 1 @0x00499fa5, CALL 0x004b4d01,
//                                 ADD ESP,0xc @0x00499fac.
//   Same push-order reasoning: LAST push (0x00499fa5, value 1) is channel=1, middle (0x00499fa3) is
//   lo=0, first (0x00499fa2, EAX) is hi=upper_bound.
//   => rng_next(own, /*channel=*/1, /*lo=*/0, /*hi=*/upper_bound).
// Both readings match tmp/decomp_lib_trans/*.c's own `llm_strat_rng_next(ch, 0, param)` call
// (verified against the push order directly, not read off the draft's argument order).
//
// ---- Signatures are the COMMITTED ones (addr/mh_calls.gen.h), not the Ghidra header's ----
// llm_rand_below:    addr/mh_calls.gen.h commits `int32_t(int32_t)`, EAX storage, __watcall.
// llm_rand_below_fx: `int32_t(uint32_t)` since the 2026-09-02 domain-close re-dump -- the
//   Ghidra-side `undefined4 (undefined4)` gap the LT1A batch report flagged was fixed in the DB
//   after the batch landed, and the regenerated mh_calls.gen.h now carries the int32 return this
//   file originally had to spell uint32_t to match. Same bits either way; the spelling tracks
//   mh_calls.gen.h exactly, as before.
//
// ---- What is deliberately OMITTED ----
// - The leading CALL utils_assert_stack_capacity(0x30) (0x00499f51 / 0x00499f8c): inert stack probe,
//   touches zero tracked regions (translator-brief rule 6). Not in callees[].
// - The stack spills at [EBP-0x1c]/[EBP-0x18] (param and return-value round-trips through the stack
//   frame): Watcom frame noise with no semantics -- already the resolution the .c draft's PLATE
//   documents (the CONCAT44 8-byte-return artifact is not a real second half).
//
// ---- Channels are separate streams -- draw counts are semantic (translator-brief rule 13) ----
// Channel 0 (llm_rand_below) is the hashed gameplay/lockstep stream; channel 1 (llm_rand_below_fx) is
// the FX stream, deliberately MASKED by the determinism harness (frame-rate-dependent draw count) --
// an accidental extra/missing/reordered ch1 draw will NOT show up in the determinism gate, only in
// `net_selftest libtranstest`. Each wrapper below advances its channel exactly once per call, via
// exactly one rng_next() call, and touches no other state.
//
// (the shadow manifest, both with extra_regions: ["_G_LLM_STRAT_RNG_STATE"] --
// added post-prep specifically for this batch; addr/mh_shadow.gen.h already carries both macros and
// both mh_shadow_install_* functions). No declared-need here, unlike most of this closure's other
// units -- install normally.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_rand_below @0x00499f49. See the header banner for the push-order derivation.
int32_t rand_below(sim_store &own, int32_t upper_bound);

// llm_rand_below_fx @0x00499f84. Same shape, channel=1. int32_t(uint32_t) to match the committed
// prototype (see the banner's re-dump note) -- the param stays unsigned unlike its sibling.
int32_t rand_below_fx(sim_store &own, uint32_t upper_bound);

} // namespace detail

// Live wrappers: the logic applied to state().own. Match each original's committed __watcall
// signature exactly.
int32_t rand_below(int32_t upper_bound);
int32_t rand_below_fx(uint32_t upper_bound);

namespace detail {
} // namespace detail

} // namespace mh::sim
