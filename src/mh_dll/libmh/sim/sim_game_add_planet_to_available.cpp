//
// sim/sim_game_add_planet_to_available.cpp -- see sim_game_add_planet_to_available.h. Translated
// from the DISASSEMBLY (tmp/decomp/game_AddPlanetToAvailable_00440208.asm).
//
#include "sim/sim_game_add_planet_to_available.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const add_planet_to_available_calls &live_add_planet_to_available_calls() {
    static const add_planet_to_available_calls c = {
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
    };
    return c;
}

namespace {

// UTF-16 "%s (%s)" @0x005009e8 (`u__s___s_`, mh_addrs.gen.h) -- the same already-named literal
// sim_game_notify_system_available.cpp's own w_sprintf call uses, and this function's own
// PUSH-immediate matches it exactly.
constexpr const wchar_t *TEXT_FMT_PLANET_NAME = L"%s (%s)";

} // namespace

namespace detail {

// ---- game_AddPlanetToAvailable @0x00440208 -----------------------------------------------------------
//
// One three-way gate, one call pair, one state write -- no else branch (every failed gate term
// branches straight to the shared "do nothing" tail at 0x004402a7).
void game_add_planet_to_available(const sim_view &v, sim_store &own,
                                  const add_planet_to_available_calls &c, uint16_t player,
                                  int32_t planet_id) {
    // 0x00440228-0x00440251: the three-way gate. See the header for the per-term address citation and
    // the field-offset cross-check (system_index@0x8, name@0x4 of mh_cfg_final_struct_Planet).
    if (static_cast<int16_t>(player) == *v.player_side &&
        v.cfg_planets[planet_id].system_index == *v.current_system && planet_id != *v.planet_index) {
        // 0x00440265-0x0044027d: w_sprintf(G_TEXT_TMP, "%s (%s)", G_TEXT_PTRS[0x78],
        // G_TEXT_PTRS[Planets[planet_id].name]) -- push order confirmed right-to-left per the header.
        c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_PLANET_NAME, v.text_ptrs[TEXT_ID_PLANET_LABEL],
                         v.text_ptrs[v.cfg_planets[planet_id].name]);

        // 0x00440285-0x0044028f: always printed once the gate passed. Return value discarded, matching
        // the .c draft.
        c.print_text_message(own.text_scratch());

        // 0x0044028f-0x004402a1: planet_time[planet_id] = GAME_CLOCK + DOUBLE_00500a68. See DECLARED
        // NEED 1 in the header -- `planet_available_notify_delay` does not exist on sim_view yet.
        own.planet_time_at(planet_id) = *v.game_clock + *v.planet_available_notify_delay;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void game_add_planet_to_available(uint16_t player, int32_t planet_id) {
    sim_state st = state();
    detail::game_add_planet_to_available(st.read, st.own, live_add_planet_to_available_calls(), player,
                                         planet_id);
}


} // namespace mh::sim
