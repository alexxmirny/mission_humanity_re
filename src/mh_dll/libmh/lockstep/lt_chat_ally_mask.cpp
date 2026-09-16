//
// lockstep/lt_chat_ally_mask.cpp -- see lt_chat_ally_mask.h. Translated from
// tmp/decomp_lib_trans/llm_net_chat_ally_mask_rebuild_0049d750.asm (the exported `.c` beside it is a
// draft; the header records where it renders x86 SHL-count masking as source).
//
#include "lockstep/lt_chat_ally_mask.h"

#include "addr/mh_addrs.gen.h"
#include "lockstep/turn_engine.h" // PLAYER_ALIVE / PLAYER_HUMAN / MAX_PLAYERS -- one definition

namespace mh::lockstep {

namespace {

// Players[].relation[] value meaning "allied" (`CMP byte[...],0x1` @0x0049d7cc). No Ghidra enum
// exists for this small relation domain (0/1/2, per sim_diplomacy_set_relation.cpp's sign mapping:
// 1 = friendly/allied, 2 = war) -- see this TU's declared_needs. Named locally rather than left a bare
// `1` in the compare below, matching this tree's convention for a single well-understood value with
// no enum to draw from yet (e.g. sim/resid/sim_clock_resync.cpp's local STATUS_ALIVE).
inline constexpr uint8_t RELATION_ALLIED = 1;

// SIMABI-CHAT. The three values llm_ui_chat_recalc_target_mode stores into
// _G_LLM_CHAT_TARGET_MODE, named for the CONDITION each branch tested rather than for any downstream
// meaning (llm_strat_input_update switches on 0/1/2/3; 1 is the ally-chat value nothing here writes --
// see the header banner). MOV byte[0x00e5898e],imm8 at 0x0049d72d / 0x0049d736 / 0x0049d73f.
inline constexpr uint8_t MODE_ALL_ELIGIBLE_SELECTED = 0; // 0x0049d72d
inline constexpr uint8_t MODE_PARTIAL_SELECTION     = 2; // 0x0049d736
inline constexpr uint8_t MODE_NO_TARGET_SELECTED    = 3; // 0x0049d73f

} // namespace

namespace detail {

// ---- llm_net_chat_ally_mask_rebuild @0x0049d750 ----------------------------------------------------
void chat_ally_mask_rebuild(const chat_ally_mask_state &st) {
    // 0x0049d768-0x0049d76f: top-level gate. Off in every mode but ally-chat -- writes nothing.
    if (*st.mode != 1) return;

    // 0x0049d7b1/0x0049d7bf: MOVZX word -- 16-bit unsigned, widened before use as both a compare
    // operand and an array index (see the header note on PlayerSide).
    const int32_t local_side = static_cast<int32_t>(*st.player_side);

    // 0x0049d77c-0x0049d7f3: for each of the 8 player slots.
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        // 0x0049d78f-0x0049d7ad: ALIVE && HUMAN, tested on _G_LLM_STRAT_PLAYERS -- the PROFILE table.
        const bool eligible = (st.players[i].status_flags & PLAYER_ALIVE) != 0 &&
                              (st.players[i].status_flags & PLAYER_HUMAN) != 0;
        // 0x0049d7b1-0x0049d7bb: and not the local player itself.
        if (!eligible || local_side == i) continue; // stale bit left UNTOUCHED -- see header

        // 0x0049d7cc: Players[local_side].relation[i] -- the DESC table, not the profile table above.
        const bool allied = st.player_desc[local_side].relation[i] == RELATION_ALLIED;

        // 0x0049d7d5-0x0049d7ed: byte-wide `MOV CL,i / MOV AL,1 / SHL AL,CL`, then OR (ally) or
        // NOT+AND (not ally) into the byte mask. `i` is always in [0,8), so the x86 SHL's implicit
        // 5-bit count mask never actually engages -- this is a plain shift, not a masked one.
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if (allied) {
            *st.mask = static_cast<uint8_t>(*st.mask | bit);
        } else {
            *st.mask = static_cast<uint8_t>(*st.mask & static_cast<uint8_t>(~bit));
        }
    }
}

// ---- llm_ui_chat_recalc_target_mode @0x0049d631 (SIMABI-CHAT) --------------------------------------
//
// A pure recompute of MODE from MASK + the profile table + PlayerSide: idempotent, and it is that
// idempotence the dual-writer story rests on (header banner).
void chat_recalc_target_mode(const chat_target_state &st) {
    // 0x0049d649-0x0049d665: four locals, in the original's own order. [EBP-0x1c] -- the fourth,
    // written three times and never read -- is deliberately absent; see the header's dead-local note.
    bool    all_selected = true;  // [EBP-0x20], init 1
    bool    any_selected = false; // [EBP-0x18], init 0
    int32_t selected     = 0;     // [EBP-0x24], init 0

    // 0x0049d6a1: MOVZX word -- 16-bit unsigned widened before the compare, exactly as above.
    const int32_t local_side = static_cast<int32_t>(*st.player_side);

    // 0x0049d66c-0x0049d714: `CMP [i],0x8 / JL` -- the same 8-slot loop as the rebuild.
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        // 0x0049d67f-0x0049d69d: ALIVE (bit1) && HUMAN (bit2) on the PROFILE table, then
        // 0x0049d6a1-0x0049d6ab: and not the local player. A slot that fails any of the three is
        // skipped ENTIRELY -- it neither counts toward `selected` nor clears `all_selected`, so a
        // lobby of one human sees MODE_NO_TARGET_SELECTED rather than "all of nothing".
        const bool eligible = (st.players[i].status_flags & PLAYER_ALIVE) != 0 &&
                              (st.players[i].status_flags & PLAYER_HUMAN) != 0;
        if (!eligible || local_side == i) continue;

        // 0x0049d6af-0x0049d6c2: MOVZX the byte mask into a 32-bit register, `MOV AL,1 / SHL EAX,CL`,
        // TEST. A 32-BIT shift here (unlike target_add's 8-bit one below) -- but `i` is in [0,8) so
        // the two agree numerically; the width is reproduced anyway.
        if ((static_cast<uint32_t>(*st.mask) & (1u << i)) != 0) {
            any_selected = true; // 0x0049d6c4
            ++selected;          // 0x0049d6ce
        } else {
            all_selected = false; // 0x0049d6f0
        }
    }

    // 0x0049d719-0x0049d73f. Note the ORDER of the two tests and that `selected > 1` is a SIGNED
    // compare (`CMP [EBP-0x24],1 / JG`): exactly one selected target is PARTIAL, not ALL.
    if (!any_selected) {
        *st.mode = MODE_NO_TARGET_SELECTED;
    } else if (all_selected && selected > 1) {
        *st.mode = MODE_ALL_ELIGIBLE_SELECTED;
    } else {
        *st.mode = MODE_PARTIAL_SELECTION;
    }
}

// ---- llm_ui_chat_target_add @0x0049d5c1 (SIMABI-CHAT) ----------------------------------------------
//
// 55 bytes: one byte-wide OR and the recalc tail call. Idempotent -- re-adding a slot already in the
// mask leaves both cells exactly as they were.
void chat_target_add(const chat_target_state &st, uint8_t player_id) {
    // 0x0049d5dc-0x0049d5e3: `MOV CL,player_id / MOV AL,1 / SHL AL,CL / OR byte[MASK],AL`. The shift
    // is EIGHT BITS WIDE with x86's implicit 5-bit count mask, so a player_id whose low 5 bits are
    // >= 8 shifts every bit out of AL and the OR is a no-op -- reproduced exactly by truncating a
    // 32-bit `1u << (player_id & 0x1f)` to uint8_t. Callers only ever pass 0..7; this is what the
    // ORIGINAL does off that path, not a guess about what it should do.
    const uint8_t bit = static_cast<uint8_t>(1u << (player_id & 0x1fu));
    *st.mask          = static_cast<uint8_t>(*st.mask | bit);

    // 0x0049d5e9: CALL llm_ui_chat_recalc_target_mode. An in-tree sibling call, never mh::call:: --
    // the same rule sim_lt_diplomacy.cpp's set_relation edge follows.
    chat_recalc_target_mode(st);
}

} // namespace detail

// ---- production entry points -----------------------------------------------------------------------
void chat_ally_mask_rebuild() {
    detail::chat_ally_mask_rebuild(live_chat_ally_mask_state());
}

void chat_recalc_target_mode() {
    detail::chat_recalc_target_mode(live_chat_target_state());
}

void chat_target_add(uint8_t player_id) {
    detail::chat_target_add(live_chat_target_state(), player_id);
}

} // namespace mh::lockstep
