#pragma once
//
// THE CORE ARM (fork F3B / plan D2) -- the half of mh.dll's boot that is not a networking question
// and not an instrument.
//
// mh.dll used to arm from one 355-line body, MH_Seam_Init, behind one gate, `[net] enable`. That
// gate switched off everything below it: the reimpl promotions, the in-memory patcher, the
// tombstones, the hostapi/libmh_in/hostevt reports, the video/overlay/capture/uidrive instruments
// and 14 of the 18 ini sections this DLL reads. F3B split the body into three named arms --
// MH_Core_Arm, MH_Net_Arm, MH_UI_Arm (all file-static in mh/seams/net_seams.cpp, where the arm
// order is documented) -- plus the harness hand-off, and demoted the gate to the nine net steps.
//
// THE CORE ARM HAS TWO PHASES, and this header declares the first, because it is the one that has
// to cross a translation unit:
//
//   MH_Core_Arm_Early()  -- paths, the state bind, the two host-api tables, the inbound surface,
//                           the session seed, the event sink, the rebind arm. DllMain calls it
//                           BEFORE MH_Harness_Init.
//   MH_Core_Arm()        -- everything else, from MH_Seam_Init (static; not declared here).
//
// WHY TWO PHASES RATHER THAN ONE FUNCTION: G104's earliest-common-point rule. Every calls-struct
// binder reads mh::host() on first use and fail-fast aborts unbound, and the harness proper's own
// installs are arm-path code that can reach one -- so the binds must precede MH_Harness_Init, while
// the rest of the core arm must follow it (the harness claims its entries before any promotion
// exists: D18, C6, C10). The first placement of these binds, at the END of MH_Seam_Init, was
// MEASURED too late: every boot died at 0xC0000409 with the abort line only on stderr, caught by
// the UI suite cycling launch-retries (2026-09-02). So the instrument runs between the two phases
// and DllMain states that order in three consecutive lines.
//
// The body still lives in mh_harness/harness.cpp, directly above `harness_enabled()`. That is the F4
// file cut, not this item's: F3B gave the code its true name and its own call site, and F4 moves
// the file boundary to match.
//
#ifdef __cplusplus
extern "C" {
#endif

// Phase 1 of the core arm. Unconditional and ini-independent in the sense that matters: it runs in
// every configuration, harness or not, `[net] enable` or not, ini present or not. Call exactly
// once, from DllMain, immediately before MH_Harness_Init.
void MH_Core_Arm_Early(void);

#ifdef __cplusplus
}
#endif
