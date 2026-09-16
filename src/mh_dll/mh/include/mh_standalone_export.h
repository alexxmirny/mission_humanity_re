#pragma once
//
// Run from a STOCK, unmodified mh.exe (I4 + I5).
//
// These are the runtime twins of the two static code manifests that `build_focus` applies today:
//
//   src/patcher/no_cd_EN.mh.patch.json             -> MH_Standalone_Install's no-CD fallback
//   src/patcher/run_without_focus_EN.mh.patch.json -> MH_Standalone_Install's focus patch
//
// Once mh.dll is loaded by the proxy shim rather than by added-import surgery, these two are all
// that stood between "the game boots from a byte-for-byte untouched retail exe" and "it doesn't".
// Doing them from here also removes the per-chain-position fragility the cave manifest has (its
// section VA and its `jmp` back are baked for ONE input layout -- gen_no_cd_manifest.py LAYOUT).
//
// Both are idempotent against an exe that ALREADY carries the static patch: the no-CD fallback sees
// a non-empty buffer and does nothing, and the focus patch's expected-bytes guard mismatches (the
// site already holds the JMP) and stays inert. So the same DLL serves stock and patched exes.
//
// --- the no-CD fallback ---
//
// SAME SEMANTICS AS THE CAVE, deliberately -- "fallback only", never a bypass:
//   * the drive scan runs UNCHANGED. A real MH-labelled disc still sets the real CD path and opens
//     CD audio exactly like retail; we never see it, because the buffer comes back non-empty.
//   * only the scan-exhausted outcome is touched: G_CD_DATA_PATH (0x00603f78) is filled with the
//     EXE'S OWN DIRECTORY, which is what llm_cd_ensure_present loops on (locate() returns 0 for
//     BOTH "found" and "no disc", so the buffer is the signal -- the disc-check RE).
//   * module-derived, not CWD-derived, for the same reason the cave is: a launcher with the wrong
//     working directory must not be able to resurrect the insert-CD modal.
//
// WHY A WRAP DETOUR rather than replacing llm_cd_ensure_present: ensure_present ZEROES the buffer at
// entry (0x004c3921) before calling locate, so nothing written earlier -- from DllMain, say -- can
// survive. The fill has to land after locate runs and before ensure_present reads the buffer, which
// is exactly the seam the cave uses.
//
#ifdef __cplusplus
extern "C" {
#endif

// Install both. Returns a bitmask: 1 = no-CD fallback hooked, 2 = focus patch written. A zero bit
// means that half stayed inert (prologue/expected-bytes guard failed, or the exe already carries the
// static patch) -- never that something was corrupted. Call once from MH_Seam_Init.
#define MH_STANDALONE_NO_CD     1
#define MH_STANDALONE_FOCUS     2
#define MH_STANDALONE_DEVCHANGE 4
int MH_Standalone_Install(void);

// U21: the WM_DEVICECHANGE NULL-lParam guard, and the probe that proves it.
//
// `guard` (default on) installs a run-before detour on llm_wnd_on_devicechange that returns 0
// when lParam is NULL instead of dereferencing it. That handler is the disc-removal half of the
// copy protection this file already deals with, which is why it lives here.
//
// `probe_ms` > 0 posts ONE malformed WM_DEVICECHANGE (wParam=DBT_DEVNODES_CHANGED, lParam=0) to
// the game's own window that many milliseconds after install -- exactly the message Windows sends
// on any device-node change. With the guard off it reproduces the crash on demand; with it on the
// game must survive. A bug with no reliable reproduction is a bug nobody can prove they fixed.
//
// Split from MH_Standalone_Install because the ini lives in net_seams and this file has no
// plumbing to it -- the caller reads the two keys and passes them in.
int MH_Standalone_InstallDevChangeGuard(int guard, int probe_ms);

// How many malformed broadcasts the guard has eaten. Zero after a run means the guard never had to
// do anything -- which is a different statement from "the guard is not installed", and the arm
// banner is what distinguishes them.
long MH_Standalone_DevChangeSwallowed(void);

// The no-CD fallback itself, exposed for the offline selftest: if `path_buf` is empty, fill it with
// the module directory (trailing backslash), else leave it alone. Returns 1 if it wrote.
int MH_Standalone_FillCdPath(char *path_buf, unsigned buf_size);

#ifdef __cplusplus
}
#endif
