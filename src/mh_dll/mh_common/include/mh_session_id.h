//
// mh_session_id.h -- the OS half of the match_id (SES0). mh_net_proto::uuid7_make() is deliberately
// pure -- it takes a Unix millisecond stamp and 10 random bytes and touches no platform header, so
// the same code serves the DLL, the future Linux relay and an offline selftest. These two inlines
// are the Windows side of that split, and nothing else.
//
// HEADER-ONLY AND INLINE, like mh_log_stamp() next door in mh_run_context.h, for the same reason:
// both mh.dll and the offline oracle (mh_nettest, which compiles net_discovery.cpp directly rather
// than linking it) need one definition, and an inline in mh_common's include directory gives them
// that with no project-file edit on either side.
//
// NEVER THE SIM RNG. The lockstep seed and llm_rand_state_advance's channels are part of the hashed
// state: drawing a single value from them at lobby creation would desync the match the id is there
// to identify. This draws from the OS CSPRNG and the wall clock, both outside the sim entirely --
// the same rule mp_gen_tag() already follows for the 32-bit lobby tag.
//
#ifndef MH_SESSION_ID_H
#define MH_SESSION_ID_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>

#pragma comment(lib, "bcrypt.lib")

#ifdef __cplusplus

// Milliseconds since the Unix epoch, from the system clock. GetSystemTimeAsFileTime is 100 ns ticks
// since 1601-01-01 UTC; 11644473600 s is the gap to 1970. UTC, not local time -- two peers in
// different time zones must produce ids that sort in real order.
inline uint64_t mh_session_unix_ms(void) {
    FILETIME       ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart / 10000ULL) - 11644473600000ULL;
}

// `n` cryptographically-random bytes. Returns true if they came from the OS CSPRNG.
//
// THE FALLBACK IS NOT SECURITY, IT IS NON-NILNESS. If BCryptGenRandom fails (it does not, in
// practice, with BCRYPT_USE_SYSTEM_PREFERRED_RNG) the buffer is filled from an xorshift over the
// tick count and the process/thread ids -- the same non-sim mixer mp_gen_tag() has used for the
// lobby tag since S2. A weak match_id still correlates two logs; an ALL-ZERO one reads as "no
// match_id" everywhere downstream (uuid7_is_nil) and would silently disable the correlation the
// whole item exists for. The return value says which happened, so a caller that cares can log it.
inline bool mh_session_random(unsigned char *out, int n) {
    if (n <= 0) return true;
    if (BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return true;
    uint32_t s = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ GetCurrentThreadId();
    for (int i = 0; i < n; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if (!s) s = 0xA5A5A5A5u; // xorshift's absorbing state
        out[i] = (unsigned char)(s & 0xff);
    }
    return false;
}

#endif // __cplusplus

#endif // MH_SESSION_ID_H
