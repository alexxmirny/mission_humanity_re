#pragma once
//
// mods:MM4 -- the big minimap overlay (seams/ui_bigmap.cpp). Mechanism in that file's head comment.
//
#ifdef __cplusplus
extern "C" {
#endif

// Called once per present from net_lockstep on_present, before the net indicator and the debug overlay.
// Reads [ui] bigmap / bigmap_center / bigmap_key / bigmap_dim on its first call; a no-op when off.
void MH_BigMap_OnPresent(void);

// Once at start-up (net_seams, after the map-zoom install): arms the click handler on the strategic pre-input
// detour and splices the draw in front of the stock cursor draw. Returns 1 when the splice took.
int MH_BigMap_Install(void);

#ifdef __cplusplus
}
#endif
