//
// seams/strat_pre_input.h -- the ONE run-before detour on llm_strat_input_update (0x00441b88), shared.
//
// Several features need to act at the top of the strategic input pump, before the key ring is drained and
// before the mouse is pumped: the pause hotkey (ui_pause.cpp, which takes its key out of the ring) and the
// map zoom (ui_map_zoom.cpp, which has to move the camera and the zoom crop before the cursor's world tile
// is computed). The entry can carry one exclusive detour (hook/detour.h), so the detour lives here and the
// features register a function; the functions run in registration order, once per call of the pump.
//
#pragma once

namespace mh::seams {

using pre_input_fn = void (*)();

// Register `fn` (and arm the shared detour on the first call). `who` names the feature in the interlock's
// refusal line. Returns false -- with the refusal already logged by the detour primitive -- when the entry
// could not be taken; `fn` is then never called. At most 4 functions.
bool strat_pre_input_add(pre_input_fn fn, const char *who);

} // namespace mh::seams
