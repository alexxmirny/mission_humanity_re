//
// seams/ui_chat_input.cpp -- mp:F3 + mp:MP-LANG: typed input and displayed text, language-agnostic in MP.
//
// Full mechanism, the defects it closes, the hooks and why chat is UTF-8 while everything else keeps
// an 8-bit codepage: include/mh_chatinput_export.h. Read that first; this file is the implementation
// and its comments assume it. The two text rules themselves (UTF-8 chat, [A-Za-z0-9] names) live in
// mh_net_proto/text_utf8.h, shared with libmh and the selftests.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "include/mh_chatinput_export.h"
#include "include/mh_run_context.h" // mh_run_path
#include "addr/mh_addrs.gen.h"      // mh::addr::_G_LLM_STRAT_CHAT_INPUT_LINE, mp_player_name, mp_game_name
#include "addr/mh_calls.gen.h"      // mh::call::llm_input_key_dequeue / llm_ui_chat_input_char_insert
#include "addr/mh_structs.gen.h"    // mh::game::mh_llm_input_key_event (0x38 stride) -- H7's ring walk
#include "config/ini_read.h"        // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "hook/detour.h"            // install_jmp + install_trampoline
#include "en_guard.h"               // EN-only build gate
#include "mh_net_proto/text_utf8.h" // MP-LANG: the UTF-8 codec + the name alphabet

#pragma comment(lib, "user32.lib") // wsprintfA / wvsprintfA / the keyboard API

namespace {

using mh::hook::install_jmp;
using mh::hook::install_trampoline;
namespace np = mh_net_proto;

// ---- the entries, and the data the replaced bodies read ----------------------------------------
//
// The entries and most of the data are spelled as raw EN VAs with their provenance rather than
// pulled from addr/mh_addrs.gen.h, on the gfx_font_guard.cpp precedent: they are `DAT_`/unlisted
// data labels and functions the DLL address manifest does not carry. Every one is cited from the
// decompile of the function named beside it (and from docs/symbols.md for the keystate bytes).
//
// NONE OF THE DATA ADDRESSES IS IN A MOVABLE REGION -- checked rather than assumed
// (`check_movable_addresses.movable_regions()` has no entry containing any of them), which matters
// because a raw literal is INVISIBLE to that gate's `mh::addr::X` scan. And the stock VA is the right
// answer here even if one became movable: H2 hands scancodes back to the ORIGINAL body, whose own
// machine code reads the stock address, so reading anywhere else would make the two halves disagree
// about which buffer they are editing.
constexpr uintptr_t ADDR_TRANSLATE  = 0x004cb91eu; // llm_input_key_dequeue_translate_ascii  (H1)
constexpr uintptr_t ADDR_CHAT_DRAIN = 0x00414a4eu; // llm_ui_chat_input_process_scancodes    (H2)
constexpr uintptr_t ADDR_ANSI2WIDE  = 0x004cf379u; // llm_str_ansi_to_wide                   (H3)
constexpr uintptr_t ADDR_MEASURE    = 0x004a2493u; // llm_gfx_font_measure_text_width        (H4, MP-LANG)

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

// The chat line and its queue. `_G_LLM_STRAT_CHAT_INPUT_LINE` is char[81] = a 41-byte edit line
// followed by the 40-byte scancode queue at +0x29; `_G_LLM_CHAT_INPUT_WRITE_POS` is how many entries
// the producers appended this frame (llm_strat_input_update zeroes it at its own entry, so the queue
// is one frame's worth and never accumulates). LEN / CURSOR are BYTE indices into the line, and the
// line's own bound is 0x28 bytes (llm_ui_chat_input_char_insert @0x00415591).
constexpr uintptr_t ADDR_CHAT_LINE      = mh::addr::_G_LLM_STRAT_CHAT_INPUT_LINE; // 0x0050a84c, char[81]
constexpr uintptr_t ADDR_CHAT_WRITE_POS = 0x0050a7d4u;                            // _G_LLM_CHAT_INPUT_WRITE_POS, int
constexpr uintptr_t ADDR_CHAT_LEN       = 0x0050a7d8u;                            // _G_LLM_CHAT_INPUT_LEN, int
constexpr uintptr_t ADDR_CHAT_ACTIVE    = 0x0050a7dcu;                            // _G_LLM_CHAT_INPUT_ACTIVE, int
constexpr uintptr_t ADDR_CHAT_CURSOR    = 0x0050a7e0u;                            // _G_LLM_CHAT_INPUT_CURSOR, int
constexpr int       CHAT_Q_OFF          = 0x29;
constexpr int       CHAT_Q_MAX          = 0x51 - CHAT_Q_OFF; // 40 -- the buffer's own remainder
constexpr int       CHAT_LINE_MAX       = 0x28;              // bytes, the insert's own clamp

// ---- MP-LANG: which widen is CHAT ---------------------------------------------------------------
//
// Chat bytes are UTF-8; every other string the game widens is 8-bit text in this peer's codepage
// (resource text -- Msgs.dat is CP1251 in the RU pack -- map names, the lobby's own lines). H3 sees
// all of them through one function, so it needs a DISCRIMINATOR, and three exist, each measured
// against the decompile of every llm_str_ansi_to_wide(_scratch) call site (EN, 2026-09-29):
//
//  (a) the SOURCE is the in-game chat edit line [0x0050a84c, +0x29): the HUD's line draw
//      (llm_ui_hud_topbar_tick 0x0041451d) and both local echoes (llm_net_chat_input_process
//      0x0044df90, llm_strat_input_update 0x00441d0b / 0x00441da7) all widen the line itself.
//  (b) the SOURCE is the lobby RX chat text, _G_LLM_LOBBY_RX_TYPE + 10 = 0x0065d678: the lobby's
//      chat packet (type 0x0d) is shown by llm_lobby_announce_line(.., &_G_LLM_LOBBY_RX_TYPE), which
//      widens `src + 10`. The LOCAL lobby echo (src + 10 = 0x00644356) is the lobby chat EDIT buffer,
//      which since the lobby follow-up is typed as UTF-8 too -- discriminator (d) below.
//  (c) the CALLER is the ORIGINAL lockstep dispatch's chat arm: its widen of the received text is
//      `call llm_str_ansi_to_wide_scratch` at 0x0049d1cd, so the scratch wrapper's return address is
//      0x0049d1d2. The scratch wrapper (0x004cf3e0) opens `push ebp; mov ebp,esp` and calls us at
//      0x004cf412, so when OUR return address is 0x004cf417 its caller's is at [EBP+4]. The PROMOTED
//      dispatch (libmh rx_dispatch.cpp handle_chat) never comes through here for the body -- it
//      decodes with the same codec itself (dispatch_calls::chat_to_wide_scratch).
constexpr uintptr_t ADDR_LOBBY_RX_TEXT     = 0x0065d678u; // _G_LLM_LOBBY_RX_TYPE (0x0065d66e) + 10
constexpr uintptr_t ADDR_LOBBY_CHAT_PACKET = 0x0064434cu; // llm_lobby_chat_send_cb's payload (type 0x0d)
constexpr uintptr_t RET_IN_SCRATCH         = 0x004cf417u; // after the scratch wrapper's call to H3
constexpr uintptr_t RET_RX_CHAT_TEXT       = 0x0049d1d2u; // after llm_net_lockstep_dispatch's body widen

// ---- MP-LANG: the name fields -------------------------------------------------------------------
//
// The edit-field modal (llm_ui_edit_field_modal_tick 0x004bbd30) edits `*param_block` of the PENDING
// widget; H1 runs inside the modal key pump that feeds it, so "which buffer is being typed into" is a
// read of that pointer. Two buffers take the [A-Za-z0-9] rule: the player name and the game name
// (the lobby title every other peer's browser lists).
constexpr uintptr_t ADDR_PENDING_WIDGET = 0x00654299u; // _G_LLM_UI_MENU_PENDING_WIDGET, llm_ui_widget *
constexpr int       WIDGET_FLAGS        = 0x08;        // llm_ui_widget.flags (the font is flags & 3)
constexpr int       WIDGET_PARAM_BLOCK  = 0x30;        // llm_ui_widget.param_block (docs/structs.md)
constexpr int       NAME_CAP            = 32;          // both globals are char[32] (0x5d0d88 / 0x5d0da8)

// ---- the LOBBY CHAT field (mp:MP-LANG follow-up, 2026-09-29) --------------------------------------
//
// The lobby's chat line is a menu EDIT FIELD, not the in-game line: widget 0x006503f7 (action_cb
// llm_lobby_chat_send_cb 0x004be7b1, draw_cb 0x004c13da), param block 0x00644f03. Its buffer is the
// text half of the chat packet itself (0x0064434c + 10), cap 0x40 (read from the block, not assumed).
// The edit field's modal tick (llm_ui_edit_field_modal_tick 0x004bbd30) edits the block ONE BYTE per
// keystroke, and it and its draw callback keep three more numbers that are all byte-for-glyph:
//
//   [4] cursor (byte index)   [5] caret x (px: the sum of FUN_004b5663(font, byte) per byte)
//   [6] scroll start (index)  [7] scroll x (px)   [8] length (bytes; the draw re-derives it by strlen)
//
// So the field holds UTF-8 only if every one of those is kept on a character boundary and measured in
// characters. H1 does the edits for this field itself whenever a byte-at-a-time edit would be wrong
// (a non-ASCII character, or backspace / delete / left / right next to one) and leaves every ASCII
// edit, Enter, Esc, Home, End and up/down to the original; H3 decodes the buffer as UTF-8 wherever it
// is widened (the draw widens `buf + [6]` and `buf + [4]`, End widens `buf`); H5 re-expresses the one
// place the draw converts between a byte offset and a unit index (the line fitter, below).
constexpr uintptr_t ADDR_LOBBY_CHAT_WIDGET = 0x006503f7u; // llm_ui_widget_006503f7 (EN)
constexpr uintptr_t ADDR_LOBBY_CHAT_BUF    = 0x00644356u; // DAT_00644356 = chat packet 0x0064434c + 10
constexpr int       LOBBY_CHAT_CAP_MAX     = 0x40;        // the block's own [2] (0x00644f0b), measured EN
// H1 must know WHO dequeued. The key ring has two consumers (ReVA xrefs to 0x004cb91e): the modal key
// pump (0x004b52be -> returns to 0x004b52c3) and llm_ui_menu_transition_settle, which drains. Only a
// keystroke the pump latches INSIDE the edit field's modal tick is typed into the field, so the lobby
// path runs only when our return address is the pump's call site AND the pump's own (at [EBP+4]: the
// pump opens `push ebp; mov ebp,esp`, and the U24 trampoline replays exactly that prologue) lies in
// the tick. The pending widget alone is not enough: it can outlive the edit.
constexpr uintptr_t RET_IN_PUMP  = 0x004b52c3u;
constexpr uintptr_t EDIT_TICK_LO = 0x004bbd30u; // llm_ui_edit_field_modal_tick
constexpr uintptr_t EDIT_TICK_HI = 0x004bc089u; // its end (exclusive)
// H5: llm_ui_text_wrap_find_break_ansi -- ONE caller, the edit field's draw (0x004c1479). It widens the
// buffer into the scratch at 0x00660d24 and then treats a BYTE offset as a UNIT index twice: the fit's
// end pointer is `scratch + (end - text) * 2`, and the fitted count it stores is read back by the draw
// as a byte offset (`buf + [6]`). Register contract (disassembly): font EAX, text EDX, out EBX, width
// ECX, [esp+4] end, [esp+8] out_count; preserves ESI/EDI; `ret 8`.
constexpr uintptr_t ADDR_WRAP_ANSI    = 0x004b60f6u;
constexpr uintptr_t ADDR_WIDE_SCRATCH = 0x00660d24u; // llm_str_ansi_to_wide_scratch's buffer
// H6: utils_WideStringToAscii -- the NARROW half of every edit field's round trip. On Enter (and Esc)
// the tick widens the buffer and hands it to llm_ui_widget_set_text, which narrows it BACK into the
// field's buffer and its Esc copy through this function, i.e. WideCharToMultiByte(CP_ACP, ...) with a
// default character. For UTF-8 lobby text that was measured to destroy the line at the moment it is
// sent: "Привет Łód!ź ok" left as "        L\xF3d!z ok" (Cyrillic -> the default char, Ł -> best-fit
// 'L'). Contract (disassembly): dst EAX, src EDX, count EBX (units, and the byte budget), writes
// dst[count] = 0; preserves ECX/ESI/EDI; plain `ret`. Two callers (utils_wide_to_short_str,
// GetAsciiString); only a lobby chat destination changes -- everything else keeps CP_ACP, as retail.
constexpr uintptr_t ADDR_WIDE2ASCII    = 0x004cf2dcu;
constexpr uintptr_t ADDR_DEFAULT_CHARP = 0x00661524u; // lpDefaultChar_00661524, the original's argument

// ---- state -------------------------------------------------------------------------------------
UINT  g_cp                 = 0;     // this peer's 8-bit codepage for NON-chat text; 0 = not resolved yet
bool  g_cp_pinned          = false; // true iff [input] codepage named a real number (not `acp`)
bool  g_chat_utf8          = true;  // false only under [input] chat_legacy_codepage=1 (test emulation of F3c)
char  g_test_join_name[32] = {};    // [input] test_join_name: a FORGED JOIN name (test only, see the export)
bool  g_armed              = false;
void *g_tramp_chat         = nullptr; // stolen prologue + jmp llm_ui_chat_input_process_scancodes+8
void *g_tramp_measure      = nullptr; // stolen prologue + jmp llm_gfx_font_measure_text_width+8
int   g_hooks              = 0;       // how many of the six actually installed
bool  g_lobby_utf8         = false;   // H1 armed AND chat is UTF-8: the lobby chat buffer holds UTF-8
bool  g_wrap_armed         = false;   // H5 armed: the lobby field's scroll is measured in characters

long g_resolved      = 0; // characters the codec produced (H1 + H2)
long g_chat_chars    = 0; // of those, characters inserted into the chat line by H2
long g_unmappable    = 0; // characters the LAYOUT produced that this peer's codepage cannot represent (H1)
long g_chat_full     = 0; // characters H2 refused whole because the line had no room for all their bytes
long g_name_rejected = 0; // keystrokes H1 refused in a name field
long g_utf8_widens   = 0; // widens H3 decoded as UTF-8 (chat)
long g_lobby_chars   = 0; // non-ASCII characters H1 inserted into the lobby chat field itself
long g_lobby_edits   = 0; // backspace/delete/left/right H1 did itself across a multi-byte character
long g_lobby_full    = 0; // lobby characters refused whole for lack of room
long g_lobby_scrolls = 0; // H5 line fits computed for the UTF-8 lobby field (logged, first few)

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
    if (!g_cp) g_cp = GetACP(); // never armed (e.g. the offline oracle) -> today's behaviour
    return g_cp;
}

// ---- THE CODEC ---------------------------------------------------------------------------------

// One code point -> one byte of this peer's codepage, or 0 if it has none. `bad` is what makes
// "unrepresentable" a MEASURED answer rather than a silent '?': with a NUL default character an
// unmappable input comes back as 0 AND sets the flag, where ToAscii would have returned 0x3F and
// looked like a successful translation of a question mark.
unsigned char encode_cp(uint32_t cp) {
    if (cp == 0 || cp > 0xFFFFu) {
        ++g_unmappable;
        return 0;
    }
    const wchar_t wc   = (wchar_t)cp;
    char          b[8] = {0};
    BOOL          bad  = FALSE;
    const int     m    = WideCharToMultiByte(resolved_cp(), 0, &wc, 1, b, (int)sizeof(b), "\0", &bad);
    if (m != 1 || bad || b[0] == '\0') {
        ++g_unmappable;
        return 0;
    }
    return (unsigned char)b[0];
}

// scancode + key-state -> the CODE POINT this keystroke means on the ACTIVE layout, or 0 for "this
// key produces no character" (the caller's cue to fall back to the game's own behaviour: H1's
// scancode table, H2's original switch arm). No codepage is involved yet: H1 encodes the result in
// this peer's codepage, H2 in UTF-8.
//
// ToUnicodeEx, not ToAsciiEx: ToAsciiEx would do the codepage conversion itself, with the layout's
// codepage rather than ours, and hand back a byte we could no longer re-encode.
uint32_t translate_sc(UINT sc, const BYTE *keystate) {
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
    uint32_t cp = 0;
    if (n == 1) {
        cp = (uint32_t)(uint16_t)w[0];
    } else if (n == 2 && w[0] >= 0xD800 && w[0] <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
        cp = 0x10000u + ((((uint32_t)w[0]) - 0xD800u) << 10) + (((uint32_t)w[1]) - 0xDC00u);
    } else {
        return 0; // 0 = no character; otherwise a multi-character ligature neither buffer takes
    }
    if (cp < 0x20u || cp == 0x7Fu) return 0; // control characters are the game's own business
    ++g_resolved;
    return cp;
}

// The OS key-state table as the codec should see it -- whatever the thread really has, which is
// exactly what ToAscii read before this seam and what the UI harness's `type` writes for the
// modifier half of a keystroke.
//
// `with_game_latches` is H2's, and only H2's. The chat drain runs a frame AFTER the producer queued
// the scancode, and the original switch it replaces read the shift state from the game's own HELD
// latches rather than from the OS -- so a shift the game believes is down must not be lost by the
// change of source. H1 translates the event it has just dequeued, in the same breath the original
// did, and passes the OS table through UNCHANGED.
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

// MP-LANG: is the edit field being typed into one of the two NAME buffers? Guarded: the pending
// widget of a non-edit context has a param_block of some other shape (a spinner block), and a
// widget pointer the menu left stale must not turn a keystroke into an access violation.
bool name_field_active() {
    __try {
        const uintptr_t w = *(volatile const uintptr_t *)ADDR_PENDING_WIDGET;
        if (!w) return false;
        const uintptr_t pb = *(volatile const uintptr_t *)(w + WIDGET_PARAM_BLOCK);
        if (!pb) return false;
        const uintptr_t buf = *(volatile const uintptr_t *)pb;
        return buf == mh::addr::mp_player_name || buf == mh::addr::mp_game_name;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the lobby chat field's own edits (H1's lobby path) ----------------------------------------

struct EditBlock {   // the edit field's param block, as llm_ui_edit_field_modal_tick indexes it (ints)
    char   *buf;     // [0]
    char   *backup;  // [1] what Esc restores the widget text from
    int32_t cap;     // [2] buffer size INCLUDING the NUL
    int32_t flags;   // [3]
    int32_t cur;     // [4] cursor, bytes
    int32_t caret;   // [5] caret x, px
    int32_t scroll;  // [6] first drawn byte
    int32_t scrollx; // [7] its x, px
    int32_t len;     // [8] length, bytes
};

// The pending widget is the lobby chat field AND its block has the shape measured above. Guarded like
// name_field_active: a stale widget pointer must not turn a keystroke into an access violation.
EditBlock *lobby_chat_block() {
    __try {
        if (*(volatile const uintptr_t *)ADDR_PENDING_WIDGET != ADDR_LOBBY_CHAT_WIDGET) return nullptr;
        EditBlock *b = *(EditBlock *volatile *)(ADDR_LOBBY_CHAT_WIDGET + WIDGET_PARAM_BLOCK);
        if (!b || (uintptr_t)b->buf != ADDR_LOBBY_CHAT_BUF) return nullptr;
        if (b->cap < 2 || b->cap > LOBBY_CHAT_CAP_MAX) return nullptr;
        if (b->len < 0 || b->len >= b->cap || b->cur < 0 || b->cur > b->len) return nullptr;
        return b;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// [5] from scratch: the width of the characters before the cursor, measured the way the game measures
// every other string (llm_ui_text_measure_width over the widened text), in the field's own font. For an
// all-ASCII prefix this is exactly the per-byte sum the original keeps, so mixing our edits with the
// original's ASCII edits stays consistent.
void lobby_recaret(EditBlock *b) {
    uint16_t w[LOBBY_CHAT_CAP_MAX + 1];
    np::utf8_to_utf16(b->buf, (size_t)b->cur, w, sizeof(w) / sizeof(w[0]));
    const uint32_t flags = *(volatile const uint32_t *)(ADDR_LOBBY_CHAT_WIDGET + WIDGET_FLAGS);
    const uint32_t font  = (uint32_t)(uintptr_t)mh::call::llm_gfx_font_desc_for_flags(flags);
    b->caret             = mh::call::llm_ui_text_measure_width(font, w);
}

// The four editing keys, when the character they step over is MULTI-byte (the original's one-byte arm
// would tear it). E0-prefixed scancodes, as the tick compares them. Returns true if handled here.
bool lobby_edit_key(EditBlock *b, UINT sc) {
    size_t len = (size_t)b->len, cur = (size_t)b->cur;
    switch (sc) {
        case 0x0eu: // backspace
            if (np::utf8_prev_len(b->buf, cur) < 2) return false;
            np::utf8_edit_erase(b->buf, &len, &cur, /*before=*/true);
            break;
        case 0xd3u: // delete
            if (np::utf8_next_len(b->buf, cur, len) < 2) return false;
            np::utf8_edit_erase(b->buf, &len, &cur, /*before=*/false);
            break;
        case 0xcbu: // left
            if (np::utf8_prev_len(b->buf, cur) < 2) return false;
            cur -= (size_t)np::utf8_prev_len(b->buf, cur);
            break;
        case 0xcdu: // right
            if (np::utf8_next_len(b->buf, cur, len) < 2) return false;
            cur += (size_t)np::utf8_next_len(b->buf, cur, len);
            break;
        default:
            return false;
    }
    b->len = (int32_t)len;
    b->cur = (int32_t)cur;
    lobby_recaret(b);
    ++g_lobby_edits;
    return true;
}

// A non-ASCII character: its whole UTF-8 sequence at the cursor, or nothing (the original's own "full"
// case also inserts nothing).
void lobby_insert(EditBlock *b, uint32_t cp) {
    char      bytes[4];
    const int nb  = np::utf8_encode(cp, bytes);
    size_t    len = (size_t)b->len, cur = (size_t)b->cur;
    if (nb > 0 && np::utf8_edit_insert(b->buf, (size_t)b->cap, &len, &cur, bytes, (size_t)nb)) {
        b->len = (int32_t)len;
        b->cur = (int32_t)cur;
        lobby_recaret(b);
        ++g_lobby_chars;
    } else {
        ++g_lobby_full;
    }
}

// ---- H1: llm_input_key_dequeue_translate_ascii -------------------------------------------------
//
// The original, line for line, with ToAscii replaced by the codec -- plus the MP-LANG name rule and the
// lobby chat field's UTF-8 path. Layout:
//   key_event[0] = the set-1 scancode (written by llm_input_key_dequeue; E0 keys carry 0x80)
//   key_event[1] = event type: 0x100 press, 0x80 release (the pump's publish reads bytes +4/+5)
//   key_event[2] = the resolved character (what every caller reads)
// `outer_ret` is the PUMP's return address when the pump is the caller, else 0 (see RET_IN_PUMP).
void key_dequeue_translate(uint32_t *key_event, uintptr_t outer_ret) {
    mh::call::llm_input_key_dequeue((uint32_t)(uintptr_t)key_event);
    const UINT sc = key_event[0];

    // THE LOBBY CHAT FIELD, typed as UTF-8. Only a PRESS latched by the pump inside the edit tick is
    // edited here; the edit is applied now and the event is neutralized (scancode and character 0 -- no
    // arm of the tick matches either), so when the pump publishes it 150 ms later there is nothing
    // left to do. Keys are latched one at a time, and only after the previous one was dispatched, so
    // the order of our edits and the original's is the order they were typed.
    const bool press = (key_event[1] & 0x100u) != 0; // releases pass through here too (never dispatched)
    EditBlock *lobby = nullptr;
    if (g_lobby_utf8 && press && outer_ret >= EDIT_TICK_LO && outer_ret < EDIT_TICK_HI)
        lobby = lobby_chat_block();
    if (lobby && lobby_edit_key(lobby, sc)) {
        key_event[0] = 0;
        key_event[2] = 0;
        return;
    }

    BYTE st[256];
    build_keystate(st, /*with_game_latches=*/false);
    const uint32_t cp = translate_sc(sc, st);
    if (lobby && cp >= 0x80u) {
        lobby_insert(lobby, cp);
        key_event[0] = 0;
        key_event[2] = 0;
        return;
    }
    // (an ASCII character in the lobby field is inserted by the original, exactly as retail does)
    const unsigned char ch = cp ? encode_cp(cp) : 0;
    if (ch) {
        key_event[2] = ch;
    } else if (cp) {
        // THE LAYOUT PRODUCED A CHARACTER THIS CODEPAGE CANNOT HOLD (a Cyrillic letter under CP1252).
        // Nothing is typed. The scancode table below is the US-QWERTY map, so falling back to it would
        // type the LATIN letter that shares the key (П -> 'g'): the defect the user reported on
        // 2026-09-29, and in a name field a Latin letter that then PASSES the [A-Za-z0-9] rule. Counted
        // (encode_cp did) so it is never silent.
        key_event[2] = 0;
        if (press && name_field_active()) {
            if (g_name_rejected < 8)
                ci_log("; [input] name field: REJECTED U+%04X (this codepage has no byte for it) -- names are "
                       "Latin letters and digits (MP-LANG)",
                       (unsigned)cp);
            ++g_name_rejected;
        }
    } else {
        // THE ORIGINAL FALLBACK, UNCHANGED. Shift/caps select the second byte of the scancode's pair;
        // the original tested `& 3` at all three sites, which is `& 1` in practice (there is no
        // E0-prefixed twin scancode for Shift or Caps Lock -- see the Ghidra plate on 0x00e69d4a).
        const bool alt = ((*(volatile const uint8_t *)ADDR_RSHIFT & 3) != 0) ||
                         ((*(volatile const uint8_t *)ADDR_LSHIFT & 3) != 0) ||
                         ((*(volatile const uint8_t *)ADDR_CAPS & 3) != 0);
        const uint8_t *tab = (const uint8_t *)ADDR_KEYTAB;
        key_event[2]       = tab[(sc & (KEYTAB_MAX - 1)) * 2 + (alt ? 1 : 0)];
        // A table byte above 0x7F would land in the UTF-8 lobby line as a lone invalid byte.
        if (lobby && key_event[2] >= 0x80u) key_event[2] = 0;
    }
    // MP-LANG: a NAME accepts [A-Za-z0-9] and nothing else. Only printable characters are judged --
    // the edit field dispatches Enter / Backspace / the arrows on the scancode, and the character
    // slot of those keys is a control byte (< 0x20) or the table's navigation code, both untouched.
    const uint32_t c = key_event[2];
    if (c >= 0x20u && c < 0x100u && !np::name_char_ok((unsigned char)c) && name_field_active()) {
        if (press && g_name_rejected < 8)
            ci_log("; [input] name field: REJECTED U+%04X (byte 0x%02x) -- names are Latin letters and digits "
                   "(MP-LANG)",
                   (unsigned)cp, (unsigned)c);
        if (press) ++g_name_rejected;
        key_event[2] = 0;
    }
}

// __watcall(uint* in EAX) -> void, plain `ret` (no stack arguments to clean). The pump's own return
// address is read from [EBP+4] only when OUR return address is the pump's call site -- only then is
// EBP known to be the pump's frame.
// clang-format off
__declspec(naked) void translate_thunk() {
    __asm {
        xor  ecx, ecx
        cmp  dword ptr [esp], 0x004b52c3 // RET_IN_PUMP
        jne  no_pump
        mov  ecx, dword ptr [ebp + 4]    // the pump's return address
    no_pump:
        push ecx
        push eax
        call key_dequeue_translate
        add  esp, 8
        ret
    }
}
// clang-format on

static_assert(RET_IN_PUMP == 0x004b52c3u, "translate_thunk spells this literal in its asm");

// ---- H3: llm_str_ansi_to_wide ------------------------------------------------------------------
//
// The original is `MultiByteToWideChar(CP_ACP, 0, src, len, dst, len); dst[len] = 0; return dst;`
// with `len` from an inline strlen. Two changes: the codepage is this peer's (F3), and a CHAT source
// (see the discriminators above) is decoded as UTF-8 instead (MP-LANG). The UTF-8 decode writes at
// most `len` units -- never more than the original would have -- so it fits every buffer the game
// sized for this call.
bool chat_source(const char *src, uintptr_t outer_ret) {
    const uintptr_t s = (uintptr_t)src;
    if (s >= ADDR_CHAT_LINE && s < ADDR_CHAT_LINE + CHAT_Q_OFF) return true; // (a) the in-game edit line
    if (s == ADDR_LOBBY_RX_TEXT) return true;                                // (b) a received lobby line
    if (outer_ret == RET_RX_CHAT_TEXT) return true;                          // (c) the original chat arm
    if (!g_lobby_utf8) return false;
    // (d) the lobby chat EDIT buffer, anywhere inside it: the field's draw widens `buf + [6]` and
    // `buf + [4]`, End widens `buf`, and the local echo (llm_lobby_announce_line) widens packet + 10,
    // which is the same buffer.
    if (s >= ADDR_LOBBY_CHAT_BUF && s < ADDR_LOBBY_CHAT_BUF + LOBBY_CHAT_CAP_MAX) return true;
    // (e) the Esc restore copy ([1]) -- but only while the lobby chat field is the one being edited: the
    // backup buffer belongs to whichever field is open, and every other field is 8-bit local text.
    if (*(volatile const uintptr_t *)ADDR_PENDING_WIDGET != ADDR_LOBBY_CHAT_WIDGET) return false; // cheap first
    if (EditBlock *b = lobby_chat_block())
        return s >= (uintptr_t)b->backup && s < (uintptr_t)b->backup + (uintptr_t)b->cap;
    return false;
}

wchar_t *ansi_to_wide(wchar_t *dst, const char *src, uintptr_t outer_ret) {
    const int len = lstrlenA(src);
    if (g_chat_utf8 && chat_source(src, outer_ret)) {
        ++g_utf8_widens;
        np::utf8_to_utf16(src, (size_t)len, (uint16_t *)dst, (size_t)len + 1);
        return dst;
    }
    MultiByteToWideChar(resolved_cp(), 0, src, len, dst, len);
    dst[len] = L'\0';
    return dst;
}

// __watcall(wchar_t *dst in EAX, char *src in EDX) -> wchar_t* in EAX.
// The caller-of-the-scratch-wrapper read is gated on our OWN return address being the wrapper's call
// site: only then is EBP known to be the wrapper's frame, so [EBP+4] is only ever dereferenced there.
// clang-format off
__declspec(naked) void ansi_to_wide_thunk() {
    __asm {
        xor  ecx, ecx
        cmp  dword ptr [esp], 0x004cf417 // RET_IN_SCRATCH
        jne  no_outer
        mov  ecx, dword ptr [ebp + 4]  // the scratch wrapper's own return address
    no_outer:
        push ecx                 // outer_ret
        push edx                 // src  (the second __watcall register argument)
        push eax                 // dst  (the first)
        call ansi_to_wide
        add  esp, 12
        ret                      // the return value is already in EAX for both conventions
    }
}
// clang-format on

static_assert(RET_IN_SCRATCH == 0x004cf417u, "ansi_to_wide_thunk spells this literal in its asm");

// ---- H4 (MP-LANG): the chat caret ---------------------------------------------------------------
//
// llm_gfx_font_measure_text_width has exactly ONE caller (xref, EN 2026-09-29): the chat caret in
// llm_ui_hud_topbar_tick @0x00414579, which measures `wcslen(status prefix) + CURSOR - 1` UTF-16
// units of "<prefix><widened line>". CURSOR is a BYTE index -- and at that moment it is one past the
// original cursor, because the tick has just inserted a placeholder space at the cursor
// (llm_ui_chat_input_char_insert_unbounded(' ')) that it erases after drawing. With a UTF-8 line the
// bytes before the cursor decode to FEWER units than there are bytes, so the count is re-expressed:
// the prefix's units are kept, the line's `CURSOR-1` bytes become the units they decode to. A run-
// before on EDX (max_chars); nothing else about the measure changes.
void caret_fix(uint32_t *edx) {
    if (!g_chat_utf8) return;
    if (*(volatile const int32_t *)ADDR_CHAT_ACTIVE == 0) return;
    const int32_t orig = *(volatile const int32_t *)ADDR_CHAT_CURSOR - 1;
    if (orig <= 0 || orig > CHAT_LINE_MAX) return;
    const int32_t count = (int32_t)*edx;
    if (count < orig) return; // not the shape the one caller produces -- leave it alone
    const size_t units = np::utf8_utf16_units((const char *)ADDR_CHAT_LINE, (size_t)orig);
    *edx               = (uint32_t)(count - orig + (int32_t)units);
}

// clang-format off
__declspec(naked) void measure_detour() {
    __asm {
        pushad
        lea  eax, [esp + 20]     // the saved EDX slot in the PUSHAD frame (EDI ESI EBP ESP EBX EDX ...)
        push eax
        call caret_fix
        add  esp, 4
        popad
        jmp  dword ptr [g_tramp_measure]
    }
}
// clang-format on

// ---- H5 (MP-LANG follow-up): the lobby chat field's scroll -------------------------------------
//
// llm_ui_text_wrap_find_break_ansi, whole-body replace. The original is: widen `text` into the scratch,
// then llm_ui_text_wrap_find_break(font, scratch, out, width, scratch + (end - text) * 2, out_count),
// which walks BACK from the cursor until the width is spent and stores how many UNITS precede the
// visible part -- a count the draw then uses as a BYTE offset (`buf + [6]`). Byte == unit only for
// 8-bit text, so for the UTF-8 lobby buffer the two conversions are done properly: bytes -> units on
// the way in, units -> bytes on the way out. Every other text (the name / IP fields) takes the
// original's exact arithmetic.
int32_t wrap_break_ansi(void *font, char *text, void *out, int32_t width, int32_t end, int32_t *out_count) {
    void           *scratch = mh::call::llm_str_ansi_to_wide_scratch(text); // H3 decodes a chat source as UTF-8
    const uintptr_t t       = (uintptr_t)text;
    if (g_lobby_utf8 && t == ADDR_LOBBY_CHAT_BUF && end >= (int32_t)t) {
        const size_t  nbytes = (size_t)(end - (int32_t)t);
        const size_t  units  = np::utf8_utf16_units(text, nbytes);
        int32_t       fitted = 0;
        const int32_t px     = mh::call::llm_ui_text_wrap_find_break(
            font, scratch, out, width, (void *)((uintptr_t)scratch + units * 2), &fitted);
        const int32_t start = (int32_t)np::utf8_bytes_for_units(text, nbytes, fitted < 0 ? 0 : (size_t)fitted);
        if (out_count) *out_count = start;
        if (++g_lobby_scrolls <= 4)
            ci_log("; [input] lobby chat field scrolled: cursor at byte %d (%d units), first drawn byte %d "
                   "(%d units), %d px hidden (MP-LANG H5)",
                   (int)nbytes, (int)units, (int)start, (int)fitted, (int)px);
        return px;
    }
    return mh::call::llm_ui_text_wrap_find_break(font, (void *)ADDR_WIDE_SCRATCH, out, width,
                                                 (void *)(ADDR_WIDE_SCRATCH + (uintptr_t)(end - (int32_t)t) * 2),
                                                 out_count);
}

// font EAX, text EDX, out EBX, width ECX, end [esp+4], out_count [esp+8]; `ret 8`. The C body keeps
// ESI/EDI/EBX/EBP (cdecl), which covers everything the original preserved (it saves ESI/EDI only).
// clang-format off
__declspec(naked) void wrap_break_ansi_thunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp + 12] // out_count
        push dword ptr [ebp + 8]  // end
        push ecx                  // width
        push ebx                  // out
        push edx                  // text
        push eax                  // font
        call wrap_break_ansi
        add  esp, 24
        pop  ebp
        ret  8
    }
}
// clang-format on

// ---- H6 (MP-LANG follow-up): the lobby chat field's narrow --------------------------------------
//
// A lobby chat DESTINATION -- the edit buffer, or (while that field is the one open) its Esc copy --
// is narrowed as UTF-8, bounded by the field's own cap and never torn; the widen that fed it was H3's
// UTF-8 decode of the same buffer, so the round trip is the identity. Anything else: the original,
// argument for argument.
bool lobby_destination(const char *dst, size_t *cap) {
    const uintptr_t d = (uintptr_t)dst;
    if (d >= ADDR_LOBBY_CHAT_BUF && d < ADDR_LOBBY_CHAT_BUF + LOBBY_CHAT_CAP_MAX) {
        *cap = (size_t)(ADDR_LOBBY_CHAT_BUF + LOBBY_CHAT_CAP_MAX - d);
        return true;
    }
    if (*(volatile const uintptr_t *)ADDR_PENDING_WIDGET != ADDR_LOBBY_CHAT_WIDGET) return false;
    EditBlock *b = lobby_chat_block();
    if (!b || d != (uintptr_t)b->backup) return false;
    *cap = (size_t)b->cap;
    return true;
}

void wide_to_ascii(char *dst, const wchar_t *src, int n) {
    size_t cap = 0;
    if (g_lobby_utf8 && n >= 0 && lobby_destination(dst, &cap)) {
        np::utf16_to_utf8((const uint16_t *)src, (size_t)n, dst, cap);
        return;
    }
    BOOL used = FALSE;
    WideCharToMultiByte(CP_ACP, 0, src, n, dst, n, *(const char *const *)ADDR_DEFAULT_CHARP, &used);
    dst[n] = '\0';
}

// dst EAX, src EDX, n EBX; ECX/ESI/EDI preserved (the C body keeps EBX/ESI/EDI; ECX by hand).
// clang-format off
__declspec(naked) void wide_to_ascii_thunk() {
    __asm {
        push ecx
        push ebx                  // n
        push edx                  // src
        push eax                  // dst
        call wide_to_ascii
        add  esp, 12
        pop  ecx
        ret
    }
}
// clang-format on

// ---- H2: llm_ui_chat_input_process_scancodes ---------------------------------------------------

// THE TYPEWRITER BLOCK: exactly the scancodes whose arm in the original `switch` assigns a
// character. Enumerated from the decompile of 0x00414a4e rather than from a keyboard picture.
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

// MP-LANG: how many times the ORIGINAL must run a control key so it steps over one whole CHARACTER.
// The original's four editing arms move by ONE BYTE (0x0e backspace, 0x53 delete, 0x4b / 0x4d the
// cursor -- arms read in the decompile of 0x00414a4e); a UTF-8 character is 1..3 bytes, so each is
// repeated for the length of the sequence it would otherwise cut. Everything else runs once.
int control_repeat(unsigned sc) {
    const char   *line = (const char *)ADDR_CHAT_LINE;
    const int32_t len  = *(volatile const int32_t *)ADDR_CHAT_LEN;
    const int32_t cur  = *(volatile const int32_t *)ADDR_CHAT_CURSOR;
    if (len < 0 || len > CHAT_LINE_MAX || cur < 0 || cur > len) return 1;
    int k = 1;
    if (sc == 0x0eu || sc == 0x4bu) k = np::utf8_prev_len(line, (size_t)cur);
    if (sc == 0x53u || sc == 0x4du) k = np::utf8_next_len(line, (size_t)cur, (size_t)len);
    return k < 1 ? 1 : k;
}

// Run-before at the drain's entry. Takes the whole queue, then replays it ONE ENTRY AT A TIME in
// the original order: a typewriter key becomes the codec's UTF-8 bytes, anything else is handed to
// the original body with a one-entry queue. Order is preserved because the replay is sequential -- a
// batch like `a b <backspace> c` still ends as "ac".
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
        const unsigned sc = q[i];
        const uint32_t cp = sc_is_typewriter(sc) ? translate_sc(sc, st) : 0;
        if (cp) {
            char bytes[4];
            int  nb = 0;
            if (g_chat_utf8) {
                nb = np::utf8_encode(cp, bytes);
            } else {
                const unsigned char b = encode_cp(cp); // chat_legacy_codepage=1: F3's 8-bit line
                if (b) bytes[nb++] = (char)b;
            }
            if (nb > 0) {
                // ROOM FOR THE WHOLE CHARACTER OR NONE OF IT. The game's insert clamps each BYTE at
                // 0x28 and, mid-line, drops the line's last byte to make room -- either of which would
                // tear a multi-byte sequence. Refusing the character up front keeps the line valid
                // UTF-8 at every step, which is also what makes the wire and the history valid.
                const int32_t len = *(volatile const int32_t *)ADDR_CHAT_LEN;
                if (len >= 0 && len + nb <= CHAT_LINE_MAX) {
                    for (int b = 0; b < nb; ++b) mh::call::llm_ui_chat_input_char_insert(bytes[b]);
                    ++g_chat_chars;
                } else {
                    ++g_chat_full;
                }
                continue;
            }
            // not encodable here: fall through to the original, which types its US letter or nothing
        }
        // Not a character on this layout: let the ORIGINAL decide, with a queue holding exactly this
        // one scancode -- repeated so an editing key steps over a whole UTF-8 character. s_void is the
        // shared __watcall shim (it preserves EBX/ESI/EDI, which a Watcom callee is free to clobber);
        // g_tramp_chat is the stolen prologue plus a jump back to the body, so this re-enters the
        // original WITHOUT re-entering our own detour.
        const int reps = g_chat_utf8 ? control_repeat(sc) : 1;
        for (int r = 0; r < reps; ++r) {
            queue[0] = (uint8_t)sc;
            *wp      = 1;
            mh::call::detail::s_void((uintptr_t)g_tramp_chat);
            *wp = 0;
        }
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

// ---- H7 (mp:U48): no key drain may eat an arrow key-UP unseen -------------------------------------
//
// llm_strat_input_update latches _G_LLM_CAM_SCROLL_{LEFT,RIGHT,UP,DOWN}_HELD on an arrow DOWN and clears
// it ONLY on that scancode's UP event. While the lockstep wait overlay (game mode 3) is up the strategic
// input is not the ring's consumer: the ring is drained -- keeping at most the key-DOWNS -- by
//   * llm_net_chat_input_process (0x0044deb3; the wait/sync overlay show + dismiss and every
//     llm_ui_menu_overlay_frame frame while the sync list is up),
//   * llm_ui_menu_transition_settle (0x004b54da; llm_ui_menu_frame runs it every mode-3 frame) and
//   * llm_ui_modal_key_pump (0x004b528f; via llm_input_key_dequeue_translate_ascii).
// An UP destroyed there leaves the latch at 1 and the camera scrolls by itself, after the stall, until
// the arrow is pressed again. The first cut of this fix hooked only the chat pump and the rig's stall row
// still produced the stuck latch in ~1 run in 4: a SUSTAINED stall sits in the menu frame, whose drain is
// the settle/modal pump, not the chat pump. So the hook sits on the one pop primitive they all share,
// llm_input_key_dequeue (0x004d1030): a run-before that looks at the event ABOUT TO BE POPPED and, if it
// is an arrow UP and the caller is NOT llm_strat_input_update (which clears the latch itself), clears that
// latch -- exactly what the stock ladder would have done with the event. Nothing is consumed or altered,
// so chat/menu typing during a stall is untouched.
//
// It reads the ring, NOT _G_LLM_INPUT_KEYSTATE, on purpose: the harness's `key`/`keyhold` and the UI-REC
// journal replay inject ring events and never touch the keystate array, so a level-state rebuild would
// clear every injected hold on the next frame (and the DI poll keeps an arrow's state in bit 1 and a
// numpad key's in bit 0, so "& 1" would be wrong for the real arrows anyway). Scancodes are the ones the
// stock ladder compares (0x48 up / 0x50 down / 0x4b left / 0x4d right) after the ring's &0x7f mask, i.e.
// arrow and numpad alike, as the stock ladder does.
//
// Camera/scroll latches are not sim state: the four words are not in any registry region (raw VAs, as
// ui_drive.cpp's trace reads them), so nothing here can reach the lockstep hash.
//
// The entry is NOT a Watcom frame: `51 52 56 57 | 89 C7 | 8B 15 ...` (push ecx/edx/esi/edi; mov edi,eax;
// mov edx,[imm32]). Eight bytes would split the `mov edx`, so 6 are stolen (an instruction boundary,
// no relative operand) and the entry guard is the first four bytes.
constexpr uintptr_t ADDR_KEY_POP    = 0x004d1030u; // llm_input_key_dequeue (H7)
constexpr uint32_t  KEY_POP_ENTRY   = 0x57565251u; // 51 52 56 57
constexpr uintptr_t ADDR_STRAT_IN   = 0x00441b88u; // llm_strat_input_update (the stock consumer)
constexpr uintptr_t STRAT_IN_END    = 0x00444400u; // generous end of that function (~10 KB); only decides who clears
constexpr uintptr_t ADDR_HELD_LEFT  = 0x005d0bbcu; // _G_LLM_CAM_SCROLL_LEFT_HELD  (scancode 0x4b)
constexpr uintptr_t ADDR_HELD_RIGHT = 0x005d0bc0u; // _G_LLM_CAM_SCROLL_RIGHT_HELD (scancode 0x4d)
constexpr uintptr_t ADDR_HELD_UP    = 0x005d0bc4u; // _G_LLM_CAM_SCROLL_UP_HELD    (scancode 0x48)
constexpr uintptr_t ADDR_HELD_DOWN  = 0x005d0bc8u; // _G_LLM_CAM_SCROLL_DOWN_HELD  (scancode 0x50)

void *g_tramp_pump   = nullptr; // stolen prologue + jmp llm_input_key_dequeue+6
long  g_keyup_saved  = 0;       // arrow key-UPs applied to a latch before a non-strategic drain discarded them
long  g_keyup_logged = 0;

void __cdecl on_key_pop(uintptr_t caller) {
    if (caller >= ADDR_STRAT_IN && caller < STRAT_IN_END) return; // the stock ladder clears its own latch
    const uint32_t r = *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_KEY_READ_IDX & 0x7f;
    const uint32_t w = *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX & 0x7f;
    if (r == w) return; // empty: the primitive pops nothing
    const auto *ev = (const mh::game::mh_llm_input_key_event *)(mh::addr::_G_LLM_INPUT_KEY_EVENTS +
                                                                (size_t)r * sizeof(mh::game::mh_llm_input_key_event));
    if ((ev->event_type & 0x100) || !(ev->event_type & 0x80)) return; // UP events only
    uintptr_t latch = 0;
    switch (ev->scancode & 0x7f) {
        case 0x4b: latch = ADDR_HELD_LEFT; break;
        case 0x4d: latch = ADDR_HELD_RIGHT; break;
        case 0x48: latch = ADDR_HELD_UP; break;
        case 0x50: latch = ADDR_HELD_DOWN; break;
        default: break;
    }
    if (!latch || !*(volatile int32_t *)latch) return;
    *(volatile int32_t *)latch = 0;
    ++g_keyup_saved;
    if (g_keyup_logged < 20) {
        ++g_keyup_logged;
        ci_log("; [input] U48: arrow key-up 0x%02x popped by a non-strategic drain (caller 0x%08x) -- latch "
               "cleared (%ld so far)",
               (unsigned)(ev->scancode & 0x7f), (unsigned)caller, g_keyup_saved);
    }
}

// clang-format off
__declspec(naked) void key_pop_detour() {
    __asm {
        pushad
        pushfd
        mov  eax, [esp + 36]  // the return address: pushfd (4) + pushad (32) sit above it
        push eax
        call on_key_pop
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_tramp_pump] // stolen prologue + jmp llm_input_key_dequeue+6
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
    // CP_UTF8 / CP_UTF7 are multi-byte and cannot round-trip through the byte-indexed menu fields;
    // the CHAT is UTF-8 regardless of this setting (MP-LANG), so there is nothing to gain here either.
    if (v == 65000u || v == 65001u) return 0;
    *pinned = true;
    return v;
}

// A name global normalized in place; the first-run PLACEHOLDER mh.exe seeds when there is no
// setup.dat ("<twoje imie>" / "<nazwa gry>", Polish, llm_game_init_first_run_defaults @0x00453e5e)
// becomes the language-neutral default rather than its Latin residue ("twojeimie").
bool normalize_name_global(uintptr_t addr, const char *placeholder, const char *fallback) {
    char *s         = (char *)addr;
    s[NAME_CAP - 1] = '\0';
    if (placeholder && lstrcmpA(s, placeholder) == 0) {
        lstrcpynA(s, fallback, NAME_CAP);
        return true;
    }
    return np::name_normalize(s, NAME_CAP, fallback);
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
    mh::config::read_ini_string("input", "codepage", "acp", buf, sizeof(buf), ini); // TL-HARN4
    // MP-LANG test emulation: behave like a pre-MP-LANG (F3c) peer on the WIRE -- 8-bit chat in the
    // local codepage, and that codepage advertised/echoed -- so the refusal a current peer gives an
    // old one can be staged on the rig. Never a player setting: it only exists to be refused.
    g_chat_utf8 = GetPrivateProfileIntA("input", "chat_legacy_codepage", 0, ini) == 0;
    // MP-LANG test emulation: the JOIN carries THESE bytes verbatim instead of the (normalized) local
    // name, so the host-side normalization of a forged name can be staged. `hex:` spells high bytes.
    {
        char tj[80];
        mh::config::read_ini_string("input", "test_join_name", "", tj, sizeof(tj), ini);
        if (tj[0] && _strnicmp(tj, "hex:", 4) == 0) {
            int k = 0;
            for (const char *h = tj + 4; h[0] && h[1] && k < (int)sizeof(g_test_join_name) - 1; h += 2) {
                char two[3]           = {h[0], h[1], 0};
                g_test_join_name[k++] = (char)strtoul(two, nullptr, 16);
            }
            g_test_join_name[k] = 0;
        } else {
            lstrcpynA(g_test_join_name, tj, sizeof(g_test_join_name));
        }
        if (g_test_join_name[0])
            ci_log("; [input] MP-LANG TEST: test_join_name set -- the JOIN will carry a FORGED %d-byte name",
                   lstrlenA(g_test_join_name));
    }
    bool       pinned = false;
    const UINT want   = parse_codepage(buf, &pinned);
    if (want == 0) {
        ci_log("; [input] codepage '%s' REFUSED (UTF-7/UTF-8 are multi-byte; the menu fields are "
               "byte-indexed -- chat is UTF-8 regardless) -- falling back to the process ACP %u",
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

    // H1 and H3 are whole-body replaces; H2 and H4 are run-befores. Each is best-effort and reported
    // individually -- a refused hook leaves that path at retail behaviour, and the seam says which.
    bool h1 = false;
    if (install_jmp(ADDR_TRANSLATE, (const void *)translate_thunk, mh::hook::entry_claim::exclusive,
                    "the F3 layout-aware key translate")) {
        ++g_hooks;
        h1 = true;
    } else
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
                    mh::hook::entry_claim::exclusive, "the F3 codepage / MP-LANG UTF-8 chat widen"))
        ++g_hooks;
    else
        ci_log("; [input] H3 NOT armed: install refused at 0x%08x (see the [interlock] line in "
               "mh_net.log) -- displayed text keeps CP_ACP",
               (unsigned)ADDR_ANSI2WIDE);

    if (install_trampoline(ADDR_MEASURE, (void *)measure_detour, &g_tramp_measure, 8,
                           mh::hook::entry_claim::exclusive, "the MP-LANG UTF-8 chat caret"))
        ++g_hooks;
    else
        ci_log("; [input] H4 NOT armed: install refused at 0x%08x -- the chat caret counts bytes, so it "
               "sits right of a non-ASCII prefix",
               (unsigned)ADDR_MEASURE);

    // H7 (mp:U48): default ON -- the fix is the shipped behaviour; `[input] scroll_keyup_rescue=0` is the
    // negative arm (retail: the overlay's chat pump discards an arrow key-up and the scroll latch sticks).
    if (GetPrivateProfileIntA("input", "scroll_keyup_rescue", 1, ini) != 0) {
        if (install_trampoline(ADDR_KEY_POP, (void *)key_pop_detour, &g_tramp_pump, 6,
                               mh::hook::entry_claim::exclusive, "the U48 arrow key-up rescue", KEY_POP_ENTRY))
            ci_log("; [input] U48 armed -- an arrow key-up a non-strategic drain pops (stall overlay / menu frame) "
                   "clears its scroll latch first (0x%08x)",
                   (unsigned)ADDR_KEY_POP);
        else
            ci_log("; [input] U48 NOT armed: install refused at 0x%08x (see the [interlock] line in mh_net.log) "
                   "-- a key-up eaten by the stall overlay leaves the scroll latch stuck",
                   (unsigned)ADDR_KEY_POP);
    } else {
        ci_log("; [input] U48 OFF ([input] scroll_keyup_rescue=0) -- retail stall-overlay key-up loss");
    }

    // H5 and H6 before the lobby switch flips: without them the field would measure UTF-8 bytes as
    // characters and narrow it through CP_ACP on Enter, so a refused one keeps the whole lobby field on
    // the 8-bit path (and says so).
    if (install_jmp(ADDR_WRAP_ANSI, (const void *)wrap_break_ansi_thunk, mh::hook::entry_claim::exclusive,
                    "the MP-LANG lobby chat field scroll")) {
        ++g_hooks;
        g_wrap_armed = true;
    } else {
        ci_log("; [input] H5 NOT armed: install refused at 0x%08x -- the lobby chat field stays in the local "
               "codepage (its wire copy is converted)",
               (unsigned)ADDR_WRAP_ANSI);
    }
    bool h6 = false;
    if (install_jmp(ADDR_WIDE2ASCII, (const void *)wide_to_ascii_thunk, mh::hook::entry_claim::exclusive,
                    "the MP-LANG lobby chat field narrow")) {
        ++g_hooks;
        h6 = true;
    } else {
        ci_log("; [input] H6 NOT armed: install refused at 0x%08x -- the lobby chat field stays in the local "
               "codepage (its wire copy is converted)",
               (unsigned)ADDR_WIDE2ASCII);
    }
    g_lobby_utf8 = g_chat_utf8 && h1 && g_wrap_armed && h6;

    g_armed = (g_hooks > 0);
    ci_log("; [input] chat/text codec armed (%d/6 hooks) -- chat %s, lobby chat field %s; other text codepage %u "
           "(%s), ACP %u, layout %08x",
           g_hooks, g_chat_utf8 ? "UTF-8 (MP-LANG)" : "LEGACY 8-bit ([input] chat_legacy_codepage=1)",
           g_lobby_utf8 ? "UTF-8" : "local codepage", resolved_cp(),
           g_cp_pinned ? "PINNED by [input] codepage" : "process default", GetACP(),
           (unsigned)(uintptr_t)GetKeyboardLayout(0));
    return g_armed ? 1 : 0;
}

extern "C" unsigned int MH_ChatInput_Codepage(void) { return resolved_cp(); }

extern "C" unsigned int MH_ChatInput_WireEncoding(void) {
    return g_chat_utf8 ? (unsigned)np::CHAT_ENCODING_UTF8 : resolved_cp();
}

extern "C" int MH_ChatInput_LobbyToWire(const char *src, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    if (!src) return 0;
    const int n = lstrlenA(src);
    if (!g_chat_utf8 || g_lobby_utf8) {
        // Legacy emulation: the wire keeps the local bytes. UTF-8 field (the normal case since the
        // lobby follow-up): the buffer already IS the wire text, cut on a sequence boundary.
        const size_t k = g_lobby_utf8 ? np::utf8_truncate(src, (size_t)n, (size_t)(cap - 1))
                                      : (size_t)(n < cap - 1 ? n : cap - 1);
        memcpy(out, src, k);
        out[k] = '\0';
        return (int)k;
    }
    // H1 or H5 refused: the field was typed in this peer's codepage, so convert it.
    wchar_t   w[512];
    const int u = n ? MultiByteToWideChar(resolved_cp(), 0, src, n, w, 511) : 0;
    char      tmp[1536];
    int       m = u > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, u, tmp, (int)sizeof(tmp) - 1, nullptr, nullptr) : 0;
    if (m < 0) m = 0;
    const size_t k = np::utf8_truncate(tmp, (size_t)m, (size_t)(cap - 1));
    memcpy(out, tmp, k);
    out[k] = '\0';
    return (int)k;
}

extern "C" int MH_ChatInput_NormalizeLocalNames(void) {
    int changed = 0;
    if (normalize_name_global(mh::addr::mp_player_name, "<twoje imie>", np::NAME_FALLBACK)) changed |= 1;
    if (normalize_name_global(mh::addr::mp_game_name, "<nazwa gry>", "Game")) changed |= 2;
    if (changed)
        ci_log("; [input] MP-LANG: local %s%s%s normalized to [A-Za-z0-9] -> '%s' / '%s'",
               (changed & 1) ? "player name" : "", changed == 3 ? " + " : "", (changed & 2) ? "game name" : "",
               (const char *)mh::addr::mp_player_name, (const char *)mh::addr::mp_game_name);
    return changed;
}

extern "C" const char *MH_ChatInput_TestJoinName(void) { return g_test_join_name[0] ? g_test_join_name : nullptr; }

extern "C" int MH_ChatInput_NormalizeName(char *s, int cap) {
    if (!s || cap <= 0) return 0;
    return np::name_normalize(s, (size_t)cap, np::NAME_FALLBACK) ? 1 : 0;
}

extern "C" void MH_ChatInput_Stats(long *resolved, long *chat_chars, long *unmappable) {
    if (resolved) *resolved = g_resolved;
    if (chat_chars) *chat_chars = g_chat_chars;
    if (unmappable) *unmappable = g_unmappable;
}

extern "C" void MH_ChatInput_LangStats(long *utf8_widens, long *chat_full, long *name_rejected) {
    if (utf8_widens) *utf8_widens = g_utf8_widens;
    if (chat_full) *chat_full = g_chat_full;
    if (name_rejected) *name_rejected = g_name_rejected;
}
