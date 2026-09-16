//
// sim/libtrans/sim_lt_invasion_alert.cpp -- see sim_lt_invasion_alert.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_strat_invasion_alert_clear_0049b4dd.asm,
// tmp/decomp_lib_trans/llm_strat_invasion_alert_poll_0049b51c.asm), not from the Ghidra .c drafts
// beside them (the .c drafts render the two x87 tests as bare `0.0 <= arr[i]` / `... < now`, which
// is exactly the mismatch the header banner's "THE TWO x87 TESTS" section rules out).
//
#include "sim/libtrans/sim_lt_invasion_alert.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// `FLDZ; FCOMP b; FNSTSW AX; SAHF; JA` -- fires (CF=0 AND ZF=0) exactly on ordered "a > b"; already
// identical to plain `>` (NaN gives false on both sides), named only so the call site reads as the
// Jcc the disassembly actually has -- same posture as mh::lockstep's x87_above.
inline bool fcom_above(double a, double b) { return a > b; } // JA

// `FLD a; FADD ...; FCOMP b; FNSTSW AX; SAHF; JNC` -- fires (CF=0) exactly on ordered "a >= b";
// already identical to plain `>=` (NaN gives false on both sides). See the header banner's full
// condition-code table for why the FALLTHROUGH (this returning false) is what covers the unordered
// case, not this function itself.
inline bool fcom_above_equal(double a, double b) { return a >= b; } // JNC / JAE

} // namespace

namespace detail {

void invasion_alert_clear(sim_store &own, int32_t planet) {
    // 0x0049b4fe/0x0049b508: see the header banner's dword-pair / -1.0 equivalence.
    own.invasion_alert_time_at(planet) = -1.0;
}

int32_t invasion_alert_poll(const sim_view &v, sim_store &own, double now) {
    // Loop bound `< 0x20` (0x0049b53b); no match after all 32 entries returns -1 (0x0049b59a).
    for (int32_t i = 0; i < 0x20; ++i) {
        // Test 1 (0x0049b551-0x0049b55c): skip when JA fires, i.e. ordered "0.0 > arr[i]". Reads
        // the slot fresh (matching the assembly's own FCOMP memory operand) rather than caching it
        // across test 1 and test 2 -- harmless either way in this single-threaded, callee-free loop
        // (nothing can write the slot between the two tests), but written to mirror the two separate
        // memory reads (0x0049b553 / 0x0049b564) rather than assume it.
        if (fcom_above(0.0, own.invasion_alert_time_at(i)))
            continue;

        // Test 2 (0x0049b564-0x0049b576): skip when JNC fires, i.e. ordered
        // "arr[i] + interval >= now". Falls through (matches) on "arr[i] + interval < now"
        // (ordered) OR an unordered compare -- see the header banner's disagreement note with the
        // batch hazard prose. The add is a single FADD with no intermediate store in the original
        // (register-resident at 80-bit precision); written as a plain C++ sum relying on this
        // module's committed /arch:IA32 /fp:precise x87 codegen to keep it register-resident, same
        // as every other single-add-then-compare site in this tree (e.g. sim_lt_math.cpp's
        // scale_pct). Flagged in uncertainties[] as an FP comparison per translator-brief rule 9.
        if (fcom_above_equal(own.invasion_alert_time_at(i) + *v.invasion_alert_interval, now))
            continue;

        // 0x0049b578-0x0049b58e: re-arm with the raw bit pattern of `now` -- bit-identical to a
        // plain double assignment (see header banner).
        own.invasion_alert_time_at(i) = now;
        return i;
    }
    return -1;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void invasion_alert_clear(int32_t planet) {
    sim_state st = state();
    detail::invasion_alert_clear(st.own, planet);
}

int32_t invasion_alert_poll(double now) {
    sim_state st = state();
    return detail::invasion_alert_poll(st.read, st.own, now);
}


} // namespace mh::sim
