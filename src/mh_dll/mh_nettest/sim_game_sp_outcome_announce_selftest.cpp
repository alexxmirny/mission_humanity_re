//
// sim_game_sp_outcome_announce_selftest.cpp -- `simtest` offline oracle for llm_game_sp_outcome_announce
// (sim/sim_game_sp_outcome_announce.h/.cpp, RI-SIM / SIM1F). Per the header banner: one
// read-only guard (`session_mode == SESSION_SP`), then three unconditional outward calls in program
// order, no sim-state write of any kind. All three callees are RECORDING STUBS (the header documents
// them as "fires for real under shadow", which is a SHADOW-ARM posture, not an offline-oracle one --
// this file is fully offline and stubs every one of them).
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_game_sp_outcome_announce_
// 0049803b.asm), not the .cpp -- a single CMP/JNZ guard then three CALLs in program order, no other
// branches to reassemble.
//
#include "sim/sim_game_sp_outcome_announce.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

enum class CallKind { PRINT,
                      SND,
                      DIALOG };
std::vector<CallKind> g_order; // records the SEQUENCE across all three stubs, to assert call ORDER

std::vector<int32_t> g_print_calls;
struct SndCall {
    int32_t id, vol;
};
std::vector<SndCall> g_snd_calls;
std::vector<uint8_t> g_dialog_calls;
int32_t              g_dialog_return = 0;

void reset_calls() {
    g_order.clear();
    g_print_calls.clear();
    g_snd_calls.clear();
    g_dialog_calls.clear();
    g_dialog_return = 0;
}

void stub_print_queue_text_id(int32_t text_id) {
    g_order.push_back(CallKind::PRINT);
    g_print_calls.push_back(text_id);
}
void stub_snd_play(int32_t sound_id, int32_t volume) {
    g_order.push_back(CallKind::SND);
    g_snd_calls.push_back({sound_id, volume});
}
int32_t stub_outcome_dialog(uint8_t outcome) {
    g_order.push_back(CallKind::DIALOG);
    g_dialog_calls.push_back(outcome);
    return g_dialog_return;
}

const game_sp_outcome_announce_calls g_calls = {
    stub_print_queue_text_id,
    stub_snd_play,
    stub_outcome_dialog,
};

void run(sim_fixture &fx) {
    reset_calls();
    sim_store own = fx.store();
    detail::game_sp_outcome_announce(fx.view(), own, g_calls);
}

} // namespace

void run_sp_outcome_announce_tests() {
    sim_fixture fx;

    // ---- session_mode == SESSION_SP: all three calls fire, IN PROGRAM ORDER, with the exact fixed
    // arguments the disassembly hardcodes (no per-race/per-player variance, unlike sibling snd_play
    // sites elsewhere in the closure -- see the header banner). Mutation note: swapping the call order,
    // or the sound id (0xa6) for the volume (100) (both plausible-looking int32_t constants), each
    // flips a distinct assertion below.
    fx.reset();
    fx.session_mode = SESSION_SP;
    g_dialog_return = 42; // discarded by the function; must not affect anything observable
    run(fx);
    ck((uint32_t)g_order.size() == 3, "SP: exactly three calls fired");
    ck(g_order.size() == 3 && g_order[0] == CallKind::PRINT && g_order[1] == CallKind::SND &&
           g_order[2] == CallKind::DIALOG,
       "SP: call ORDER is print_queue_text_id, then snd_play, then outcome_dialog");
    ck(g_print_calls.size() == 1 && g_print_calls[0] == TEXT_ID_SP_OUTCOME,
       "SP: ui_print_queue_text_id(0x7e)");
    ck(g_snd_calls.size() == 1 && g_snd_calls[0].id == SND_SP_OUTCOME_ID &&
           g_snd_calls[0].vol == SND_SP_OUTCOME_VOLUME,
       "SP: llm_snd_play(id=0xa6, vol=100)");
    ck(g_dialog_calls.size() == 1 && g_dialog_calls[0] == OUTCOME_DIALOG_KIND,
       "SP: llm_ui_outcome_dialog(4)");

    // ---- session_mode == 0 (uninitialised/none) -> guard fails, no calls at all. -------------------
    fx.reset();
    fx.session_mode = 0;
    run(fx);
    ck((uint32_t)g_order.size() == 0, "mode=0: no calls (guard requires exactly SESSION_SP)");

    // ---- session_mode == 2 and == 3 (MP session modes, incl. MP lockstep) -> also no-op. The guard is
    // an EQUALITY test against SESSION_SP specifically, not merely "nonzero" or "not MP" -- these two
    // values are deliberately adjacent to SESSION_SP(1) to catch an off-by-one in the compare.
    fx.reset();
    fx.session_mode = 2;
    run(fx);
    ck((uint32_t)g_order.size() == 0, "mode=2: no calls");
    fx.reset();
    fx.session_mode = 3;
    run(fx);
    ck((uint32_t)g_order.size() == 0, "mode=3: no calls");

    // ---- session_mode == -1 (a value the enum domain doesn't own) -> still just "not SP" -> no-op,
    // confirming the guard is a plain equality compare with no implicit truthiness/sign quirks.
    fx.reset();
    fx.session_mode = -1;
    run(fx);
    ck((uint32_t)g_order.size() == 0, "mode=-1: no calls");
}

} // namespace mh::sim::test
