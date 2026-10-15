//
// seams/bldg_pip_guard.h -- hotfix rc9.4 `[net] bldg_pip_guard`: the retail carrier (bldg_pip_guard.cpp).
//
#pragma once

// Splice the two guard sites (building_tick 0x0046fe67, update_charge_pips 0x00478ecb) when `on`; log one boot
// line either way. Per site: PATCHED, DISPLACED by promotion (libmh's twin carries it) or MISMATCHED (refused).
void install_bldg_pip_guard(bool on);
