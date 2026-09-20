//
// mh.dll entry point + exported thunks. The DLL is injected into mh.exe via an added import
// (src/patcher/mhpatch.py). Keep this file to the live surface: DllMain and the thin C exports.
// Larger logic lives elsewhere -- the WinMain/dump experiment in winmain_experiment.c.
//
#include <windows.h>
#include "mh_lib_c_export.h"    // MH_LZW_Decompress, MH_MISC_GetSightAreaFromRadius (mh_lib)
#include "mh_core_arm_export.h" // MH_Core_Arm_Early (mh_lib/harness.cpp) -- the core arm's phase 1
#include "mh_module_bind.h"     // MH_ModuleBind_Early (mh/seams/module_bind.cpp) -- fork F4A
#include "mh_harness_export.h"  // MH_Harness_Init (mh_lib/harness.cpp) -- determinism replay harness
#include "mh_seam_export.h"     // MH_Seam_Init (mh_lib/net_seams.cpp) -- MP transport seam wiring
#include "mh_launch_export.h"   // MH_Launch_Init (mh_lib/launch.cpp) -- D17 launch-to-state harness
#include "mh_crash_export.h"    // MH_CrashMarker_Init/Shutdown (mh/seams/crash_marker.cpp) -- LA4

BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        // BEFORE THE ZEROTH CALL, AND THAT IS NOT A CONTRADICTION OF THE CONTRACT BELOW. dist LA4's
        // crash handler is the one arm here that depends on NOTHING -- no satellite, no export
        // table, no run context, no ini section any other arm reads. It is AddVectoredExceptionHandler
        // plus two environment reads. Putting it first is what makes it cover the rest of DllMain:
        // a fault inside a module bind or inside MH_Seam_Init is precisely the crash a player cannot
        // describe and a log cannot show, and a handler installed after the boot is a handler that
        // is absent for the part of the run most likely to need it. It writes no line into
        // mh_net.log, so the arm-order gate (tools/check_arm_order.py) sees nothing new.
        MH_CrashMarker_Init();
        // FOUR CALLS, and the ORDER between them is a contract, not a convenience (fork F3B split
        // what used to be three). The three ARMING subsystems are orthogonal -- each hooks DISJOINT
        // functions, no memory-patch overlap -- so all can arm in the same process:
        //   MH_Launch_Init    -- launch-to-state verbs (--load/--newgame/--mp-host/--mp-join); UI driver.
        //   MH_Core_Arm_Early -- fork F3B: the core arm's PHASE 1 -- paths, the state bind, the two
        //     host-api tables, the inbound surface, the session seed, the event sink, the rebind arm.
        //     Unconditional, in every configuration. It was the head of MH_Harness_Init until F3B and
        //     runs at exactly the same point in the boot; it is named separately because it is mh.dll
        //     core, not instrument, and because G104 requires it AHEAD of the harness while the rest
        //     of the core arm must follow it. See mh_core_arm_export.h for the measurement.
        //   MH_Harness_Init   -- determinism per-step state hash (arms on `[harness] enable=1`).
        //   MH_Seam_Init      -- the core arm's PHASE 2 (MH_Core_Arm) and, from inside it, the net and
        //     UI arms. Arms UNCONDITIONALLY as of the ship build (2026-07-25): multiplayer is the
        //     product, not a debug option, so a plain install with no ini at all is a working MP client
        //     AND host. Since F3B `[net] enable=0` switches off the NINE net steps -- the transport
        //     installs and the control-frame handler binds -- and nothing else; it used to switch off
        //     the whole DLL, promotions, patcher, tombstones and instruments included.
        // Compositions that matter:
        //   --load + [harness] enable=1        = fully-scripted SP determinism run (fixed_step=1).
        //   --mp-host/join + [net] + [harness] = A3: hash a LIVE MP game on both peers, diff the logs.
        //     For that the [harness] block MUST set fixed_step=0 (real lockstep drives the clock,
        //     don't pin it) and seed_mode=2 (both peers already share identical start via the force-entry).
        //   All of it in ONE mh_net.ini since fork F2G -- the separate mh_harness.ini is gone, and a
        //   leftover copy beside the exe is REFUSED (mh/config/config.h) rather than ignored.
        // A FOURTH used to live here: MH_Pool_Init, the DLL-hosted projectile pool. RETIRED 2026-07-30
        // (RI-STATE / ST0) -- see the MH_HostedPoolBase note below and the retired-patch log.
        // A ZEROTH CALL SINCE FORK F4A, AND IT IS DELIBERATELY OUTSIDE THE CONTRACT ABOVE.
        // MH_ModuleBind_Early is the SIBLING-DLL BIND: at F4 the spine, the transport and the
        // harness are separate DLLs, and every one of the four lines below is code that either
        // lives in one of them or calls into one -- so a module bind can only be FIRST. It is here
        // rather than merged into MH_Core_Arm_Early for the same reason that function exists at
        // all: a bind that ran after its first consumer is G104, and G104 at module scope is the
        // failure where every gate stayed green and every boot died at 0xC0000409
        // (mh_core_arm_export.h:21-28).
        //
        // SINCE F4B IT BINDS A REAL SATELLITE, UNCONDITIONALLY: mh_net.dll, the MP transport. There
        // is no enable key and there must not be one -- absence IS the configuration (F4A ruling
        // (a)); a per-module switch would recreate the "off or absent?" ambiguity F3B spent an item
        // removing from `[net] enable`. `[net] module=none` still declines the LOAD, and that is
        // the one key involved. Whatever happens, exactly one `; [modules] mh_net:` line lands in
        // mh_net.log and the boot continues: a missing transport degrades to the no-module
        // configuration (the MP menu still arms, the browser says why), it never fails the boot.
        // See mh/include/mh_module_bind.h and docs/dll-split.md.
        //
        // AND SINCE F4D THERE ARE TWO SATELLITES BOUND HERE, mh_net.dll then libmh.dll -- THE
        // SPINE. That one is the reason the zeroth call had to exist at all: 94.6% of what used to
        // be this DLL now ships as libmh.dll, and every [promote] installer MH_Core_Arm_Early
        // reaches is a call into it. Its absence is not a degradation, it is CONFIGURATION (1)
        // (ruling Q10, ratified): mh.dll genuinely does not contain the spine, so with no
        // libmh.dll beside it the game runs the original binary's own bodies -- which is a shipped
        // configuration, not a failure. One `; [modules] libmh:` line lands in mh_net.log saying
        // which configuration the run is in, and the boot continues either way.
        //
        // AND SINCE F4E THERE ARE THREE, the last being mh_harness.dll -- the determinism/replay
        // INSTRUMENT, 8k lines that used to be this DLL's single largest file. Its absence is the
        // third distinct meaning of "not there" (docs/dll-split.md's inheritance table): not a
        // degradation, not a configuration of what runs, but UNINSTRUMENTED -- the game is
        // bit-for-bit the game it always was, because the harness only ever observes. So the twelve
        // shims answer what the un-armed bodies already answered and nothing behaves differently.
        // The one case that is NOT silent is `[harness] enable=1` with no module: an operator who
        // configured an instrument and will not get one, refused loudly on three channels (G178).
        //
        // ORDER BETWEEN THE FIRST TWO IS NOT A CONTRACT: mh_net and libmh do not know about each
        // other, neither DllMain does anything, and nothing between them reads the other's table.
        // mh_net stays first so its arm-log line keeps the position F4B's committed baselines
        // already record, and libmh's is F4D's one inserted step.
        //
        // THE THIRD ONE'S POSITION *IS* A CONTRACT, and it is the only ordering here that is. The
        // instrument late-binds its ~30 spine rows straight out of libmh.dll and REFUSES LOUDLY when
        // they are not there (ruling Q4), and mh.dll tells it which configuration the run is in as
        // an argument to its init. A harness bound before libmh would be handed a verdict mh.dll had
        // not reached yet -- so it goes last, and the comment says so rather than the ordering being
        // an accident somebody later "tidies".
        MH_ModuleBind_Early((HMODULE)hModule);
        MH_LibmhBind_Early((HMODULE)hModule);
        MH_HarnessBind_Early((HMODULE)hModule);
        MH_Launch_Init();
        MH_Core_Arm_Early(); // core arm, phase 1 -- must precede the harness (G104)
        MH_Harness_Init();
        MH_Seam_Init(); // core arm, phase 2 -> the net arm -> the UI arm -> the hand-off
    } else if (dwReason == DLL_PROCESS_DETACH) {
        // THE FIRST DETACH ARM THIS DllMain HAS EVER HAD, and exactly one thing needs it. Byte
        // patches, trampolines and hosted tables all die with the process that owns them, which is
        // why there was nothing here before dist LA4. A VECTORED EXCEPTION HANDLER does not: it is
        // a node in a process-wide list the OS walks on every exception, so an mh.dll that unloaded
        // while its handler was still registered would turn the next exception in the process --
        // any exception, including one the game handles routinely -- into a call into unmapped
        // memory. See mh/include/mh_crash_export.h.
        //
        // Nothing else is unwound here on purpose. A process TERMINATING (lpReserved != NULL) is
        // the overwhelmingly common case and the OS is about to reclaim everything; adding
        // teardown for things that do not need it is how a detach path acquires a deadlock.
        MH_CrashMarker_Shutdown();
    }
    return TRUE;
}

//
// Exports -- thin C thunks over mh_lib / the pool module.
//

__declspec(dllexport) int DecompressLZWData(void *input, void *output, size_t size) {
    return MH_LZW_Decompress(input, output, size);
}

__declspec(dllexport) void *GetSightAreaFromRadius(unsigned char radius) {
    return MH_MISC_GetSightAreaFromRadius(radius);
}

// THE FORCE-LOAD ANCHOR. Importing this from the exe is what makes the loader map mh.dll at process
// init, so DllMain runs before any exe code does. That -- not the pool -- is why every injection
// patch imports it: net_load, net_load_EN, harness_load and launch_load all carry
// `imports = {"mh.dll": ["MH_HostedPoolBase"]}` and none of them ever CALLS it.
//
// The name is now historical. It used to return the base of a DLL-hosted projectile pool
// (VirtualAlloc'd at a fixed VA); that experiment is RETIRED (RI-STATE / ST0, the retired-patch log)
// because it put game state outside the binary at an address every state manifest still spelled by
// hand -- the exact hazard RI-STATE exists to remove. The SYMBOL stays, and must: renaming it would
// unload the DLL from every already-deployed patched exe, the rig VMs' included.
//
// Returns NULL, and nothing reads it. If a future ST3 `owned_by_dll` region wants DLL-hosted storage
// again, it gets its own export and its own registry entry -- not this one's meaning back.
__declspec(dllexport) void *MH_HostedPoolBase(void) {
    return NULL;
}
