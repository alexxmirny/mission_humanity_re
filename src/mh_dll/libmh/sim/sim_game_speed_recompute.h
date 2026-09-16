//
// sim/sim_game_speed_recompute.h -- llm_game_speed_recompute: rebuilds the GLOBAL game_speed scalar
// from scratch each call as the product of every ALIVE-and-HUMAN player's own per-player speed
// factor, clamped to 1.0 whenever no such player exists or the product comes out non-positive
// (RI-SIM / SIM1F).
//
//   llm_game_speed_recompute @0x00497623 (0xaa B), `void __watcall llm_game_speed_recompute(void)`
//   (committed prototype -- no parameters, no return value; addr/mh_calls.gen.h already carries
//   `mh::call::llm_game_speed_recompute` as an ordinary void() thunk).
//
// Translated from the DISASSEMBLY (tmp/decomp/llm_game_speed_recompute_00497623.asm). The Ghidra .c
// draft (tmp/decomp/llm_game_speed_recompute_00497623.c) agrees with the assembly's control flow and
// its plate is accurate -- used as a map, but every line below is re-derived from the raw instruction
// sequence per the translator brief.
//
// ---- THE INIT (0x0049763b-0x00497645) ------------------------------------------------------------
// Two dword stores to game_speed's low/high halves: `[0xe587d9]=0x00000000`, `[0xe587dd]=0x3ff00000`
// -- the Watcom all-immediate idiom for storing a `double` constant, and the bit pattern
// 0x3FF0000000000000 is exactly 1.0. So `game_speed = 1.0;` unconditionally, before the loop even
// starts (matches the .c draft).
//
// ---- THE LOOP (LAB_00497656..LAB_004976a0) ---------------------------------------------------------
// A classic Watcom "test-at-top, increment-at-a-detached-label" shape: LAB_00497656 tests
// `local_1c < 8` (JL into the body at 0x497666; not-taken falls through to the exit at 0x4976a2).
// The body computes `EAX = local_1c * 0x740` (0x740 is llm_strat_player_profile's own stride --
// sim_view::profiles / RID_STRAT_PLAYERS) TWICE, once per bit test:
//   `TEST byte[EAX+0xcff060],0x2; JZ <skip>`   -- 0xcff060 IS _G_LLM_STRAT_PLAYERS' base address
//     (addr::_G_LLM_STRAT_PLAYERS, mh_addrs.gen.h), offset 0 = status_flags, so this is byte 0 of
//     `profiles[i].status_flags`, bit 0x2 -- ALIVE (E_STRAT_PLAYER_STATUS bit1 per
//     mh_structs.gen.h's own status_flags field comment, which *names this exact call site* --
//     "b2 feeds the game_speed product (llm_game_speed_recompute 0x49766d)" -- confirming the
//     second test below is bit2/0x4, HUMAN, not a guess).
//   `TEST byte[EAX+0xcff060],0x4; JNZ <do-multiply>` -- same byte, bit 0x4 -- HUMAN-CONTROLLED
//     (E_STRAT_PLAYER_STATUS bit2, same field-comment citation). Falling through here (bit unset)
//     joins the same "skip" path as the ALIVE test failing (LAB_00497686 -> LAB_004976a0, the
//     increment), so BOTH bits must be set for the multiply to run -- an AND, not an OR, matching
//     the .c draft's `&&`.
//   Do-multiply (LAB_00497688): `EAX=local_1c; SHL EAX,3; FLD [EAX+0x5d554c]` -- 0x5d554c is
//     addr::_G_LLM_GAME_SPEED_PLAYER_FACTOR's base (sim_view::game_speed_player_factor), indexed by
//     `local_1c*8` (sizeof(double)) -- i.e. `game_speed_player_factor[i]`. `FMUL [0xe587d9]; FSTP
//     [0xe587d9]` -- `game_speed = game_speed_player_factor[i] * game_speed;` (order matches the
//     FLD-then-FMUL-by-memory sequence exactly: ST(0)=factor, `FMUL m64` multiplies ST(0) by
//     game_speed, so the product is commutative and value-identical either operand order -- written
//     as `factor * game_speed` below to mirror the FPU register/memory positions literally).
// Increment: LAB_0049765e reads `EAX=local_1c` (unused -- a dead load, no consumer reads EAX before
// the next write) then `INC local_1c; JMP LAB_00497656` (back to the top-of-loop test). Reproduced as
// an ordinary `for (i = 0; i < MAX_PLAYERS; ++i)`.
//
// ---- THE FLOOR CHECK (LAB_004976a2-0x004976c3), the function's only FCOMP ---------------------------
// `FLDZ; FCOMP [game_speed]; FNSTSW AX; SAHF; JC <skip reset>`. FCOMP compares ST(0) (0.0, just
// loaded) against the memory operand (game_speed) and sets CF=1 iff ST(0) < Source, i.e. iff
// `0.0 < game_speed`; JC (CF=1) skips the reset. So the reset (`game_speed = 1.0`) fires exactly
// when CF=0, i.e. when the compare was ORDERED and `0.0 >= game_speed` -- which is precisely what
// C++'s ordinary `game_speed <= 0.0` evaluates to (IEEE `<=` is false whenever either operand is
// NaN, matching the x87 unordered case setting CF=1 -> "not reset", the SAME outcome). So
// `if (game_speed <= 0.0) game_speed = 1.0;`, exactly as the .c draft has it, reproduces the
// hardware branch INCLUDING its NaN corner case with an ordinary C++ comparison -- verified
// algebraically below in the .cpp's comment rather than asserted; see uncertainties[] for why this
// is flagged rather than silently assumed (rule 9/19: every FP comparison is worth a citation, and
// this codebase has a documented sibling gotcha -- ai_selftest.cpp's is_alive test -- where the
// naive translation of a CF-based x87 branch does NOT match a plain C++ operator).
//
// ---- NO OUTWARD CALLS AT ALL ------------------------------------------------------------------------
// The only CALL in the whole body is the opening `CALL utils_assert_stack_capacity` (rule 6, inert,
// omitted). No `_calls` struct is needed for this TU -- unlike sim_prod_shuttle_complete.cpp /
// sim_game_speed_adjust.cpp, which both call back out to originals, this function only reads/writes
// state.
//
// ---- FIELDS / GLOBALS: ALREADY NAMED / BOUND, NOT BYTE OFFSETS ---------------------------------------
// sim_view::profiles (llm_strat_player_profile[8] @ RID_STRAT_PLAYERS, `.status_flags`),
// sim_view::game_speed_player_factor (double[8] @ RID... already bound this same slice per
// sim_game_speed_adjust.h's precedent), sim_store::game_speed() (double& @ RID_GAME_SPEED, this
// function's sole writer per that accessor's own sim_state.h comment). MAX_PLAYERS=8 (sim_state.h).
// None of these needed a fresh declared_needs entry -- the conductor pre-bound them this same slice.
//
// ---- THE ALIVE/HUMAN BIT CONSTANTS (rule 17a) --------------------------------------------------------
// No committed Ghidra ENUM exists for E_STRAT_PLAYER_STATUS (mh_structs.gen.h's status_flags field is
// plain `uint`, with `[E_STRAT_PLAYER_STATUS]` only as a comment TAG, not an applied enum datatype --
// checked tools/data/dll_struct_layouts.json, `"type": "uint"`). This codebase's established response
// to that exact situation (sim_step.cpp's own comment on STRAT_PLAYER_STATUS_ALIVE) is a local,
// file-scoped, CITED copy of the bit value rather than a fresh shared header -- there are already four
// independent copies of the ALIVE bit alone (ai_state.h's PLAYER_STATUS_ALIVE, sim_step.cpp's
// STRAT_PLAYER_STATUS_ALIVE, sim_combat_credit_planet_conquest_kills.h's own copy, seams/harness.cpp's
// PS_ALIVE) and one of the HUMAN bit (seams/harness.cpp's PS_HUMAN, same field, same value). This TU
// adds its own copies of both, named to match sim_step.cpp's `STRAT_PLAYER_STATUS_*` convention (the
// nearest sibling in libmh/sim/ testing the SAME struct's SAME field).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_game_speed_recompute @0x00497623. No parameters, no return, no outward calls -- see the header
// banner above for the full per-line derivation.
void game_speed_recompute(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed void(void) signature.
void game_speed_recompute();

namespace detail {
} // namespace detail

} // namespace mh::sim
