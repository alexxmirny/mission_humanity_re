//
// mh_config_dir.h -- THE ONE PATH RESOLVER FOR OUR FILES (dist RL3, plan decision D3).
//
// "Our files" are mh_net.ini, mh_key.txt, mh_config_refused.log, mh_run.txt and the logs root. They
// do NOT include the retail game's own data (`save\`, `setup.dat`), the lang\ packs, or the DLLs
// that sit beside mh.exe: those belong to the INSTALLATION and keep resolving from the exe's
// directory. The distinction matters because the exe's directory is usually `C:\Program Files
// (x86)\...`, where a non-elevated process is UAC-virtualized and every write silently lands in
// `%LOCALAPPDATA%\VirtualStore\...` -- the launcher reads the real folder and finds nothing (LA13's
// whole story). Settings, the key and logs are the player's, so they live in the player's profile.
//
// RESOLUTION ORDER for the config directory (first that works wins; the result is a directory with
// a trailing backslash):
//
//   1. env  MH_CONFIG_DIR    an explicit directory, created if need be. Overrides everything.
//   2. portable mode         `mh_net.ini` exists BESIDE mh.exe -> the exe's own directory. This is
//                            what every rig lane (tools/make_lane.py) and every pre-0.2.0 install
//                            relies on, and it is the only mode that needs no profile at all.
//   3. user storage          %LOCALAPPDATA%\MissionHumanity\games\<hash16>\  (created). <hash16> is
//                            the LAUNCHER's game-dir hash, byte for byte (see game_dir_hash16).
//   4. exe directory         last resort, only when (3) cannot be created or cannot be spelled in
//                            the ANSI code page. Reported as SRC_EXE so the caller can say so.
//
// THE LOGS ROOT FOLLOWS FOR FREE: `<config dir>logs`. In portable mode that is `<exe>\logs`, exactly
// what it has always been; in user storage it is `...\games\<hash16>\logs`. MH_LOG_ROOT (the
// launcher's own root) still beats it -- that is run_context.cpp's business, not this header's.
//
// THE G104 PROPERTY, STATED HERE BECAUSE EVERY CALLER DEPENDS ON IT. The answer is a function of
// three things and nothing else: the process image path (GetModuleFileName), the environment
// (MH_CONFIG_DIR, LOCALAPPDATA) and the filesystem (does `mh_net.ini` exist beside the exe). It
// reads no other module's state and needs no init call, so it is correct for WHOEVER ASKS FIRST --
// DllMain's crash marker, the config selector, the harness, the transports. There is no ordering to
// get right, which is precisely what G104 (a value that is right only if some other module's init
// ran first) cannot survive. It is computed ONCE per module and cached under a
// compare-exchange state machine, so concurrent first callers agree and later ones pay a load.
//
// THE CACHE IS PER MODULE (the functions are inline, so mh.dll, mh_net.dll and mh_net_udp.dll each
// hold a copy), and that is sound for the same reason: all copies compute the same function of the
// same three inputs. The one way they could disagree is `mh_net.ini` appearing or vanishing beside
// the exe BETWEEN two modules' first calls; nothing in the product does that (the launcher writes
// its ini before the process starts), and the first caller in practice is mh.dll's DllMain, before
// either satellite is loaded.
//
// HEADER-ONLY AND CRT-LIGHT ON PURPOSE. The harness DLL links neither mh_common nor mh_net_proto,
// and the injected context avoids CRT dependence, so the SHA-256 below is self-contained rather than
// borrowed from mh_net_proto. `net_selftest.exe runctxtest` pins it to the FIPS 180-4 vectors and
// to the same hash16 strings the launcher's paths.rs test asserts.
//
#ifndef MH_CONFIG_DIR_H
#define MH_CONFIG_DIR_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

namespace mh::cfgdir {

// Which rule produced the directory. Stable values: logged and asserted by number in selftests.
enum source_t {
    SRC_ENV      = 0, // MH_CONFIG_DIR
    SRC_PORTABLE = 1, // mh_net.ini beside the exe -> the exe directory
    SRC_USER     = 2, // %LOCALAPPDATA%\MissionHumanity\games\<hash16> (trailing backslash in the result)
    SRC_EXE      = 3, // fallback: user storage was unusable
};

inline const char *source_name(source_t s) {
    switch (s) {
        case SRC_ENV: return "env MH_CONFIG_DIR";
        case SRC_PORTABLE: return "portable (mh_net.ini beside the exe)";
        case SRC_USER: return "user storage (%LOCALAPPDATA%)";
        default: return "exe directory (user storage unusable)";
    }
}

namespace detail {

// ---- SHA-256 (FIPS 180-4), one-shot ------------------------------------------------------------
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void sha256_block(uint32_t h[8], const unsigned char *p) {
    static const uint32_t K[64] = {
        0x428a2f98,
        0x71374491,
        0xb5c0fbcf,
        0xe9b5dba5,
        0x3956c25b,
        0x59f111f1,
        0x923f82a4,
        0xab1c5ed5,
        0xd807aa98,
        0x12835b01,
        0x243185be,
        0x550c7dc3,
        0x72be5d74,
        0x80deb1fe,
        0x9bdc06a7,
        0xc19bf174,
        0xe49b69c1,
        0xefbe4786,
        0x0fc19dc6,
        0x240ca1cc,
        0x2de92c6f,
        0x4a7484aa,
        0x5cb0a9dc,
        0x76f988da,
        0x983e5152,
        0xa831c66d,
        0xb00327c8,
        0xbf597fc7,
        0xc6e00bf3,
        0xd5a79147,
        0x06ca6351,
        0x14292967,
        0x27b70a85,
        0x2e1b2138,
        0x4d2c6dfc,
        0x53380d13,
        0x650a7354,
        0x766a0abb,
        0x81c2c92e,
        0x92722c85,
        0xa2bfe8a1,
        0xa81a664b,
        0xc24b8b70,
        0xc76c51a3,
        0xd192e819,
        0xd6990624,
        0xf40e3585,
        0x106aa070,
        0x19a4c116,
        0x1e376c08,
        0x2748774c,
        0x34b0bcb5,
        0x391c0cb3,
        0x4ed8aa4a,
        0x5b9cca4f,
        0x682e6ff3,
        0x748f82ee,
        0x78a5636f,
        0x84c87814,
        0x8cc70208,
        0x90befffa,
        0xa4506ceb,
        0xbef9a3f7,
        0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch  = (e & f) ^ (~e & g);
        const uint32_t t1  = hh + S1 + ch + K[i] + w[i];
        const uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2  = S0 + maj;
        hh                 = g;
        g                  = f;
        f                  = e;
        e                  = d + t1;
        d                  = c;
        c                  = b;
        b                  = a;
        a                  = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

inline void sha256(const unsigned char *msg, unsigned len, unsigned char out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned off  = 0;
    for (; off + 64 <= len; off += 64) sha256_block(h, msg + off);
    // The tail: remaining bytes, 0x80, zero fill, 64-bit big-endian bit length -- one or two blocks.
    unsigned char  tail[128];
    const unsigned rem = len - off;
    for (unsigned i = 0; i < 128; ++i) tail[i] = 0;
    for (unsigned i = 0; i < rem; ++i) tail[i] = msg[off + i];
    tail[rem]              = 0x80;
    const unsigned blocks  = (rem + 1 + 8 <= 64) ? 1u : 2u;
    const uint64_t bit_len = (uint64_t)len * 8u;
    for (int i = 0; i < 8; ++i) tail[blocks * 64 - 1 - i] = (unsigned char)(bit_len >> (8 * i));
    for (unsigned b = 0; b < blocks; ++b) sha256_block(h, tail + 64 * b);
    for (int i = 0; i < 8; ++i) {
        out[4 * i]     = (unsigned char)(h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(h[i] >> 8);
        out[4 * i + 3] = (unsigned char)(h[i]);
    }
}

// ---- small string helpers (no CRT) --------------------------------------------------------------
inline bool is_sep(char c) { return c == '\\' || c == '/'; }
inline bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

inline BOOL  make_dir(const char *p) { return CreateDirectoryA(p, nullptr); }
inline BOOL  make_dir(const wchar_t *p) { return CreateDirectoryW(p, nullptr); }
inline DWORD attrs(const char *p) { return GetFileAttributesA(p); }
inline DWORD attrs(const wchar_t *p) { return GetFileAttributesW(p); }

// Create every missing level of `path` (no trailing-separator requirement). True when the final
// directory exists afterwards, whoever made it. Works on a private copy; `n` is the length.
template <typename C, int CAP>
inline bool make_dirs(const C *path) {
    C   buf[CAP];
    int n = 0;
    while (path[n] != 0 && n < CAP - 1) {
        buf[n] = path[n];
        ++n;
    }
    if (path[n] != 0) return false; // does not fit
    buf[n] = 0;
    while (n > 1 && is_sep(buf[n - 1]) && buf[n - 2] != (C)':') buf[--n] = 0;
    // A drive root (`C:\`) is never "created"; a UNC `\\server` attempt just fails harmlessly.
    for (int i = 1; i < n; ++i) {
        if (!is_sep(buf[i]) || buf[i - 1] == (C)':') continue;
        const C keep = buf[i];
        buf[i]       = 0;
        make_dir(buf); // ok if it already exists; a real failure surfaces at the end
        buf[i] = keep;
    }
    make_dir(buf);
    const DWORD a = attrs(buf);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline int len_of(const char *s) {
    int n = 0;
    while (s[n] != 0) ++n;
    return n;
}
inline int len_of(const wchar_t *s) {
    int n = 0;
    while (s[n] != 0) ++n;
    return n;
}

} // namespace detail

// ---- the launcher's hash -----------------------------------------------------------------------
//
// MIRRORS src/launcher/src/paths.rs `game_dir_hash` EXACTLY: the UTF-8 text of the game directory,
// trailing `\` and `/` trimmed, ASCII A-Z folded to a-z (and nothing else -- Rust's
// to_ascii_lowercase leaves Cyrillic and accented letters alone), SHA-256, first 8 bytes as 16
// lower-case hex digits. `out` needs 17 bytes. False only when the text is absurdly long.
//
// The input is UTF-8, not the ANSI path the rest of this tree carries, because the launcher hashes
// Rust's UTF-8 view of the path: an ANSI hash would agree on ASCII installs and silently disagree on
// every Cyrillic one (this game's audience). Callers holding the ANSI path from GetModuleFileNameA
// must go through GetModuleFileNameW (see real_inputs) -- never hash the ANSI string.
inline bool game_dir_hash16(const char *utf8, char out[17]) {
    out[0] = '\0';
    if (utf8 == nullptr) return false;
    char      buf[MAX_PATH * 4];
    const int cap = (int)sizeof(buf);
    int       n   = 0;
    for (; utf8[n] != 0; ++n) {
        if (n >= cap - 1) return false;
        const char c = utf8[n];
        buf[n]       = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    while (n > 0 && detail::is_sep(buf[n - 1])) --n;
    unsigned char digest[32];
    detail::sha256((const unsigned char *)buf, (unsigned)n, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        out[2 * i]     = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[16] = '\0';
    return true;
}

// ---- the pure resolver --------------------------------------------------------------------------
//
// Everything the answer depends on, handed in. The cached accessors below fill it from the process;
// the selftest fills it from temp directories, which is how all three arms (and the fallback) are
// proven without touching the real %LOCALAPPDATA% or the test binary's own folder.
struct inputs {
    const char    *exe_dir;       // ANSI, WITH trailing separator -- where mh.exe lives
    const char    *game_dir_utf8; // the same directory as UTF-8 (hash input); separators optional
    const char    *env_dir;       // value of MH_CONFIG_DIR, or nullptr / "" when unset
    const wchar_t *local_appdata; // value of %LOCALAPPDATA%, or nullptr / L"" when unset
};

struct result {
    char     dir[MAX_PATH]; // WITH trailing backslash
    source_t source;
};

// Longest config directory accepted from rules 1 and 3. Callers append leaf names and `logs\<name>\`
// into MAX_PATH buffers; a directory that leaves less room than this would turn every join into a
// silent truncation, so it is rejected up front and the next rule gets a turn (LA10 was a ~230-char
// install -- the exe-dir rules cannot reject, they are what the player's install already is).
constexpr int kMaxDirLen = MAX_PATH - 64;

inline bool user_dir_ansi(const wchar_t *local_appdata, const char *hash16, char *out_dir) {
    wchar_t        w[2 * MAX_PATH];
    int            n    = 0;
    const wchar_t *base = local_appdata;
    int            bl   = detail::len_of(base);
    while (bl > 0 && detail::is_sep(base[bl - 1])) --bl;
    static const wchar_t kTail[] = L"\\MissionHumanity\\games\\";
    const int            tl      = (int)(sizeof(kTail) / sizeof(kTail[0])) - 1;
    if (bl + tl + 16 + 1 + 1 > (int)(sizeof(w) / sizeof(w[0]))) return false;
    for (int i = 0; i < bl; ++i) w[n++] = base[i];
    for (int i = 0; i < tl; ++i) w[n++] = kTail[i];
    for (int i = 0; i < 16; ++i) w[n++] = (wchar_t)hash16[i];
    w[n++] = L'\\';
    w[n]   = 0;
    if (!detail::make_dirs<wchar_t, 2 * MAX_PATH>(w)) return false;

    // Spell it in the ANSI code page, which is what every file call in this tree uses. A profile
    // path the code page cannot represent (a Cyrillic user name on an English system) would come
    // back as '?'s that name a DIFFERENT, non-existent folder -- so the conversion must be lossless,
    // and when it is not, the 8.3 short name of the (now existing) directory is ASCII by construction.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const wchar_t *src = w;
        wchar_t        sh[2 * MAX_PATH];
        if (attempt == 1) {
            const DWORD got = GetShortPathNameW(w, sh, (DWORD)(sizeof(sh) / sizeof(sh[0])));
            if (got == 0 || got >= (DWORD)(sizeof(sh) / sizeof(sh[0]))) return false;
            src = sh;
        }
        BOOL lossy = FALSE;
        char a[MAX_PATH];
        // A UTF-8 ACP (the beta "Unicode UTF-8" setting) represents everything and rejects the
        // lossy-conversion arguments outright, so it takes the plain call.
        const int got = (GetACP() == CP_UTF8)
                            ? WideCharToMultiByte(CP_ACP, 0, src, -1, a, (int)sizeof(a), nullptr, nullptr)
                            : WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, src, -1, a, (int)sizeof(a), nullptr, &lossy);
        if (got <= 0 || lossy) continue;
        int al = got - 1;
        if (al > kMaxDirLen) return false;
        if (al == 0 || !detail::is_sep(a[al - 1])) {
            if (al + 1 >= (int)sizeof(a)) return false;
            a[al++] = '\\';
            a[al]   = '\0';
        }
        for (int i = 0; i <= al; ++i) out_dir[i] = a[i];
        return true;
    }
    return false;
}

inline source_t resolve(const inputs &in, result &out) {
    out.dir[0] = '\0';

    // 1. explicit override.
    if (in.env_dir != nullptr && in.env_dir[0] != '\0') {
        char env[MAX_PATH];
        int  n = 0;
        for (; in.env_dir[n] != 0 && n < MAX_PATH - 2; ++n) env[n] = in.env_dir[n];
        env[n] = '\0';
        if (in.env_dir[n] == 0) {
            while (n > 1 && detail::is_sep(env[n - 1]) && env[n - 2] != ':') env[--n] = '\0';
            if (n <= kMaxDirLen && detail::make_dirs<char, MAX_PATH>(env)) {
                int m = n;
                for (int i = 0; i < n; ++i) out.dir[i] = env[i];
                if (!detail::is_sep(out.dir[m - 1])) out.dir[m++] = '\\';
                out.dir[m]        = '\0';
                return out.source = SRC_ENV;
            }
        }
        // unusable override: fall through, as MH_LOG_ROOT does -- a typo must not brick a launch.
    }

    // 2. portable mode: an ini beside the exe.
    {
        char              ini[MAX_PATH];
        static const char kIni[] = "mh_net.ini";
        const int         dl     = detail::len_of(in.exe_dir);
        if (dl + (int)sizeof(kIni) <= MAX_PATH) {
            for (int i = 0; i < dl; ++i) ini[i] = in.exe_dir[i];
            for (int i = 0; i < (int)sizeof(kIni); ++i) ini[dl + i] = kIni[i];
            const DWORD a = GetFileAttributesA(ini);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                for (int i = 0; i <= dl; ++i) out.dir[i] = in.exe_dir[i];
                return out.source = SRC_PORTABLE;
            }
        }
    }

    // 3. user storage.
    char h16[17];
    if (in.local_appdata != nullptr && in.local_appdata[0] != L'\0' &&
        game_dir_hash16(in.game_dir_utf8, h16) && user_dir_ansi(in.local_appdata, h16, out.dir))
        return out.source = SRC_USER;

    // 4. last resort.
    const int dl = detail::len_of(in.exe_dir);
    for (int i = 0; i <= dl && i < MAX_PATH - 1; ++i) out.dir[i] = in.exe_dir[i];
    out.dir[MAX_PATH - 1] = '\0';
    return out.source     = SRC_EXE;
}

// ---- legacy key adoption (dist RL4) -------------------------------------------------------------
//
// Before v0.2.0 the key lived beside the exe. A player who launches the game by hand (no launcher, so
// no launcher migration) and has `mh_key.txt` there but no `mh_net.ini` would otherwise resolve to user
// storage (rule 3), find no key, and quietly mint a NEW one -- cutting them off from every relay game
// they had been playing. So, once, when the config directory is user storage and holds no key:
// COPY the exe-side key in. A copy, not a move: the exe folder may be read-only, and the launcher's
// migration owns removing it (with a backup). Never overwrites a key that exists in the config
// directory, and only for SRC_USER (portable mode IS the exe directory; an env override is explicit).
inline bool adopt_legacy_key(const char *exe_dir, const result &r) {
    if (r.source != SRC_USER || exe_dir == nullptr) return false;
    static const char kKey[] = "mh_key.txt";
    const int         el     = detail::len_of(exe_dir);
    const int         cl     = detail::len_of(r.dir);
    if (el + (int)sizeof(kKey) > MAX_PATH || cl + (int)sizeof(kKey) > MAX_PATH) return false;
    char from[MAX_PATH], to[MAX_PATH];
    for (int i = 0; i < el; ++i) from[i] = exe_dir[i];
    for (int i = 0; i <= (int)sizeof(kKey) - 1; ++i) from[el + i] = kKey[i];
    for (int i = 0; i < cl; ++i) to[i] = r.dir[i];
    for (int i = 0; i <= (int)sizeof(kKey) - 1; ++i) to[cl + i] = kKey[i];
    if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES) return false; // a key (or a dir) is there: it wins
    const DWORD fa = GetFileAttributesA(from);
    if (fa == INVALID_FILE_ATTRIBUTES || (fa & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    return CopyFileA(from, to, TRUE) != 0;
}

// ---- the real process ---------------------------------------------------------------------------
struct real_buffers {
    char    exe_dir[MAX_PATH];
    char    game_utf8[MAX_PATH * 4];
    char    env_dir[MAX_PATH];
    wchar_t local_appdata[2 * MAX_PATH];
};

// Fill `b` from the process: image path (ANSI for composing, wide->UTF-8 for the hash), the two
// environment variables. `b` must outlive the returned `inputs`.
inline inputs real_inputs(real_buffers &b) {
    b.exe_dir[0] = b.game_utf8[0] = b.env_dir[0] = '\0';
    b.local_appdata[0]                           = L'\0';

    GetModuleFileNameA(nullptr, b.exe_dir, MAX_PATH);
    char *slash = nullptr;
    for (char *p = b.exe_dir; *p != '\0'; ++p)
        if (detail::is_sep(*p)) slash = p;
    if (slash != nullptr) slash[1] = '\0';

    wchar_t     wexe[2 * MAX_PATH];
    const DWORD wn = GetModuleFileNameW(nullptr, wexe, (DWORD)(sizeof(wexe) / sizeof(wexe[0])));
    if (wn > 0 && wn < (DWORD)(sizeof(wexe) / sizeof(wexe[0]))) {
        int last = -1;
        for (int i = 0; wexe[i] != 0; ++i)
            if (detail::is_sep(wexe[i])) last = i;
        if (last >= 0) wexe[last + 1] = 0;
        WideCharToMultiByte(CP_UTF8, 0, wexe, -1, b.game_utf8, (int)sizeof(b.game_utf8), nullptr, nullptr);
    }

    const DWORD en = GetEnvironmentVariableA("MH_CONFIG_DIR", b.env_dir, MAX_PATH);
    if (en == 0 || en >= MAX_PATH) b.env_dir[0] = '\0';
    const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", b.local_appdata, (DWORD)(sizeof(b.local_appdata) / sizeof(b.local_appdata[0])));
    if (ln == 0 || ln >= (DWORD)(sizeof(b.local_appdata) / sizeof(b.local_appdata[0]))) b.local_appdata[0] = L'\0';

    inputs in;
    in.exe_dir       = b.exe_dir;
    in.game_dir_utf8 = b.game_utf8;
    in.env_dir       = b.env_dir;
    in.local_appdata = b.local_appdata;
    return in;
}

namespace detail {

struct cache_t {
    volatile LONG state; // 0 = unset, 1 = computing, 2 = ready
    result        r;
    char          exe_dir[MAX_PATH];
};

inline cache_t &cache() {
    static cache_t c; // zero-initialised
    return c;
}

inline cache_t &ensure() {
    cache_t &c = cache();
    if (c.state == 2) return c;
    if (InterlockedCompareExchange(&c.state, 1, 0) == 0) {
        static real_buffers b; // large; static keeps it off the (possibly small) DllMain stack
        const inputs        in = real_inputs(b);
        resolve(in, c.r);
        adopt_legacy_key(in.exe_dir, c.r); // RL4: a hand-launch player keeps the key they had
        const int n = len_of(in.exe_dir);
        for (int i = 0; i <= n; ++i) c.exe_dir[i] = in.exe_dir[i];
        MemoryBarrier();
        c.state = 2;
    } else {
        while (c.state != 2) Sleep(0); // another thread is computing
    }
    return c;
}

} // namespace detail

// THE DIRECTORY (trailing backslash). Created. Cached for the life of the module.
inline const char *config_dir() { return detail::ensure().r.dir; }
inline source_t    source() { return detail::ensure().r.source; }
// True when the config directory IS the exe directory (rule 2, or the rule-4 fallback).
inline bool portable() {
    const source_t s = source();
    return s == SRC_PORTABLE || s == SRC_EXE;
}
// The exe's own directory (trailing backslash), for the things that belong to the installation.
inline const char *exe_dir() { return detail::ensure().exe_dir; }

// "<config dir><leaf>" into out. False (out untouched) when it would not fit `cap`.
inline bool join(char *out, int cap, const char *leaf) {
    const char *d  = config_dir();
    const int   dl = detail::len_of(d);
    const int   ll = detail::len_of(leaf);
    if (dl + ll + 1 > cap) return false;
    for (int i = 0; i < dl; ++i) out[i] = d[i];
    for (int i = 0; i <= ll; ++i) out[dl + i] = leaf[i];
    return true;
}

// "<config dir>mh_net.ini" -- the one configuration file. Every ini read in the tree starts here.
inline void ini_path(char *out /* MAX_PATH */) {
    if (!join(out, MAX_PATH, "mh_net.ini")) out[0] = '\0';
}

// Selftest hook ONLY: forget the cached answer so the next call re-resolves from the (changed)
// environment. Nothing in the product calls this.
inline void reset_cache_for_test() {
    detail::cache_t &c = detail::cache();
    c.state            = 0;
}

} // namespace mh::cfgdir

#endif // MH_CONFIG_DIR_H
