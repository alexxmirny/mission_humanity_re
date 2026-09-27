//
// mh_relanding_export.h -- mp:D37a: A PIONEER RE-LANDING NO LONGER REFILLS THE STARTING STOCK ON
// THE NON-OWNER PEERS, CARRIED BY mh.dll.
//
// THE BUG (rc4 Last Question match, first per-step diff 10179). `llm_strat_bldg_completion_dispatch`
// (0x004795dd), the mothership arm of the CONSTRUCTION state: when a mothership/pioneer lands it
// credits Building[type].capacity[1..9] (5400/4000/3000/2000 for the human pioneer) + human_transport
// population if `mother_established == 0 || player != PlayerSide`. The first half is the real first
// landing. The second is a per-peer test (PlayerSide is the local side): on a RE-landing after a
// lift-off, every NON-owner peer re-credits the template stock and the owner does not. In single
// player the non-local players are all AI, so it only ever refilled the AI; in lockstep it refills a
// HUMAN on every peer but one -> resource_spent/player_resources desync. Real cargo (prod shuttle
// slots) is credited by order 0x16, not here.
//
// THE FIX (decided 2026-09-27). In a lockstep session (_G_LLM_GAME_SESSION_MODE == 3) the re-landing
// credit goes to AI players only -- status_flags bit 3, net-owned and written in lockstep order on
// every peer (hashed in strat_players), the same bit the arm tests a few instructions later for the
// AI .DMP injection. Identical on all peers; the AI keeps retail's refill. Every other session mode is
// retail verbatim (the thunk evaluates retail's own compare).
//
// ONE guarded edit, 13 bytes at 0x004798f1 (the JZ at 0x004798ec already sent mother_established==0
// to the credit block at 0x004798fe; the preceding MOV EAX,[EBP-0x18] at 0x004798ee stays):
//   66 3B 05 54 83 E5 00   CMP AX,word ptr [PlayerSide]
//   0F 84 40 01 00 00      JZ 0x00479a3e                     (skip the credit)
// ->
//   E8 <rel32>             CALL refill_thunk                 (every register kept; ZF = 1 -> skip)
//   0F 84 42 01 00 00      JZ 0x00479a3e
//   90 90
// Both successors (0x004798fe, 0x00479a3e) reload EAX/EDX from the frame, so only the flags matter.
// libmh's body (sim/sim_bldg_completion_dispatch.cpp, detail::mother_relanding_credits) applies the
// same rule for configuration (2).
//
// NOT UNDER `[config] mode=original` (that selector means "the game's own bodies" -- the D35 shape).
// A promoted parent (hook/promoted.h) makes it DISPLACED. Off with `[net] pioneer_refill_fix=0` --
// the reproduction arm.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Apply the edit (best-effort: a byte mismatch, a promoted parent, mode=original, the knob, or a
// refused VirtualProtect leaves the function untouched and logs it). Returns 1 when armed.
int MH_Relanding_Install(void);

#ifdef __cplusplus
}
#endif
