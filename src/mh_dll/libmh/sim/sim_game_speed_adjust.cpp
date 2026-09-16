//
// sim/sim_game_speed_adjust.cpp -- see sim_game_speed_adjust.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_game_speed_increase_004976cd.asm, tmp/decomp/llm_game_speed_decrease_00497734.asm),
// which the decompile agrees with exactly -- no hidden control flow, no phantom stores.
//
#include "sim/sim_game_speed_adjust.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const game_speed_adjust_calls &live_game_speed_adjust_calls() {
    static const game_speed_adjust_calls c = {
        MH_LIBMH_BIND(llm_game_speed_recompute),
        mh::state::evt::text_game_speed,
    };
    return c;
}

namespace detail {

// 0x004976e5-0x0049772a. The player id arrives full-width in EAX but the assembly stores it to a
// stack dword and then re-reads only the LOW WORD (MOVZX EAX, word ptr [...]) for every array-index
// computation (three times: the compare load, the multiply load, and the store) -- a real 32->16-bit
// narrowing per the house rule that integer width is semantic, reproduced here via the cast rather
// than indexing with the full 32-bit player. Behaviourally inert for the real player domain (0..7,
// MAX_PLAYERS) since it only differs from the unmasked value for player >= 0x10000, which no caller
// produces, but the cast keeps the translation matching what the assembly actually computes.
void game_speed_increase(const sim_view &v, sim_store &own, const game_speed_adjust_calls &c,
                         uint32_t player) {
    double &factor = own.game_speed_player_factor_at((uint32_t)(uint16_t)player);
    // 0x004976ef-0x004976fe: FLD factor; FCOMP game_speed_factor_max; FNSTSW AX; SAHF; JNC skip.
    // SAHF's CF=C0 (ST(0) < src), so JNC (CF==0) skips when factor >= max -- the body below runs
    // exactly when factor < max, matching the decompile's `<` reading with no ambiguity.
    if (factor < *v.game_speed_factor_max) {
        // 0x00497700-0x0049771a: FLD factor; FMUL step_up; FSTP factor -- compute-then-store BEFORE
        // either outward call, same order as the assembly.
        factor *= *v.game_speed_factor_step_up;
        c.game_speed_recompute();
        c.ui_print_game_speed();
    }
}

// 0x0049774c-0x00497791. Mirror of game_speed_increase; same 32->16-bit player narrowing (three
// re-reads of the stored word at 0x0049774f/0x00497767/0x0049777a).
void game_speed_decrease(const sim_view &v, sim_store &own, const game_speed_adjust_calls &c,
                         uint32_t player) {
    double &factor = own.game_speed_player_factor_at((uint32_t)(uint16_t)player);
    // 0x00497756-0x00497765: FLD factor; FCOMP game_speed_factor_min; FNSTSW AX; SAHF; JBE skip.
    // JBE (CF==1 or ZF==1) skips when factor <= min -- the body runs exactly when factor > min,
    // matching the decompile's `min < factor` reading with no ambiguity.
    if (*v.game_speed_factor_min < factor) {
        // 0x0049776e-0x00497781: FLD factor; FDIV step_down; FSTP factor.
        factor /= *v.game_speed_factor_step_down;
        c.game_speed_recompute();
        c.ui_print_game_speed();
    }
}

} // namespace detail

void game_speed_increase(uint32_t player) {
    sim_state st = state();
    detail::game_speed_increase(st.read, st.own, live_game_speed_adjust_calls(), player);
}

void game_speed_decrease(uint32_t player) {
    sim_state st = state();
    detail::game_speed_decrease(st.read, st.own, live_game_speed_adjust_calls(), player);
}

} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
