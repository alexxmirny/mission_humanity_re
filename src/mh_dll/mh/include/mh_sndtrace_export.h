#pragma once
//
// [log] snd_trace -- an OBSERVE-ONLY trace of every sound request the game makes (mh_snd.log).
//
// WHY. Players report that a move-order acknowledgement voice and a building-selection sound "sometimes do
// not play", also as an isolated first order, and it cannot be reproduced on demand. The sound path has
// FIVE independent silent drops and none of them logs (tmp/m1/order_sound.md, EN /eng/mh.exe):
//
//   1. llm_strat_group_order_ack_voice (0x004259b2) refuses an order within COOLDOWN_SEC (2.0) GAME seconds
//      of the last played ack (strict <); every 6th refusal plays the generic cue 0x44 instead.
//   2. llm_snd_play (0x00425233) returns silently when the cfg entry has no sample (wave_ptr null; ids >=
//      the table count fall back to entry 0), SND_ENABLED == 0, or the master volume == 0.
//   3. ... when 3 channels already play the same id.
//   4. ... when every channel is busy with priority >= the request's.
//   5. The DirectSound layer under it (llm_snd_channel_configure_voice_and_play 0x004d276c) re-creates a
//      secondary buffer per play and RETURNS on a failed CreateSoundBuffer / Lock; the Play HRESULT is
//      discarded. The device is opened EXCLUSIVE (SetCooperativeLevel level 3) on the window in
//      0x00fe93e8, so DirectSound mutes the game whenever that window is not the foreground window.
//
// HOW. `[log] snd_trace=1` (C_DEV, so it needs `[dev] unlock=1`; default 0; local only, never synced)
// arms two call-through observers (hook points snd_play / snd_order_ack) plus a WH_CALLWNDPROC focus tap.
// Each snapshots the game's own sound state before the original runs and diffs it after: no behaviour
// change, no register change, no floating point. Lines go to mh_snd.log in the SESSION folder through the
// async log sink (no file I/O on the game thread); the report bundle packs every file of the session
// folder, so it ships with a bug report unchanged.
//
// FORMAT (one line per event, each prefixed `[hh:mm:ss.mmm] `, the same wall stamp mh_net.log carries):
//   SND id=<id> vol=<v> prio=<cfg priority> clk=<game seconds> tick=<GetTickCount> caller=0x<VA> <name> fg=<0|1>
//       -> played chN (free|STOLEN) ds=<playing|DS-FAIL: ...|NOT-PLAYING ...> st=0x<GetStatus>
//        | dropped: no sample | sound disabled | master volume 0 | same-id limit | no free channel
//        | ANOMALY ...                       [ chans=[<n>:<busy|free> p<prio> id<id> | ...] when it explains]
//   ACK clk=<s> last_play=<s> cooldown=<s> gap=<ms> supp_before=<n> caller=0x<VA> -> EMIT | SUPPRESSED n | GENERIC 0x44
//   FOCUS WM_ACTIVATEAPP|WM_ACTIVATE|WM_KILLFOCUS|WM_SETFOCUS|WM_SIZE hwnd=<h> is_ds_hwnd=<0|1> ...
//   STATE enabled=<SND_ENABLED> master_volume=<n> subsystem_ready=<0|1> directsound=<open|NONE> ds_hwnd=<h>
//         channels=<n> cfg_entries=<n> requests_so_far=<n>     (on any change; a ~30 s heartbeat while requests flow)
// `caller=` is the RETURN address llm_snd_play was called with (call site + 5), with a name for the known sites.
// `fg=1` means the foreground window is the DirectSound cooperative-level window (0 = DirectSound is muting).
// An ACK line follows the SND line(s) it caused (the SND lines are logged when llm_snd_play returns, i.e.
// before the enclosing ACK returns).
//
// Building selection: llm_strat_play_building_select_sound (0x0049a48c), called from llm_strat_input_update at
// 0x00443a59 / 0x00443aa1, reaches llm_snd_play at the call sites 0x0049a514 (Building[].sound_click, seen as
// caller=0x0049a519), 0x0049a548 (offline building, ids 0x7d/0x7e), 0x0049a559 (under construction / not the owner's
// finished building, id 0x7b) and 0x0049a60e (energy-deficit cue 0x7c, volume = deficit percent).
// Move acknowledgement: llm_strat_group_order_ack_voice (0x004259b2) -> llm_snd_play at 0x00425a1f (race voice) /
// 0x00425a55 (the 6th refusal's generic 0x44). Selection voices: llm_strat_play_select_voice_single / _group.
//
// ROTATION. mh_snd.log rotates one generation (mh_snd.log.prev) at 32 MB; a combat-heavy minute is ~1.4 MB
// (measured: planet 5, 2026-10-14 report, 32k requests in 6 minutes).
//
// THE VOICE FIXES (2026-10-14; ini-only, C_DEV, default ON, local -- sound never reaches the sim or the hash).
// The trace of a heavy-combat report showed 16 of 36 move-ack voices and 4 of 10 group-select voices dropped
// with "no free channel": the 6 channels were full of weapon/projectile sounds at priority 15, the same
// priority the cfg gives voices, and llm_snd_play only steals a channel of STRICTLY lower priority.
//   [snd] voice_priority=1     A request from a voice / UI call site (order ack, select voices, race alert,
//                              building select, the input cues, the HUD) runs with its cfg priority raised by
//                              100 for that one call: it steals the lowest-priority combat channel, and the
//                              channel then holds the raised priority, so combat cannot steal it back. The
//                              cfg byte is restored right after. The steal path of llm_snd_play does not write
//                              the channel's sound id (retail omission); with this key on the seam writes it,
//                              so the same-id limit and llm_snd_stop_stale_voice_channels see the real id.
//   [snd] ack_cooldown_on_play=1  llm_strat_group_order_ack_voice stamps LAST_PLAY_TIME even when its voice
//                              was dropped, so one dropped ack silenced the next ~2 s of orders. With this key
//                              the stamp is put back when the voice did not play. (LAST_PLAY_TIME is an
//                              MF_VIEW region: not hashed.)
// "Did it play" is decided by the seam, not guessed: before the call every idle channel's id is set to a
// sentinel, so a play on an idle channel always rewrites a record; a steal always rewrites the priority byte.
// Untouched sentinels are put back before the detour returns.
// The seam arms when the trace OR either fix is on (both hook points are exclusive, so the trace and the fixes
// share the two detours). With libmh bound and the order-ack function displaced, the ack fix logs NOT ACTIVE.
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [log] snd_trace + [snd] voice_priority / ack_cooldown_on_play and arm the detours. EN-only,
// best-effort: a refusal never blocks the DLL.
// Returns 1 if at least one observer is armed, 0 otherwise (off, wrong build, or refused).
int MH_SndTrace_Install(void);

#ifdef __cplusplus
}
#endif
