#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this body makes, indirected so detail:: stays testable under simtest
// (translator-brief rule 3b: llm_ui_dlg_build_from_table is a frontier original, not an in-tree
// sibling, so it is NOT called through mh::call:: directly).
struct menu_teardown_calls {
    void (*ui_dlg_build_from_table)(int32_t table_id); // LIBMH_EVK_SCR_DLG_FROM_TABLE (was
                                                       // llm_ui_dlg_build_from_table @0x004c7941)
};

const menu_teardown_calls &live_menu_teardown_calls();

namespace detail {

// llm_menu_force_return_to_main @0x004c862f. Reads nothing (no sim_view parameter needed); writes
// seven UI/menu/dialog regions through `own` in the order the header banner lists, then calls the
// dialog builder through `c` BEFORE the final GAME_MODE store (order is load-bearing -- see the
// header's ordering note).
void menu_force_return_to_main(sim_store &own, const menu_teardown_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().own and live_menu_teardown_calls(). Matches the
// original's committed `void __watcall llm_menu_force_return_to_main(void)` signature -- no
// parameters, no return value.
void menu_force_return_to_main();

} // namespace mh::sim
