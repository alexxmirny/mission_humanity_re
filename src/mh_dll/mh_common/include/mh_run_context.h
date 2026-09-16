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

// "<exedir>\logs\<runid>_<role>\" (created; trailing backslash). Shared, computed once.
const char *MH_RunDir(void);

// "<exedir>\" (trailing backslash). For config/seed inputs that live next to the exe.
const char *MH_ExeDir(void);

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

#endif // MH_RUN_CONTEXT_H
