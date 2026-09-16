#pragma once
//
// mh_harness_module.h -- THE mh.dll <-> mh_harness.dll CONTRACT (fork F4E).
//
// mh_harness_export.h declares WHAT the determinism harness does. This header declares HOW it is
// reached once it stops being compiled into mh.dll: the thirteen symbols mh.dll resolves with
// GetProcAddress, the value each answers when the instrument is not there, and the two module-level
// entries (init + probe) that are not part of the harness API at all.
//
// It is the third of these, and it is deliberately the same shape as mh_net_module.h's (F4B) rather
// than gen_libmh_contract.py's (F4D). Twenty-three rows were small enough to carry as an X-macro a
// human reads and a hundred were not; THIRTEEN is firmly on the readable side, AND -- the deciding
// reason -- every row's absent value here is a SEMANTIC decision that has to be written down
// somewhere. A generator that derives the list cannot derive those, so the list lives beside them.
// The derivation still happens: tools/gen_harness_contract.py re-measures mh.dll's undefined
// externals against the harness's objects and REFUSES if this list is not exactly that set, so a
// FOURTEENTH symbol cannot arrive unnoticed the way a G106 hand list lets one. (The thirteenth did
// arrive, at F5J, and the derivation is what made adding it a mechanical step rather than a memory
// test -- which is the whole claim.)
//
// ---- WHY ABSENCE IS SILENT HERE AND LOUD ONE LEVEL DOWN -------------------------------------------
//
// Three satellites, three meanings of "not there" (docs/dll-split.md's inheritance table):
//
//   mh_net.dll   absent = a real DEGRADATION the player can see -- no transport, and the browser
//                says why.
//   libmh.dll    absent = CONFIGURATION (1) -- the game runs the original binary's own bodies.
//                Nothing is lost because nothing was ever promoted.
//   mh_harness   absent = UNINSTRUMENTED. Nothing about what the game DOES changes: the harness
//                installs detours that hash state and records journals, and a run without it is the
//                run every player has always had. So every row below answers exactly what the real
//                body answers with no `[harness] enable=1` -- which is not a stub of convenience but
//                the value the un-armed instrument already returned, so an absent module and a
//                disarmed one are indistinguishable to every caller.
//
// THE ONE CASE THAT IS NOT SILENT is `[harness] enable=1` with no mh_harness.dll beside mh.dll: an
// operator who configured an instrument and will not get one. That is refused LOUDLY by the bind
// (mh/seams/harness_bind.cpp), on all three channels, because a determinism run whose instrument
// silently never installed is the worst outcome this project knows (G178). It is refused rather than
// fatal: the harness installs nothing, so its absence cannot change which bodies run, and every
// consumer of mh_harness.log reds on a log that is not there.
//
// ---- THE ABSENT VALUES, AND WHY EACH IS THE UN-ARMED BODY'S OWN ANSWER ----------------------------
//
//   MH_Harness_Init            0   "Returns 1 when the harness armed"; without `enable=1` the real
//                                  body returns 0 from its first statement.
//   MH_Harness_LateArm         -   no-op. "No-op when no harness is armed" is already its contract.
//   MH_Harness_OnPresent       -   no-op; called unconditionally every present, and the real body is
//                                  "cheap when idle (one branch) and inert without an armed harness".
//   MH_Harness_OnSimTick       -   no-op; runs the fixed-timestep pin ONLY when the sim_tick detour
//                                  armed this run, so a direct-calling frame body stays
//                                  byte-equivalent to the entry path in every configuration.
//   MH_Harness_OnMovieTick     -   no-op, for the same reason one anchor over.
//   MH_Harness_Rebind*    (4)  0   "0 if the harness is not armed in this process, in which case
//                                  nobody owns the entry and an ordinary entry install is the
//                                  correct route" -- so the promotion installer takes the entry
//                                  itself, which is exactly right with no instrument contending.
//   MH_Harness_Wants*     (3)  0   three questions about what an armed harness would need: the
//                                  present tick, the movie tick, and whether the wall-clock pin will
//                                  own time_GetCurrentTime's entry. No harness, no requirement.
//   MH_Harness_StepFence      -1   the sim STEP counter is the instrument's, and -1 is what the real
//                                  body answers when its sim_step detour never installed. NOT 0:
//                                  zero is a legitimate step count (a peer still in the menu), so a
//                                  zero absent value would read as "armed, at step 0" and ui_drive's
//                                  `simstep` would wait out its watchdog instead of refusing by name.
//                                  This is the one row whose absent value is a SENTINEL rather than
//                                  the quiet answer, and it is because the caller is a test script
//                                  whose whole failure mode is a predicate nothing can satisfy.
//
// NOT A ROW, AND THE REASON IS THE FILE CUT: MH_Harness_ReportRelocation. It reports the relocating
// state bind, which happens inside MH_Core_Arm_Early and therefore inside mh.dll -- so at F4E it
// moved to mh/seams/core_arm.cpp with the arena and the report it reads, and stopped crossing the
// boundary. F4E's surface measurement predicted thirteen crossing rows and it was TWELVE, because
// one piece of code went to the side that owns its data (R9). The list is thirteen again today for
// an unrelated reason -- F5J's MH_Harness_StepFence -- so do not read the count as that finding.
//
#ifndef MH_HARNESS_MODULE_H
#define MH_HARNESS_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything in this header or mh_harness_export.h changes shape. mh.dll REFUSES a
// module whose ABI disagrees rather than calling into it: two DLLs that ship separately can be
// mismatched by a player copying one file, and a silent mismatch is a crash in somebody else's
// stack frame. Same discipline as MH_NET_MODULE_ABI and LIBMH_MODULE_ABI.
#define MH_HARNESS_MODULE_ABI 0xF4E00002u

// What mh.dll hands the instrument at bind time. THREE FIELDS BESIDES THE HANDSHAKE, and the last
// two are the whole of Q4's refusal.
//
// `spine_bound` says whether libmh.dll is in this process. The harness binds its own spine rows out
// of libmh directly (GetModuleHandle, never LoadLibrary -- video.cpp's rule, adopted verbatim by
// docs/dll-split.md for exactly this edge), and this field is the same fact stated by the side that
// did the loading, so the two cannot disagree about what configuration the run is in.
//
// `configured` decides whether a failed spine bind is SHOUTED or merely recorded, and it is here
// because the difference is the whole point. Configuration (1) is a SHIPPED configuration: a player
// with no libmh.dll and no `[harness] enable=1` is having a perfectly ordinary evening, and writing
// mh_harness_refused.log into their game folder would be this project shouting at somebody who asked
// for nothing. Measured on the first armed boot after the split -- a lane with the harness DISARMED
// still produced 819 bytes on stderr, because the module was refusing a dependency nobody wanted.
// With `configured`, the loud path belongs to exactly the operator who asked for an instrument.
typedef struct MH_HarnessModuleHost {
    unsigned    size;        /* sizeof(MH_HarnessModuleHost) as MH.DLL was compiled              */
    unsigned    abi;         /* MH_HARNESS_MODULE_ABI as MH.DLL was compiled                     */
    const char *run_dir;     /* "<exedir>\logs\<runid>_<role>\" -- borrowed, valid for the run   */
    int         spine_bound; /* 1 = libmh.dll is bound in this process (configuration (2))       */
    int         configured;  /* 1 = `[harness] enable=1`, i.e. this run ASKED for an instrument  */
} MH_HarnessModuleHost;

// What the module's own DllMain observed, plus what its two binds resolved. The R2 measurement is
// the first three; `host_bound` / `spine_bound` are F4E's own, and they are what makes the refusal
// checkable from outside: a harness that loaded but could not resolve its spine is a DIFFERENT fact
// from one that is not there, and the bind line has to be able to say which.
typedef struct MH_HarnessModuleProbe {
    unsigned  size;         /* sizeof(MH_HarnessModuleProbe) as the MODULE was compiled          */
    unsigned  abi;          /* MH_HARNESS_MODULE_ABI as the MODULE was compiled                  */
    unsigned  attach_calls; /* DLL_PROCESS_ATTACH count -- must be exactly 1                     */
    unsigned  attach_tid;   /* the thread its DllMain ran on (compare with mh.dll's)             */
    unsigned  init_calls;   /* MH_HarnessModule_Init count -- must be exactly 1                  */
    long long attach_qpc;   /* QueryPerformanceCounter at DLL_PROCESS_ATTACH                     */
    int       host_rows;    /* contract rows resolved out of mh.dll  (of MH_HARNESS_HOST_COUNT)  */
    int       spine_rows;   /* contract rows resolved out of libmh.dll (of ..._SPINE_COUNT)      */
    int       host_total;   /* the module's own row counts, so a MISMATCHED pair is visible      */
    int       spine_total;
} MH_HarnessModuleProbe;

/* Hand the instrument its run context and the spine verdict. Returns MH_HARNESS_MODULE_ABI, or 0 if
 * `host` is malformed. Called by mh.dll's DllMain immediately after the exports resolve. The module
 * binds its two contract tables from here -- NOT from its DllMain, which is inert by the rule every
 * satellite keeps (docs/dll-split.md). */
int MH_HarnessModule_Init(const MH_HarnessModuleHost *host);

/* Fill *out with the observations above. Safe to call before MH_HarnessModule_Init, in which case
 * the row counts read 0 -- deliberately, so mh.dll can probe the LOAD before it has told the module
 * anything. */
void MH_HarnessModule_Probe(MH_HarnessModuleProbe *out);

#ifdef __cplusplus
}
#endif

// ---- THE BOUND SURFACE ---------------------------------------------------------------------------
//
// X(ret, name, PARAMS, ARGS, ABSENT) -- identical to mh_net_module.h's, so the two read the same
// way. ABSENT is a statement executed INSTEAD of the forwarded call when the module is not bound;
// writing it here, next to the signature, is what makes "what does this answer with no instrument" a
// property of the contract rather than of whoever wrote the shim.
//
// ORDER IS THE COMMITTED ORDER: mh_harness.def, the generator's cross-check and the bind log's
// export count all read this list.
#define MH_HARNESS_MODULE_SYMBOLS(X)                                \
    X(int, MH_Harness_Init, (void), (), { return 0; })              \
    X(void, MH_Harness_LateArm, (void), (), { return; })            \
    X(void, MH_Harness_OnPresent, (void), (), { return; })          \
    X(void, MH_Harness_OnSimTick, (void), (), { return; })          \
    X(void, MH_Harness_OnMovieTick, (void), (), { return; })        \
    X(int, MH_Harness_WantsPresentTick, (void), (), { return 0; })  \
    X(int, MH_Harness_WantsMovieTick, (void), (), { return 0; })    \
    X(int, MH_Harness_WantsWallclockPin, (void), (), { return 0; }) \
    X(int, MH_Harness_StepFence, (int target), (target), {          \
        (void)target;                                               \
        return -1;                                                  \
    })                                                              \
    X(int, MH_Harness_RebindSimTick, (void *ours), (ours), {        \
        (void)ours;                                                 \
        return 0;                                                   \
    })                                                              \
    X(int, MH_Harness_RebindSimStep, (void *ours), (ours), {        \
        (void)ours;                                                 \
        return 0;                                                   \
    })                                                              \
    X(int, MH_Harness_RebindOrderDispatch, (void *ours), (ours), {  \
        (void)ours;                                                 \
        return 0;                                                   \
    })                                                              \
    X(int, MH_Harness_RebindLandPlayers, (void *ours), (ours), {    \
        (void)ours;                                                 \
        return 0;                                                   \
    })

// The count, derived from the list rather than written next to it (a hand-kept count is the same
// G106 shape one level down). Used by the bind log and by the gate.
#define MH_HARNESS_MODULE_COUNT_ONE(r, n, p, a, ab) +1
#define MH_HARNESS_MODULE_SYMBOL_COUNT              (0 MH_HARNESS_MODULE_SYMBOLS(MH_HARNESS_MODULE_COUNT_ONE))

#endif // MH_HARNESS_MODULE_H
