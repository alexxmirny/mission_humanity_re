#pragma once
//
// mh_core_arm_paths.h -- the per-run paths mh.dll composes and the harness reads (fork F4E).
//
// build_paths() lives in mh/seams/core_arm.cpp and runs as the first statement of
// MH_Core_Arm_Early, before anything can resolve a region. It composes one ini path (INPUT, beside
// the exe) and eight per-run output paths (OUTPUT, inside MH_RunDir()'s timestamped folder). Until
// F4E every reader of those strings was in the same translation unit; now mh_harness.dll is a
// separate image with ~350 uses of the same nine names.
//
// THE ALTERNATIVE WAS TO COMPOSE THEM TWICE, AND IT IS THE ONE THING THIS HEADER EXISTS TO PREVENT.
// MH_RunDir() is self-initialising and the composition is four lines, so a second image could
// trivially build its own copies -- and they would agree, until the day one of them did not. A run
// folder is a decision (ruling Q1: run_context.cpp stays mh.dll-side precisely so exactly one place
// in the process knows where this run's logs live), and two derivations of a decision is two
// decisions. So mh.dll composes, and the harness COPIES at MH_Harness_Init -- after
// MH_Core_Arm_Early has filled them, which DllMain guarantees by calling the two back to back.
//
// BORROWED POINTERS, VALID FOR THE RUN. They point into mh.dll's own statics. The harness copies the
// bytes into its own arrays rather than holding these pointers: two static CRTs mean two heaps, and
// the standing rule for every cross-image row in this project is that nothing crosses owning memory
// (docs/dll-split.md, F4D's "two static CRTs" note).
//
#ifndef MH_CORE_ARM_PATHS_H
#define MH_CORE_ARM_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_CoreArmPaths {
    unsigned    size;          /* sizeof(MH_CoreArmPaths) as MH.DLL was compiled                  */
    const char *exe_dir;       /* "...\\" -- the exe's directory, trailing separator kept          */
    const char *ini_path;      /* "<exedir>mh_net.ini" -- the one config INPUT                     */
    const char *log_path;      /* "<rundir>mh_harness.log"                                        */
    const char *seed_in;       /* "<exedir>mh_harness_seed.bin"  (inject)                         */
    const char *seed_out;      /* "<rundir>mh_harness_seed.bin"  (dump)                           */
    const char *boot_snap_out; /* "<rundir>mh_boot_snapshot.bin"                                  */
    const char *world_out;     /* "<rundir>mh_world.bin"                                          */
    const char *orders_in;     /* "<exedir>mh_orders.bin"  (replay)                               */
    const char *orders_out;    /* "<rundir>mh_orders.bin"  (record)                               */
    const char *clock_in;      /* "<exedir>mh_clock.bin"   (replay)                               */
    const char *clock_out;     /* "<rundir>mh_clock.bin"   (record)                               */
} MH_CoreArmPaths;

/* Never null, and every field is filled once MH_Core_Arm_Early has run. Called before that, the
 * strings are empty rather than garbage (the arrays are zero-initialised .bss) -- which is the
 * honest answer, because before the core arm there IS no run folder. */
const MH_CoreArmPaths *MH_Core_ArmPaths(void);

/* LIB-REF-IN: mh::libmh_in::trap_count() at the instant libmh_in_open() returned inside
 * MH_Core_Arm_Early. The harness reports refusals SINCE open, and the baseline is what makes that
 * number mean something: this arm legitimately runs pre-open boot frames, so a raw count is not a
 * defect while a refusal after open is. */
int MH_Core_TrapsAtOpen(void);

#ifdef __cplusplus
}
#endif

#endif /* MH_CORE_ARM_PATHS_H */
