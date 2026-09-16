//
// sim/libtrans/sim_lt_progress_finalize.cpp -- see sim_lt_progress_finalize.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_progress_finalize_acquire_004404cc.asm), not the Ghidra `.c`
// draft beside it (the draft is a faithful transcription, cross-checked against the raw bytes anyway
// per house rule).
//
#include "sim/libtrans/sim_lt_progress_finalize.h"

#include "sim/libtrans/sim_lt_available_remove.h" // the rebound Remove twins (LT1B adoptions)

#include "addr/mh_calls.gen.h" // typed callables for the two RemoveFromAvailable* originals

namespace mh::sim {

namespace {

// ---- cfg_enum_E_INVETION_TYPE (rule 17a; see the header's "THE cfg_enum_E_INVETION_TYPE DOMAIN"
// section for why this is a fresh, file-local, ANONYMOUS-namespace copy rather than a shared include
// or a header-scope constant -- ODR safety, matching sim_game_update_progress.cpp's own precedent).
// Only the three values this function's switch actually dispatches on; values cross-checked against
// sim_game_handle_progress.h's already-verified INVENTION_TYPE_* constants (same domain, same
// mapping).
constexpr uint8_t INVENTION_TYPE_BUILDING = 1;
constexpr uint8_t INVENTION_TYPE_PROJECT  = 3;
constexpr uint8_t INVENTION_TYPE_SYSTEM   = 5;

// game::e::event codes this function needs -- DECLARED NEED, see the header banner. Own local copies
// (NOT edited into the shared sim_event_codes.h, which a translator may not touch).
constexpr uint32_t BUILD_BUILDINGS_REFRESH = 0xf; // game::e::event member 15
constexpr uint32_t BUILD_PROJECTS_REFRESH  = 0x7; // game::e::event member 7

} // namespace

const lt_progress_finalize_calls &live_lt_progress_finalize_calls() {
    static const lt_progress_finalize_calls c = {
        &mh::sim::remove_from_available_buildings, // REBOUND 2026-09-02: the LT1B adoption is translated; ours binds directly
        &mh::sim::remove_from_available_projects,  // REBOUND 2026-09-02: same
    };
    return c;
}

namespace detail {

void progress_finalize_acquire(const sim_view &v, sim_store &own, const lt_progress_finalize_calls &c,
                               uint16_t player, uint16_t pid, const game_set_event_calls &c_set_event) {
    // 0x00440506-0x00440519 then 0x00440520-0x00440533: the original recomputes the record address
    // TWICE (one IMUL/LEA/ADD sequence per write), but both land on the same `progress[player][pid]`
    // record -- ONE fetch here (house one-record-by-reference contract), BOTH writes preserved in
    // ORDER: `.f3` (offset +0x2) first, then `.available` (offset +0x0).
    player_progress &rec = own.progress_at(player, static_cast<int32_t>(pid));
    rec.f3               = true;  // 0x00440519
    rec.available        = false; // 0x00440533

    // 0x0044053a-0x00440544: CALL NOTHING2() -- omitted. Same precedent as sim_game_handle_progress.h:
    // NOTHING2's committed marshalling (addr/mh_calls.gen.h) is `s_void` (zero argument registers), so
    // the XOR EBX,EBX / two MOVZX loads immediately before this call site are dead.

    // 0x00440549-0x0044056c: switch on the RAW Progress[pid].type (the original indexes on the
    // derived type-1 value for its jump table -- a codegen detail, not a semantic one). Only
    // BUILDING/PROJECT/SYSTEM have a case body; every other type (UNIT/PLANET/UPGRADE, type 0, or any
    // value > 6) lands on the shared no-op epilogue with no further effect beyond the two writes
    // above -- an ordinary switch with no `default:` arm already does that.
    switch (v.cfg_inventions[pid].type) {
        case INVENTION_TYPE_BUILDING: {                         // caseD_0, 0x00440573-0x004405a0
            const uint16_t index = v.cfg_inventions[pid].index; // 0xe16305
            c.remove_from_available_buildings(player, static_cast<int32_t>(index));
            // 0x0044058d-0x0044059f: the PlayerSide gate guards ONLY this SetEvent, never the removal
            // call above.
            if (player == static_cast<uint16_t>(*v.player_side)) {
                mh::sim::detail::game_set_event(v, own, c_set_event, BUILD_BUILDINGS_REFRESH);
            }
            break;
        }

        case INVENTION_TYPE_PROJECT: {                          // caseD_2, 0x004405a5-0x004405d2
            const uint16_t index = v.cfg_inventions[pid].index; // 0xe16305
            c.remove_from_available_projects(player, static_cast<uint32_t>(index));
            // 0x004405bf-0x004405d1: same gate shape as BUILDING above -- guards only this SetEvent.
            if (player == static_cast<uint16_t>(*v.player_side)) {
                mh::sim::detail::game_set_event(v, own, c_set_event, BUILD_PROJECTS_REFRESH);
            }
            break;
        }

        case INVENTION_TYPE_SYSTEM: // caseD_4, 0x004405d4-0x0044061f
            // 0x004405d4/0x004405db: loop SEEDED AT PLANET 1, not 0 -- slot 0 is never a candidate.
            // Bound `< 0x20` (32 planets). Preserve exactly, do not "fix".
            for (int32_t i = 1; i < 0x20; ++i) {
                // 0x004405eb-0x00440600: `Progress[pid].index` (uint16) re-read fresh every
                // iteration, matching the assembly's own re-load even though `pid` is loop-invariant.
                const uint16_t inv_index = v.cfg_inventions[pid].index;
                if (static_cast<int32_t>(inv_index) == v.cfg_planets[i].system_index) {
                    // 0x0044060f: `Planets[i].invention_index` is a full int32 field (@+0), but this
                    // site reads it with a 16-bit MOVZX -- only the low 16 bits reach the recursive
                    // call. Unit b1's sibling reads the SAME field as a full dword; the two widths are
                    // deliberately different, do not harmonise.
                    const uint16_t next_pid = static_cast<uint16_t>(v.cfg_planets[i].invention_index);
                    // 0x0044061a: DIRECT self-recursion into this function's own entry, same
                    // `v`/`own`/`c`/`c_set_event`, same `player` -- only `pid` changes. See the header
                    // banner: this is exactly why the ORIGINAL site is not shadow-armed; the C++
                    // recursion itself is an ordinary detail:: self-call.
                    progress_finalize_acquire(v, own, c, player, next_pid, c_set_event);
                }
            }
            break;

        default:
            // type==0 (UNDEFINED), UNIT(2), PLANET(4), UPGRADE(6), or any value > 6 -- the JA-taken
            // bounds-check path in the assembly, which has no case body at all.
            break;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void progress_finalize_acquire(uint16_t player, uint16_t pid) {
    sim_state st = state();
    detail::progress_finalize_acquire(st.read, st.own, live_lt_progress_finalize_calls(), player, pid);
}


} // namespace mh::sim
