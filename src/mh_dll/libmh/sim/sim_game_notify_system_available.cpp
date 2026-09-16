//
// sim/sim_game_notify_system_available.cpp -- see sim_game_notify_system_available.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim/llm_game_notify_system_available_004401a6.asm), not from the Ghidra
// .c draft -- the draft reads the WRONG field (see the header's FIELD OFFSET BUG note).
//
#include "sim/sim_game_notify_system_available.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const game_notify_system_available_calls &live_game_notify_system_available_calls() {
    static const game_notify_system_available_calls c = {
        MH_CRT(w_sprintf__vss),
    };
    return c;
}

namespace {

// UTF-16 "%s (%s)" @0x005009e8 (`u__s___s_`, mh_addrs.gen.h) -- the literal PUSH-immediate this
// function's own call site uses. See the header's note distinguishing it from the byte-identical but
// separately-addressed 0x005017bc string other dispatch handlers use.
constexpr const wchar_t *TEXT_FMT_SYSTEM_NAME = L"%s (%s)";

// G_TEXT_PTRS[0x77] -- the "System" label, per the function's own plate ("text 0x77"). Named per the
// same convention sim_bldg_state_destroyed.h's TEXT_ID_BUILDING_DESTROYED_NAME uses.
constexpr int32_t TEXT_ID_SYSTEM_LABEL = 0x77;

// The System record's stride in int32 UNITS (0x8c bytes / 4 = 35), matching sim_view::
// system_define_index_base's own comment: `system_define_index_base[system_idx*(0x8c/4) + 1]`.
constexpr int32_t SYSTEM_STRIDE_INTS = 0x8c / 4;

} // namespace

namespace detail {

// ---- llm_game_notify_system_available @0x004401a6 -------------------------------------------------
//
// One condition, one call, no else branch (0x004401ff is both the JNZ target and the fall-through
// tail -- there is nothing on the "not the local player" path but the epilogue).
void game_notify_system_available(const sim_view &v, sim_store &own,
                                  const game_notify_system_available_calls &c, uint16_t player,
                                  int32_t system_idx) {
    // 0x004401c3-0x004401cd: 16-bit bit-pattern compare (CMP AX, word ptr PlayerSide), same idiom
    // sim_bldg_notify_ui.cpp / sim_unit_notify.cpp / sim_unit_on_destroyed.cpp already use for this
    // exact global.
    if (static_cast<int16_t>(player) != *v.player_side) return;

    // 0x004401cf-0x004401d6: define_index = *(int32_t*)(System + system_idx*0x8c + 4) -- see the
    // header's FIELD OFFSET BUG note (the Ghidra .c draft reads offset +0, `.invention`, instead).
    const int32_t define_index = v.system_define_index_base[system_idx * SYSTEM_STRIDE_INTS + 1];

    // 0x004401df-0x004401f7: w_sprintf(G_TEXT_TMP, "%s (%s)", G_TEXT_PTRS[0x77],
    // G_TEXT_PTRS[define_index]) -- push order confirmed right-to-left per the header's derivation.
    c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_SYSTEM_NAME, v.text_ptrs[TEXT_ID_SYSTEM_LABEL],
                     v.text_ptrs[define_index]);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void game_notify_system_available(uint16_t player, int32_t system_idx) {
    sim_state st = state();
    detail::game_notify_system_available(st.read, st.own, live_game_notify_system_available_calls(),
                                         player, system_idx);
}


} // namespace mh::sim
