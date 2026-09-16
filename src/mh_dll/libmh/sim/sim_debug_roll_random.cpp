//
// sim/sim_debug_roll_random.cpp -- see sim_debug_roll_random.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_debug_roll_random_0049fc6f.asm): PUSH order confirms `ADD ESP,0x14` = 5 dwords
// (dst, format, clock-as-double, roll), matching w_sprintf's measured `vdi` shape exactly.
//
#include "sim/sim_debug_roll_random.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const debug_roll_random_calls &live_debug_roll_random_calls() {
    static const debug_roll_random_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
        MH_CRT(w_sprintf__vdi),
    };
    return c;
}

namespace {
// 0x00501b18, read via ReVA get-data (2026-08-10): L"random %.2f  %d" -- TWO spaces before %d.
// Supplied as our own literal rather than a raw VA, same convention as sim_order_dispatch_bldg.cpp's
// TEXT_FMT_NAME_REASON.
constexpr const wchar_t *TEXT_FMT_RANDOM = L"random %.2f  %d";
} // namespace

namespace detail {

void debug_roll_random(const sim_view &v, sim_store &own, const debug_roll_random_calls &c) {
    const int32_t roll = c.rand_below(100);
    // _G_LLM_STRAT_GAME_CLOCK is a double; the original pushes it as two dwords (CONCAT44 in the
    // decompile) purely because it is a raw stack-marshalled vararg call -- w_sprintf__vdi's `vdi`
    // shape thunk does that split for us, so the C++ body just passes the double.
    c.w_sprintf__vdi(own.text_scratch(), TEXT_FMT_RANDOM, *v.game_clock, roll);
}

} // namespace detail

void debug_roll_random() {
    sim_state st = state();
    detail::debug_roll_random(st.read, st.own, live_debug_roll_random_calls());
}

} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
