//
// sim/libtrans/sim_lt_progress_unlock_fixpoint.cpp -- see sim_lt_progress_unlock_fixpoint.h.
// Translated from the DISASSEMBLY (tmp/decomp_lib_trans/llm_progress_propagate_unlocks_00440cb5.asm,
// llm_progress_collect_available_projects_004990bf.asm, llm_progress_recheck_projects_00499174.asm,
// llm_progress_recheck_buildings_004991f2.asm); the .c files beside each are drafts and all four
// agree with the assembly branch-for-branch (see the header banner).
//
#include "sim/libtrans/sim_lt_progress_unlock_fixpoint.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// ---- cfg_enum_E_INVETION_TYPE (translator-brief rule 17a; Ghidra's own field-comment spelling) ----
// No committed Ghidra ENUM TYPE exists for this domain (only the type-comment string) -- per the
// established precedent (sim_game_handle_progress.h / sim_game_update_progress.cpp), this TU defines
// BUILDING/PROJECT come from sim_game_handle_progress.h's inline constexpr set (this TU already
// includes it for detail::game_handle_progress, and a local copy at the same namespace scope is a
// C2872 ambiguity, found at wave-2 integration). UNDEFINED=0 is this unit's own addition (needed
// by propagate_unlocks's `type != UNDEFINED` gate; the strategic-sim notes' full member listing
// names it, the sibling TUs never needed the zero case spelled out).
constexpr uint8_t INVENTION_TYPE_UNDEFINED = 0;

// Reconstructs a `depend[]` entry as the 16-bit word the assembly actually reads (DECLARED NEED G2:
// `mh_cfg_final_struct_Invention::depend` is typed `uint8_t depend[32]` but is a 16-entry
// `cfg_t_invention_index_s[16]` array in truth -- see the header banner). `j` is already bound-
// checked by the caller's `for (j = 0; j < 16; ++j)` (hazard 1's hoisted bound test), so `j*2+1` never
// exceeds index 31. Little-endian byte pair, matching this x86 target -- not a reinterpret_cast
// (translator-brief rule 2: never one inside a body).
inline uint16_t depend_word(const cfg_invention &inv, int32_t j) {
    return static_cast<uint16_t>(inv.depend[j * 2]) |
           static_cast<uint16_t>(static_cast<uint16_t>(inv.depend[j * 2 + 1]) << 8);
}

} // namespace

namespace detail {

// ---- llm_progress_propagate_unlocks @0x00440cb5 ----------------------------------------------------
void progress_propagate_unlocks(const sim_view &v, sim_store &own, uint16_t player,
                                const handle_progress_calls &c_hp) {
    // 0x00440cd0 / 0x00440e08-0x00440e0c: the outer fixpoint. Re-run the WHOLE 1..299 scan whenever
    // any game_HandleProgress call fired during the pass; NO iteration cap (hazard 4 -- preserve
    // exactly, translator-brief rule 14).
    for (;;) {
        bool any_fired = false;

        // 0x00440cd7: seed 1, not 0 -- hazard 5, asymmetric with the three sweepers below.
        for (int32_t i = 1; i < PROGRESS_ROW_COUNT; ++i) {
            // 0x00440cf4-0x00440d06 / 0x00440d0f-0x00440d21: progress[player][i].available==false &&
            // .f3==false. `player` here is this function's OWN 16-bit parameter (unlike the three
            // sweepers, propagate_unlocks carries no width split -- its storage is AX:2 throughout).
            const player_progress &row = progress_of(v, static_cast<int32_t>(player), i);
            if (row.available || row.f3)
                continue;

            // 0x00440d2c-0x00440d37: Progress[i].type != UNDEFINED(0).
            const cfg_invention &inv = v.cfg_inventions[i];
            if (inv.type == INVENTION_TYPE_UNDEFINED)
                continue;

            // ---- hazard 1: depend[] prerequisite scan, bound test HOISTED ahead of the read. The
            // original reads depend[j] BEFORE testing j<16 (0x00440d4c-0x00440d67); at j==16 that is
            // an OOB read of the 16-entry array, but both the `dep==0` exit and the `j>=16` fallthrough
            // land on the SAME label with the "prereqs ok" flag unchanged (true) either way -- see the
            // header banner for the full address-by-address derivation. Hoisting `j < 16` into the
            // loop condition reproduces that exact exit state without ever reading past depend[31].
            bool prereqs_ok = true;
            for (int32_t j = 0; j < 16; ++j) {
                const uint16_t dep = depend_word(inv, j);
                if (dep == 0)
                    break; // 0x00440d65: terminator -- prereqs_ok stays true.
                if (!progress_of(v, static_cast<int32_t>(player), dep).acquired) {
                    prereqs_ok = false; // 0x00440d88->0x00440d8a
                    break;
                }
            }
            if (!prereqs_ok)
                continue;

            // ---- hazard 2/3: the PROJECT gate. Proceeds when type != PROJECT, OR the CURRENT
            // PLANET's own invention is NOT YET acquired by the player at the GLOBAL PlayerSide (NOT
            // this function's own `player` parameter -- 0x00440dc1 reads PlayerSide directly). Full
            // 32-bit read of invention_index (hazard 3 -- do not narrow to 16-bit like unit b2's
            // sibling read of the same field).
            if (inv.type == INVENTION_TYPE_PROJECT) {
                const int32_t planet_invention = v.cfg_planets[*v.planet_index].invention_index;
                // MOVZX @0x00440dc1 -- ZERO-extend of the 16-bit PlayerSide, so the widening goes
                // through uint16_t (sim_view::player_side is const int16_t*; a straight int32_t cast
                // sign-extends -- the verify-B confirmed divergence, fixed 2026-09-02; the
                // sim_invasion.cpp:85 / sim_lt_diplomacy two-step idiom).
                const int32_t side = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
                if (progress_of(v, side, planet_invention).acquired)
                    continue; // gate==0 (0x00440de2): already acquired by the human -- skip.
            }

            // 0x00440df7: game_HandleProgress(player, i) -- reached DIRECTLY through its own detail::
            // body (never the public wrapper / mh::call::), so an armed sibling site is never nested
            // inside a call from this unit (see the header banner's nesting-hazard note).
            mh::sim::detail::game_handle_progress(v, own, c_hp, player, static_cast<uint16_t>(i));
            any_fired = true;
        }

        if (!any_fired)
            return;
    }
}

// ---- llm_progress_recheck_projects @0x00499174 ------------------------------------------------------
void progress_recheck_projects(const sim_view &v, sim_store &own, uint32_t player,
                               const handle_progress_calls &c_hp) {
    // 0x0049918f: seed 0 (hazard 5).
    for (int32_t i = 0; i < PROGRESS_ROW_COUNT; ++i) {
        // hazard 6: `progress` is indexed with the FULL 32-bit `player`; only the low 16 bits are
        // passed to any callee below.
        if (v.cfg_inventions[i].type != INVENTION_TYPE_PROJECT)
            continue;
        if (progress_of(v, static_cast<int32_t>(player), i).f3)
            continue;
        mh::sim::detail::game_handle_progress(v, own, c_hp, static_cast<uint16_t>(player),
                                              static_cast<uint16_t>(i));
    }
    // 0x004991e3: same-TU tail call, FORWARDING the same c_hp (the "sharing ONE _calls table" note).
    progress_propagate_unlocks(v, own, static_cast<uint16_t>(player), c_hp);
}

// ---- llm_progress_recheck_buildings @0x004991f2 -----------------------------------------------------
void progress_recheck_buildings(const sim_view &v, sim_store &own, uint32_t player,
                                const handle_progress_calls &c_hp) {
    for (int32_t i = 0; i < PROGRESS_ROW_COUNT; ++i) {
        if (v.cfg_inventions[i].type != INVENTION_TYPE_BUILDING)
            continue;
        const player_progress &row = progress_of(v, static_cast<int32_t>(player), i);
        // hazard 7: `available == false` here -- the OPPOSITE sense from
        // collect_available_projects's `available != false` below. Not a copy-paste of one another.
        if (row.available)
            continue;
        if (row.f3)
            continue;
        mh::sim::detail::game_handle_progress(v, own, c_hp, static_cast<uint16_t>(player),
                                              static_cast<uint16_t>(i));
    }
    progress_propagate_unlocks(v, own, static_cast<uint16_t>(player), c_hp);
}

// ---- llm_progress_collect_available_projects @0x004990bf ---------------------------------------------
void progress_collect_available_projects(const sim_view &v, sim_store &own, uint32_t player,
                                         const game_update_progress_calls &c_up,
                                         const handle_progress_calls      &c_hp) {
    for (int32_t i = 0; i < PROGRESS_ROW_COUNT; ++i) {
        if (v.cfg_inventions[i].type != INVENTION_TYPE_PROJECT)
            continue;
        const player_progress &row = progress_of(v, static_cast<int32_t>(player), i);
        if (!row.available) // hazard 7: `available != false` required (opposite of recheck_buildings).
            continue;
        if (row.acquired)
            continue;
        if (row.f3)
            continue;
        // The ONE sweeper that reaches game_UpdateProgress instead of game_HandleProgress -- the
        // ACQUISITION half, not the AVAILABILITY half.
        mh::sim::detail::game_update_progress(v, own, c_up, static_cast<uint16_t>(player),
                                              static_cast<uint16_t>(i));
    }
    // 0x00499165: same-TU tail call, forwarding c_hp down alongside this function's own c_up.
    progress_propagate_unlocks(v, own, static_cast<uint16_t>(player), c_hp);
}

} // namespace detail

// ---- the public wrappers ----------------------------------------------------------------------------

void progress_propagate_unlocks(uint16_t player) {
    sim_state st = state();
    detail::progress_propagate_unlocks(st.read, st.own, player);
}

void progress_recheck_projects(uint32_t player) {
    sim_state st = state();
    detail::progress_recheck_projects(st.read, st.own, player);
}

void progress_recheck_buildings(uint32_t player) {
    sim_state st = state();
    detail::progress_recheck_buildings(st.read, st.own, player);
}

void progress_collect_available_projects(uint32_t player) {
    sim_state st = state();
    detail::progress_collect_available_projects(st.read, st.own, player);
}


} // namespace mh::sim
