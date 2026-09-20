//
// mh_run_context.h -- shared per-run log directory for all injected mh.dll modules.
//
// Phase 0a of the MP instrumentation work: instead of every module overwriting a fixed log name
// next to the exe (mh_harness.log / mh_net.log / mh_lockstep.log / mh_launch.log) each run, all
// LOG output is routed into a per-run folder "<exedir>\logs\<YYYYMMDD_HHMMSS>_<role>\" so history
// is kept and a host<->client pair is unambiguous to tools/mp_analyze.py.
//
// The run-id + role are computed ONCE (first caller wins) and shared across every module, so the
// host and client each get one folder for the whole run. Role is detected from the command line
// (--mp-host / --mp-join) then mh_net.ini [net] role, else "solo".
//
// SES1 (2026-09-17) SPLIT THAT ONE FOLDER IN TWO. "One folder for the whole run" was right while a
// run meant a match; it stopped being right the moment three matches could be played without
// restarting, because the three then shared a directory and a report was a directory plus a
// timestamp range the sender had to remember. The folder is now per SESSION -- opened at lobby
// create (host) / JOIN (client), closed at gameover / leave / host-left / timeout / quit -- and the
// PROCESS folder (`<UTC>_menu_<role>`, today's folder renamed) holds everything before the first
// session and after the last. See MH_RunDir vs MH_ProcessDir below for which streams go where, and
// mh_session_dir.h for the naming, the rollover rules and the SESSION_BEGIN/END record.
//
// IMPORTANT: only *.log OUTPUTS move. The config input (mh_net.ini -- ONE file since fork F2G,
// [harness] included) and the harness
// seed blob stay at the exe dir -- use MH_ExeDir() for those. If the logs folder can't be created,
// MH_RunDir() falls back to the exe dir so logs never silently vanish.
//
#ifndef MH_RUN_CONTEXT_H
#define MH_RUN_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

// THE CURRENT output directory (created; trailing backslash). Until SES1 this was one folder per
// PROCESS; it is now one folder per SESSION, and the process folder only while no match is open:
//
//     "<exedir>\logs\<UTC>_menu_<role>\"              -- before any session, and after one closes
//     "<exedir>\logs\<UTC>_<mid8>_<slot>_<role>\"     -- while a match is open
//
// CALL IT AT OPEN TIME, NOT ONCE. A writer that composes its path at arm time and caches the string
// will keep writing into the directory the process started in. The cheap fix is mh_run_path() below;
// the check is one integer compare per line, and these logs already open/append/close per line.
const char *MH_RunDir(void);

// The PROCESS ("menu") directory -- what MH_RunDir() used to be, for the streams that are about the
// process rather than about a match: the determinism harness's outputs (mh_harness.*, composed once
// in mh/seams/core_arm.cpp and copied by mh_harness.dll at init), the UI-automation channel the rig
// polls (mh_uidrive.log, capture_*.bmp, rig_*.flag -- the runner discovers ONE directory at launch
// and would lose the peer the moment a lobby opened), and the arm-time banners (mh_video.log,
// mh_overlay.log). Everything else follows MH_RunDir().
const char *MH_ProcessDir(void);

// "<exedir>\" (trailing backslash). For config/seed inputs that live next to the exe.
const char *MH_ExeDir(void);

// "host" / "client" / "solo" -- the role this process detected at boot (cmdline verb, then ini).
const char *MH_RunRole(void);

// Bumped every time MH_RunDir() starts naming a different folder. Compare-and-rebuild token for any
// writer that caches a composed path; see mh_run_path().
unsigned long MH_RunDirGeneration(void);

// ---- the session transitions (mh/seams/net_discovery.cpp is the only caller) ----------------------
// Return values are MH_SESSION_NONE / _OPENED / _CLOSED / _ROLLED from mh_session_dir.h. Begin is
// idempotent for the SAME match_id (a host re-advertises its lobby ~1 Hz) and rolls over for a
// different one. If the directory cannot be created, Begin reports the session NOT opened and output
// stays where it was -- the fallback that has always guaranteed logs are never silently lost.
int         MH_RunDir_SessionBegin(const char *match_id_hex, int slot);
int         MH_RunDir_SessionEnd(void);
int         MH_RunDir_SessionActive(void);
const char *MH_RunDir_SessionMatchId(void);         // "" when no session is open
const char *MH_ProcessDirLeaf(void);                // "<UTC>_menu_<role>" -- recorded in session.json
int         MH_RunDir_UtcStamp(char *dst, int cap); // "YYYYMMDDTHHMMSSZ"; cap >= MH_SESSION_STAMP_CAP

// Write "[HH:MM:SS.mmm] " (local wall clock) into dst; returns the length written. dst needs >= 16 B.
//
// Lives here, in the run-context header, because BOTH mh_net.log writers need the identical format
// and they are in different modules: the transport's logf() (mh_common) and the seams' seam_log()
// (mh/seams). One derivation, or the "; " and "net: " lines would drift into two clocks and stop
// interleaving -- which is the entire point of stamping them (2026-08-06).
//
// ABSOLUTE, not relative-to-first-line like mh_uidrive.log: the questions this log answers are
// cross-process and cross-machine (which host instance was alive when a joiner connected; whether a
// drop lines up with an SSH channel opening). Matches mh_tunnel.bat's stamps and the run-dir names.
#ifdef __cplusplus
inline int mh_log_stamp(char *dst) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return wsprintfA(dst, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// ---- the one line every cached-path writer needs (SES1) ------------------------------------------
//
// Replaces the `if (!g_log_ready) { wsprintfA(g_log, "%smh_x.log", MH_RunDir()); g_log_ready = true; }`
// idiom that this stack had in a dozen places. Returns TRUE when the path was (re)built, which is
// the signal a writer holding an OPEN HANDLE on the old file needs: close it and let the next write
// reopen. Writers that open/append/close per line can ignore the return.
//
//   static unsigned long gen = 0;
//   mh_run_path(g_log, MAX_PATH, "%smh_net.log", &gen);
//
// `gen` starts at 0 and MH_RunDirGeneration() starts at 1, so the first call always builds.
//
// `fmt` IS A FORMAT STRING WITH ONE %s, NOT A BARE LEAF NAME, and that is deliberate. It keeps the
// call site spelled `"%smh_net.log"` -- the exact shape tools/check_instrument_wiring.py's channel
// census (CHANNEL_RE) and its planted-violation arms read, so moving a dozen writers onto one helper
// did not have to move the instrument-ownership gate with them.
//
// THE `%s` IS CHECKED, and that check cost a rig run to learn. A `fmt` written as the bare leaf
// `"mh_net.log"` compiles, passes every review, and produces a RELATIVE path -- so the log lands in
// the process's working directory (the game folder) instead of the run folder, quietly, while every
// other stream moves correctly. Refusing the composition and leaving `dst` empty makes the writer
// fail visibly instead.
inline bool mh_run_path(char *dst, int cap, const char *fmt, unsigned long *gen) {
    unsigned long g = MH_RunDirGeneration();
    if (dst[0] != '\0' && *gen == g) return false;
    if (fmt == nullptr || fmt[0] != '%' || fmt[1] != 's') return false;
    *gen = g;
    wsprintfA(dst, fmt, MH_RunDir());
    (void)cap; // the composition is bounded by MAX_PATH on both halves
    return true;
}

// The same, pinned to the PROCESS directory: for the harness/rig channels that must not move when a
// match opens. It still takes a generation so the two call shapes read identically at the call site,
// and so a future process-directory rotation would reach these too.
inline bool mh_proc_path(char *dst, int cap, const char *fmt, unsigned long *gen) {
    if (dst[0] != '\0' && *gen != 0) return false;
    if (fmt == nullptr || fmt[0] != '%' || fmt[1] != 's') return false; // see mh_run_path
    *gen = 1;
    wsprintfA(dst, fmt, MH_ProcessDir());
    (void)cap;
    return true;
}
#endif

#endif // MH_RUN_CONTEXT_H
