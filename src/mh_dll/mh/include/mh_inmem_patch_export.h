#pragma once
//
// F1E -- the in-DLL static-patch spike (the fork plan supplementary ruling).
//
// The src/patcher manifests are applied today by mhpatch.py to the exe ON DISK. In the fork the
// distribution is a stock retail exe plus mh.dll, so the same manifests have to be carried in the
// DLL and applied to the LOADED image, with VirtualAlloc standing in for the appended section cave.
// This is the arm point for that; the mechanism is mh/patch/inmem_patch.h.
//
// DEFAULT OFF, and it must stay that way until F4: the one compiled-in manifest (no_cd_EN) already
// has a BEHAVIOURAL twin in seams/standalone.cpp, so arming both in the same run is redundant, and
// a run with neither is exactly what ships today.
//
//   [patch] inmem=1          arm the applier (default 0 -- nothing is read, nothing is written)
//   [patch] dump=<path>      after applying, write the parity dump the checker reads (default none)
//   [patch] dump_exit=1      terminate right after the dump -- the headless parity run (default 0)
//
#ifdef __cplusplus
extern "C" {
#endif

// Returns a bitmask. 0 means the spike is disarmed (the shipping state) OR the applier refused; the
// two are distinguished in the log, and by MH_InMemPatch_Refusal below.
#define MH_INMEM_ARMED   1 // [patch] inmem=1 was read
#define MH_INMEM_APPLIED 2 // the manifest went in
#define MH_INMEM_DUMPED  4 // the parity dump was written
int MH_InMemPatch_Install(const char *ini_path);

// Why nothing was applied, or nullptr. Exposed so a caller can report it without re-deriving it.
const char *MH_InMemPatch_Refusal(void);

#ifdef __cplusplus
}
#endif
