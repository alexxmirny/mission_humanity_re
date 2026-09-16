#pragma once
//
// UI capture harness -- Phase 1: rendered-frame capture.
//
// Hooks llm_gfx_present_flip (the shared "push composed frame to screen" primitive) and, on a
// trigger, dumps the locked RGB565 back-buffer (_G_LLM_FRAMEBUFFER) to a 24-bit BMP under the run
// log dir. Trigger (prototype): the F12 hotkey (rising edge) + [capture] frames=N ini burst.
// Convert the BMP -> PNG with tools/bmp_to_png.py. Read-only w.r.t. game state; gate it behind the
// [capture] ini so normal runs are untouched.
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [capture] config from mh_net.ini (frames=N first-N burst, every=K periodic) + enable. EN-only.
// Returns 1 if enabled. Does NOT install a hook -- capture piggybacks on the lockstep present hook.
int MH_Capture_Install(void);

// Per-present callback: called from the lockstep present hook (net_lockstep on_present) at present-flip
// ENTRY (frame composed incl cursor, buffer still locked). Checks the trigger + grabs a frame. No-op
// unless a trigger fires. Cheap when idle.
void MH_Capture_OnPresent(void);

// On-demand named capture: dump the current frame to capture_<name>.bmp. Used by the UI-script
// interpreter (ui_drive.cpp) to grab a labelled shot at each screen. MUST be called at present-flip
// ENTRY (i.e. from the on_present path) so the back-buffer is still locked.
void MH_Capture_Shot(const char *name);

#ifdef __cplusplus
}
#endif
