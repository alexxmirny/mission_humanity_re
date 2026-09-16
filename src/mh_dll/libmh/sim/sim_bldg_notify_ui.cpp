//
// sim/sim_bldg_notify_ui.cpp -- see sim_bldg_notify_ui.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_notify_ui_00470bdd.asm). The Ghidra .c draft happens to match the
// assembly branch-for-branch here (unlike sim_unit_tick's draft, which got two FP boundaries
// backwards) -- read and cross-checked anyway, not trusted on sight, per the translator brief.
//
#include "sim/sim_bldg_notify_ui.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_notify_ui_calls &live_bldg_notify_ui_calls() {
    static const bldg_notify_ui_calls c = {
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// game::e::event member 7 (the strategic-sim notes' 25-member Ghidra enum) -- BUILD_PROJECTS_REFRESH.
// Kept FILE-LOCAL (anonymous namespace, not `mh::sim` scope) deliberately: sim_unit_population_
// remove.h already declares an identically-named, identically-valued `inline constexpr uint32_t
// EVENT_BUILD_PROJECTS_REFRESH = 7;` at mh::sim namespace scope, and mh/seams/reimpl_probe.cpp
// includes BOTH that header and this TU's own header in one translation unit -- a second
// namespace-scope declaration of the same name would be a hard redefinition error there, the exact
// failure mode sim_event_codes.h's own banner documents for SESSION_SP/EVENT_INFO_REFRESH. See this
// file's declared_needs: the real fix is hoisting EVENT_BUILD_PROJECTS_REFRESH into
// sim_event_codes.h (removing sim_unit_population_remove.h's copy), which this translation cannot do
// itself (shared headers are conductor-owned).
constexpr uint32_t kEventBuildProjectsRefresh = 7;

} // namespace

namespace detail {

// ---- llm_strat_bldg_notify_ui @0x00470bdd --------------------------------------------------------
//
// Two MUTUALLY EXCLUSIVE arms (the .asm's first arm JMPs straight past the second at 0x00470c26) --
// see the header SHAPE notes for the full derivation; kept as two separate branches per the task
// hazard note, not merged into one `||` condition, so the observable game_SetEvent CALL COUNT stays
// identical to the original under the shadow oracle.
void bldg_notify_ui(const sim_view &v, sim_store &own, const bldg_notify_ui_calls &c, uint16_t player,
                    uint32_t b_index) {
    // 16-bit bit-pattern compare (CMP AX, word ptr PlayerSide), same idiom sim_unit_notify.cpp /
    // sim_unit_population_remove.cpp already use for this exact global.
    if (static_cast<int16_t>(player) == *v.player_side) {
        // 0x00470c06 -- one MOV dword ptr, setting both contiguous int16_t fields at once (see header
        // SHAPE note).
        own.change_flag()  = 1;
        own.change_flag2() = 0;

        // 0x00470c10-0x00470c1a: 32-bit compare, the 16-bit UI_SELECTED_BLDG_INDEX global
        // zero-extended against the full 32-bit b_Index parameter -- ordinary unsigned promotion
        // reproduces this without an explicit cast, but the cast is written anyway to keep the width
        // change visible at the call site (translator brief: integer width is semantic).
        if (static_cast<uint32_t>(own.ui_selected_bldg_index()) == b_index) {
            c.set_event(kEventBuildProjectsRefresh);
        }
        return;
    }

    // else: a COMPLETELY SEPARATE condition (0x00470c28-0x00470c49) on the click-select target pair.
    // (player & 0xffff) | 0x40, compared against the 16-bit flags word zero-extended to 32 bits --
    // matches the .asm's `OR AL,0x40` (low byte only) followed by `MOVZX EAX,AX` (which folds AH,
    // i.e. bits 8-15 of player, back in unchanged); player is already uint16_t here so the `& 0xffffu`
    // is a no-op kept for the same documentation reason sim_unit_notify.cpp's analogous compare keeps
    // it. click_select_target_id is READ (never written) through the existing mutable-only accessor,
    // same established precedent as sim_unit_notify.cpp / sim_unit_on_destroyed.cpp.
    if (static_cast<uint16_t>((static_cast<uint32_t>(player) & 0xffffu) | 0x40u) ==
            *v.click_select_target_flags &&
        own.click_select_target_id() == b_index) {
        c.set_event(kEventBuildProjectsRefresh);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_notify_ui(uint16_t player, uint32_t b_index) {
    sim_state st = state();
    detail::bldg_notify_ui(st.read, st.own, live_bldg_notify_ui_calls(), player, b_index);
}


} // namespace mh::sim
