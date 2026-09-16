//
// crt_sprintf_selftest.cpp -- the offline oracle for the vendored sprintf family (`crttest`, LIB-CRT).
//
// WHAT IT PROVES, AND WHY IT IS NOT A CAPTURE-AND-REPLAY. LIB-CRT's done_when asks for bit-equivalence
// "against captured originals". For the sprintf family that framing turns out to be heavier than the
// question deserves, because the live vocabulary was measured and it is FIVE conversions -- %s, %d,
// %2d, %02d and one %.2f (crt/crt_sprintf.h enumerates every format string behind them). None of the
// four that matter has any vendor-dependent behaviour to capture: no locale, no rounding, no
// precision, no digit-count cutoff. Their meaning is fixed by the C standard, and Watcom's engine and
// this one either both implement it or one has a bug. So the expectations below are written out from
// the standard by hand rather than recorded from a running game -- which makes them READABLE as a
// specification, and independent of whether anyone can still boot the rig in five years.
//
// The one conversion that genuinely IS vendor-dependent, %.2f, is delegated to the host CRT rather
// than reimplemented, is reachable from exactly one named-debug site, and lands in a shadow-compared
// region. It is checked here for shape, not for equivalence -- the shadow arm is the check that can
// actually see a Watcom disagreement, and it exists already.
//
// WHERE THE EFFORT IS AIMED. Not evenly across the 12 shapes. 33 of the 39 sites format into
// G_TEXT_TMP (MF_VIEW | MF_MEASURED) and their return values are discarded -- gated twice already, by
// the shadow compare and by the UI baselines. The other 6 build FILENAMES, nothing compares them, and
// a wrong one silently loads a DIFFERENT FILE. Cases C and E are that set, swept past the %02d field
// width, through zero, and through negatives.
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <type_traits>

#include "addr/mh_calls.gen.h"
#include "crt/crt_sprintf.h"

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// ---- the comparison, with the failure message the negative arm needs ------------------------------
//
// Separated from `ck` so case I can drive it with a deliberately wrong expectation and assert it goes
// red AND names the shape. A compare that cannot be shown to fail is not evidence.
bool cmp_n(const char *shape, const char *got, const char *want, char *why, size_t why_n) {
    if (std::strcmp(got, want) == 0) return true;
    std::snprintf(why, why_n, "%s: got \"%s\", want \"%s\"", shape, got, want);
    return false;
}

bool cmp_w(const char *shape, const wchar_t *got, const wchar_t *want, char *why, size_t why_n) {
    if (std::wcscmp(got, want) == 0) return true;
    std::snprintf(why, why_n, "%s: wide result differs from expectation", shape);
    return false;
}

void ck_n(const char *shape, const char *got, const char *want) {
    char       why[256] = {0};
    const bool ok       = cmp_n(shape, got, want, why, sizeof(why));
    ck(ok, ok ? shape : why);
}

void ck_w(const char *shape, const wchar_t *got, const wchar_t *want) {
    char       why[256] = {0};
    const bool ok       = cmp_w(shape, got, want, why, sizeof(why));
    ck(ok, ok ? shape : why);
}

} // namespace

int run_crttest() {
    printf("=== crttest: the vendored CRT (LIB-CRT) ===\n");

    char    n[256];
    wchar_t w[256];

    // ---- A. THE SHAPES ARE IDENTICAL, checked by the compiler ------------------------------------
    //
    // crt/crt_select.h substitutes one namespace for the other at a call site with no other edit, so
    // a shape that drifts apart would bind a DIFFERENT ARITY and read the caller's stack past its
    // arguments. addr/mh_calls.gen.h is regenerated from the DB, so that drift is a real event and not
    // a hypothetical. Taking both addresses through one deduced type turns it into a build break.
#define MH_CRT_SAME_TYPE(fn)                                                               \
    do {                                                                                   \
        static_assert(std::is_same_v<decltype(&::mh::call::fn), decltype(&::mh::crt::fn)>, \
                      #fn ": mh::call:: and mh::crt:: signatures have drifted apart");     \
    } while (0)
    MH_CRT_SAME_TYPE(utils_sprintf__vi);
    MH_CRT_SAME_TYPE(utils_sprintf__vs);
    MH_CRT_SAME_TYPE(utils_sprintf__vssii);
    MH_CRT_SAME_TYPE(w_sprintf__v);
    MH_CRT_SAME_TYPE(w_sprintf__vi);
    MH_CRT_SAME_TYPE(w_sprintf__vs);
    MH_CRT_SAME_TYPE(w_sprintf__vii);
    MH_CRT_SAME_TYPE(w_sprintf__vis);
    MH_CRT_SAME_TYPE(w_sprintf__vss);
    MH_CRT_SAME_TYPE(w_sprintf__vsss);
    MH_CRT_SAME_TYPE(w_sprintf__visis);
    MH_CRT_SAME_TYPE(w_sprintf__vdi);
#undef MH_CRT_SAME_TYPE
    ck(true, "A: all 12 shapes match addr/mh_calls.gen.h (static_assert -- a drift is a build break)");

    // ---- B. THE LIVE FORMATS, exactly as the 39 sites use them -----------------------------------
    mh::crt::w_sprintf__vss(w, L"%s (%s)", L"Reinforcements", L"Blue");
    ck_w("B/vss L\"%s (%s)\"", w, L"Reinforcements (Blue)");

    mh::crt::w_sprintf__vss(w, L"%s: %s", L"Blue", L"gg");
    ck_w("B/vss L\"%s: %s\"", w, L"Blue: gg");

    mh::crt::w_sprintf__vs(w, L"%s", L"Planet lost");
    ck_w("B/vs L\"%s\"", w, L"Planet lost");

    mh::crt::w_sprintf__vii(w, L"[%d,%d]", 12, 34);
    ck_w("B/vii L\"[%d,%d]\"", w, L"[12,34]");

    mh::crt::w_sprintf__vis(w, L"%2d.%s", 7, L"Marine");
    ck_w("B/vis L\"%2d.%s\" (width 2, space pad)", w, L" 7.Marine");

    mh::crt::w_sprintf__vi(w, L"%2d", 3); // _G_LLM_TACT_SIDEBAR_FMT_UNIT_ID, byte-verified game data
    ck_w("B/vi L\"%2d\" (the sidebar unit-id format, read from the game's text table)", w, L" 3");

    mh::crt::w_sprintf__vsss(w, L"%s %s %s", L"a", L"b", L"c");
    ck_w("B/vsss three wide strings", w, L"a b c");

    mh::crt::w_sprintf__visis(w, L"%d%s%d%s", 1, L"-", 2, L"!");
    ck_w("B/visis interleaved int/string", w, L"1-2!");

    mh::crt::w_sprintf__v(w, L"Paused");
    ck_w("B/v a format with no conversions at all", w, L"Paused");

    mh::crt::utils_sprintf__vs(n, "panelb_%s.gfx", "sniper");
    ck_n("B/narrow vs \"panelb_%s.gfx\"", n, "panelb_sniper.gfx");

    // ---- C. THE FILENAMES -- the half nothing else checks ----------------------------------------
    //
    // A difference here does not misdraw a pixel; it opens another file. Swept past the field width,
    // through zero, and through a negative, because %02d's behaviour at each is a separate decision.
    mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", 0);
    ck_n("C: init\\AI%02d.SCR at 0", n, "init\\AI00.SCR");
    mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", 3);
    ck_n("C: init\\AI%02d.SCR at 3", n, "init\\AI03.SCR");
    mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", 99);
    ck_n("C: init\\AI%02d.SCR at the field width", n, "init\\AI99.SCR");
    mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", 100);
    ck_n("C: init\\AI%02d.SCR PAST the field width -- widens, never truncates", n, "init\\AI100.SCR");
    mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", -1);
    ck_n("C: init\\AI%02d.SCR negative -- the sign leads the zero pad", n, "init\\AI-1.SCR");

    mh::crt::utils_sprintf__vi(n, "poz%do.dat", 3);
    ck_n("C: the HUMAN tactical mission file", n, "poz3o.dat");
    mh::crt::utils_sprintf__vi(n, "poz%dl.dat", 12);
    ck_n("C: the ALIEN tactical mission file, two digits", n, "poz12l.dat");

    mh::crt::utils_sprintf__vssii(n, "%s%s_%02d%02d.DMP", "init\\", "H", 1, 3);
    ck_n("C: the AI base-layout DMP path (race letter is ONE char)", n, "init\\H_0103.DMP");
    mh::crt::utils_sprintf__vssii(n, "%s%s_%02d%02d.DMP", "init\\", "A", 0, 11);
    ck_n("C: the DMP path, alien, zero planet", n, "init\\A_0011.DMP");

    // ---- D. BOUNDARY INTEGERS --------------------------------------------------------------------
    //
    // INT_MIN is the one that catches the classic implementation bug: negating it in int32_t is
    // undefined, so a digit loop written as `-value` loses exactly this input and nothing else.
    mh::crt::w_sprintf__vi(w, L"%d", 0);
    ck_w("D: %d of 0", w, L"0");
    mh::crt::w_sprintf__vi(w, L"%d", -1);
    ck_w("D: %d of -1", w, L"-1");
    mh::crt::w_sprintf__vi(w, L"%d", 2147483647);
    ck_w("D: %d of INT_MAX", w, L"2147483647");
    mh::crt::w_sprintf__vi(w, L"%d", INT32_MIN);
    ck_w("D: %d of INT_MIN -- the input a naive negate loses", w, L"-2147483648");

    // ---- E. WIDTH AND PAD, stated as separate decisions -------------------------------------------
    mh::crt::w_sprintf__vi(w, L"%2d", 100);
    ck_w("E: %2d past its width widens", w, L"100");
    mh::crt::w_sprintf__vi(w, L"%02d", -5);
    ck_w("E: %02d of -5 -- sign first, then zeros", w, L"-5");
    mh::crt::w_sprintf__vi(w, L"%04d", -5);
    ck_w("E: %04d of -5 is \"-005\", never \"0-05\"", w, L"-005");
    mh::crt::w_sprintf__vi(w, L"%4d", -5);
    ck_w("E: %4d of -5 space-pads BEFORE the sign", w, L"  -5");

    // ---- F. %s TAKES THE FORMAT'S OWN CHARACTER WIDTH ---------------------------------------------
    //
    // The whole reason this formatter is hand-written. MSVC's swprintf reads %s as wide by default and
    // as NARROW under _CRT_STDIO_ISO_WIDE_SPECIFIERS; here the answer is in the code, so no build
    // setting can change what 33 call sites mean.
    mh::crt::w_sprintf__vs(w, L"[%s]", L"wide");
    ck_w("F: %s in a WIDE format consumes a wchar_t *", w, L"[wide]");
    mh::crt::utils_sprintf__vs(n, "[%s]", "narrow");
    ck_n("F: %s in a NARROW format consumes a char *", n, "[narrow]");

    // ---- G. THE RETURN VALUE AND THE TERMINATOR ---------------------------------------------------
    //
    // Both originals return the length and then NUL-terminate AT it (`dst[iVar1] = '\0'` narrow,
    // `*(dst + iVar1 * 2) = 0` wide). Every migrated site discards the return, but the terminator is
    // load-bearing for the filename sites, which pass the buffer straight to a file open.
    std::memset(w, 0x7f, sizeof(w));
    const int32_t rw = mh::crt::w_sprintf__vss(w, L"%s: %s", L"ab", L"cde");
    ck(rw == 7, "G: the wide shape returns the character count (7 for \"ab: cde\")");
    ck(w[7] == L'\0', "G: ...and the wide NUL lands exactly at it");
    std::memset(n, 0x7f, sizeof(n));
    const int32_t rn = mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", 7);
    ck(rn == 13, "G: the narrow shape returns the character count");
    ck(n[13] == '\0', "G: ...and the narrow NUL lands exactly at it -- the filename sites need this");

    // ---- H. AN UNSUPPORTED CONVERSION IS COUNTED, NOT A CRASH -------------------------------------
    //
    // Four of the live formats are GAME DATA (the two sidebar formats and the two control-mode lines),
    // so a localisation or a mod can introduce a conversion this formatter does not implement. The
    // required behaviour is "visible and survivable", never "fault in the sim".
    const uint32_t before = mh::crt::detail::unsupported_seen;
    mh::crt::w_sprintf__vi(w, L"<%q>", 5);
    ck(mh::crt::detail::unsupported_seen == before + 1,
       "H: an unimplemented conversion increments the counter");
    ck_w("H: ...and emits a fixed marker rather than mangling the line", w, L"<<?>>");
    const uint32_t clean = mh::crt::detail::unsupported_seen;
    mh::crt::w_sprintf__vss(w, L"%s (%s)", L"a", L"b");
    ck(mh::crt::detail::unsupported_seen == clean,
       "H: ...and the LIVE vocabulary trips it zero times (the over-refusal arm)");

    // ---- I. THE NEGATIVE ARM ----------------------------------------------------------------------
    //
    // Every case above is a compare, so the whole file is worth nothing unless a compare can be shown
    // to go red and to say which shape did. Feed the comparison a perturbed expectation -- one digit
    // of the AI-script filename -- and require BOTH: a failure, and a message that names it.
    {
        char why[256] = {0};
        mh::crt::utils_sprintf__vi(n, "init\\AI%02d.SCR", 3);
        const bool ok = cmp_n("utils_sprintf__vi", n, "init\\AI04.SCR", why, sizeof(why));
        ck(!ok, "I: a perturbed expectation FAILS the compare (the arm that makes the rest evidence)");
        ck(std::strstr(why, "utils_sprintf__vi") != nullptr,
           "I: ...and the failure message names the shape, not just 'differs'");
        ck(std::strstr(why, "init\\AI03.SCR") != nullptr,
           "I: ...and reports what was actually produced");
    }
    {
        // The same arm for the wide half, whose comparison path is a different function.
        char why[256] = {0};
        mh::crt::w_sprintf__vss(w, L"%s: %s", L"Blue", L"gg");
        const bool ok = cmp_w("w_sprintf__vss", w, L"Blue: GG", why, sizeof(why));
        ck(!ok, "I: the WIDE compare goes red on a perturbed expectation too");
        ck(std::strstr(why, "w_sprintf__vss") != nullptr, "I: ...and names the wide shape");
    }

    // ---- J. %.2f -- SHAPE ONLY, and the note about why ---------------------------------------------
    //
    // Delegated to the host CRT (crt/crt_sprintf.h `case 'f'`). Checking it here would only assert
    // that MSVC agrees with itself. What can actually catch a Watcom disagreement is the shadow
    // compare on G_TEXT_TMP, where sim_debug_roll_random's line lands.
    mh::crt::w_sprintf__vdi(w, L"random %.2f  %d", 0.5, 4);
    ck_w("J: %.2f formats through the host CRT and the double consumes TWO argument dwords", w,
         L"random 0.50  4");

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
