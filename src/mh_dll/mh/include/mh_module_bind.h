#pragma once
//
// mh/include/mh_module_bind.h -- HOW mh.dll BINDS ITS SIBLING DLLs (fork F4A's ruling, F4B's first
// real subject).
//
// ---- THE PROBLEM, STATED ONCE -------------------------------------------------------------------
//
// F4 splits mh.dll into four modules. mh.dll's whole arm runs in DllMain, and the three options for
// reaching a sibling from there each have a standing objection:
//
//   static import        kills absent-tolerance. A missing mh_net.dll / libmh.dll / mh_harness.dll
//                        then fails PROCESS LOAD, before any of our code runs, with a Windows error
//                        box naming a DLL the player never heard of. MEASURED at F4A: exit
//                        0xC0000135, zero log directories, nothing on stderr. Every "degrade, don't
//                        fail" ruling in F3/F4 becomes unimplementable on top of it.
//   LoadLibrary here     objected to by three standing source rules: net_lockstep.cpp:335-337,
//                        video.cpp:969-972, msvfw32/proxy_core.cpp:14-16 -- all three citing
//                        The Windows-compat notes. F4A took them one at a time (docs/dll-split.md
//                        "Why the loader-lock ban does not bite this shape"): each is right about its
//                        own case, and what makes them right is what the load DRAGS IN behind it.
//                        A leaf DLL of ours, with an inert DllMain and no import outside mh.dll's
//                        own set, drags in nothing.
//   two-phase off-lock   net_lockstep.cpp:361 on_present, "the first place that is both off the
//                        loader lock and guaranteed to run". Works (measured), and stays the
//                        documented fallback in docs/dll-split.md -- but it binds 525 ms into the
//                        run, which is far too late for anything the arm itself needs.
//
// ---- THE RULING (F4A) ---------------------------------------------------------------------------
//
//   mh.dll binds a sibling DLL with LoadLibrary on an ABSOLUTE PATH NEXT TO ITSELF, plus
//   GetProcAddress per symbol, as the FIRST statement of DLL_PROCESS_ATTACH. Every satellite's own
//   DllMain touches nothing outside its own module. mh.dll's DllMain stays the sole orchestrator,
//   calling each satellite's exported init explicitly, in an order it chooses.
//
// THE SUBSET RULE is what makes that safe, and it is a property a future satellite must CHECK rather
// than inherit: a satellite is safe to load from here when (a) its own DllMain is inert and (b) its
// static imports are a SUBSET of mh.dll's own (KERNEL32, USER32, WINMM, WS2_32). Then the load maps
// a file, snaps an import table against modules the loader has already initialised, and runs one
// inert DllMain. Both halves are gates, not paragraphs: check_module_bind.py --dllmain-inert and
// --subset, both lint_repo rows.
//
// ---- WHAT F4B CHANGED --------------------------------------------------------------------------
//
// F4A proved the mechanism on a SPIKE satellite armed by `[modules] spike`, default off. F4B ships
// the first real one and the knob DIES with it (F4A ruling (a)): a real satellite is unconditional,
// and ABSENCE IS THE CONFIGURATION. A per-module enable key would recreate exactly the confusion
// F3B spent an item removing from `[net] enable` -- a single switch that means both "off" and
// "absent", which the caller that has to tell them apart (the UI) cannot read.
//
// So there is ONE arm point now, not two: MH_ModuleBind_Early, from DllMain. MH_ModuleBind_OnPresent
// is gone with the spike; the mechanism it measured is still recorded in docs/dll-split.md as the
// fallback, and net_lockstep.cpp:361 is still the place it would go.
//
// ---- WHY mh.dll'S DllMain STAYS THE SOLE ORCHESTRATOR -------------------------------------------
//
// mh.c:15-44 calls four inits in an order that is "a contract, not a convenience". A satellite that
// armed anything from its OWN DllMain would insert itself into that contract at a point the loader
// chooses, not one we do -- and for a STATICALLY imported satellite the loader's choice is 181 us
// BEFORE mh.dll's DllMain runs at all (measured). That is G104 at module scope: the last time an arm
// ran on the wrong side of a required predecessor, every gate stayed green and every real boot died
// at 0xC0000409 with the abort line only on stderr (mh_core_arm_export.h:21-28).
//
#ifndef MH_MODULE_BIND_H
#define MH_MODULE_BIND_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call as the FIRST statement of DLL_PROCESS_ATTACH, passing DllMain's own hModule (the satellite is
// resolved next to MH.DLL, not next to the exe -- see the note in module_bind.cpp). Binds
// mh_net.dll; logs exactly one `; [modules] mh_net: ...` outcome line into mh_net.log either way,
// and NEVER fails the boot.
void MH_ModuleBind_Early(HMODULE self);

// Did mh_net.dll load and resolve its whole contract? This is what mh::net::transport_present()
// answers with since F4B (mh/include/mh_transport_present.h) -- the load RESULT, where F3F had a
// `[net] module` key standing in for it. It is also correct BEFORE the bind runs (answering 0),
// which matters because it is the value every forwarding shim in module_bind.cpp tests.
int MH_NetModule_IsBound(void);

// mp:SES4: which transport module actually got bound -- "udp" / "tcp", or "none" once every
// no-transport path (declined, not found, wrong contract, ABI mismatch) has run. This is the value
// session.json's `transport` field must record: before SES4 that field copied the CONFIGURED value
// (what the ini asked for) even when the bind never happened or bound the other file, so every
// 2026-09-20 session read "tcp" while the wire the peers actually played on was udp (the shipping
// default since that date). Correct before the bind runs too (answering "none").
const char *MH_NetModule_BoundTransport(void);

// The `[net] transport` value this run's ini asked for ("udp"/"tcp"), independent of whether the
// bind above then succeeded -- or "none" if `[net] module=none` meant no transport was ever
// requested. Exists only so a caller that wants to show BOTH (session.json: the bound value plus
// this one, when they differ) can.
const char *MH_NetModule_ConfiguredTransport(void);

// ---- libmh.dll, the SPINE (fork F4D) -------------------------------------------------------------
//
// The same mechanism, the same DllMain arm point, a different meaning for absence. Call it right
// after MH_ModuleBind_Early and before anything else: the whole four-call contract below it either
// lives in the spine or calls into it, and every [promote] installer MH_Core_Arm_Early reaches is a
// libmh row. Logs exactly one `; [modules] libmh: ...` outcome line into mh_net.log either way.
//
// ABSENCE IS CONFIGURATION (1), not a degradation -- mh.dll genuinely does not contain the spine
// (Q10, ratified), so with no libmh.dll beside it the game runs the original binary's own bodies.
// There is no enable key and there must not be one (F4A ruling (a)). See seams/libmh_bind.cpp.
void MH_LibmhBind_Early(HMODULE self);

// Did libmh.dll load and resolve its whole ~100-row contract? Correct before the bind runs too
// (answering 0).
int MH_LibmhModule_IsBound(void);

// Emit the crossing report -- F4D's STANDING ARM. Called once from net_lockstep.cpp's on_present,
// i.e. off the loader lock, after the whole arm has run, and AFTER the window check_arm_order
// gates (so it costs no baseline edit). Reports how many calls actually went through the DLL
// boundary; tools/check_module_bind.py --libmh reds a lane that bound the module and never crossed.
void MH_Libmh_OnPresent(void);

// ---- mh_harness.dll, the INSTRUMENT (fork F4E) ---------------------------------------------------
//
// The same mechanism a third time, and the third meaning of absence: UNINSTRUMENTED. Nothing about
// what the game DOES changes -- the harness installs detours that hash and record, and a run without
// it is the run every player has always had. So every one of the twelve forwarding shims in
// seams/harness_bind.cpp answers what the UN-ARMED body already answered, and absence is silent.
//
// It is bound AFTER libmh for one reason, and the reason is Q4: the instrument late-binds ~30 spine
// symbols out of libmh.dll with a HARD LOUD REFUSAL when they are not there, and mh.dll tells it
// which configuration the run is in (MH_HarnessModuleHost::spine_bound). Binding the instrument
// first would mean handing it a verdict mh.dll had not reached yet.
//
// THE ONE LOUD CASE: `[harness] enable=1` with no mh_harness.dll. An operator configured an
// instrument and will not get one, and a determinism run whose instrument silently never installed
// is the worst outcome this project knows (G178). Refused on all three channels; the boot continues,
// because the harness installs nothing and therefore its absence cannot change which bodies run.
void MH_HarnessBind_Early(HMODULE self);

// Did mh_harness.dll load and resolve its whole 12-row contract? Correct before the bind runs too
// (answering 0). This is NOT "is the harness armed" -- that is MH_Harness_Init's return, and the two
// are different questions: a bound instrument with no `[harness] enable=1` arms nothing.
int MH_HarnessModule_IsBound(void);

#ifdef __cplusplus
}
#endif

#endif // MH_MODULE_BIND_H
