//
// sim_game_notify_system_available_selftest.cpp -- offline oracle for
//   llm_game_notify_system_available @0x004401a6 (sim/sim_game_notify_system_available.h/.cpp)
//
// WHY OFFLINE. A prior shadow arm of this function was found VACUOUS regardless of call count: void
// return, 0 compared regions (its one outward call writes only into the non-hashed text-scratch
// buffer, per the header's own "WHAT THIS DOES NOT TOUCH" section), so no number of live calls is
// equivalence evidence. This translated-and-reviewed function was never given execution evidence of
// any kind -- this file is that oracle: it drives detail::game_notify_system_available() directly
// against a fixture and inspects the one recorded w_sprintf__vss call.
//
// EVERY EXPECTED VALUE IS DERIVED FROM sim_game_notify_system_available.h's OWN BANNER: the 16-bit
// player-side compare, the format string "%s (%s)" @0x005009e8, G_TEXT_PTRS[0x77] as the first text
// arg (TEXT_ID_SYSTEM_LABEL), and -- the one real correctness question in this small function -- the
// CORRECTED define_index field: `system_define_index_base[system_idx*(0x8c/4) + 1]` (raw offset +4,
// .name/define_index), NOT the Ghidra .c draft's offset +0 (.invention) the header's "FIELD OFFSET
// BUG" section documents and explicitly rules out. Both the correct and the wrong field are seeded
// below with DISTINCT decoy text-pointer indices so a translation that regressed to the draft's wrong
// field would fail this test outright rather than merely coincide with it.
//
#include <cwchar>

#include "sim/sim_game_notify_system_available.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// DISTINCT, NON-DEFAULT, NON-SYMMETRIC seeds (sim_test_support.h's own rule).
constexpr uint16_t PLAYER_LOCAL  = 11;       // != fixture default player_side(0); set explicitly below
constexpr uint16_t PLAYER_REMOTE = 4;        // != PLAYER_LOCAL, != 0
constexpr int32_t  SYSTEM_IDX    = 5;        // an arbitrary non-zero system slot
constexpr int32_t  STRIDE_INTS   = 0x8c / 4; // 35 ints/record -- header's own derivation

// Text-pointer indices this function's own body/plate cites or that this test needs to tell apart:
constexpr int32_t TEXT_ID_SYSTEM_LABEL = 0x77; // G_TEXT_PTRS[0x77], the "System" label (per the plate)
constexpr int32_t TEXT_ID_CORRECT_NAME = 0x2a; // decoy standing in for the CORRECT (+4/.name) field
constexpr int32_t TEXT_ID_WRONG_NAME   = 0x99; // decoy standing in for the WRONG (+0/.invention) field
                                               // the Ghidra .c draft reads -- must never be selected

// ---- recorder --------------------------------------------------------------------------------------
// A captureless lambda converts to a plain function pointer, same shape as every other sim/ TU's
// `_calls` stub table (e.g. sim_prod_completion_selftest.cpp's g_pc/g_da/g_sa).
struct rec_t {
    int32_t        n_calls = 0;
    void          *dst     = nullptr;
    const wchar_t *fmt     = nullptr;
    const wchar_t *a0      = nullptr;
    const wchar_t *a1      = nullptr;
    void           reset() { *this = rec_t{}; }
};
rec_t g_rec;

const game_notify_system_available_calls &rec_calls() {
    static const game_notify_system_available_calls c = {
        [](void *dst, const wchar_t *fmt, const wchar_t *a0, const wchar_t *a1) -> int32_t {
            g_rec.n_calls++;
            g_rec.dst = dst;
            g_rec.fmt = fmt;
            g_rec.a0  = a0;
            g_rec.a1  = a1;
            return 0;
        },
    };
    return c;
}

// sim_view::system_define_index_base is NOT bound by sim_fixture::view() (sim_test_support.h) --
// this is the only translated function reading it so far -- so this test wires it itself by
// overwriting the field on the sim_view value view() returns, before calling detail:: directly (the
// same sanctioned pattern the fixture uses for every other region: real extents, out-of-range lands
// in owned memory). The System record for SYSTEM_IDX carries TWO DISTINCT values at its two leading
// int32 slots: +0 (.invention, the WRONG field the .c draft reads) and +1 int/+4 bytes (.name/
// define_index, the CORRECT field the disassembly reads) -- exactly the pair the header's "FIELD
// OFFSET BUG" section contrasts.
struct system_table {
    std::vector<int32_t> raw = std::vector<int32_t>((size_t)((SYSTEM_IDX + 1) * STRIDE_INTS));
    system_table() {
        raw[(size_t)(SYSTEM_IDX * STRIDE_INTS + 0)] = TEXT_ID_WRONG_NAME;   // .invention -- must be ignored
        raw[(size_t)(SYSTEM_IDX * STRIDE_INTS + 1)] = TEXT_ID_CORRECT_NAME; // .name/define_index -- must be read
    }
};

void test_local_player_formats_with_corrected_field() {
    sim_fixture f;
    g_rec.reset();
    f.player_side                     = (int16_t)PLAYER_LOCAL;
    f.text_ptrs[TEXT_ID_SYSTEM_LABEL] = L"System";
    f.text_ptrs[TEXT_ID_CORRECT_NAME] = L"CorrectSystemName";
    f.text_ptrs[TEXT_ID_WRONG_NAME]   = L"WrongInventionName";

    system_table sys;

    sim_view  v                = f.view();
    sim_store own              = f.store();
    v.system_define_index_base = sys.raw.data();

    detail::game_notify_system_available(v, own, rec_calls(), PLAYER_LOCAL, SYSTEM_IDX);

    ck_eq((uint32_t)g_rec.n_calls, 1u, "local player: w_sprintf__vss called exactly once");
    ck(g_rec.dst == (void *)f.text_scratch.data(),
       "local player: dst is the fixture's text-scratch buffer (G_TEXT_TMP)");
    ck(g_rec.fmt != nullptr && wcscmp(g_rec.fmt, L"%s (%s)") == 0,
       "local player: format string is \"%s (%s)\" (@0x005009e8, per the header)");
    ck(g_rec.a0 == f.text_ptrs[TEXT_ID_SYSTEM_LABEL],
       "local player: first text arg is G_TEXT_PTRS[0x77] (the \"System\" label)");
    ck(g_rec.a1 == f.text_ptrs[TEXT_ID_CORRECT_NAME],
       "local player: second text arg is G_TEXT_PTRS[define_index] read from the CORRECTED +4 field "
       "(.name) -- proves the fix over the Ghidra .c draft's wrong +0 (.invention) read");
    ck(g_rec.a1 != f.text_ptrs[TEXT_ID_WRONG_NAME],
       "local player: second text arg is NOT G_TEXT_PTRS[the .invention-derived index] -- the "
       "regression guard for the FIELD OFFSET BUG");
}

void test_non_local_player_is_a_no_op() {
    sim_fixture f;
    g_rec.reset();
    f.player_side                     = (int16_t)PLAYER_LOCAL;
    f.text_ptrs[TEXT_ID_SYSTEM_LABEL] = L"System";
    f.text_ptrs[TEXT_ID_CORRECT_NAME] = L"CorrectSystemName";

    system_table sys;

    sim_view  v                = f.view();
    sim_store own              = f.store();
    v.system_define_index_base = sys.raw.data();

    detail::game_notify_system_available(v, own, rec_calls(), PLAYER_REMOTE, SYSTEM_IDX);

    ck_eq((uint32_t)g_rec.n_calls, 0u,
          "non-local player (PLAYER_REMOTE != PlayerSide): zero w_sprintf__vss calls -- the else "
          "branch is a pure no-op per the header's own body dump");
}

} // namespace

void run_game_notify_system_available_tests() {
    test_local_player_formats_with_corrected_field();
    test_non_local_player_is_a_no_op();
}

} // namespace mh::sim::test
