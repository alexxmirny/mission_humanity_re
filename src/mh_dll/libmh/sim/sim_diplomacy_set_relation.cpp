//
// sim/sim_diplomacy_set_relation.cpp -- see sim_diplomacy_set_relation.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_diplomacy_set_relation_0049a0b3.asm); the Ghidra decompile's four-way
// `if`/`goto` cascade is confirmed correct branch-for-branch, and is collapsed here into an
// equivalent three-way `if`/`else if`/`else` (see the comment on the branch below) rather than
// transcribed as a goto chain.
//
#include "sim/sim_diplomacy_set_relation.h"

#include "addr/mh_calls.gen.h"
#include "lockstep/lt_chat_ally_mask.h" // the rebound mask rebuild  // typed callables for the original functions we still call OUT to
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

namespace {
// The wide-string format literals are not bound via the addr manifest (established convention --
// see sim_bldg_state_destroyed.cpp / sim_debug_roll_random.cpp): local constexpr transcribed from
// the Ghidra symbol names / EOL comments in the .asm. The second and third format strings are two
// DISTINCT storage locations that both happen to hold the literal text "%s" (0x50170e / 0x501726) --
// kept as two separate named constants per this module's distinct-storage-location convention, not
// coalesced into one shared string.
constexpr const wchar_t *TEXT_FMT_HEADER   = L"%d (%s) %d (%s)"; // u_%d_(%s)_%d_(%s)_005016c8
constexpr const wchar_t *TEXT_FMT_S_50170E = L"%s";              // u_%s_0050170e
constexpr const wchar_t *TEXT_FMT_S_501726 = L"%s";              // u_%s_00501726

} // namespace

const diplomacy_set_relation_calls &live_diplomacy_set_relation_calls() {
    static const diplomacy_set_relation_calls c = {
        mh::host().ansi_to_wide_scratch,
        MH_CRT(w_sprintf__visis),
        MH_CRT(w_sprintf__vs),
        MH_LIBMH_BIND(llm_diplomacy_ai_relation_swap),
        &mh::lockstep::chat_ally_mask_rebuild, // REBOUND 2026-09-02: translated (LT1D d7), ours binds directly
    };
    return c;
}

namespace detail {

void diplomacy_set_relation(const sim_view &v, sim_store &own, const diplomacy_set_relation_calls &c,
                            int32_t player_a, int32_t player_b, uint8_t relation) {
    // 0x0049a0d2-0x0049a0dc: Players[player_a].relation[player_b] = relation. Write-only -- this
    // function never reads the old value.
    own.player_relation_at((uint32_t)player_a, (uint32_t)player_b) = relation;

    // 0x0049a0e2-0x0049a117: convert both players' ANSI names to the shared wide scratch buffer.
    // Original call order is player_b's name FIRST, then player_a's name -- both results are held
    // live (in registers, in the original) across both calls before either is consumed, so the call
    // order itself has no observable effect here, but it is reproduced anyway for fidelity with the
    // disassembly.
    // llm_str_ansi_to_wide_scratch's committed signature takes non-const `char*` (Ghidra never marks
    // it const) and returns `void*`; the converter only reads its input, so the const_cast is a
    // calling-convention formality, not a hazard -- kept at the call site so the `_calls` struct
    // binds mh::call:: directly (no wrapper).
    const wchar_t *name_b_wide = (const wchar_t *)c.ansi_to_wide_scratch(
        const_cast<char *>(v.profiles[player_b].name));
    const wchar_t *name_a_wide = (const wchar_t *)c.ansi_to_wide_scratch(
        const_cast<char *>(v.profiles[player_a].name));

    // 0x0049a117-0x0049a12d: w_sprintf(G_TEXT_TMP, "%d (%s) %d (%s)", player_a, name_a, player_b,
    // name_b) -- "player_a (name_a) player_b (name_b)".
    c.w_sprintf_visis(own.text_scratch(), TEXT_FMT_HEADER, player_a, name_a_wide, player_b,
                      name_b_wide);

    // 0x0049a130-0x0049a1b3: derive the relation SIGN passed to llm_diplomacy_ai_relation_swap, and
    // append a phrase over the header line just written (w_sprintf(G_TEXT_TMP, "%s", G_TEXT_TMP) is
    // self-referential -- it re-reads and rewrites the same buffer).
    //
    // The disassembly's actual shape is a 4-way cascade: relation==0 jumps DIRECTLY to the
    // relation>2 fallback block (0x0049a192); relation==1 and relation==2 each get their own block;
    // relation>2 falls through the same cascade into the same fallback block relation==0 jumped to.
    // Two of the four disassembly blocks are byte-identical (fallback and relation==0 both: sign=0,
    // format u_%s_0050170e) purely because they share a jump target -- collapsing that shared target
    // into one `else` arm below changes nothing observable for any relation value 0..255.
    int32_t relation_sign;
    if (relation == 1) {
        relation_sign = 1;
        c.w_sprintf_vs(own.text_scratch(), TEXT_FMT_S_50170E, own.text_scratch());
    } else if (relation == 2) {
        relation_sign = -1;
        c.w_sprintf_vs(own.text_scratch(), TEXT_FMT_S_501726, own.text_scratch());
    } else { // relation == 0, or relation > 2
        relation_sign = 0;
        c.w_sprintf_vs(own.text_scratch(), TEXT_FMT_S_50170E, own.text_scratch());
    }

    // 0x0049a1b3-0x0049a1c6: EFFECTFUL callees (the sim closure's seam table) -- routed through
    // `_calls` like everything else; whether/how this site is armed is the conductor's decision.
    c.diplomacy_ai_relation_swap(player_a, player_b, relation_sign);
    c.net_chat_ally_mask_rebuild();
}

} // namespace detail

void diplomacy_set_relation(int32_t player_a, int32_t player_b, uint8_t relation) {
    sim_state st = state();
    detail::diplomacy_set_relation(st.read, st.own, live_diplomacy_set_relation_calls(), player_a,
                                   player_b, relation);
}

} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
