//
// mh_diploecho_export.h -- mp:U39: THE DIPLOMACY DIALOG'S OPTIMISTIC RELATION ECHO.
//
// THE WRITE. `llm_ui_diplomacy_apply_and_resume` (0x004c7fc6) applies the dialog: for every other
// player whose relation spinner changed it ISSUES order 0xf4 (`llm_strat_order_set_player_relation`,
// applied on every peer at commit by `llm_diplomacy_set_relation`, whose first statement is the
// same store) AND, 22 bytes at 0x004c8070..0x004c8085, writes `Players[PlayerSide].relation[j]`
// DIRECTLY on the clicking peer:
//   0F B7 15 54 83 E5 00   MOVZX EDX, word [0x00e58354]   ; PlayerSide
//   6B D2 34               IMUL  EDX, EDX, 0x34           ; stride
//   03 55 D4               ADD   EDX, [EBP-0x2c]          ; + j
//   8A 45 D8               MOV   AL, [EBP-0x28]           ; relation value
//   88 82 F1 87 E5 00      MOV   [EDX + 0x00e587f1], AL   ; THE WRITE
// Both defs are dead right after (EAX is clobbered at 0x004c8086, EDX redefined at 0x004c809b before
// its first use) and no branch lands inside the span, so NOPing it is instruction-safe.
//
// WHY IT MATTERS. Players[8] (0x00e587e9) is hashed (HASH_REGIONS[55] `players`, added by D23), so
// between the dialog's Ok and the order's commit the two peers' state hashes differ -- MEASURED
// 2026-09-21 on the rig (u39_diplomacy, three runs): `players` differs for EXACTLY the 4 steps
// between the local write and the commit, then re-converges. Retail never noticed (dead at the
// wire); our restored MP compares every step. It is ordinary client-side prediction, not a bug in
// retail's terms: the `!=` guard at 0x004c8051 reads the same cell, so the echo keeps a reopened
// dialog from re-issuing the order before it round-trips.
//
// THE FIX. patch_bytes_guarded over the 22 expect bytes -> 22 NOPs, so the only writer of the cell is
// the order handler every peer runs at commit. Cost: a dialog reopened before the commit shows the OLD
// value and re-issues the same order (idempotent). Off with `[net] diplo_echo_nop=0` -- the negative
// arm the u39_diplomacy_echo scenario runs, which must still name `players`.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Apply the NOP (best-effort: a wrong byte string, a promoted parent or a refused VirtualProtect
// leaves the function untouched and logs it). Returns 1 when the span is NOPed, 0 otherwise. Never
// fails the process.
int MH_DiploEcho_Install(void);

#ifdef __cplusplus
}
#endif
