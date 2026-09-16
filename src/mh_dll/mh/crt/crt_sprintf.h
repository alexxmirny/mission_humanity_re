//
// crt/crt_sprintf.h -- the vendored sprintf family (LIB-CRT).
//
// WHAT THIS REPLACES. 39 sites in the libmh-destined modules format text through the game's own
// statically-linked Watcom CRT, reached as a VA: `utils_sprintf` @0x004cfb9c (narrow, 6 sites) and
// `w_sprintf` @0x004d0320 (wide, 33 sites), each marshalled through one of 12 fixed-arity shapes in
// addr/mh_calls.gen.h. A standalone libmh cannot call a VA, so this header supplies signature-
// identical bodies and crt/crt_select.h picks between them.
//
// ---- WHY THIS IS A HAND FORMATTER AND NOT A CALL TO MSVC's swprintf -------------------------------
//
// Delegating would reintroduce exactly the dependency LIB-CRT exists to remove. The wide family's
// `%s` is the specific trap: MSVC's `swprintf` reads `%s` as WIDE by default but as NARROW when
// `_CRT_STDIO_ISO_WIDE_SPECIFIERS` is defined, so the meaning of every one of the 33 wide sites would
// hang on a preprocessor symbol set somewhere else in the build. Watcom's `__uprtf` has its own
// answer to the same question. Rather than establish which flag makes two vendors agree -- a fact
// that is true until someone edits a property sheet -- the conversions are spelled out here, where
// they are readable, testable offline, and cannot be changed by a build setting.
//
// ---- THE VOCABULARY IS FIVE CONVERSIONS, MEASURED, NOT ASSUMED -----------------------------------
//
// Every format string reachable from the 39 sites was read (2026-09-08). The complete live set:
//
//     %s      both families -- L"%s (%s)", L"%s: %s", L"%s", "panelb_%s.gfx", L"ENTER - %s, ESC - %s"
//     %d      L"[%d,%d]", "poz%do.dat"/"poz%dl.dat", the two u"(%d) %d" control-mode lines
//     %2d     ": %2d", L"%2d" (_G_LLM_TACT_SIDEBAR_FMT_UNIT_ID), L"%2d.%s"
//     %02d    "init\\AI%02d.SCR", "%s%s_%02d%02d.DMP"
//     %.2f    sim_debug_roll_random ONLY -- L"random %.2f  %d", a named debug line
//
// FOUR of those formats are GAME DATA, not source literals, so they could not be simplified even if
// we wanted to: _G_LLM_TACT_SIDEBAR_FMT_UNIT_ID @0x005004d8 (byte-verified L"%2d"),
// _G_LLM_TACT_SIDEBAR_FMT_UNIT_NAME @0x005004e0 (L"%s"), and the control-mode pair u__005011cc /
// u__00501250. They are OWN_READONLY regions, but a localisation or a mod edits the text tables, so
// this formatter must interpret an ARBITRARY format string rather than only the five above. That is
// why unsupported conversions have a defined, non-crashing behaviour (see `unsupported_seen`).
//
// ---- WHAT BIT-EQUIVALENCE IS ACTUALLY WORTH HERE, MEASURED ---------------------------------------
//
// No sprintf destination in the migrated set reaches the lockstep hash or a savegame -- checked
// against the whole generated region table, not sampled:
//
//     G_TEXT_TMP @0x00e15178                       MF_VIEW | MF_MEASURED    (all 33 wide sites; the
//                                                  tact `text_scratch()` accessor is this region)
//     _G_LLM_STRAT_LAND_DMP_PATH_SCRATCH @0xe58146 MF_VIEW
//     _G_LLM_STRAT_DMP_PATH_SCRATCH @0x00e58245    MF_VIEW
//     scr_planet / mission_file / the panel buffer  local stack in our translation
//
// CORRECTING A STALE CLAIM: tools/data/varargs_shapes.json still says G_TEXT_TMP is "MF_VIEW only --
// not MF_HASH, MF_SAVE or MF_MEASURED" (measured 2026-08-08, for 9 sites). MF_MEASURED is set. Its
// conclusion survives -- formatting cannot reach determinism -- but the reason changes, and so does
// the consequence: because the region IS shadow-tracked, every armed shadow site that formats into
// it BYTE-COMPARES the result, so a divergence here goes red rather than passing unnoticed.
//
// So the risk splits, and the two halves want different evidence:
//
//   the 33 WIDE sites   are gated twice already -- the shadow compare on G_TEXT_TMP, and the UI
//                       regression baselines that render the text. A difference is caught.
//   the 6 NARROW sites  build FILENAMES ("init\AI03.SCR", "init\A_0103.DMP", "poz3o.dat",
//                       "panelb_x.gfx"). Nothing compares them, and a wrong one does not misdraw a
//                       pixel -- it silently loads a DIFFERENT FILE. This is where the fixture earns
//                       its keep, and it is why the offline cases sweep %02d past its field width,
//                       through 0, and through negatives.
//
// The offline oracle is `net_selftest crttest` (mh_nettest/crt_sprintf_selftest.cpp).
//
#pragma once

#include <cstdint>
#include <cstdio> // snprintf -- the one delegated conversion (%f); see the `case 'f'` note
#include <cstring>

namespace mh::crt {

namespace detail {

// The original's own argument model: Watcom's `utils_sprintf` does `local_10 = &param_3` and hands
// the engine a pointer to the first vararg, so arguments are a flat array of stack dwords. Keeping
// that model is what lets one core serve all 12 shapes, and it makes the 8-byte `double` of `%.2f`
// fall out correctly as two dwords rather than needing a special case at the wrapper.
struct arg_cursor {
    const uint32_t *p;

    uint32_t u32() { return *p++; }
    int32_t  i32() { return static_cast<int32_t>(*p++); }
    double   f64() {
        double d;
        std::memcpy(&d, p, sizeof(d));
        p += 2;
        return d;
    }
    template <class T>
    const T *ptr() {
        return reinterpret_cast<const T *>(static_cast<uintptr_t>(*p++));
    }
};

// A conversion this formatter does not implement was still REACHED -- only possible from a format
// string that came from the game's text tables, since the source literals are enumerated above. The
// counter exists so `crttest` can assert the live vocabulary needs none of it, and so a future
// mod-introduced conversion is visible as a number rather than as mangled text nobody traces back.
// Not thread-safe by design: it is a diagnostic, and libmh's sim is single-threaded.
inline uint32_t unsupported_seen = 0;

// ---- the integer conversion ----------------------------------------------------------------------
//
// Written out rather than delegated because it is the half that MUST be exact: it builds filenames.
// INT_MIN is the reason the digits are accumulated in UNSIGNED arithmetic -- negating it in `int32_t`
// is undefined, and the naive `-value` loop is the classic way to lose that one input.
//
// `out` receives at most 16 chars (11 digits + sign is the widest 32-bit case), returns the length.
inline int render_int(char *out, int32_t value, bool is_signed, int width, bool zero_pad) {
    char       digits[12];
    int        n   = 0;
    const bool neg = is_signed && value < 0;
    uint32_t   mag = neg ? (0u - static_cast<uint32_t>(value)) : static_cast<uint32_t>(value);
    do {
        digits[n++] = static_cast<char>('0' + (mag % 10u));
        mag /= 10u;
    } while (mag != 0u);

    int       len  = 0;
    const int body = n + (neg ? 1 : 0);
    if (zero_pad) {
        // The sign leads the zeros: %04d of -5 is "-005", never "0-05".
        if (neg) out[len++] = '-';
        for (int i = body; i < width; ++i) out[len++] = '0';
    } else {
        for (int i = body; i < width; ++i) out[len++] = ' ';
        if (neg) out[len++] = '-';
    }
    while (n > 0) out[len++] = digits[--n];
    return len;
}

// ---- the format walker ---------------------------------------------------------------------------
//
// One implementation over both char widths. `CH` is the character type of BOTH the format and the
// destination; `%s` therefore takes the same width as the format it appears in -- the explicit answer
// to the ISO-vs-Microsoft question in the banner. A narrow format's `%s` is a `char *`, a wide
// format's `%s` is a `wchar_t *`, and neither depends on a build flag.
//
// Returns the character count written, matching the originals (both return the length, and both then
// NUL-terminate at it -- `dst[iVar1] = '\0'` narrow, `*(dst + iVar1 * 2) = 0` wide).
template <class CH>
int format_core(CH *dst, const CH *fmt, arg_cursor args) {
    int out = 0;
    for (const CH *f = fmt; *f != CH(0); ++f) {
        if (*f != CH('%')) {
            dst[out++] = *f;
            continue;
        }
        ++f;
        if (*f == CH('%')) {
            dst[out++] = CH('%');
            continue;
        }
        if (*f == CH(0)) break; // a trailing bare '%' -- emit nothing, as both engines do

        bool zero_pad = false;
        while (*f == CH('0') || *f == CH('-') || *f == CH('+') || *f == CH(' ')) {
            if (*f == CH('0')) zero_pad = true;
            ++f; // '-'/'+'/' ' are accepted and ignored: no live format uses one
        }
        int width = 0;
        while (*f >= CH('0') && *f <= CH('9')) width = width * 10 + static_cast<int>(*f++ - CH('0'));
        int precision = -1;
        if (*f == CH('.')) {
            ++f;
            precision = 0;
            while (*f >= CH('0') && *f <= CH('9'))
                precision = precision * 10 + static_cast<int>(*f++ - CH('0'));
        }
        while (*f == CH('l') || *f == CH('h') || *f == CH('L')) ++f; // length modifiers: no-ops here

        switch (static_cast<int>(*f)) {
            case 's': {
                const CH *s = args.ptr<CH>();
                if (s == nullptr) break; // the originals would fault; libmh must not
                int n = 0;
                while (s[n] != CH(0)) ++n;
                for (int i = n; i < width; ++i) dst[out++] = CH(' ');
                for (int i = 0; i < n; ++i) dst[out++] = s[i];
                break;
            }
            case 'd':
            case 'i':
            case 'u': {
                char       tmp[24];
                const bool sgn = (*f != CH('u'));
                const int  n   = render_int(tmp, args.i32(), sgn, width, zero_pad);
                for (int i = 0; i < n; ++i) dst[out++] = static_cast<CH>(tmp[i]);
                break;
            }
            case 'f': {
                // THE ONE DELEGATED CONVERSION, and the one place a vendor difference could still hide.
                // It is reachable from a single site -- sim_debug_roll_random's L"random %.2f  %d", a
                // named debug line -- whose output lands in G_TEXT_TMP and is therefore shadow-compared,
                // so a Watcom/MSVC rounding disagreement shows up as a red arm rather than as silent
                // drift. Delegating is the honest trade: reimplementing correctly-rounded decimal
                // conversion to earn one debug string is a large amount of subtle code for no reachable
                // behaviour, and the gate that would catch the difference already exists.
                char tmp[64], spec[8];
                int  si        = 0;
                spec[si++]     = '%';
                spec[si++]     = '.';
                spec[si++]     = static_cast<char>('0' + (precision < 0 ? 6 : precision % 10));
                spec[si++]     = 'f';
                spec[si]       = '\0';
                const double d = args.f64();
                const int    n = std::snprintf(tmp, sizeof(tmp), spec, d);
                for (int i = 0; i < n; ++i) dst[out++] = static_cast<CH>(tmp[i]);
                break;
            }
            default:
                // Unreachable from any format string measured in this tree -- so if it fires, the format
                // came from a text table someone edited. Emit a marker of fixed width instead of either
                // crashing or dropping the conversion silently, and count it.
                ++unsupported_seen;
                dst[out++] = CH('<');
                dst[out++] = CH('?');
                dst[out++] = CH('>');
                break;
        }
    }
    dst[out] = CH(0); // both originals NUL-terminate at the returned length
    return out;
}

inline int narrow(void *dst, const char *fmt, const uint32_t *args) {
    return format_core<char>(static_cast<char *>(dst), fmt, arg_cursor{args});
}
inline int wide(void *dst, const wchar_t *fmt, const uint32_t *args) {
    return format_core<wchar_t>(static_cast<wchar_t *>(dst), fmt, arg_cursor{args});
}

inline uint32_t as_dw(const void *p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
inline uint32_t as_dw(int32_t v) { return static_cast<uint32_t>(v); }

} // namespace detail

// ---- the 12 shapes ---------------------------------------------------------------------------------
//
// Signature-identical to their addr/mh_calls.gen.h counterparts, so crt_select.h can substitute one
// for the other at a call site with no other edit. The shape suffix reads left to right after the
// destination: `vss` = (void *dst, format, wchar_t *, wchar_t *).

inline int32_t utils_sprintf__vi(void *dst, const char *format, int32_t a0) {
    const uint32_t a[] = {detail::as_dw(a0)};
    return detail::narrow(dst, format, a);
}
inline int32_t utils_sprintf__vs(void *dst, const char *format, const char *a0) {
    const uint32_t a[] = {detail::as_dw(a0)};
    return detail::narrow(dst, format, a);
}
inline int32_t utils_sprintf__vssii(void *dst, const char *format, const char *a0, const char *a1,
                                    int32_t a2, int32_t a3) {
    const uint32_t a[] = {detail::as_dw(a0), detail::as_dw(a1), detail::as_dw(a2), detail::as_dw(a3)};
    return detail::narrow(dst, format, a);
}

inline int32_t w_sprintf__v(void *dst, const wchar_t *format) {
    const uint32_t a[] = {0u};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vi(void *dst, const wchar_t *format, int32_t a0) {
    const uint32_t a[] = {detail::as_dw(a0)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vs(void *dst, const wchar_t *format, const wchar_t *a0) {
    const uint32_t a[] = {detail::as_dw(a0)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vii(void *dst, const wchar_t *format, int32_t a0, int32_t a1) {
    const uint32_t a[] = {detail::as_dw(a0), detail::as_dw(a1)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vis(void *dst, const wchar_t *format, int32_t a0, const wchar_t *a1) {
    const uint32_t a[] = {detail::as_dw(a0), detail::as_dw(a1)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vss(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1) {
    const uint32_t a[] = {detail::as_dw(a0), detail::as_dw(a1)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vsss(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1,
                               const wchar_t *a2) {
    const uint32_t a[] = {detail::as_dw(a0), detail::as_dw(a1), detail::as_dw(a2)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__visis(void *dst, const wchar_t *format, int32_t a0, const wchar_t *a1,
                                int32_t a2, const wchar_t *a3) {
    const uint32_t a[] = {detail::as_dw(a0), detail::as_dw(a1), detail::as_dw(a2), detail::as_dw(a3)};
    return detail::wide(dst, format, a);
}
inline int32_t w_sprintf__vdi(void *dst, const wchar_t *format, double a0, int32_t a1) {
    // The double occupies TWO argument dwords, exactly as it does on the original's stack.
    uint32_t a[3];
    std::memcpy(a, &a0, sizeof(a0));
    a[2] = detail::as_dw(a1);
    return detail::wide(dst, format, a);
}

} // namespace mh::crt
