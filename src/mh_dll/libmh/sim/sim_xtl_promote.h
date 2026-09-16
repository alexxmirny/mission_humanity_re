#pragma once

namespace mh::sim {

// True once install_promotion_xtl() has installed at least one seam this process.
bool xtl_promotion_active();

// Install the 3 seams. `default_on` is the SHIPPING DEFAULT for `[promote] sim_xtl`, PASSED IN by
// the seams layer (mh/seams/reimpl_probe.cpp, SHIP_PROMOTE_SIM_XTL) rather than read from a header
// here: this module must not include a seams header (the layering lint keeps binary-bound seam code
// out of the reimplementation), so the one call site is where "what ships" is answerable.
// Returns the number of seams installed.
int install_promotion_xtl(int default_on);

} // namespace mh::sim
