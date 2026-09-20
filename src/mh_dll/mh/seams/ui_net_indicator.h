#pragma once
//
// seams/ui_net_indicator.h -- mp:L1, the PLAYER-VISIBLE connection indicator.
//
// The debug overlay (`[debug]`, gfx_overlay.cpp) already puts every net number on screen, and it is
// not this: it ships OFF, it is a developer instrument, it paints with our own DLL-native RGB565
// blitter and an embedded font, and it shows eighteen readouts. This one ships ON, paints with the
// GAME's own font/text path, and shows exactly the three numbers plus the one message the latency
// research argued a PLAYER can act on -- per-peer ping, a stability bar, command latency, and the
// NAME of the peer a stall is waiting for. The two are siblings, not duplicates: a number a
// developer reads while chasing a pacing bug and a number a player reads while deciding whether to
// keep playing are different products of the same measurement.
//
// See ui_net_indicator.cpp's head comment for the three numbers' definitions, the draw path and the
// determinism argument, and `[hud]` in mh_net.example.ini for the knobs.
//
#ifdef __cplusplus
extern "C" {
#endif

/* Read `[hud]` from mh_net.ini and arm. Installs no hook -- like the capture/overlay seams it rides
 * the lockstep present hook -- so it adds no line to the arm-log sequence and reports through its
 * own first-draw line instead (which is emitted IN A MATCH, after the arm window has closed).
 * Returns 1 when the indicator is enabled, 0 when `[hud] net_indicator=0` or the build is not EN. */
int MH_NetIndicator_Install(void);

/* Per present, from net_lockstep.cpp's on_present. Returns immediately outside a live lockstep
 * match, which is what makes solo/single-player draw nothing at all. */
void MH_NetIndicator_OnPresent(void);

/* Test/diagnostic accessor: frames drawn, stall episodes named. */
void MH_NetIndicator_Stats(long *frames_drawn, long *stalls_named);

/* mp:L1 -- DEFINED IN net_lockstep.cpp (the additive getter this seam asked T3's owner for, so that
 * the lookahead controller stays that item's to edit). Which peer is binding COMMITTED right now,
 * as a PEER_HORIZON / strategic-player slot, and how long the sim has been blocked on it; -1 when
 * the sim is not blocked. Reads only state `lateness_tick` already maintains -- it computes nothing
 * of its own and writes nothing at all. */
int MH_Lockstep_StallBindingPeer(unsigned long *blocked_ms);

#ifdef __cplusplus
}
#endif
