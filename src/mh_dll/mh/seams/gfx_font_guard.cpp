//
// seams/gfx_font_guard.cpp -- mp:F2: the bounds guard in front of the glyph-table lookup, plus the
// `[fonts] probe_text` render probe.
//
// Full mechanism, the two DIFFERENT defects it covers, and why the probe has to exist at all:
// include/mh_fontguard_export.h. In short: `llm_gfx_font_layout_text` indexes
// `glyph_ptr[charmap[code_unit]]` with neither index checked; on the menu fonts that renders the
// font's scratch buffer as a garbage glyph, and on the HUD font (whose charmap is 257 entries, not
// 65536) it reads past the array and dereferences whatever integer it finds.
//
// THIS IS A WHOLE-BODY REPLACE (`install_jmp`), not a run-before: the decision is inside the loop,
// one branch per character, and there is no seam in the original to hang it off. The replacement is
// otherwise the original line for line -- same scratch list, same NUL terminator, same width
// accumulation -- which is what keeps every already-blessed ASCII baseline bit-identical.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>

#include "include/mh_fontguard_export.h"
#include "include/mh_run_context.h" // MH_RunDir / mh_run_path
#include "addr/mh_calls.gen.h"      // mh::call::llm_gfx_font_select / llm_gfx_draw_text_blend_clipped
#include "config/ini_read.h"        // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "hook/detour.h"            // install_jmp + WATCOM_PROLOGUE
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / wvsprintfA

namespace {

using mh::hook::install_jmp;

// ---- the globals the layout pass walks ---------------------------------------------------------
//
// These five are still `DAT_`/data labels in Ghidra rather than manifest entries, so they are
// spelled here as raw EN VAs with their provenance rather than pulled from addr/mh_addrs.gen.h.
// Every one is cited from the decompile of llm_gfx_font_layout_text / _font_load / _font_select
// and from docs/symbols.md; naming them in the DB is a follow-up (RE-DB), not a blocker.
constexpr uintptr_t ADDR_LAYOUT_TEXT    = 0x004a2400u; // llm_gfx_font_layout_text (the replaced body)
constexpr uintptr_t ADDR_ACTIVE_CHARMAP = 0x005d81f4u; // int** -- set per slot by llm_gfx_font_select
constexpr uintptr_t ADDR_CHARMAP_SLOT0  = 0x005d81f8u; // slot 0's charmap (FONTY08), 257 entries
constexpr uintptr_t ADDR_GLYPH_TABLE    = 0x005d81b4u; // _G_LLM_GFX_CUR_FONT_GLYPH_PTR_TABLE, byte**
constexpr uintptr_t ADDR_LAYOUT_LIST    = 0x005d6190u; // the glyph-pointer scratch list (NUL-terminated)
constexpr uintptr_t ADDR_LAYOUT_WIDTH   = 0x005d85f8u; // _G_LLM_GFX_TEXT_LAYOUT_WIDTH
constexpr uintptr_t ADDR_FONT_DATA_PTRS = 0x005d8198u; // _G_LLM_GFX_FONT_DATA_PTRS[7]; null until boot stage 3

// THE TWO EXTENTS, measured from the image rather than assumed (2026-09-17):
//   slot 0   0x005d81f8 .. 0x005d85fb, and 0x005d85fc is the "fnt\" font-name buffer -> 257 entries.
//   slots1-6 0x0060434c .. 0x0064434b, and 0x0064434c holds a non-zero dword (0x0000000d) -> exactly
//            0x40000 bytes = 65536 entries, i.e. the full u16 code-unit space.
constexpr unsigned CHARMAP_SLOT0_ENTRIES  = 257u;
constexpr unsigned CHARMAP_SHARED_ENTRIES = 65536u;

// The substitute, in preference order. U+FFFD is the box `src/formats/fnt.py merge` appends; U+00A4
// is the classic stand-in if some other layer ever carries one; '?' is in every shipped FONTLAY, so
// on STOCK retail fonts the guard still has something real to draw.
constexpr unsigned FALLBACK_CODEPOINTS[] = {0xFFFDu, 0x00A4u, 0x003Fu};

bool g_guard_on = true; // [fonts] glyph_guard
bool g_armed    = false;

// probe (`[fonts] probe_text`) -- see the header for why a probe is the only way F2 can show a
// non-ASCII glyph on a frame before mp:F3 lands.
wchar_t  g_probe[128] = {0};
int      g_probe_x = 16, g_probe_y = 16, g_probe_slot = 4;
uint16_t g_probe_color = 0xFFFFu;

// Diagnostics. Read off the log; nothing parses them, so they are not log-format registry rows.
long g_subst = 0; // characters the guard substituted with the box
long g_drop  = 0; // unmapped characters left on retail's blank cell (no substitute in this font)
long g_oob   = 0; // of those, ones whose code unit was outside the charmap -- the crash arm

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

void fg_log(const char *fmt, ...) {
    // mh_video.log: the existing mh.dll-only advisory GFX channel (CHANNEL_OWNERS in
    // tools/check_instrument_wiring.py). A new channel would be an unruled one.
    mh_run_path(g_log, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[320];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
    CloseHandle(h);
}

// Ordinal for the substitute glyph in the CURRENT font, or 0 if this font has none of them.
// Resolved per substitution rather than cached: it is at most three array reads, it is only reached
// on a character that would otherwise have been wrong, and a cache would have to be invalidated on
// every llm_gfx_font_select -- a correctness risk bought with nothing.
int fallback_ordinal(const int *charmap, unsigned limit) {
    for (unsigned i = 0; i < sizeof(FALLBACK_CODEPOINTS) / sizeof(FALLBACK_CODEPOINTS[0]); ++i) {
        const unsigned cp = FALLBACK_CODEPOINTS[i];
        if (cp < limit && charmap[cp]) return charmap[cp];
    }
    return 0;
}

// The replacement body. Line for line the original except for the substitution branch.
void layout_text_guarded(uint16_t *text) {
    const int *charmap = *(const int *const *)ADDR_ACTIVE_CHARMAP;
    uint8_t  **glyphs  = *(uint8_t ***)ADDR_GLYPH_TABLE;
    uint8_t  **out     = (uint8_t **)ADDR_LAYOUT_LIST;
    int        width   = 0;

    if (!text || !charmap || !glyphs) { // never observed; a replaced body must not add a new crash
        *(uint8_t **)ADDR_LAYOUT_LIST = nullptr;
        *(int *)ADDR_LAYOUT_WIDTH     = 0;
        return;
    }

    const unsigned limit =
        (charmap == (const int *)ADDR_CHARMAP_SLOT0) ? CHARMAP_SLOT0_ENTRIES : CHARMAP_SHARED_ENTRIES;

    for (const uint16_t *p = text; *p; ++p) {
        const unsigned u  = *p;
        const bool     ob = (u >= limit);
        // THE BOUNDS CLAMP IS UNCONDITIONAL, and `[fonts] glyph_guard=0` does NOT restore it. An
        // A/B switch that re-enables a wild read is not an observation mode, it is a way to crash
        // the rig on purpose; out of range reads as "unmapped", which is what every in-bounds
        // unmapped code unit already did. What the flag turns off is the SUBSTITUTION below.
        int ix = ob ? 0 : charmap[u];
        // ORDINAL 0 IS THE SPACE, NOT AN ERROR -- and the first version of this file got that
        // wrong, substituted '?' for every space, and the rendered frame said so (the probe read
        // "F2?GUARD?[??]?ascii?intact"). U+0020 IS NOT IN ANY SHIPPED FONTLAY: `llm_gfx_font_load`
        // allocates a zero-filled scratch buffer whose byte 0 is the UI-scaled line height, parks
        // it at `glyph_ptr[0]`, and every code unit the charmap does not map therefore draws a
        // BLANK one-em cell. That is how the game draws a space, so ordinal 0 is a real fallback
        // the original relies on, not the garbage glyph it looks like in the decompile.
        //
        // So the substitution is keyed on the CODE UNIT, not on the ordinal: at or below U+0020
        // (space and the C0 controls) ordinal 0 is left exactly as retail leaves it; above it, a
        // character with no glyph is a genuinely missing one and gets the box.
        //
        // THE C1 CONTROLS (U+007F..U+009F) ARE BLANKS TOO (2026-09-26). The HUD's label:value rows
        // pad their numbers with U+0087 (llm_ui_hud_label_value_row_draw -> llm_str_itoa_pad_left(v,
        // 6, 0x87)) precisely BECAUSE no font maps it: the pad draws retail's blank one-em cell, and
        // the row's right-alignment subtracts that cell's width three times. Substituting it drew the
        // stats panel (E) as `?????0`. No codepage widens a printable character into this range --
        // it is controls in Unicode -- so leaving it blank cannot hide real text.
        const bool control = (u <= 0x20u) || (u >= 0x7Fu && u <= 0x9Fu);
        if (ix == 0 && !control) {
            if (ob) ++g_oob;
            if (g_guard_on) {
                ix = fallback_ordinal(charmap, limit);
                if (ix == 0) { // no substitute in this font: fall back to retail's blank cell
                    ++g_drop;
                } else {
                    ++g_subst;
                }
            }
        }
        uint8_t *g = glyphs[ix];
        if (!g) { // never observed; glyph_ptr[0] is set by llm_gfx_font_load and 1..N are records
            continue;
        }
        *out++ = g;
        width += *g;
    }
    *out                      = nullptr;
    *(int *)ADDR_LAYOUT_WIDTH = width;
}

// __watcall(ushort* in EAX) -> void, plain `ret` (verified: the original's epilogue is
// `lea esp,[ebp-0x14]; pop edi/esi/edx/ecx/ebx/ebp; ret`, i.e. no stack arguments to clean).
// EBX/ESI/EDI/EBP are preserved by the __cdecl callee MSVC generates below; EAX/ECX/EDX are
// volatile under both conventions.
// clang-format off
__declspec(naked) void layout_text_thunk() {
    __asm {
        push eax                    // the __watcall argument
        call layout_text_guarded
        add  esp, 4
        ret
    }
}
// clang-format on

int parse_int(const char *s) {
    int v = 0, sign = 1;
    while (*s == ' ') ++s;
    if (*s == '-') {
        sign = -1;
        ++s;
    }
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
    return v * sign;
}

int hex_digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// Resolve `\uXXXX` (and `\\`) in place. THE PROBE TEXT HAS TO BE ASCII IN THE INI FILE and this is
// why: `tools/ui_test.py` reads the `--extra-ini` fragment and re-emits it into the peer's
// mh_net.ini through Python's default text encoding, which on this rig is cp1252 -- a literal
// U+4E00 in the fragment raises UnicodeEncodeError in the RUNNER, before the game ever starts
// (measured 2026-09-17). Escapes keep the whole chain 7-bit and keep the ini readable in any editor;
// raw UTF-8 still works for anything inside the ANSI range.
void unescape_unicode(wchar_t *s) {
    wchar_t *w = s;
    for (const wchar_t *r = s; *r;) {
        if (r[0] == L'\\' && (r[1] == L'u' || r[1] == L'U')) {
            int v = 0, n = 0;
            for (; n < 4; ++n) {
                const int d = hex_digit(r[2 + n]);
                if (d < 0) break;
                v = v * 16 + d;
            }
            if (n == 4) {
                *w++ = (wchar_t)v;
                r += 6;
                continue;
            }
        }
        if (r[0] == L'\\' && r[1] == L'\\') {
            *w++ = L'\\';
            r += 2;
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

unsigned parse_hex(const char *s) {
    unsigned v = 0;
    for (; *s; ++s) {
        const char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned)(c - 'A' + 10);
        else break;
    }
    return v;
}

} // namespace

extern "C" void MH_FontGuard_OnPresent(void) {
    if (!g_probe[0]) return;
    // The present hook also runs before boot stage 3 (llm_gfx_font_system_init). Selecting a slot
    // whose data pointer is still null dereferences it for the line height, so the probe waits for
    // the font system rather than for a game mode -- one load, no policy.
    if (g_probe_slot < 0 || g_probe_slot > 6) return;
    const uint8_t *const *font_data = (const uint8_t *const *)ADDR_FONT_DATA_PTRS;
    if (!font_data[g_probe_slot]) return;
    mh::call::llm_gfx_font_select(g_probe_slot);
    mh::call::llm_gfx_draw_text_blend_clipped(g_probe_x, g_probe_y, (uint16_t *)g_probe,
                                              (int16_t)g_probe_color);
}

extern "C" int MH_FontGuard_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    if (g_armed) return 1;

    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);

    // Default ON with no ini (ship semantics): the guard is a strict bug fix -- the code unit it
    // rejects had no glyph to draw under any reading of the data.
    g_guard_on = GetPrivateProfileIntA("fonts", "glyph_guard", 1, ini) != 0;

    char buf[512];
    GetPrivateProfileStringA("fonts", "probe_text", "", buf, sizeof(buf), ini);
    // A TRAILING `; comment` IS PART OF THE VALUE to GetPrivateProfileString. Until 2026-09-20 the
    // example ini's own line was `probe_text=   ; TEST AFFORDANCE, empty = off ...`, so a STOCK ini
    // -- the ship zip's, verbatim -- drew that comment on every present. Same trim as `[net] relay`
    // (net_discovery.cpp relay_addr_cached, the 2026-09-19 rc3 bug) -- now the SHARED helper
    // (TL-HARN4, config/ini_read.h) rather than a second hand-rolled copy of the same loop: cut at
    // the first `;`, drop the whitespace before it, and say so once. `;` cannot be part of a probe
    // on purpose: the probe is a test affordance and a code point is written as a `\uXXXX` escape,
    // never as raw text with punctuation. The ini side is gated by tools/lint_ini_string_keys.py
    // (user ruling 2026-09-20).
    if (mh::config::strip_ini_comment(buf)) {
        fg_log("; [fonts] probe_text carried a trailing `;` comment; using `%s` (the comment is "
               "not part of the probe -- remove it from the ini)",
               buf);
    }
    if (buf[0]) {
        // The ini is read as bytes; the probe's whole point is arbitrary code points, so the value
        // is widened as UTF-8 (never through an ANSI codepage) and then `\uXXXX`-unescaped. Write
        // the escapes, not raw UTF-8 -- see unescape_unicode() for the runner that cannot carry it.
        //
        // THE WIDEN TARGET IS THE ESCAPED LENGTH, NOT THE RESULT'S. Every `\uXXXX` is six
        // characters that become one, so a 30-glyph probe is a ~180-character ini value: widening
        // straight into g_probe[128] returned 0 and left the probe silently OFF, with the arm line
        // still saying the guard was armed (measured 2026-09-17 -- the run looked healthy and the
        // frame simply had no probe on it). Hence the roomy staging buffer AND the else-branch
        // below: a probe that cannot be built says so in the log.
        wchar_t staged[512];
        if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, staged,
                                sizeof(staged) / sizeof(staged[0])) == 0) {
            g_probe[0] = 0;
            fg_log("; [fonts] probe DISABLED: probe_text (%d bytes) will not widen as UTF-8 "
                   "(err %lu) -- the guard is still armed",
                   lstrlenA(buf), GetLastError());
        } else {
            unescape_unicode(staged);
            const int n   = lstrlenW(staged);
            const int cap = (int)(sizeof(g_probe) / sizeof(g_probe[0])) - 1;
            lstrcpynW(g_probe, staged, cap + 1);
            if (n > cap)
                fg_log("; [fonts] probe TRUNCATED: %d code units resolved, %d kept", n, cap);
        }
    }
    mh::config::read_ini_string("fonts", "probe_xy", "16,16", buf, sizeof(buf), ini); // TL-HARN4
    {
        char *comma = buf;
        while (*comma && *comma != ',') ++comma;
        if (*comma) {
            *comma    = 0;
            g_probe_y = parse_int(comma + 1);
        }
        g_probe_x = parse_int(buf);
    }
    g_probe_slot = GetPrivateProfileIntA("fonts", "probe_font", 4, ini);                // 4 = PFMENU2, the menu face
    mh::config::read_ini_string("fonts", "probe_color", "ffff", buf, sizeof(buf), ini); // TL-HARN4
    g_probe_color = (uint16_t)parse_hex(buf);

    if (!install_jmp(ADDR_LAYOUT_TEXT, (const void *)layout_text_thunk,
                     mh::hook::entry_claim::exclusive, "the F2 glyph-table bounds guard")) {
        fg_log("; [fonts] guard DISARMED: install refused at 0x%08x (the reason is on the "
               "[interlock] line for this address in mh_net.log)",
               (unsigned)ADDR_LAYOUT_TEXT);
        return 0;
    }
    g_armed = true;
    fg_log("; [fonts] glyph guard armed (%s) -- layout 0x%08x, charmap extents slot0=%u shared=%u%s",
           g_guard_on ? "substitute ON" : "OBSERVE-ONLY, [fonts] glyph_guard=0",
           (unsigned)ADDR_LAYOUT_TEXT, CHARMAP_SLOT0_ENTRIES, CHARMAP_SHARED_ENTRIES,
           g_probe[0] ? "; probe armed" : "");
    if (g_probe[0])
        fg_log("; [fonts] probe at %d,%d font slot %d colour 0x%04x, %d code unit(s)", g_probe_x,
               g_probe_y, g_probe_slot, g_probe_color, lstrlenW(g_probe));
    return 1;
}

// Test/diagnostic accessor: substitutions, drops, and how many of those were genuinely outside the
// charmap (the arm that would have been a wild pointer on font slot 0).
extern "C" void MH_FontGuard_Stats(long *subst, long *dropped, long *out_of_bounds) {
    if (subst) *subst = g_subst;
    if (dropped) *dropped = g_drop;
    if (out_of_bounds) *out_of_bounds = g_oob;
}
