//
// sim/libtrans/sim_lt_menu_teardown.cpp -- see sim_lt_menu_teardown.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_menu_force_return_to_main_004c862f.asm), not from the
// Ghidra .c draft beside it.
//
#include "sim/libtrans/sim_lt_menu_teardown.h"

#include "state/host_events.h"  // the one outward edge, now a SCREEN record (LIFT-SCREEN)
#include "addr/mh_export.gen.h" // mh::exp::addr_llm_ui_menu_bg_redraw_cb -- the stored callback's VA

namespace mh::sim {

const menu_teardown_calls &live_menu_teardown_calls() {
    static const menu_teardown_calls c = {
        mh::state::evt::dlg_build_from_table,
    };
    return c;
}

namespace detail {

void menu_force_return_to_main(sim_store &own, const menu_teardown_calls &c) {
    // 0x004c8647: forced-quit-teardown flag. Dword store of 1 -- LT-PREP retyped this region from
    // bool to int32_t for exactly this reason (a whole dword is written, not a byte).
    own.quit_teardown_forced_flag() = 1;

    // 0x004c8651: the active menu's widget-list pointer, cleared.
    own.ui_menu_widget_list() = nullptr;

    // 0x004c865b: _G_LLM_UI_MENU_ASYNC_CALLBACK -- the BASE slot of the 3-pointer family (BASE
    // @0x006542a9, _A @0x006542ad, _B @0x006542b1, untouched here). Cleared to null.
    // ui_menu_async_callback_base() hands back the raw `void **` slot address (an address-escape
    // accessor for a single pointer-wide region, not a per-record wrapper) -- write through it once.
    *own.ui_menu_async_callback_base() = nullptr;

    // 0x004c8665: _G_LLM_UI_MENU_ASYNC_CALLBACK_A -- the SECOND, DIFFERENT slot (+4 bytes from the
    // one just cleared above). Stores the ADDRESS of llm_ui_menu_bg_redraw_cb as a function-pointer
    // VALUE; it is never called through here (the accessor's own sim_state.h comment: "the store
    // only installs and clears it, never calls through it"). Uses the generated address constant
    // rather than a bare literal because, unlike sim_start_tutorial's llm_tutorial_menu_redraw_cb
    // precedent, this callee DOES have a committed prototype and therefore a real
    // mh_export.gen.h entry to cite.
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the VALUE stored here is an ORIGINAL FUNCTION ADDRESS, and standalone there is
    // no function at it. Null is the honest standalone content, not a placeholder: the slot's own
    // accessor comment records that "the store only installs and clears it, never calls through it",
    // and `nullptr` is the value the two lines above write to its sibling slots -- so a standalone
    // host reading this slot sees "no callback", which is true of a standalone host.
    //
    // WHY THIS IS NOT A DECLARED DIVERGENCE, measured rather than assumed: RID 408
    // (_G_LLM_UI_MENU_ASYNC_CALLBACK_A @0x006542ad) carries MF_VIEW only -- it is in no HASH_REGIONS
    // slice, so the two arms cannot differ in any lockstep or state hash. That is exactly the shape
    // the trap is about: a region no hash covers is one whose divergence no hash
    // oracle can see, so the arm that proves this equal has to be an explicit CONTENT check, never a
    // hash comparison. LIB-BOOT's `Progress` memcmp arm is the precedent.
    own.ui_menu_async_callback_a() = nullptr;
#else
    own.ui_menu_async_callback_a() = reinterpret_cast<void *>(mh::exp::addr_llm_ui_menu_bg_redraw_cb);
#endif

    // 0x004c866f-0x004c86ba: _G_LLM_DLG_STATE_FLAGS. Eight memory-touching instructions on this one
    // dword region (see the header banner for the full per-instruction breakdown and the byte/dword
    // access-width note) reduce to a single net effect: set bits 0, 1, 4, 5; clear bit 6; leave bits
    // 2, 3, 7 untouched -- i.e. `(flags & 0xffffffac) | 0x33`. Collapsed into one 32-bit
    // read-modify-write per house style; dlg_state_flags() is already int32_t&.
    own.dlg_state_flags() = (own.dlg_state_flags() & 0xffffffac) | 0x33;

    // 0x004c86c1: _G_LLM_UI_MENU_STATE, byte store of 3.
    own.ui_menu_state_mut() = 3;

    // 0x004c86c8-0x004c86cd: build the dialog from the fixed table at 0x00656d6a
    // (llm_ui_dlg_table_00656d6a -- a named Ghidra label with no mh_addrs.gen.h constant; see the
    // header's declared-need note). ORDER MATTERS: this call happens BEFORE the GAME_MODE store
    // below -- the builder is classified effectful and can read menu state that GAME_MODE=3 would
    // otherwise already reflect. Do not reorder.
    c.ui_dlg_build_from_table(LIBMH_SCR_DLGT_MAIN_MENU_QUIT); // was &llm_ui_dlg_table_00656d6a

    // 0x004c86d2: _G_LLM_GAME_MODE, byte store of 3 -- strategic/tactical mode selector, forced to
    // the main-menu value. Must follow the dialog build above (see the note there).
    own.game_mode() = 3;
}

} // namespace detail

void menu_force_return_to_main() {
    sim_state st = state();
    detail::menu_force_return_to_main(st.own, live_menu_teardown_calls());
}

} // namespace mh::sim
