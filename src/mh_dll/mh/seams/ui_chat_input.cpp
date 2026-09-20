//
// seams/ui_chat_input.cpp -- mp:F3: non-English typed input, on one codec with a PINNED codepage.
//
// Full mechanism, the two halves of the defect, the three hooks and why the codepage is a session
// property rather than a machine one: include/mh_chatinput_export.h. Read that first; this file is
// the implementation and its comments assume it.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "include/mh_chatinput_export.h"
#include "include/mh_run_context.h" // mh_run_path
#include "addr/mh_addrs.gen.h"      // mh::addr::_G_LLM_STRAT_CHAT_INPUT_LINE
#include "addr/mh_calls.gen.h"      // mh::call::llm_input_key_dequeue / llm_ui_chat_input_char_insert
#include "hook/detour.h"            // install_jmp + install_trampoline
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / wvsprintfA / the keyboard API

namespace {

using mh::hook::install_jmp;
using mh::hook::install_trampoline;

// ---- the three entries, and the data the replaced bodies read ----------------------------------
//
// The entries and most of the data are spelled as raw EN VAs with their provenance rather than
// pulled from addr/mh_addrs.gen.h, on the gfx_font_guard.cpp precedent: they are `DAT_`/unlisted
// data labels and three functions the DLL address manifest does not carry. Every one is cited from
// the decompile of the function named beside it (and from docs/symbols.md for the three keystate
// bytes). Naming `_G_LLM_CHAT_INPUT_WRITE_POS` in the manifest is a follow-up (RE-DB), not a blocker.
//
// NONE OF THE THREE DATA ADDRESSES IS IN A MOVABLE REGION -- checked rather than assumed
// (`check_movable_addresses.movable_regions()` has no entry containing 0x0050a84c, 0x0050a7d4 or
// 0x00e69d3a), which matters because a raw literal is INVISIBLE to that gate's `mh::addr::X` scan.
// And the stock VA is the right answer here even if one became movable: H2 hands scancodes back to
// the ORIGINAL body, whose own machine code reads the stock address, so reading anywhere else would
// make the two halves disagree about which buffer they are editing.
constexpr uintptr_t ADDR_TRANSLATE  = 0x004cb91eu; // llm_input_key_dequeue_translate_ascii  (H1)
constexpr uintptr_t ADDR_CHAT_DRAIN = 0x00414a4eu; // llm_ui_chat_input_process_scancodes    (H2)
constexpr uintptr_t ADDR_ANSI2WIDE  = 0x004cf379u; // llm_str_ansi_to_wide                   (H3)

// The ToAscii-failure fallback table the original translate step consults: byte pairs indexed by
// SCANCODE, `[sc*2]` unshifted and `[sc*2+1]` shifted. It carries the navigation keys the menus
// depend on, so the replacement keeps it verbatim -- this seam changes which CHARACTER a character
// key produces, not what a non-character key means.
constexpr uintptr_t ADDR_KEYTAB = 0x0065fb08u;
constexpr unsigned  KEYTAB_MAX  = 256u; // the index is masked to a byte; the original never checked

// The three HELD latches the original translate step and the original chat switch both read
// (_G_LLM_INPUT_KEYSTATE + the key's set-1 scancode; docs/symbols.md).
constexpr uintptr_t ADDR_LSHIFT = 0x00e69d3au; // _G_LLM_KEY_LSHIFT_HELD   (KEYSTATE + 0x2a)
constexpr uintptr_t ADDR_RSHIFT = 0x00e69d46u; // _G_LLM_KEY_RSHIFT_HELD   (KEYSTATE + 0x36)
constexpr uintptr_t ADDR_CAPS   = 0x00e69d4au; // _G_LLM_KEY_CAPSLOCK_HELD (KEYSTATE + 0x3a)

// The chat scancode queue. `_G_LLM_STRAT_CHAT_INPUT_LINE` is char[81] = a 41-byte edit line
// followed by the 40-byte queue at +0x29; `_G_LLM_CHAT_INPUT_WRITE_POS` is how many entries the
// producers appended this frame (llm_strat_input_update zeroes it at its own entry, so the queue is
// one frame's worth and never accumulates).
constexpr uintptr_t ADDR_CHAT_LINE      = mh::addr::_G_LLM_STRAT_CHAT_INPUT_LINE; // 0x0050a84c, char[81]
constexpr uintptr_t ADDR_CHAT_WRITE_POS = 0x0050a7d4u;                            // _G_LLM_CHAT_INPUT_WRITE_POS, int
constexpr int       CHAT_Q_OFF          = 0x29;
constexpr int       CHAT_Q_MAX          = 0x51 - CHAT_Q_OFF; // 40 -- the buffer's own remainder

// ---- state -------------------------------------------------------------------------------------
UINT g_cp        = 0;     // the pinned codepage; 0 = not resolved yet
bool g_cp_pinned = false; // true iff [input] codepage named a real number (not `acp`)
// mp:F3c -- THE SESSION OVERRIDE. A codepage is a property of the SESSION (F3), and the session is
// the host's: a joiner ADOPTS the host's value from the advert at its JOIN and gets its own back
// when the session closes. `g_cp` stays this peer's own resolution (the ini pin or the ACP) for the
// whole process; `g_cp_session` is what the three hooks and the wire read while it is non-zero.
// `[input] codepage_adopt=0` is how a player declares this peer will NOT switch -- the host then
// refuses it by name, and F3c's other half delivers that refusal to the joiner's screen.
UINT  g_cp_session = 0;    // 0 = no session override; else the host's codepage, adopted at JOIN
bool  g_cp_adopt   = true; // [input] codepage_adopt (default 1)
bool  g_armed      = false;
void *g_tramp_chat = nullptr; // stolen prologue + jmp llm_ui_chat_input_process_scancodes+8
int   g_hooks      = 0;       // how many of the three actually installed

long g_resolved   = 0; // characters the codec produced (both hooks)
long g_chat_chars = 0; // of those, ones inserted into the chat line by H2
long g_unmappable = 0; // characters the LAYOUT produced that the PINNED codepage cannot represent

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

// mh_input.log: the existing mh.dll-only advisory INPUT channel (CHANNEL_OWNERS in
// tools/check_instrument_wiring.py), already written by ui_keyrepeat.cpp. A new channel would be an
// unruled one.
void ci_log(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_input.log", &g_log_gen);
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

UINT resolved_cp() {
    if (g_cp_session) return g_cp_session; // F3c: the host's, for the length of the session
    if (!g_cp) g_cp = GetACP();            // never armed (e.g. the offline oracle) -> today's behaviour
    return g_cp;
}

// ---- THE CODEC ---------------------------------------------------------------------------------

// One UTF-16 code unit -> one byte of the pinned codepage, or 0 if it has none. `bad` is what makes
// "unrepresentable" a MEASURED answer rather than a silent '?': with a NUL default character an
// unmappable input comes back as 0 AND sets the flag, where ToAscii would have returned 0x3F and
// looked like a successful translation of a question mark.
unsigned char encode_cp(wchar_t wc) {
    char      b[8] = {0};
    BOOL      bad  = FALSE;
    const int m    = WideCharToMultiByte(resolved_cp(), 0, &wc, 1, b, (int)sizeof(b), "\0", &bad);
    if (m != 1 || bad || b[0] == '\0') {
        ++g_unmappable;
        return 0;
    }
    return (unsigned char)b[0];
}

// scancode + key-state -> the byte this keystroke means on the ACTIVE layout under the PINNED
// codepage. 0 = "this key produces no character", which is the caller's cue to fall back to the
// game's own behaviour (H1: the scancode table; H2: the original switch arm).
//
// ToUnicodeEx, not ToAsciiEx: ToAsciiEx would do the codepage conversion itself, with the layout's
// codepage rather than ours, and hand back a byte we could no longer re-encode. Taking the UTF-16
// result and encoding it here is what makes `[input] codepage` a real choice.
unsigned char translate_sc(UINT sc, const BYTE *keystate) {
    const HKL  hkl = GetKeyboardLayout(0);
    const UINT vk  = MapVirtualKeyExA(sc, MAPVK_VSC_TO_VK, hkl); // the original's own first step
    if (!vk) return 0;
    wchar_t   w[8] = {0};
    const int n    = ToUnicodeEx(vk, sc, keystate, w, 8, 0, hkl);
    if (n < 0) {
        // A DEAD KEY. It has just been latched into the layout's state; call again to flush it back
        // out, exactly as the harness's own resolver does, so the next real keystroke is not
        // silently composed with it. The keystroke itself produces nothing.
        ToUnicodeEx(vk, sc, keystate, w, 8, 0, hkl);
        return 0;
    }
    if (n != 1) return 0; // 0 = no character; >1 = a ligature the 8-bit storage cannot hold
    const unsigned char b = encode_cp(w[0]);
    if (b) ++g_resolved;
    return b;
}

// The OS key-state table as the codec should see it -- whatever the thread really has, which is
// exactly what ToAscii read before this seam and what the UI harness's `type` writes for the
// modifier half of a keystroke.
//
// `with_game_latches` is H2's, and only H2's. The chat drain runs a frame AFTER the producer queued
// the scancode, and the original switch it replaces read the shift state from the game's own HELD
// latches rather than from the OS -- so a shift the game believes is down must not be lost by the
// change of source. H1 translates the event it has just dequeued, in the same breath the original
// did, and passes the OS table through UNCHANGED: every menu/lobby baseline in the suite was blessed
// against exactly that table, and widening the shift source there would be a behaviour change bought
// with nothing.
void build_keystate(BYTE *st, bool with_game_latches) {
    if (!GetKeyboardState(st)) memset(st, 0, 256);
    if (!with_game_latches) return;
    const bool shift = ((*(volatile const uint8_t *)ADDR_LSHIFT & 1) != 0) ||
                       ((*(volatile const uint8_t *)ADDR_RSHIFT & 1) != 0);
    if (shift) {
        st[VK_SHIFT]  = 0x80;
        st[VK_LSHIFT] = 0x80;
    }
}

// ---- H1: llm_input_key_dequeue_translate_ascii -------------------------------------------------
//
// The original, line for line, with ToAscii replaced by the codec. Layout:
//   key_event[0] = the set-1 scancode (written by llm_input_key_dequeue)
//   key_event[2] = the resolved character (what every caller reads)
void key_dequeue_translate(uint32_t *key_event) {
    mh::call::llm_input_key_dequeue((uint32_t)(uintptr_t)key_event);
    const UINT sc = key_event[0];

    BYTE st[256];
    build_keystate(st, /*with_game_latches=*/false);
    const unsigned char ch = translate_sc(sc, st);
    if (ch) {
        key_event[2] = ch;
        return;
    }
    // THE ORIGINAL FALLBACK, UNCHANGED. Shift/caps select the second byte of the scancode's pair;
    // the original tested `& 3` at all three sites, which is `& 1` in practice (there is no
    // E0-prefixed twin scancode for Shift or Caps Lock -- see the Ghidra plate on 0x00e69d4a).
    const bool alt = ((*(volatile const uint8_t *)ADDR_RSHIFT & 3) != 0) ||
                     ((*(volatile const uint8_t *)ADDR_LSHIFT & 3) != 0) ||
                     ((*(volatile const uint8_t *)ADDR_CAPS & 3) != 0);
    const uint8_t *tab = (const uint8_t *)ADDR_KEYTAB;
    key_event[2]       = tab[(sc & (KEYTAB_MAX - 1)) * 2 + (alt ? 1 : 0)];
}

// __watcall(uint* in EAX) -> void, plain `ret` (no stack arguments to clean).
// clang-format off
__declspec(naked) void translate_thunk() {
    __asm {
        push eax
        call key_dequeue_translate
        add  esp, 4
        ret
    }
}
// clang-format on

// ---- H3: llm_str_ansi_to_wide ------------------------------------------------------------------
//
// The original is `MultiByteToWideChar(CP_ACP, 0, src, len, dst, len); dst[len] = 0; return dst;`
// with `len` from an inline strlen. The only change is the codepage. It stays a whole-body replace
// rather than a run-before because the codepage is the FIRST argument of the one call it makes.
wchar_t *ansi_to_wide(wchar_t *dst, const char *src) {
    const int len = lstrlenA(src);
    MultiByteToWideChar(resolved_cp(), 0, src, len, dst, len);
    dst[len] = L'\0';
    return dst;
}

// __watcall(wchar_t *dst in EAX, char *src in EDX) -> wchar_t* in EAX.
// clang-format off
__declspec(naked) void ansi_to_wide_thunk() {
    __asm {
        push edx                 // src  (the second __watcall register argument)
        push eax                 // dst  (the first)
        call ansi_to_wide
        add  esp, 8
        ret                      // the return value is already in EAX for both conventions
    }
}
// clang-format on

// ---- H2: llm_ui_chat_input_process_scancodes ---------------------------------------------------

// THE TYPEWRITER BLOCK: exactly the scancodes whose arm in the original `switch` assigns a
// character. Enumerated from the decompile of 0x00414a4e rather than from a keyboard picture, and
// the boundaries matter -- 0x0e/0x0f/0x1c/0x1d/0x2a/0x36/0x38 sit INSIDE these spans' neighbourhood
// and are control keys the original handles itself.
//
//   0x02..0x0d  the number row + - =        0x10..0x1b  q..p [ ]
//   0x1e..0x29  a..l ; ' `                  0x2b..0x35  \ z..m , . /
//   0x39        space
//
// Everything else -- including Backspace (0x0e), Enter (0x1c), Esc (0x01), Delete (0x53), the two
// cursor keys (0x4b/0x4d) and the two history keys (0x48/0x50) -- is handed to the original body.
// Classifying by the ORIGINAL's own arms (rather than by "does ToUnicode give me a character") is
// what keeps the numeric keypad out: with NumLock on, ToUnicode(VK_NUMPAD8) is '8', and scancode
// 0x48 un-prefixed IS numpad 8 -- routing it through the codec would type a digit where retail
// walks the chat history.
bool sc_is_typewriter(unsigned sc) {
    return (sc >= 0x02u && sc <= 0x0du) || (sc >= 0x10u && sc <= 0x1bu) ||
           (sc >= 0x1eu && sc <= 0x29u) || (sc >= 0x2bu && sc <= 0x35u) || sc == 0x39u;
}

// Run-before at the drain's entry. Takes the whole queue, then replays it ONE ENTRY AT A TIME in
// the original order: a typewriter key becomes the codec's byte, anything else is handed to the
// original body with a one-entry queue. Order is preserved because the replay is sequential -- a
// batch like `a b <backspace> c` still ends as "ac", which a "insert all the printables first,
// then let the original run" shortcut would have got wrong.
void on_chat_drain() {
    volatile int32_t *wp = (volatile int32_t *)ADDR_CHAT_WRITE_POS;
    int               n  = *wp;
    if (n <= 0) return;
    if (n > CHAT_Q_MAX) n = CHAT_Q_MAX; // the producers bound this; a corrupt count must not read past

    uint8_t *queue = (uint8_t *)(ADDR_CHAT_LINE + CHAT_Q_OFF);
    uint8_t  q[CHAT_Q_MAX];
    for (int i = 0; i < n; ++i) q[i] = queue[i];

    BYTE st[256];
    build_keystate(st, /*with_game_latches=*/true);

    *wp = 0; // the ORIGINAL body runs immediately after this function returns (we are a run-before,
             // not a replacement); zeroing the count is what makes that pass a no-op. The producer
             // re-zeroes it at its own entry every frame, so nothing downstream reads the 0.

    for (int i = 0; i < n; ++i) {
        const unsigned      sc = q[i];
        const unsigned char ch = sc_is_typewriter(sc) ? translate_sc(sc, st) : 0;
        if (ch) {
            mh::call::llm_ui_chat_input_char_insert((char)ch); // the game's own BOUNDED insert
            ++g_chat_chars;
            continue;
        }
        // Not a character on this layout (or unrepresentable in the pinned codepage): let the
        // ORIGINAL decide, with a queue holding exactly this one scancode. s_void is the shared
        // __watcall shim (it preserves EBX/ESI/EDI, which a Watcom callee is free to clobber);
        // g_tramp_chat is the stolen prologue plus a jump back to the body, so this re-enters the
        // original WITHOUT re-entering our own detour.
        queue[0] = (uint8_t)sc;
        *wp      = 1;
        mh::call::detail::s_void((uintptr_t)g_tramp_chat);
        *wp = 0;
    }

    for (int i = 0; i < n; ++i) queue[i] = q[i]; // put the queue back as we found it
}

// clang-format off
__declspec(naked) void chat_drain_detour() {
    __asm {
        pushad
        pushfd
        call on_chat_drain
        popfd
        popad
        jmp  dword ptr [g_tramp_chat] // stolen prologue + jmp llm_ui_chat_input_process_scancodes+8
    }
}
// clang-format on

// `[input] codepage` -- a decimal codepage number, or `acp`/`0`/absent for GetACP().
UINT parse_codepage(const char *s, bool *pinned) {
    *pinned = false;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return GetACP();
    if ((s[0] == 'a' || s[0] == 'A') && (s[1] == 'c' || s[1] == 'C') && (s[2] == 'p' || s[2] == 'P'))
        return GetACP();
    unsigned v = 0;
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (unsigned)(*s - '0');
    if (v == 0) return GetACP();
    // CP_UTF8 / CP_UTF7 are multi-byte and cannot round-trip through a char[81] the game indexes by
    // byte; refusing them here is cheaper than a garbled chat line nobody can explain.
    if (v == 65000u || v == 65001u) return 0;
    *pinned = true;
    return v;
}

} // namespace

extern "C" int MH_ChatInput_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    if (g_armed) return 1;

    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);

    char buf[64];
    GetPrivateProfileStringA("input", "codepage", "acp", buf, sizeof(buf), ini);
    g_cp_adopt        = GetPrivateProfileIntA("input", "codepage_adopt", 1, ini) != 0; // F3c
    bool       pinned = false;
    const UINT want   = parse_codepage(buf, &pinned);
    if (want == 0) {
        ci_log("; [input] codepage '%s' REFUSED (UTF-7/UTF-8 are multi-byte; the game's text buffers "
               "are byte-indexed) -- falling back to the process ACP %u",
               buf, GetACP());
        g_cp = GetACP();
    } else if (pinned && !IsValidCodePage(want)) {
        ci_log("; [input] codepage %u is NOT INSTALLED on this machine -- falling back to the "
               "process ACP %u",
               want, GetACP());
        g_cp = GetACP();
    } else {
        g_cp        = want;
        g_cp_pinned = pinned;
    }

    // H1 and H3 are whole-body replaces; H2 is a run-before. Each is best-effort and reported
    // individually -- a refused hook leaves that path at retail behaviour, and the seam says which.
    if (install_jmp(ADDR_TRANSLATE, (const void *)translate_thunk, mh::hook::entry_claim::exclusive,
                    "the F3 layout-aware key translate"))
        ++g_hooks;
    else
        ci_log("; [input] H1 NOT armed: install refused at 0x%08x (see the [interlock] line in "
               "mh_net.log) -- menu/lobby fields keep ToAscii",
               (unsigned)ADDR_TRANSLATE);

    if (install_trampoline(ADDR_CHAT_DRAIN, (void *)chat_drain_detour, &g_tramp_chat, 8,
                           mh::hook::entry_claim::exclusive, "the F3 in-game chat input codec"))
        ++g_hooks;
    else
        ci_log("; [input] H2 NOT armed: install refused at 0x%08x (see the [interlock] line in "
               "mh_net.log) -- in-game chat keeps the US-QWERTY switch",
               (unsigned)ADDR_CHAT_DRAIN);

    if (install_jmp(ADDR_ANSI2WIDE, (const void *)ansi_to_wide_thunk,
                    mh::hook::entry_claim::exclusive, "the F3 pinned-codepage ANSI widen"))
        ++g_hooks;
    else
        ci_log("; [input] H3 NOT armed: install refused at 0x%08x (see the [interlock] line in "
               "mh_net.log) -- displayed text keeps CP_ACP",
               (unsigned)ADDR_ANSI2WIDE);

    g_armed = (g_hooks > 0);
    ci_log("; [input] chat/text codec armed (%d/3 hooks) -- codepage %u (%s), ACP %u, layout %08x",
           g_hooks, resolved_cp(), g_cp_pinned ? "PINNED by [input] codepage" : "process default",
           GetACP(), (unsigned)(uintptr_t)GetKeyboardLayout(0));
    return g_armed ? 1 : 0;
}

extern "C" unsigned int MH_ChatInput_Codepage(void) { return resolved_cp(); }
extern "C" unsigned int MH_ChatInput_OwnCodepage(void) {
    if (!g_cp) g_cp = GetACP();
    return g_cp;
}

// F3c: adopt the host's codepage for the match being joined, or say why not. The two refusals are the two
// ways a peer "cannot switch": the player said so (`[input] codepage_adopt=0`), or Windows has no
// such codepage installed (WideCharToMultiByte would fail on every character). A host that made
// no claim (0) has nothing to adopt, and adopting our own value is a no-op that reports success.
extern "C" int MH_ChatInput_AdoptCodepage(unsigned int cp) {
    if (cp == 0) return 0;
    if (cp == MH_ChatInput_OwnCodepage()) {
        g_cp_session = 0; // same value: no override needed (and none left behind from an earlier lobby)
        return 1;
    }
    if (!g_cp_adopt) {
        ci_log("; [input] host pins codepage %u, ours is %u -- NOT adopting ([input] codepage_adopt=0)", cp,
               MH_ChatInput_OwnCodepage());
        return 0;
    }
    if (cp == 65000u || cp == 65001u || !IsValidCodePage(cp)) {
        ci_log("; [input] host pins codepage %u, which this machine cannot encode (not installed or "
               "multi-byte) -- NOT adopting",
               cp);
        return 0;
    }
    g_cp_session = cp;
    ci_log("; [input] adopted the host's codepage %u for this session (ours is %u)", cp, MH_ChatInput_OwnCodepage());
    return 1;
}

extern "C" void MH_ChatInput_RestoreCodepage(void) {
    if (!g_cp_session) return;
    ci_log("; [input] session over -> codepage back to our own %u (was the host's %u)", MH_ChatInput_OwnCodepage(),
           g_cp_session);
    g_cp_session = 0;
}

// F3c: re-encode a NUL-terminated string typed under `from` so it means the same characters under
// `to` (in place, `cap` bytes incl. NUL). A character `to` cannot represent becomes '?'. The joiner's
// player name was typed BEFORE it knew the host's codepage; without this the host's lobby would
// widen those bytes under the session codepage and show a different name than the player typed.
extern "C" void MH_ChatInput_Transcode(char *s, int cap, unsigned int from, unsigned int to) {
    if (!s || cap <= 1 || from == 0 || to == 0 || from == to) return;
    // ASCII is the same under every single-byte codepage this seam accepts: nothing to do, and no
    // risk of a WideCharToMultiByte best-fit substituting a byte the player did not type.
    bool ascii = true;
    for (const char *p = s; *p; ++p)
        if ((unsigned char)*p >= 0x80) ascii = false;
    if (ascii) return;
    wchar_t   w[64];
    const int n = MultiByteToWideChar(from, 0, s, -1, w, 64);
    if (n <= 0) return;
    char      out[64];
    const int m = WideCharToMultiByte(to, 0, w, -1, out, (int)sizeof(out), "?", nullptr);
    if (m <= 0) return;
    lstrcpynA(s, out, cap);
}

extern "C" void MH_ChatInput_Stats(long *resolved, long *chat_chars, long *unmappable) {
    if (resolved) *resolved = g_resolved;
    if (chat_chars) *chat_chars = g_chat_chars;
    if (unmappable) *unmappable = g_unmappable;
}
