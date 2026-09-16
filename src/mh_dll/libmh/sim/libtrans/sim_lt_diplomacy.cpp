//
// sim/libtrans/sim_lt_diplomacy.cpp -- see sim_lt_diplomacy.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_diplomacy_init_multiplayer_00465ac0.asm,
// tmp/decomp_lib_trans/llm_diplomacy_init_skirmish_00465baf.asm), not from the exported `.c` drafts.
//
#include "sim/libtrans/sim_lt_diplomacy.h"

#include "lockstep/lt_chat_ally_mask.h" // SIMABI-CHAT: chat_target_add, ours since 2026-09-10
#include "ai/ai_state.h"                // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"

namespace mh::sim {

const lt_diplomacy_calls &live_lt_diplomacy_calls() {
    static const lt_diplomacy_calls c = {
        // SIMABI-CHAT 2026-09-10: INTERNALIZED. Was mh::host().llm_ui_chat_target_add, the last
        // `chat` entry of the sim host table; the body is now lockstep/lt_chat_ally_mask.cpp and this
        // slot binds it directly (the shape sim_diplomacy_set_relation.cpp:39 uses). The UNTRANSLATED
        // UI caller llm_ui_diplomacy_apply_and_resume @0x004c7fc6 still drives the ORIGINAL into the
        // same two region-registered cells -- safe only because both ops are idempotent and this
        // translation is exact; see that header's dual-writer note.
        &mh::lockstep::chat_target_add,
    };
    return c;
}

namespace detail {

// ---- llm_diplomacy_init_multiplayer @0x00465ac0 -----------------------------------------------
void diplomacy_init_multiplayer(const sim_view &v, sim_store &own, const lt_diplomacy_calls &c,
                                const diplomacy_set_relation_calls &c_set_relation) {
    // 0x00465ad8-0x00465ba5: outer loop over player i, 0..7 inclusive (MAX_PLAYERS).
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        // 0x00465aef-0x00465b19: enabled(i) && human(i) -> register i as a chat target. bit0 = slot
        // enabled, bit2 = human-controlled (mh_structs.gen.h's status_flags field comment is the
        // authority -- neither bit has a committed C++/Ghidra-enum name, see the header's
        // declared_needs note).
        if ((v.profiles[i].status_flags & 0x01u) != 0 && (v.profiles[i].status_flags & 0x04u) != 0) {
            c.chat_target_add(static_cast<uint8_t>(i));
        }

        // 0x00465b19-0x00465b9e: inner loop over player j, 0..7 inclusive -- the all-pairs relation
        // pass.
        for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
            // 0x00465b30-0x00465b50: both i and j must be enabled slots before either relation is
            // set at all -- an inactive slot's relation entries are left stale, not cleared
            // (translator-brief rule 11). Short-circuited exactly like the asm: j's flag is not
            // even tested when i is already disabled.
            if ((v.profiles[i].status_flags & 0x01u) == 0 || (v.profiles[j].status_flags & 0x01u) == 0) {
                continue;
            }

            // 0x00465b52-0x00465b9e: relation=1 (ally) iff i==j OR (bit3(i) && bit3(j)); else
            // relation=2 (enemy). Bit3 = AI-controlled -- READ LITERALLY (see uncertainties[]):
            // every AI-controlled slot allies with every other AI-controlled slot regardless of
            // team, and every human slot is hostile to everyone but itself. bit3(j) is only tested
            // when bit3(i) already held, matching the asm's short-circuit read order exactly.
            uint8_t relation;
            if (i == j) {
                relation = 1;
            } else if ((v.profiles[i].status_flags & 0x08u) != 0 &&
                       (v.profiles[j].status_flags & 0x08u) != 0) {
                relation = 1;
            } else {
                relation = 2;
            }

            // 0x00465b87 / 0x00465b99: llm_diplomacy_set_relation(i, j, relation). Reached as an
            // in-tree sibling directly (CONDUCTOR CORRECTION, rule 3c) -- never mh::call::, which
            // would fault under net_selftest.exe (that VA is unmapped there).
            detail::diplomacy_set_relation(v, own, c_set_relation, i, j, relation);
        }
    }
}

// ---- llm_diplomacy_init_skirmish @0x00465baf --------------------------------------------------
void diplomacy_init_skirmish(const sim_view &v, sim_store &own,
                             const diplomacy_set_relation_calls &c_set_relation) {
    // 0x00465bc7-0x00465c64: outer loop over player i, 0..7 inclusive.
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        // 0x00465bde-0x00465c5f: inner loop over player j, 0..7 inclusive.
        for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
            // 0x00465bf5-0x00465c15: both slots must be enabled (bit0) before either relation is
            // set -- an inactive slot's relation entries are left stale (translator-brief rule 11).
            if ((v.profiles[i].status_flags & 0x01u) == 0 || (v.profiles[j].status_flags & 0x01u) == 0) {
                continue;
            }

            // 0x00465c17-0x00465c5d: the actual disassembly is a 4-way goto cascade -- i==j jumps
            // DIRECTLY to the ally arm (0x00465c1d); otherwise PlayerSide==i jumps to the enemy arm
            // (0x00465c29); otherwise PlayerSide==j falls through to the SAME enemy arm
            // (0x00465c35); otherwise (neither i nor j is PlayerSide) falls through to the ally arm.
            // Traced arm-by-arm, that is logically identical for every (i, j) to the if/else-if
            // below, which is a REASSEMBLED shape rather than a transcription (see uncertainties[]).
            // PlayerSide is read via MOVZX at both comparison sites -- a 16-bit UNSIGNED value
            // widened to int32 BEFORE the compare (uint16_t, not int16_t; a negative int16_t
            // reinterpretation would compare wrong here). The second read only happens when the
            // first didn't match i, matching the asm's fallthrough exactly via && short-circuit.
            uint8_t relation;
            if (i == j) {
                relation = 1; // 0x00465c1d -> 0x00465c39 -> 0x00465c4d (ally arm)
            } else if (static_cast<int32_t>(static_cast<uint16_t>(*v.player_side)) != i &&
                       static_cast<int32_t>(static_cast<uint16_t>(*v.player_side)) != j) {
                relation = 1; // 0x00465c39 -> 0x00465c4d (ally arm)
            } else {
                relation = 2; // 0x00465c29 or 0x00465c35 -> 0x00465c3b (enemy arm)
            }

            // 0x00465c46 / 0x00465c58: llm_diplomacy_set_relation(i, j, relation). Same in-tree
            // sibling call as the multiplayer body above.
            detail::diplomacy_set_relation(v, own, c_set_relation, i, j, relation);
        }
    }
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

void diplomacy_init_multiplayer() {
    sim_state st = state();
    detail::diplomacy_init_multiplayer(st.read, st.own, live_lt_diplomacy_calls());
}

void diplomacy_init_skirmish() {
    sim_state st = state();
    detail::diplomacy_init_skirmish(st.read, st.own);
}


} // namespace mh::sim
