#include "ai/ai_hq_attack_scenario.h"

namespace mh::ai {
namespace detail {

void start_hq_attack_scenario(const ai_view &v, const ai_store &own, const ai_calls &gc) {
    *own.ai_enabled = 1;

    // Both operands are read off _G_LLM_STRAT_PLAYERS[0] specifically -- the original hardcodes
    // player index 0 here, it is not the ticking/local player. `% width` / `% height` are the
    // original's own signed IDIV remainder (EDX after `CDQ`-style sign-extend then IDIV), which is
    // exactly C++'s `%` truncating-toward-zero semantics on these int32_t operands
    // (0x004ba81a-0x004ba844).
    const player_profile &p0 = v.strat_players[0];
    const int32_t         x  = (p0.landing_x[0x1f] - 4) % *v.map_width;
    const int32_t         y  = (p0.landing_y[0x1f] + 0x14) % *v.map_height;

    // Register trace at the call site (0x004ba80a-0x004ba848): EAX=x, EDX=y, EBX=2, ECX=1, and one
    // stack dword pushed earlier (0x004ba80f, value 1, truncates to the callee's `char` 5th param).
    *own.ai_attack_hq_unit_id = gc.unit_create_soldier(x, y, 2, 1, 1);

    // Four diplomacy_set_relation calls, (player_a=EAX, player_b=EDX, relation=EBX) each, read off
    // the register sets at 0x004ba852-0x004ba891: (0,0,1), (1,1,1), (0,1,2), (1,0,2).
    gc.diplomacy_set_relation(0, 0, 1);
    gc.diplomacy_set_relation(1, 1, 1);
    gc.diplomacy_set_relation(0, 1, 2);
    gc.diplomacy_set_relation(1, 0, 2);

    gc.unit_commit_attack_on_enemy_hq();

    *own.tutorial_hq_attack_scenario_done = true;
}

} // namespace detail

void start_hq_attack_scenario() {
    const ai_state st = state();
    detail::start_hq_attack_scenario(st.read, st.own, live_calls());
}

} // namespace mh::ai
