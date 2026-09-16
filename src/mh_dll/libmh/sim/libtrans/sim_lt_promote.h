#pragma once

namespace mh::sim {

// True once install_promotion_lib_trans has installed at least one of the 43 seams this file owns.
// Mirrors mh::sim::resid_promotion_active() / mh::orders::promotion_active()'s shape. Does NOT reflect
// the frame pair -- that is mh::sim::lt_frame_promotion_active() (sim/libtrans/sim_lt_frame.h), a
// separate flag for a separately-gated installer.
bool lib_trans_promotion_active();

// Reads `[promote] lib_trans` (falling back to `default_on` when the key or the file is absent) and,
// if armed, installs all 43 verified lib_trans seams this file owns (skipping any
// `[promote_skip] <orig_name>=1` row; NEVER touching the frame pair -- see the header banner, that
// pair's own installer is called separately from net_lockstep.cpp). Returns the number actually
// installed. Logs a REFUSED line per failed install (naming the owning detour via
// `mh::hook::entry_owner_of` when one holds the entry, else the flat "entry guard mismatch" message --
// time_GetCurrentTime is the row expected to take the owned-entry branch under `[harness]
// pin_wallclock=1`, see the header banner), a PARTIAL warning if the installed count falls short of
// the attempted count, and on full success a summary line naming the domain and the installed count,
// followed by one `; [promote] lib_trans: + <orig_name>` line per installed row -- the promoted set is
// named, not just counted.
int install_promotion_lib_trans(int default_on);

} // namespace mh::sim
