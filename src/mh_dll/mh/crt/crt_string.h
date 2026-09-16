//
// crt/crt_string.h -- the vendored byte/string CRT (LIB-CRT, the MSVC-LOCAL and part of the
// VENDOR-EQUIV class).
//
// WHAT THIS REPLACES. Seven callees the libmh-destined modules reach through a fixed VA into the
// game's statically-linked Watcom CRT. crt/crt_select.h's `MH_CRT(fn)` picks between the VA (hosted,
// unchanged forever) and these bodies (standalone). Sites and shapes as measured 2026-09-08:
//
//     utils_fill_data     @0x004d1780  11 sites   memset, returning the destination
//     utils_w_str_copy    @0x004d02d2   6 sites   wcscpy(dst, src) -- NOTE the reversed parameter
//                                                 order, see below
//     utils_concat        @0x004d02ea   4 sites   wcscat
//     utils_str_cmp       @0x004d16d0   2 sites   strcmp, NORMALISED to -1/0/+1
//     utils_str_cmp_ci    @0x004de2ae   1 site    ASCII-only case-insensitive strcmp, NOT normalised
//     atoi                @0x004daa6e   3 sites   Watcom atoi -- wrapping, no overflow detection
//     strtok              @0x004dab17   1 site    Watcom strtok, with our own saved pointer
//
// ---- WHY THESE ARE WRITTEN OUT RATHER THAN FORWARDED TO MSVC's CRT -------------------------------
//
// Four of the seven are NOT their standard-library namesakes, and forwarding would be a silent
// behaviour change at a call site nobody would think to re-read:
//
//   * `utils_str_cmp` returns -1/0/+1 (`SBB EAX,EAX` / `OR AL,1` @0x004d176a). MSVC's `strcmp` is
//     free to return the byte difference. A caller that tests `== 1` -- and `tact_mission_load` does
//     compare against a literal -- reads a different answer from each.
//   * `utils_str_cmp_ci` lowercases ONLY 'A'-'Z' (@0x004de2ba, a bare 0x41..0x5a range test), so it
//     is locale-independent by construction. `_stricmp` is locale-sensitive, and the game's data
//     files carry Latin-2 bytes above 0x7f that a Polish or a C locale would fold differently.
//   * `atoi` accumulates in wrapping 32-bit arithmetic with NO overflow detection (@0x004daabb) and
//     recognises exactly six whitespace bytes. MSVC's `atoi` is `strtol`-based and clamps.
//   * `strtok` keeps its saved pointer in the CRT's per-thread block (thread_data+0x10). Ours keeps
//     it in this header. That is a real difference and it is why the sequence is a fixture case.
//
// The other three are genuine byte ops with nothing to diverge on, and this file's doctrine
// (the endgame plan D-E4) already sends that class to MSVC locally -- but they are spelled out
// anyway, because a one-line loop is cheaper to READ than the argument for why a delegation is safe,
// and because the reference arm in `crttest` has to sweep them either way.
//
// ---- TWO TRAPS IN THE SIGNATURES, both load-bearing ----------------------------------------------
//
// 1. `utils_w_str_copy(src, dst)` TAKES THE SOURCE FIRST. The original is a register-argument
//    function whose destination is in EAX and whose source is in EDX, and the generated wrapper in
//    addr/mh_calls.gen.h therefore reads `utils_w_str_copy(void *src, void *dst)` -- EDX is the
//    FIRST declared parameter for that shape (`s_u32_EDX_EAX`). `utils_concat(dst, src)` is the
//    other way round (`s_u32_EAX_EDX`). The two adjacent wide-string helpers disagree, and
//    `crttest` case A pins both by taking a function pointer through one declared type.
// 2. `utils_fill_data(ptr, size, byte)` returns the ORIGINAL pointer, not the end. The original
//    brackets its call to FastFillData with PUSH EAX / POP EAX (@0x004d1780, @0x004d1796) precisely
//    because FastFillData advances EAX.
//
// ---- ONE DELIBERATE DIFFERENCE FROM THE ORIGINAL, and it is not a divergence ---------------------
//
// The original `utils_str_cmp` compares FOUR BYTES AT A TIME (@0x004d16d8) with the classic
// `x + 0xfefefeff & ~x & 0x80808080` zero-byte test, so it reads up to three bytes PAST the
// terminating NUL. That is safe in the original's flat, never-unmapped data segment and it is not
// safe in ours -- ASan would fault on a heap string sized exactly to its length. Ours is a byte loop.
// The RESULT is identical for every input (the reference arm in `crttest` sweeps that), only the
// access pattern differs. Recorded here rather than in a commit message because the next person to
// "optimise this back" needs the reason in front of them.
//
// The offline oracle is `net_selftest crttest` (mh_nettest/crt_vendor_selftest.cpp), whose reference
// arm is the original assembly transcribed verbatim into `__declspec(naked)` functions.
//
#pragma once

#include <cstdint>
#include <cstring>

namespace mh::crt {

namespace detail {

// The Watcom ctype table @0x005073a4 is indexed by (c + 1) & 0xff. Its two bits that atoi reads were
// dumped out of the binary 2026-09-08 rather than assumed from <cctype>:
//   bit 0x02 (leading-skip) set for exactly {9, 10, 11, 12, 13, 32}
//   bit 0x20 (digit)        set for exactly {'0' ... '9'}
// So the predicates below ARE the table for atoi's purposes, and no locale can move them.
constexpr bool crt_isspace(unsigned char c) {
    return c == ' ' || (c >= 9 && c <= 13);
}
constexpr bool crt_isdigit(unsigned char c) {
    return c >= '0' && c <= '9';
}

// @0x004de2ba / @0x004de2ca: a bare 0x41..0x5a range test, applied to the byte value. Not <cctype>'s
// `tolower`, which is locale-sensitive and would fold the Latin-2 bytes in the game's data files.
constexpr unsigned char lower_az(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 0x20) : c;
}

} // namespace detail

// ---- utils_fill_data @0x004d1780 -----------------------------------------------------------------
//
// Broadcasts DL into all four bytes of EDX (@0x004d1782-0x004d178c) and calls FastFillData
// @0x004e1370, which fills `size` BYTES -- ECX is a byte count, not a dword count (it aligns EAX to 4
// one byte at a time, then does size/4 dwords and size&3 trailing bytes). size == 0 returns
// immediately (@0x004e1370 `OR ECX,ECX / JZ`), so a zero fill is a no-op, not a 4 GiB one.
inline void *utils_fill_data(void *ptr, uint32_t size, uint8_t default_) {
    if (size != 0u) std::memset(ptr, default_, size);
    return ptr;
}

// ---- utils_w_str_copy @0x004d02d2 ----------------------------------------------------------------
//
// wcscpy. The declared order is (src, dst) -- see trap 1 in the banner. Copies UTF-16 units up to and
// including the terminator and returns the DESTINATION (EBX, saved at @0x004d02d4).
inline void *utils_w_str_copy(void *src, void *dst) {
    const uint16_t *s = static_cast<const uint16_t *>(src);
    uint16_t       *d = static_cast<uint16_t *>(dst);
    for (;;) {
        const uint16_t c = *s++;
        *d++             = c;
        if (c == 0u) break;
    }
    return dst;
}

// ---- utils_concat @0x004d02ea --------------------------------------------------------------------
//
// wcscat: walk `dst` to its terminator (@0x004d02ee), then copy `src` over it including the
// terminator, and return the ORIGINAL `dst`.
inline void *utils_concat(void *dst, void *src) {
    uint16_t       *d = static_cast<uint16_t *>(dst);
    const uint16_t *s = static_cast<const uint16_t *>(src);
    while (*d != 0u) ++d;
    for (;;) {
        const uint16_t c = *s++;
        *d++             = c;
        if (c == 0u) break;
    }
    return dst;
}

// ---- utils_str_cmp @0x004d16d0 -------------------------------------------------------------------
//
// strcmp NORMALISED to -1/0/+1 (@0x004d176a), with an identical-pointer early-out that returns 0
// (@0x004d16d4). The comparison is on UNSIGNED bytes -- `CMP AL,CL` sets CF by unsigned order, and
// `SBB EAX,EAX / OR AL,1` turns CF into -1 and its absence into +1.
inline int32_t utils_str_cmp(char *param_1, char *param_2) {
    if (param_1 == param_2) return 0;
    const unsigned char *a = reinterpret_cast<const unsigned char *>(param_1);
    const unsigned char *b = reinterpret_cast<const unsigned char *>(param_2);
    for (;; ++a, ++b) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        if (*a == 0u) return 0;
    }
}

// ---- utils_str_cmp_ci @0x004de2ae ----------------------------------------------------------------
//
// NOT normalised: it returns the DIFFERENCE of the two lowered bytes, as unsigned values widened to
// int (@0x004de2e3-0x004de2f0). The loop stops on the first differing lowered byte, or when the two
// agree and are NUL. There is no identical-pointer early-out here -- the original does not have one.
inline int32_t utils_str_cmp_ci(char *param_1, char *param_2) {
    const unsigned char *a = reinterpret_cast<const unsigned char *>(param_1);
    const unsigned char *b = reinterpret_cast<const unsigned char *>(param_2);
    for (;; ++a, ++b) {
        const unsigned char la = detail::lower_az(*a);
        const unsigned char lb = detail::lower_az(*b);
        if (la != lb || lb == 0u) return static_cast<int32_t>(la) - static_cast<int32_t>(lb);
    }
}

// ---- atoi @0x004daa6e ----------------------------------------------------------------------------
//
// Watcom's atoi, which is NOT strtol underneath: no overflow detection, no base prefix, no errno.
// The sign character is sampled ONCE (@0x004daa8f, into CL) before the optional '+'/'-' is skipped,
// and re-tested at the end (@0x004daacc) -- so a lone "-" yields 0 negated, i.e. 0, and "--5" stops
// at the second '-' and yields 0.
//
// The accumulation is spelled in UNSIGNED arithmetic here for exactly the reason crt_sprintf.h's
// `render_int` is: the original's `IMUL EBX,EBX,0xa` / `NEG EBX` wrap silently, and reproducing that
// in signed C++ is undefined behaviour, not a faithful translation. The bit pattern is the same.
inline int32_t atoi(char *nptr) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(nptr);
    while (detail::crt_isspace(*p)) ++p;

    const unsigned char sign = *p;
    if (sign == '+' || sign == '-') ++p;

    uint32_t acc = 0u;
    while (detail::crt_isdigit(*p)) {
        acc = acc * 10u + static_cast<uint32_t>(*p) - 0x30u;
        ++p;
    }
    if (sign == '-') acc = 0u - acc;
    return static_cast<int32_t>(acc);
}

// ---- strtok @0x004dab17 --------------------------------------------------------------------------
//
// THE ONE PIECE OF MUTABLE STATE THIS HEADER OWNS. The original parks its saved pointer in the CRT's
// per-thread block (thread_data+0x10, written @0x004dab95 / @0x004dabac). libmh has no such block, so
// it lives here. libmh's sim is single-threaded, which is the same assumption `unsupported_seen` in
// crt_sprintf.h already makes; if that ever stops being true this is one of the two cells to move.
//
// Behaviour transcribed from the assembly, including the parts standard strtok leaves unspecified:
//   * a NULL `str` with no saved pointer returns NULL without touching the delimiter set
//     (@0x004dab2b);
//   * a run that finds only delimiters returns NULL and DOES NOT clear the saved pointer
//     (@0x004dab68 jumps to the NULL return, past every write to +0x10) -- so a subsequent
//     strtok(NULL, ...) resumes from wherever the previous successful call left off;
//   * reaching the terminator with a token in hand clears the saved pointer (@0x004dabac).
// The delimiter set is built by __setbits @0x004e9299 as a 32-byte bitmap over all 256 byte values,
// and it stops at the delimiter string's NUL -- so '\0' is never itself a delimiter.
inline char *strtok_saved = nullptr;

inline char *strtok(char *str, char *delim) {
    char *p = str;
    if (p == nullptr) {
        p = strtok_saved;
        if (p == nullptr) return nullptr;
    }

    // __setbits @0x004e9299: map[c >> 3] |= 1 << (c & 7), for each byte of `delim`.
    unsigned char map[32] = {0};
    for (const unsigned char *d = reinterpret_cast<const unsigned char *>(delim); *d != 0u; ++d) {
        map[*d >> 3] |= static_cast<unsigned char>(1u << (*d & 7u));
    }
    const auto is_delim = [&map](unsigned char c) {
        return (map[c >> 3] & (1u << (c & 7u))) != 0u;
    };

    // @0x004dab5e-0x004dab64: skip leading delimiters.
    unsigned char c = 0u;
    for (;;) {
        c = static_cast<unsigned char>(*p);
        if (c == 0u || !is_delim(c)) break;
        ++p;
    }
    if (c == 0u) return nullptr; // @0x004dab66-0x004dab68 -- saved pointer left as it was.

    // @0x004dab9e-0x004dabb3: run to the next delimiter or to the terminator.
    for (char *q = p;; ++q) {
        const unsigned char qc = static_cast<unsigned char>(*q);
        if (qc == 0u) {
            strtok_saved = nullptr;
            return p;
        }
        if (is_delim(qc)) {
            *q           = '\0';
            strtok_saved = q + 1;
            return p;
        }
    }
}

} // namespace mh::crt
