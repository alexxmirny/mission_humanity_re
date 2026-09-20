//
// mh_crash_marker.h -- the PURE half of the crash marker (dist LA4, plan decision D12).
//
// WHAT A MARKER IS. When a fatal exception reaches mh.dll's vectored handler, the game does not try
// to write its own minidump. It writes a dozen lines of text -- what faulted, where, on which
// thread, in which match -- signals the launcher, and BLOCKS. The launcher (a 64-bit process that
// is not the one that is broken) then calls MiniDumpWriteDump on it from outside and releases it.
// That split is Microsoft's own guidance and Crashpad's design: a process that has just taken an
// access violation is the worst possible place to run the several thousand lines of DbgHelp that a
// minidump costs, because the heap it would allocate from is exactly what may be corrupt.
//
// SO THE MARKER IS THE HANDOFF, and everything about its shape follows from being written by a
// process that is already dying:
//
//   * PLAIN ASCII `key=value`, one per line. No length prefixes, no binary struct, no versioned
//     record -- a truncated marker still parses down to the last complete line, and a human reading
//     one in a bug report needs no tool.
//   * ONE WriteFile OF ONE PRE-SIZED BUFFER. The text is composed into a stack buffer with the
//     no-CRT primitives below and written in a single call, so the handler allocates nothing and
//     the file is never half-written by a second call that did not happen.
//   * `pointers=` IS AN ADDRESS IN THE FAULTING PROCESS. It is the EXCEPTION_POINTERS the handler
//     was given, and it stays valid only while the faulting thread is blocked -- which is why the
//     handler waits for the launcher's acknowledgement before returning. `MiniDumpWriteDump` reads
//     it across the process boundary (`ClientPointers=TRUE`); a marker read after the process has
//     exited still has every other field, and the launcher then writes no dump rather than reading
//     freed memory.
//
// THE OFFSET CONTRACT IS NOT OURS TO INVENT. `module` + `offset` are the two fields
// tools/crash_report.py's `resolve_module_fault()` already consumes, via the launcher's `crash`
// object, and it defines `offset` module-dependently: image-relative for the game exe, an RVA for
// mh.dll. Both are `address - module_base` -- the exe's preferred base IS its load base (no ASLR on
// this binary, the premise the whole DLL rests on) and a DLL's RVA is by definition its distance
// from wherever the loader put it. One subtraction serves both, which is why this header computes
// it rather than asking the caller to know which rule applies.
//
// NO CRT, like every other header in mh_common: mh.dll is injected into a Watcom binary, so the
// formatting is hand-rolled. The primitives come from mh_session_dir.h, which is already the
// project's OS-free string kit, plus the hex writer this file adds.
//
#ifndef MH_CRASH_MARKER_H
#define MH_CRASH_MARKER_H

#include "mh_session_dir.h" // mh_sd_put / mh_sd_put_int / mh_sd_copy, and the stamp + match-id caps

/* Bumped only if a READER would misinterpret an older marker. Adding a field does not qualify --
 * an unknown key is ignored by construction, which is the point of key=value. */
#define MH_CRASH_MARKER_VERSION 1

/* Every field is bounded, so the whole text is. 1 KiB is ~3x the worst case and still a single
 * stack buffer in a handler that must not allocate. */
#define MH_CRASH_MARKER_CAP 1024

/* A module file name ("mh.dll", "mh.focus.exe"). MAX_PATH's basename never approaches this. */
#define MH_CRASH_MODULE_CAP 64

/* "0.1.0-rc1+abc12345" -- MH_VERSION_FULL, copied at arm time so the handler reads no macro. */
#define MH_CRASH_BUILD_CAP 64

#ifdef __cplusplus

// ---- is this exception the end of the process? --------------------------------------------------
//
// TWO BITS DECIDE IT, AND THE SECOND ONE IS THE ONE PEOPLE FORGET.
//
// Bits 31-30 == 0b11 is NTSTATUS severity ERROR: 0xC0000005 (access violation), 0xC000001D, 0xC0000374
// (heap corruption), 0xC0000409 (stack buffer overrun), 0xC00000FD (stack overflow). Testing the
// SHAPE rather than a list is what makes an unfamiliar fault still read as fatal -- the same rule
// src/launcher/src/launch.rs applies to the EXIT code, so the handler's verdict and the launcher's
// agree by construction instead of by two lists being maintained in step.
//
// Bit 29 is the CUSTOMER bit, and it must be CLEAR. `0xE06D7363` -- MSVC's C++ `throw` -- has
// severity ERROR and would otherwise read as a crash, and C++ exceptions are THROWN AND CAUGHT
// routinely; a handler that treated each one as a fatal fault would write a marker, block the
// thread, and hang the game on its first caught exception. That is not a hypothetical: it is the
// single most common way a first-chance vectored handler gets a program wrong. Everything Windows
// itself raises fatally has the customer bit clear.
//
// STATUS_BREAKPOINT (0x80000003) is severity WARNING and so is already excluded -- deliberately: a
// process stopping at a breakpoint was being debugged, and reporting that as a game crash aims a
// false positive at exactly the people able to file the best bug reports.
inline bool mh_crash_is_fatal(unsigned long code) {
    if ((code & 0xC0000000ul) != 0xC0000000ul) return false; // not severity ERROR
    if ((code & 0x20000000ul) != 0ul) return false;          // customer-defined (C++ throw, ours)
    return true;
}

// ---- hex, fixed width, lowercase ----------------------------------------------------------------
// `0x%08lx` by hand. Fixed width rather than minimal so two markers line up in a diff, and
// lowercase to match what tools/crash_report.py's EXCEPTION_NAMES table is keyed by (`"c0000005"`)
// and what Windows' own event log prints.
inline int mh_cm_put_hex(char *dst, int cap, int at, unsigned long v) {
    static const char digits[] = "0123456789abcdef";
    at                         = mh_sd_put(dst, cap, at, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        if (at < cap - 1) dst[at++] = digits[(v >> shift) & 0xful];
    }
    if (at < cap) dst[at] = '\0';
    return at;
}

// ---- the record ----------------------------------------------------------------------------------
struct MH_CrashFacts {
    unsigned long code;                               // ExceptionRecord->ExceptionCode
    unsigned long flags;                              // ExceptionRecord->ExceptionFlags
    unsigned long process_id;                         // GetCurrentProcessId() -- the launcher opens this to dump it
    unsigned long thread_id;                          // the faulting thread; MiniDumpWriteDump wants it named
    unsigned long address;                            // ExceptionRecord->ExceptionAddress
    unsigned long pointers;                           // the EXCEPTION_POINTERS the handler was passed, IN THIS PROCESS
    unsigned long module_base;                        // load base of the module holding `address` (0 = not resolved)
    char          module[MH_CRASH_MODULE_CAP];        // its file name, "" when not resolved
    char          match_id[MH_SESSION_MATCH_HEX_CAP]; // SES0's id, "" outside a session
    char          build[MH_CRASH_BUILD_CAP];          // MH_VERSION_FULL
    char          when[MH_SESSION_STAMP_CAP];         // "YYYYMMDDTHHMMSSZ"
    // dist LA5. Set when a `<marker>.ctx32` sidecar was written alongside this marker -- the raw
    // x86 EXCEPTION_RECORD + CONTEXT bytes, captured natively inside the (still-blocked) faulting
    // process. See mh/seams/crash_marker.cpp `write_context()` for the writer and
    // src/launcher/src/crash.rs `append_exception_stream()` for the reader: LA4 measured that a
    // 64-bit MiniDumpWriteDump cannot read a WOW64 target's EXCEPTION_POINTERS (ERROR_NOACCESS), so
    // this sidecar is how a dump still ends up with a real Exception stream.
    int has_context;
};

inline void mh_crash_facts_clear(MH_CrashFacts *f) {
    char *p = (char *)f;
    for (int i = 0; i < (int)sizeof(MH_CrashFacts); ++i) p[i] = '\0';
}

// The fault's distance from its module's base -- an RVA for a DLL, an image-relative offset for the
// exe, and the same subtraction either way (see the header note). 0 when the module is unresolved,
// which the reader distinguishes from a genuine 0 by `module=` being empty.
inline unsigned long mh_crash_offset(const MH_CrashFacts *f) {
    if (f->module_base == 0ul || f->address < f->module_base) return 0ul;
    return f->address - f->module_base;
}

// ---- the file ------------------------------------------------------------------------------------
//
// Returns the number of bytes written into `dst` (never `cap` or more, always NUL-terminated).
// `version` leads so a reader can refuse an unknown one before interpreting anything after it.
inline int mh_crash_marker_text(const MH_CrashFacts *f, char *dst, int cap) {
    int at = mh_sd_put(dst, cap, 0, "mh_crash=");
    at     = mh_sd_put_int(dst, cap, at, MH_CRASH_MARKER_VERSION);
    at     = mh_sd_put(dst, cap, at, "\ncode=");
    at     = mh_cm_put_hex(dst, cap, at, f->code);
    at     = mh_sd_put(dst, cap, at, "\nflags=");
    at     = mh_cm_put_hex(dst, cap, at, f->flags);
    at     = mh_sd_put(dst, cap, at, "\npid=");
    at     = mh_sd_put_int(dst, cap, at, (long)f->process_id);
    at     = mh_sd_put(dst, cap, at, "\ntid=");
    at     = mh_sd_put_int(dst, cap, at, (long)f->thread_id);
    at     = mh_sd_put(dst, cap, at, "\naddress=");
    at     = mh_cm_put_hex(dst, cap, at, f->address);
    at     = mh_sd_put(dst, cap, at, "\npointers=");
    at     = mh_cm_put_hex(dst, cap, at, f->pointers);
    at     = mh_sd_put(dst, cap, at, "\nmodule=");
    at     = mh_sd_put(dst, cap, at, f->module);
    at     = mh_sd_put(dst, cap, at, "\nmodule_base=");
    at     = mh_cm_put_hex(dst, cap, at, f->module_base);
    at     = mh_sd_put(dst, cap, at, "\noffset=");
    at     = mh_cm_put_hex(dst, cap, at, mh_crash_offset(f));
    at     = mh_sd_put(dst, cap, at, "\nmatch_id=");
    at     = mh_sd_put(dst, cap, at, f->match_id);
    at     = mh_sd_put(dst, cap, at, "\nbuild=");
    at     = mh_sd_put(dst, cap, at, f->build);
    at     = mh_sd_put(dst, cap, at, "\nwhen=");
    at     = mh_sd_put(dst, cap, at, f->when);
    // dist LA5. Adding a field does not bump MH_CRASH_MARKER_VERSION (see the constant's comment):
    // an older launcher reading this marker simply ignores a `ctx=` key it does not recognise.
    at = mh_sd_put(dst, cap, at, "\nctx=");
    at = mh_sd_put_int(dst, cap, at, f->has_context ? 1 : 0);
    at = mh_sd_put(dst, cap, at, "\n");
    return at;
}

#endif /* __cplusplus */

// ---- the process-to-process contract, named once so both sides cite the same strings -------------
//
// These are ENVIRONMENT VARIABLES the launcher sets on the child, not ini keys, and that is the
// whole reason the handshake is safe to leave armed in every build: a game started WITHOUT a
// launcher sees neither of them, so the handler writes its marker to the default path and returns
// immediately instead of waiting 15 seconds for an acknowledgement that is never coming. "Is there
// a launcher listening" is a fact about how this process was started; an ini key would have been a
// setting that could disagree with it.
//
// MH_CRASH_CHANNEL is an opaque token the launcher mints per child; the two event names are derived
// from it below. It is not a secret and it is not a capability -- the worst a hostile local process
// can do with one is release a crashed game early, and it could terminate the game outright.
#define MH_CRASH_ENV_CHANNEL "MH_CRASH_CHANNEL"
#define MH_CRASH_ENV_MARKER  "MH_CRASH_MARKER"

/* "Local\mh_crash_<channel>_req"  -- the game signals it, the launcher waits on it.
 * "Local\mh_crash_<channel>_ack"  -- the launcher signals it once the dump is written. */
#define MH_CRASH_EVENT_PREFIX "Local\\mh_crash_"
#define MH_CRASH_EVENT_REQ    "_req"
#define MH_CRASH_EVENT_ACK    "_ack"

/* How long the faulting thread waits for the launcher before giving up and dying. Generous on
 * purpose: MiniDumpWriteDump over a 300 MB game with DataSegs takes seconds on a slow disk, and the
 * cost of waiting too long is a late crash dialog while the cost of waiting too little is no dump
 * at all. Only ever paid when a launcher told us it was listening. */
#define MH_CRASH_ACK_TIMEOUT_MS 20000

/* The marker's default name when the launcher did not name one. `<pid>` keeps concurrent rig lanes
 * -- which all run an exe called mh.focus.exe out of sibling directories -- from overwriting each
 * other's evidence, the same per-lane discipline tools/crash_report.py enforces on WER reports. */
#define MH_CRASH_MARKER_LEAF "mh_crash_%lu.marker"

/* dist LA5. `<marker path>` + this suffix -- never a separate name of its own, so the launcher needs
 * no new field to find it, only to know whether to look (the marker's `ctx=1`). Spelled once here and
 * once in src/launcher/src/crash.rs (a Rust translation unit cannot include a C++ header), the same
 * duplication MH_CRASH_ENV_CHANNEL/MH_CRASH_ENV_MARKER already accept. */
#define MH_CRASH_CTX_SUFFIX ".ctx32"

#endif /* MH_CRASH_MARKER_H */
