//
// ui/player_strings.h -- mods:LANG4: mh.dll's own player-visible text, per language pack.
//
// Every string mh.dll draws for a PLAYER (the lobby/browser status line, join refusals, the map
// transfer notices, the desync alert, the cheat refusal, the in-match connection indicator, the
// LANG2 fallback menu button) is a row of ui/player_strings.def. English is compiled in; a language
// pack may carry `lang/<id>/mh_strings.txt` (UTF-8, `key = text` per line, `#` comments) and every
// key it translates is shown in that language. A key the file lacks, carries twice, or carries with
// a different printf shape (the conversions and their order) shows the English -- a translation can
// never crash a wsprintfW call site. Debug overlay / ImGui / log text stays English (user ruling
// 2026-09-29) and is not in the table.
//
// Stock (`[lang] pack` unset / `en`): no file is read and every lookup is the English, so the EN
// build draws exactly the bytes it drew before the table existed.
//
#pragma once
#include <cstddef>

namespace mh {
namespace ui {

enum class Str : int {
#define MH_STR(id, key, en) id,
#include "ui/player_strings.def"
#undef MH_STR
    COUNT
};

constexpr int STR_COUNT = (int)Str::COUNT;
constexpr int STR_MAX   = 256; // units per translated string, NUL included (the status line holds 256)

// The text to show: the pack's translation, else the English. Never null.
const wchar_t *tr(Str s);
const char    *str_key(Str s);
const wchar_t *str_en(Str s);

// Do two format strings take the same arguments (the same %s/%d/%u/%lu sequence; %% ignored)?
bool str_format_compatible(const wchar_t *a, const wchar_t *b);

// PURE: parse a mh_strings.txt body into `out` (STR_COUNT slots of STR_MAX units; out_have[i] set for
// every accepted key). Nothing global is touched -- the selftests run this directly.
struct StrLoadReport {
    int lines;      // non-blank, non-comment lines
    int loaded;     // keys accepted
    int unknown;    // keys no row has
    int bad_format; // translations whose printf shape differs from the English (English is shown)
    int duplicate;  // a key given twice (the FIRST wins)
    int malformed;  // lines with no `=` or an empty key
};
void strings_parse(const char *text, size_t n, wchar_t (*out)[STR_MAX], bool *out_have, StrLoadReport *rep);

// Load lang/<id>/mh_strings.txt beside the exe (exe_dir ends in a separator). `id` "" or null = stock,
// nothing read. `drop_key` (TEST ONLY, `[lang] test_drop_string`) discards one loaded key, so the
// English fallback of a deleted key can be staged on the rig. Logs to mh_video.log. Idempotent.
void strings_load(const char *exe_dir, const char *id, const char *drop_key);

// Keys loaded from the pack file (0 = stock / no file).
int strings_loaded_count();

// A JOIN refusal reason or the relay notice, as the HOST / module composed it in English
// (mh_net_proto join_refusal_text, the UDP module's line), in this peer's language: matched against
// the English rows above (numbers carried over), else the received text widened as-is. Writes a
// NUL-terminated line of at most cap units; returns its length. *matched (optional) says whether a
// row recognized the text -- the selftest's handle on "the host's reasons and the table agree".
int tr_refusal_reason(const char *ascii, wchar_t *out, int cap, bool *matched = nullptr);
int tr_relay_line(const char *ascii, wchar_t *out, int cap, bool *matched = nullptr);

} // namespace ui
} // namespace mh
