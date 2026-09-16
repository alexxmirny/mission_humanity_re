//
// lockstep/lt_chat_ally_mask.h -- llm_net_chat_ally_mask_rebuild @0x0049d750 (batch D / d7,
// RI-LOCKSTEP lib_trans). Rebuilds the per-player ally-chat recipient bitmask
// _G_LLM_CHAT_TARGET_MASK from the local player's diplomacy relations, but only while the game is in
// ally-chat mode.
//
// Zero outward calls -- the only CALL in the body is the inert `PUSH 0x20 / CALL
// utils_assert_stack_capacity` stack-touch prologue every function in this image opens with (settled:
// it touches zero tracked state regions, so omitting it can never produce a divergence; not
// reproduced here, same as everywhere else in this tree). So this unit carries NO `_calls` struct,
// matching sim/resid/sim_clock_resync.{h,cpp}.
//
// A SEPARATE `chat_ally_mask_state`, NOT a new engine_state member -- lockstep/resync.h is the
// precedent: every lockstep residue TU since C8-c defines its own state struct sized to exactly what
// it reads/writes, rather than growing turn_engine.h's `engine_state` without bound. (The batch
// context proposed adding fields to `engine_state`; this header follows the house shape that already
// shipped instead.)
//
// ---- THE TOP-LEVEL GATE, AND WHY IT NOW HAS A NAME IN mh_addrs.gen.h ------------------------------
// `CMP byte ptr [0x00e5898e],0x1 / JNZ 0x0049d7f5` at 0x0049d768-0x0049d76f: if
// _G_LLM_CHAT_TARGET_MODE != 1 the function returns immediately, having written nothing. At LT-PREP
// time this global had no `mh_addrs.gen.h` constant and no region row -- it existed only inside a
// comment at mh_effects.gen.h:555. LT1D (2026-09-02) registered it
// (`_G_LLM_CHAT_TARGET_MODE`, RID_CHAT_TARGET_MODE, MF_VIEW); this header just consumes that. Its
// sibling `_G_LLM_CHAT_TARGET_MASK` (0x00e5898b) IS a distinct byte three bytes earlier, already
// carrying RID_CHAT_TARGET_MASK (MF_VIEW|MF_MEASURED) -- do not conflate the two.
//
// ---- TWO DIFFERENT PLAYER TABLES IN ONE 8-ITERATION LOOP --------------------------------------------
// The status_flags gate (`TEST byte[...],0x2` @0x0049d796 / `TEST byte[...],0x4` @0x0049d7a6) reads
// _G_LLM_STRAT_PLAYERS (mh::game::mh_llm_strat_player_profile[8], stride 0x740, base 0x00cff060) --
// the same profile table turn_engine.h's PLAYER_ALIVE/PLAYER_HUMAN test in every barrier loop. The
// relation read (`CMP byte ptr [PlayerSide*0x34 + i + 0xe587f1],0x1` @0x0049d7cc) is a DIFFERENT
// table: `Players[8]` (mh::addr::Players, base 0x00e587e9, mh::game::mh_llm_strat_player_desc, stride
// 0x34); its `relation[8]` field sits at +0x8 (0xe587f1 - 0xe587e9 = 0x8), one byte per opposing
// player, and mh_structs.gen.h already types + static_asserts that offset. Bound here as
// `player_desc`, READ-ONLY -- this function never writes it. Conflating the two tables is the one
// thing this comment exists to prevent.
//
// ---- SELF IS SKIPPED --------------------------------------------------------------------------------
// `PlayerSide != i` (0x0049d7b1-0x0049d7bb). PlayerSide itself is read with a 16-bit MOVZX
// (`word ptr [0x00e58354]`), the same widening turn_engine.h's `engine_state::player_side` documents,
// and is used both as the "exclude self" index and as the row selector into `player_desc`.
//
// ---- THE MASK OP IS BYTE-WIDE, NOT THE 32-BIT INT THE DRAFT `.c` RENDERS ---------------------------
// `MOV CL,[i] / MOV AL,1 / SHL AL,CL` then `OR byte[MASK],AL` (ally) or `NOT AL / AND byte[MASK],AL`
// (not ally) -- an 8-bit shift and an 8-bit OR/AND. The exported `.c`'s `'\x01' << (i & 0x1f)` is x86's
// own SHL-count masking surfacing as source, not a real mask in the assembly; `i` never exceeds 7 in
// this loop so the numeric value agrees either way, but the operand width is a `uint8_t`, not an
// `int`, and this header's implementation reproduces that width.
//
// ---- A SLOT THAT FAILS THE GATE LEAVES ITS BIT STALE, NOT CLEARED ----------------------------------
// A player that is not ALIVE&&HUMAN, or is self, is simply skipped for that iteration -- the mask bit
// it owns keeps whatever value it already held. Not a bug to "fix" by clearing it on the way past.
//
// ---- POPULATION ---------------------------------------------------------------------------------
// MP-only: this function's sole caller is llm_diplomacy_set_relation, so an SP run calls it zero
// times. It is also gated on _G_LLM_CHAT_TARGET_MODE == 1 (ally-chat mode); an MP run in the wrong
// chat mode logs identically to a clean one, so set the mode before treating a rig run as having
// entered this row.
//
// Translated from tmp/decomp_lib_trans/llm_net_chat_ally_mask_rebuild_0049d750.asm; the `.c` beside it
// is a draft (see the mask-width note above for where it diverges).
//
// ==== SIMABI-CHAT (2026-09-10): TWO MORE BODIES OVER THE SAME TWO BYTES =============================
//
// `llm_ui_chat_recalc_target_mode` @0x0049d631 (287 B) and `llm_ui_chat_target_add` @0x0049d5c1 (55 B)
// were the last two `host-callback:chat` entries of the sim host table. They are INTERNALIZED here --
// not converted onto a notify channel -- because they are bookkeeping over player state, touch neither
// render nor the OS, and their writes are read back by libmh IN THE SAME FRAME (the R3b hazard the two
// rows carried). Owning both bodies dissolves that hazard by construction: writer and readers are one
// synchronous call graph again.
//
// They live in THIS file rather than a new TU because they read and write exactly the state the mask
// rebuild above does -- MODE, MASK, the profile table and PlayerSide -- and every trap this banner
// already documents (the two player tables, the MOVZX PlayerSide, the byte-wide mask op, self skipped)
// applies to them unchanged. A second TU would have had to restate all four.
//
// ---- R5 IS DUAL-WRITER HERE, NOT SOLE OWNERSHIP (xref-verified 2026-09-10) ------------------------
// Internalizing does NOT patch the originals: their bytes stay at 0x0049d631/0x0049d5c1 and
// UNTRANSLATED UI code keeps calling them into the SAME region-registered cells --
// `llm_ui_chat_target_remove` @0x0049d5f8 (which tail-calls the original recalc) and
// `llm_ui_diplomacy_apply_and_resume` @0x004c7fc6, which calls both add and remove from the in-game
// DIPLOMACY screen. That is safe ONLY because both ops are idempotent over their cells (a bitmask OR;
// a pure recompute of MODE from MASK + the profile table) AND our bodies are exact -- a divergence
// would not merely be wrong, it would be wrong INTERMITTENTLY, depending on which writer ran last.
// Hence the offline arms below, and hence the exactness notes at every branch in the .cpp.
//
// ---- THE MODE DOMAIN, AND THE VALUE THIS FUNCTION NEVER WRITES ------------------------------------
// recalc writes exactly three values (0x0049d72d / 0x0049d736 / 0x0049d73f): 0, 2, 3. The gate at the
// top of the mask rebuild above tests for 1, and NOTHING in the image writes 1 through a disp32 store
// -- `_G_LLM_CHAT_TARGET_MODE` has six xrefs total and the only three writes are recalc's own
// (re-verified 2026-09-10). Do not "fix" that by making some branch here produce 1: whatever sets
// ally-chat mode does it through a path this xref set cannot see (a block load), and inventing a
// fourth value would silently arm the rebuild.
//
// ---- ONE DEAD LOCAL IS DELIBERATELY NOT REPRODUCED -------------------------------------------------
// The original keeps a FOURTH flag at [EBP-0x1c]: initialised 1, cleared 0 at 0x0049d6e7 and
// 0x0049d70d off `Players[PlayerSide].relation[i] == 1`. It is never read -- three writes, zero reads
// in the whole body -- so the two relation loads are dead computation with no observable effect, and
// Ghidra's decompiler eliminates them too. They are NOT reproduced here, which is why
// `chat_target_state` binds no `player_desc` even though the disassembly reads that table. This
// paragraph exists so the next reader diffing the .asm against the .cpp finds the answer instead of
// filing a missing-read bug.
//
// Translated from tmp/decomp_chat/llm_ui_chat_recalc_target_mode_0049d631.asm and
// tmp/decomp_chat/llm_ui_chat_target_add_0049d5c1.asm. Offline oracle: `net_selftest libtranstest`
// cases t18 (recalc: every branch, incl. the empty selection and the `selected > 1` boundary) and
// t19 (add: incl. the already-set case and the 8-bit shift width) in mh_nettest/lib_trans_selftest.cpp.
//
#pragma once
#include <cstdint>

#include "addr/mh_structs.gen.h"

namespace mh::lockstep {

// Everything llm_net_chat_ally_mask_rebuild reads or writes, as typed pointers. Bound to the live game
// by live_chat_ally_mask_state(); a test may bind plain locals instead.
struct chat_ally_mask_state {
    const uint8_t *mode; // _G_LLM_CHAT_TARGET_MODE (0x00e5898e) -- gate; rebuild only while == 1
    uint8_t       *mask; // _G_LLM_CHAT_TARGET_MASK  (0x00e5898b) -- rebuilt bit by bit; WRITTEN
    const mh::game::mh_llm_strat_player_profile
        *players;                // _G_LLM_STRAT_PLAYERS[8] (0x00cff060, stride 0x740) -- status_flags gate only,
                                 // READ-ONLY here
    const uint16_t *player_side; // PlayerSide (0x00e58354) -- MOVZX 16-bit unsigned; both the
                                 // "exclude self" index and the row selector into player_desc
    const mh::game::mh_llm_strat_player_desc
        *player_desc; // Players[8] (mh::addr::Players, 0x00e587e9, stride 0x34) -- relation[8] at
                      // +0x8 is the only field this function reads; READ-ONLY, never written here
};

chat_ally_mask_state live_chat_ally_mask_state();

// SIMABI-CHAT. Everything llm_ui_chat_recalc_target_mode + llm_ui_chat_target_add read or write. A
// SEPARATE struct from chat_ally_mask_state above, and deliberately so: these two WRITE `mode`, which
// the rebuild only reads, and they never touch `player_desc` (see the dead-local note in the banner).
// Widening the rebuild's struct instead would have thrown away exactly that documentation.
struct chat_target_state {
    uint8_t *mode; // _G_LLM_CHAT_TARGET_MODE (0x00e5898e) -- WRITTEN by recalc; 0 / 2 / 3 only
    uint8_t *mask; // _G_LLM_CHAT_TARGET_MASK  (0x00e5898b) -- WRITTEN by target_add, read by recalc
    const mh::game::mh_llm_strat_player_profile
        *players;                // _G_LLM_STRAT_PLAYERS[8] (0x00cff060, stride 0x740) -- status_flags
                                 // gate only, READ-ONLY
    const uint16_t *player_side; // PlayerSide (0x00e58354) -- MOVZX 16-bit unsigned; "exclude self"
};

chat_target_state live_chat_target_state();

namespace detail {

// llm_net_chat_ally_mask_rebuild @0x0049d750. Pure over `st` -- no outward calls at all.
void chat_ally_mask_rebuild(const chat_ally_mask_state &st);

// llm_ui_chat_recalc_target_mode @0x0049d631. Pure over `st`; writes only `*st.mode`.
void chat_recalc_target_mode(const chat_target_state &st);

// llm_ui_chat_target_add @0x0049d5c1. Sets one bit in `*st.mask`, then recalcs the mode -- the
// original's own tail call, reproduced as an in-tree sibling call (not through the entry VA).
void chat_target_add(const chat_target_state &st, uint8_t player_id);

} // namespace detail

// ---- production entry points (bound to the live game state) ----------------------------------------
void chat_ally_mask_rebuild();
void chat_recalc_target_mode();
void chat_target_add(uint8_t player_id);

namespace detail {

// ---- differential-oracle site -----------------------------------------------------------------------

} // namespace detail

} // namespace mh::lockstep
