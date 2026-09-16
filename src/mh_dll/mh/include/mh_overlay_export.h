#pragma once
//
// In-game debug overlay -- P0.
//
// Draws live engine/sim state straight onto the composed frame: a DLL-native RGB565 text blitter
// (embedded 6x10 ASCII font, overlay_font.h) writing into _G_LLM_FRAMEBUFFER from the shared
// present hook. Deliberately does NOT use the game's own text stack -- that is font-state dependent
// (every caller of llm_ui_text_draw_rgb16 is tactical-mode UI, so in menus/boot the overlay would be
// silently invisible exactly where it is most useful) and its strings are UTF-16.
//
// What is displayed is DATA, not code: `[debug]` in mh_net.ini declares pages over a registry of
// named value providers, so adding a readout later is registering one provider, not editing a layout.
//
// Read-only w.r.t. game state (reads globals, writes pixels -- never calls sim code, never consumes
// RNG), so it is determinism-neutral. Gated behind the `[debug]` ini section: no section, nothing
// installs.
//
#ifdef __cplusplus
extern "C" {
#endif

// A value provider: write the item's VALUE (not its label) into `buf`, at most `cap` bytes. Runs
// inside the present hook every frame, so it must be read-only, allocation-free and cheap.
typedef void (*MH_OverlayProviderFn)(char *buf, int cap);

// Register a provider under `name`, so an ini page can list it as an item. This is how a seam
// contributes its own readouts without gfx_overlay.cpp knowing anything about them -- the net/lockstep
// family lives in net_lockstep.cpp, which owns those addresses and the transport stats. Call from
// install, main thread; order w.r.t. MH_Overlay_Install does not matter (items resolve by name at
// paint time). A no-op if the table is full or the overlay never armed.
void MH_Overlay_RegisterProvider(const char *name, MH_OverlayProviderFn fn);

// Read [debug] config from mh_net.ini and enable. EN-only. Returns 1 if the overlay armed (i.e. a
// [debug] section exists), 0 otherwise. Does NOT install a hook -- the overlay piggybacks the
// lockstep present hook, like the capture/ui-drive seams.
int MH_Overlay_Install(void);

// Per-present callback: called from the lockstep present hook (net_lockstep on_present) at
// present-flip ENTRY -- frame fully composed, back buffer still locked. Polls the hotkeys, evaluates
// the mode gate and paints the active page. MUST run BEFORE MH_Capture_OnPresent so captured frames
// include the overlay (overlay constraint 3). Cheap when idle/disabled.
void MH_Overlay_OnPresent(void);

#ifdef __cplusplus
}
#endif
