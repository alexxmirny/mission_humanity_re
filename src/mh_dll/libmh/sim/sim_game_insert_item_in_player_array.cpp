//
// sim/sim_game_insert_item_in_player_array.cpp -- see sim_game_insert_item_in_player_array.h.
// Translated from the DISASSEMBLY (tmp/decomp/game_InsertItemInPlayerArray_00414106.asm), which the
// Ghidra .c draft agrees with exactly (independently re-traced register-by-register: the do-while scan
// order, the "remaining != 0" match-found gate, the new_item==0 shift-and-zero-fill path, and the
// MOVZX-not-MOVSX PlayerSide widening at the tail all match the draft's own reading).
//
#include "sim/sim_game_insert_item_in_player_array.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const insert_item_in_player_array_calls &live_insert_item_in_player_array_calls() {
    static const insert_item_in_player_array_calls c = {
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// game::e::event member 7 (the strategic-sim notes' resolved 25-member table: "mark build page 2
// dirty; project acquired / population change"). Declared locally per-TU (not hoisted to
// sim_event_codes.h) to avoid the ODR-collision risk that header's own banner documents already
// having happened once for a bare name at `mh::sim` scope -- same reasoning
// sim_game_add_to_available_buildings.cpp's own BUILD_BUILDINGS_REFRESH gives.
inline constexpr uint32_t INSERT_ITEM_BUILD_PROJECTS_REFRESH = 7u;

} // namespace

namespace detail {

// 0x00414106-0x0041419e. See the header banner for the branch-by-branch derivation against the
// assembly.
void insert_item_in_player_array(const sim_view &v, const insert_item_in_player_array_calls &c,
                                 int32_t *arr, int32_t size, uint32_t player, int32_t item,
                                 int32_t new_item) {
    // 0x00414127-0x00414140: do { if (*p == item) break; p++; remaining--; } while (remaining > 0);
    // -- the compare runs BEFORE the decrement/advance, so `remaining` ends nonzero iff a match was
    // found before the scan exhausted the array (a do-while, not a for-loop with a leading test).
    int32_t *p         = arr;
    int32_t  remaining = size;
    while (true) {
        if (*p == item) break;
        ++p;
        --remaining;
        if (remaining <= 0) break; // 0x0041413c-0x00414140: JG not taken -> falls through, no match
    }

    // 0x00414142-0x00414146: remaining == 0 means the scan exhausted without a match -- no append
    // path, return unchanged.
    if (remaining != 0) {
        // 0x00414148-0x0041414e: overwrite the matched slot.
        *p = new_item;

        // 0x00414150-0x00414182: new_item == 0 is a REMOVAL -- shift every element after the matched
        // slot down by one, then zero-fill the vacated last slot. new_item != 0 skips this whole block
        // (the original's JNZ jumps straight past it to the PlayerSide gate).
        if (new_item == 0) {
            // 0x00414156-0x00414177: while (--remaining > 0) { *p = p[1]; ++p; }
            while (--remaining > 0) {
                *p = p[1];
                ++p;
            }
            // 0x00414179-0x00414182: zero-fill the vacated last slot.
            *p = 0;
        }

        // 0x00414182-0x00414198: a change was made -- notify the viewing player only. The PlayerSide
        // load is `MOVZX EAX, word ptr [PlayerSide]`, a ZERO-extend (not sign-extend) before the
        // 32-bit compare against the full `player` dword -- reproduced explicitly rather than relying
        // on int16_t's implicit sign-extension, which would differ for a negative PlayerSide value.
        if (static_cast<uint32_t>(static_cast<uint16_t>(*v.player_side)) == player) {
            c.set_event(INSERT_ITEM_BUILD_PROJECTS_REFRESH);
        }
    }
}

} // namespace detail

// ---- the public wrapper ---------------------------------------------------------------------------
//
// Takes `void *arr` (matching sig_game_InsertItemInPlayerArray exactly, not `int32_t *`) because the
// shadow/export thunk machinery assigns this function directly to a pointer-to-function variable of
// that exact type, which requires an exact parameter-type match.
void insert_item_in_player_array(int32_t *arr, int32_t size, uint32_t player, int32_t item,
                                 int32_t new_item) {
    sim_state st = state();
    detail::insert_item_in_player_array(st.read, live_insert_item_in_player_array_calls(),
                                        arr, size, player, item, new_item);
}


} // namespace mh::sim
