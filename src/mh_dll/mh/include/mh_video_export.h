#pragma once
//
// Display-mode (resolution) selection -- D13.
//
// mh.exe already supports three render resolutions (llm_view_set_size_mode: 0 = 640x480,
// 1 = 800x576, 2 = 1024x768), already exposes the choice as a widget in the in-game options menu,
// and already PERSISTS it as the first int of the 0x58-byte settings block in setup.dat. What it
// does not offer is a way to pin the mode from OUTSIDE the game -- that block is LZW-compressed, so
// a test rig (or a mod that wants to ship a resolution) cannot just edit the file.
//
// [video] size_mode=N in mh_net.ini fills that gap: a run-before seam on the main-menu loader writes
// _G_LLM_VIEW_SIZE_MODE just before the loader caches it, which is bit-for-bit the same state the
// game would be in had the user picked mode N in the options menu on a previous run. Nothing else is
// forced: the options menu can still change it, and the value still round-trips to setup.dat.
//
#ifdef __cplusplus
extern "C" {
#endif

// PROCESS ATTRIBUTES ONLY -- currently DPI awareness. Split out of MH_Video_Install and called
// EARLY, before MH_Seam_Init's `[net] enable=0` gate, because neither of the two things that make it
// correct has anything to do with networking:
//   * it must run before ANY window exists (SetProcessDPIAware's requirement), and
//   * a build with the MP seams switched off still renders, so it still needs DPI awareness.
// Previously it rode along inside MH_Video_Install, which sits well after that gate -- so
// `[net] enable=0` silently gave back the blurry, compositor-stretched window that the HIGHDPIAWARE
// shim used to paper over (found 2026-07-28 while standing up parallel test lanes, whose ini sets
// enable=0). Idempotent and safe to call more than once.
void MH_Video_ApplyProcessAttrs(void);

// Read [video] from mh_net.ini and, if size_mode is 0..2, arm the main-menu run-before seam.
// Returns 1 when armed, 0 when the section is absent/out of range or the build is not EN.
int MH_Video_Install(void);

#ifdef __cplusplus
}
#endif
