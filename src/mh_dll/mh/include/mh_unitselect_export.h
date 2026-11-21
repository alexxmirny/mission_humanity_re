#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Installs two guarded call-site wrappers, never the native drag-selection body.
// Returns 1 only when both wrappers are armed; repeated successful calls return 1.
int MH_UnitSelect_Install(void);
// Session/planet/load/replay teardown clears local gesture history, not the selection.
void MH_UnitSelect_Reset(void);

#ifdef __cplusplus
}
#endif
