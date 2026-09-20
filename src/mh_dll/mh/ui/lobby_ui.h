//
// mh/ui -- THE UI-OWNED FIXUP MODULE (fork F3E / plan D4).
//
// WHAT LIVES HERE, and the rule that decides it. The boundary is by DEPENDENCY, not by tag: a fixup
// belongs to this module when its body is about WIDGETS, GEOMETRY, or a FORMAT STRING and it needs
// to read nothing from the transport, the peer table or the session. Everything that reads those --
// the per-lobby-frame dispatch seam, the peer mirror, the join/leave/start handlers, the entry
// driver -- stays net-owned in mh/seams and CALLS IN here. That is why this header is mostly a list
// of things the net side calls, plus three setters through which the net side hands this module the
// two facts it is not allowed to look up for itself (whether logging is on, and whether this peer is
// a client in a live session).
//
// The module reaches mh.exe's fixed VAs directly (mh::addr) -- that is its whole job, and it is why
// it is NOT in tools/check_sim_addresses.py's MODULES list: that check is for modules whose state
// goes through a binder, and a UI fixup's "state" is the game's own widget records.
//
// THE SLIDE BLOCK IS THE ONE PIECE WITH ITS OWN DOCUMENT. Its mechanism was re-derived from live
// instrumentation at F3E rather than carried over -- the lobby-slide notes. Do not re-introduce the
// pre-F3E folklore (a frozen client clock, a non-terminating loop, an unexplained browser slide);
// all three are measured false and the measurements are in that file.
//
#pragma once

namespace mh {
namespace ui {

// ---- arm-time wiring ---------------------------------------------------------------------------
//
// Three injections, and each exists so this module names no net symbol. Same shape mh::desync uses
// (set_logger) and the same reason.

// Where this module's diagnostic lines go. Today the seam's mh_net.log writer; once mh_net.dll
// splits out (F4) it is whatever mh.dll's own log is, with no text change.
void set_logger(void (*fn)(const char *));

// The verbose gate ([net] lockstep_log). A POINTER, not a copy: the flag is loaded by
// lockstep_install_core, and while that happens to run before the UI arm today, a module that
// snapshots someone else's boot-order is a module that breaks silently when the order moves.
void set_diag_flag(const bool *flag);

// 1 iff this peer is a CLIENT with a live session. The slide take-over's gate (see
// The lobby-slide notes "The gate"); it is a transport question, so the net side answers it.
void set_client_session_gate(int (*fn)(void));

// ---- installs ----------------------------------------------------------------------------------
//
// One per fixup, each doing its own install_trampoline/patch with the name the U30 refusal summary
// and tools/check_arm_order.py print. The ORDER is the caller's (install_mp_bootstrap) and is part
// of the arm contract -- these are deliberately not bundled into one "arm everything" call.
bool install_scrollbar_guard();          // browser scrollbar draw guard (empty/unbound list)
bool suppress_self_removal_dialog();     // Phase 2c: NOP the spurious "you were removed" modal
bool install_peer_clear_fix();           // host-Start clear-loop fix (AI-slot count never decremented)
bool install_lobby_slide_takeover();     // U3b/U29: settle the lobby slide instantly
bool install_screen_slide_observer();    // U38: the second slide loop, observe + dedup
bool install_remove_player_slot_guard(); // N2: player 0 is the host and is never removed
bool patch_browser_row_format();         // S7: the browser row's "occ/cap" count format

// ---- per-frame / event entry points, called by net-owned readers --------------------------------

// U3/U7: the client's lobby frame is parked off-screen -> settle it on-screen and re-centre.
// Called from the lobby-dispatch seam with the role already decided by the caller; this function
// makes the SCREEN test (is the lobby the active list) and the geometry test itself.
void lobby_frame_snap_on_screen();

// U8 Gap 2: the client adopted the host's map out of band, so re-run the two retail refreshes the
// map-transfer path would have driven (map-info panel text + the slot rows). Latches on the map's
// player count, so calling it every frame is free.
void lobby_refresh_for_map();

// U16: the host's "<name> joined/left" broadcast. POST runs on the recv thread (the net handler has
// already decided the announce is not about us); DRAIN runs on the main/UI thread and renders each
// queued line through the retail announce path.
void announce_post(const char *name, int is_join);
void announce_drain();

// U23: an involuntary lobby exit tells the player why, on the browser's own status line.
// cause: 1 = the host left, 2 = the link died, 0 = a deliberate Cancel (says nothing).
void browser_notice_arm(int cause);

// mp:F3c: the host REFUSED this peer's JOIN, and said why. Same carrier and dwell as the two causes
// above; the line is "Refused: <reason>" with the host's ASCII reason text verbatim (it names both
// codepages, or the format and the bound -- whatever the refusal was about; <= 20 chars by contract).
void browser_notice_arm_refused(const char *reason);

// mp:R4a: the relay this peer dialled is BEHIND this build (its WELCOME carried a lower protocol level,
// or none -- a pre-R4a relay -- or it speaks a leg version this build cannot read). Same carrier and
// dwell as the three above; `line` is the UDP module's ASCII text verbatim ("Relay outdated (protocol
// 0 < 1)", "Incompatible relay (leg v2/v1)"), <= 32 characters so it fits the unwrapped status line. Unlike a lobby-exit cause it is
// armed while the player is ALREADY on the browser (the first browser is where a relay is dialled,
// mp:R7), so the dwell starts on the next frame.
void browser_notice_arm_relay(const char *line);

// F3F ruling Q2: with no network module the browsers can never list anything, so they carry a
// standing line saying so. Armed once, at arm time. U42 (ruling Q8 -- ARM and explain, not gate):
// the SAME line also paints on a hosted manual lobby, which on_host_advertise() (net_discovery.cpp)
// arms unconditionally regardless of transport, so a module=none host reaches a real "Network
// players" screen with no working Start -- see lobby_notice.cpp for how one widget covers both.
void browser_notice_arm_no_module();

// Both notices repaint from the present hook -- the status-line buffer is not ours and retail
// re-clears it (see lobby_notice.cpp). Cheap when idle.
void browser_notice_tick();

// U37's third instrument ([net] slide_diag): log-on-change of the menu frame geometry, from the
// present hook. Inert unless slide_diag is on.
void slide_geom_watch();

} // namespace ui
} // namespace mh
