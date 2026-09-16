//
// sim/libtrans/sim_lt_diplomacy.h -- llm_diplomacy_init_multiplayer @0x00465ac0 and
// llm_diplomacy_init_skirmish @0x00465baf (LT lib_trans batch D, unit d2_diplomacy; see
// tmp/prep_lib_trans/context_D.md). Two once-per-session diplomacy bootstraps that walk every
// (player_a, player_b) pair across all 8 player slots and hand the resulting relation (1=ally,
// 2=enemy) to llm_diplomacy_set_relation.
//
// ---- llm_diplomacy_set_relation IS already translated -- CONDUCTOR CORRECTION wins over section 3
// (context_D.md, dated 2026-09-02): `sim/sim_diplomacy_set_relation.{h,cpp}` carries
// `detail::diplomacy_set_relation(v, own, c, a, b, rel)`. Both bodies below call that detail::
// function DIRECTLY (rule 3c: a cross-TU sibling's own `_calls` struct is threaded in via a
// defaulted parameter, never re-bound as a fresh `live_*_calls()` inside this TU, and never called
// as `mh::call::llm_diplomacy_set_relation` -- that VA is unmapped inside net_selftest.exe).
//
// llm_diplomacy_init_multiplayer additionally registers every enabled HUMAN slot as a chat target
// (llm_ui_chat_target_add @0x0049d5c1, still original -- classified `state`, uncensused
// presentation/net wall per context_D.md section 2) before its relation pass.
// llm_diplomacy_init_skirmish makes no outward call of its own at all.
//
// ---- NEITHER FUNCTION WRITES ANY GLOBAL DIRECTLY (context_D.md section 3) --------------------------
// Both bodies contain zero absolute-address stores; every observable effect is delegated to the two
// callees above. The migration ledger's empty writes_shared/writes_island for both is therefore
// correct, not a measurement gap.
//
// ---- STATUS-FLAG BITS: bit0/bit3 have no C++ or Ghidra-enum name (declared_needs) ------------------
// Both bodies test player_profile.status_flags (`v.profiles[i].status_flags` -- mh_structs.gen.h's
// field comment is the authority) as a byte: bit0 = slot enabled, bit2 = human-controlled, bit3 =
// AI-controlled. `mh::lockstep::turn_engine.h` already names bit1 (PLAYER_ALIVE=0x02) and bit2
// (PLAYER_HUMAN=0x04) but has nothing for bit0 or bit3, and no Ghidra enum (E_STRAT_PLAYER_STATUS)
// is committed either -- both diplomacy functions still carry `todo:enum` in Ghidra for exactly this
// gap. Per this batch's explicit instruction this is a `declared_needs[]` item, NOT a local
// `inline constexpr` fallback (translator-brief rule 17a) -- unlike three older sim/ TUs
// (sim_land_players_on_planet.cpp, sim_game_update_progress.cpp, sim_landing_spot.cpp) that each
// rolled their own file-local `STRAT_PLAYER_STATUS_SLOT_ENABLED` copy under that same rule's
// fallback clause. So the raw bit literals (0x01u / 0x04u / 0x08u) appear inline in the .cpp with a
// citing comment instead of a named constant, pending that enum/constant landing in Ghidra.
//
// ---- THE MULTIPLAYER ALLY RULE IS SURPRISING -- READ LITERALLY, NOT "FIXED" (uncertainties) --------
// 0x00465b52-0x00465b9e: relation=1 (ally) iff i==j OR (bit3(i) && bit3(j)); else relation=2. Bit3 is
// "AI-controlled", so EVERY AI-controlled slot allies with every other AI-controlled slot regardless
// of team, and every human slot is hostile to everyone but itself. That is what the disassembly
// does; it is transcribed here and flagged in uncertainties[], not corrected.
//
// ---- THE SKIRMISH ALLY RULE'S ASM SHAPE IS A GOTO CASCADE, REASSEMBLED HERE (uncertainties) --------
// 0x00465c17-0x00465c5d: relation=1 iff i==j OR (PlayerSide != i AND PlayerSide != j); else
// relation=2. The actual disassembly is a 4-way goto cascade (i==j -> ally directly; else
// PlayerSide==i -> enemy; else PlayerSide==j -> enemy; else -> ally) -- traced arm-by-arm in the
// .cpp against the if/else-if below, which is logically identical for every (i, j) but is a
// reassembled shape, not a transcription (same disposition context_D.md records for this range).
// PlayerSide (`*v.player_side`, `const int16_t *`) is read via MOVZX at both comparison sites: a
// 16-bit UNSIGNED value widened to int32 BEFORE the compare against the loop counter, so the
// widening goes through uint16_t here, never int16_t.
//
// ---- BOTH GATE THE WHOLE PAIR ON BOTH SLOTS ENABLED BEFORE SETTING EITHER RELATION -----------------
// (multiplayer 0x00465b30-0x00465b50, skirmish 0x00465bf5-0x00465c15). An inactive slot's relation
// entries are left STALE, not cleared -- preserved here (translator-brief rule 11).
//
// ---- the relation argument ---------------------------------------------------------------------
// EBX carries a full dword (1 or 2) at every call site, but llm_diplomacy_set_relation's committed
// signature types it uint8_t (matches Players[a].relation[b] being a byte) -- both call sites here
// pass a uint8_t literal, matching sim_diplomacy_set_relation.cpp's own raw-literal-relation style
// (no named ALLY/ENEMY constant exists there either, so none is invented here).
//
// (CONDUCTOR SITE DECISION, 2026-09-02) -- installed normally below, no manifest declared-need here.
//
// `utils_assert_stack_capacity(0x24)` @0x00465ac8 / @0x00465bb7 is inert and omitted
// (translator-brief rule 6).
//
// ---- Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_diplomacy_init_multiplayer_00465ac0.asm,
// tmp/decomp_lib_trans/llm_diplomacy_init_skirmish_00465baf.asm); the exported `.c` drafts beside
// them are NOT the spec -- see the skirmish goto-cascade note above for the one place they diverge
// in shape (not in outcome).
//
#pragma once
#include <cstdint>

#include "sim/sim_diplomacy_set_relation.h" // sibling with its own _calls struct (rule 3c): detail::diplomacy_set_relation
#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this TU owns directly (llm_ui_chat_target_add, multiplayer only).
// llm_diplomacy_set_relation is reached as an in-tree sibling instead (threaded
// diplomacy_set_relation_calls, rule 3c) -- it is deliberately NOT a member here.
struct lt_diplomacy_calls {
    void (*chat_target_add)(uint8_t player_id); // llm_ui_chat_target_add @0x0049d5c1
};

const lt_diplomacy_calls &live_lt_diplomacy_calls();

namespace detail {

// llm_diplomacy_init_multiplayer @0x00465ac0.
void diplomacy_init_multiplayer(
    const sim_view &v, sim_store &own, const lt_diplomacy_calls &c,
    const diplomacy_set_relation_calls &c_set_relation = live_diplomacy_set_relation_calls());

// llm_diplomacy_init_skirmish @0x00465baf. Makes no outward call of its own -- only the
// llm_diplomacy_set_relation sibling, threaded in the same way.
void diplomacy_init_skirmish(
    const sim_view &v, sim_store &own,
    const diplomacy_set_relation_calls &c_set_relation = live_diplomacy_set_relation_calls());

} // namespace detail

// Live wrappers: the logic applied to state() and live_lt_diplomacy_calls() /
// live_diplomacy_set_relation_calls(). Both match the originals' __watcall(void) shape.
void diplomacy_init_multiplayer();
void diplomacy_init_skirmish();

namespace detail {
} // namespace detail

} // namespace mh::sim
