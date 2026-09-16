//
// tact/tact_selection_clear_unless_ctrl.cpp -- see tact_selection_clear_unless_ctrl.h. Translated
// from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_selection_clear_unless_ctrl.h"


namespace mh::tact {
namespace detail {

void selection_clear_unless_ctrl(const tact_view &v, tact_store &own) {
    // @0x0042af85-0x0042afa9: no-op unless BOTH bits of the packed LCtrl keystate byte read zero
    // -- see the header banner. NEEDS a view member for _G_LLM_KEY_LCTRL_HELD@0x00e69d2d, which
    // does not exist yet (declared_needs).
    const bool lctrl_held = ((*v.key_lctrl_held & 0x1) != 0) || ((*v.key_lctrl_held & 0x2) != 0);
    if (lctrl_held) {
        return;
    }

    // @0x0042afaf-0x0042afe6: unconditionally clear the selection bit on every unit slot.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        tact_unit &u = own.unit_at(i);
        u.status &= 0xfe;
    }
}

} // namespace detail

void selection_clear_unless_ctrl() {
    tact_state st = state();
    detail::selection_clear_unless_ctrl(st.read, st.own);
}

} // namespace mh::tact
