// mh_net_proto -- mp:MP-LANG: the two TEXT RULES a multiplayer session needs to be language-agnostic.
// Header-only and portable (no platform headers), because three different builds use it: the injected
// DLL (typing + drawing chat, name entry, the join path), libmh (the promoted lockstep dispatch widens
// received chat), and the selftests that prove both.
//
// THE PROBLEM IT CLOSES. The game's text is 8-bit end to end -- the chat line is char[81] and player
// names are char[32] -- and every string is widened for display through ONE codepage
// (llm_str_ansi_to_wide). mp:F3 made that codepage explicit and F3c made a joiner adopt the host's, so
// two peers agreed -- but only on ONE codepage per session. A CP1251 peer and a CP1250 peer could never
// both type their own letters into the same match, and a CP1251 language pack's resource text had to
// share its codepage with the chat. The ruling (user, 2026-09-29): MP is language-agnostic INCLUDING
// chat; player names are Latin letters and digits; debug text stays English.
//
// SO THE WIRE CARRIES TWO KINDS OF TEXT, AND NEITHER DEPENDS ON A CODEPAGE:
//
//   CHAT  = UTF-8 bytes, in the SAME char[81] buffers and the SAME wire fields (sizes unchanged). A
//           code point is 1..3 bytes (the BMP; a 4-byte sequence is legal and decodes to a surrogate
//           pair, but no single keystroke produces one). Every buffer is cut on a SEQUENCE BOUNDARY,
//           never inside one -- utf8_truncate() -- so a decoder never sees a torn character.
//   NAMES = [A-Za-z0-9] only (name_char_ok). ASCII is identical in every codepage and in UTF-8, so a
//           name means the same letters on every peer with no conversion at all -- which also keeps it
//           out of the codepage question for the lockstep hash, where players[].name lives.
//
// CHAT_ENCODING_UTF8 is the value SESSION_INFO.codepage and JoinRequest.codepage now carry (65001 is
// Windows' own number for UTF-8, CP_UTF8). It is what makes the change a clean protocol step rather
// than a silent one: a pre-MP-LANG peer advertises / echoes its 8-bit codepage, the existing
// join_admit() equality test refuses it by name (RefusedCodepage), and F3c's refusal announce puts the
// reason on that peer's screen. No layout change and no format bump were needed -- see session_info.h.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mh_net_proto {

constexpr std::uint16_t CHAT_ENCODING_UTF8 = 65001;  // CP_UTF8: "this peer's chat bytes are UTF-8"
constexpr std::uint16_t UTF16_REPLACEMENT  = 0xFFFD; // what an invalid byte decodes to

// ---- UTF-8 ------------------------------------------------------------------------------------------

inline bool utf8_is_cont(unsigned char b) noexcept { return (b & 0xC0u) == 0x80u; }

// Length of the sequence a LEAD byte announces (1..4), or 0 for a continuation byte or an invalid
// lead (0xC0/0xC1 overlongs, 0xF5..0xFF).
inline int utf8_seq_len(unsigned char lead) noexcept {
    if (lead < 0x80u) return 1;
    if (lead < 0xC2u) return 0;
    if (lead < 0xE0u) return 2;
    if (lead < 0xF0u) return 3;
    if (lead < 0xF5u) return 4;
    return 0;
}

// One code point -> 1..4 bytes in `out`. Returns the byte count, or 0 for a value that has no UTF-8
// form (a lone surrogate, or above U+10FFFF). NUL encodes as 0 bytes: the buffers are NUL-terminated.
inline int utf8_encode(std::uint32_t cp, char out[4]) noexcept {
    if (cp == 0) return 0;
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp > 0x10FFFFu) return 0;
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

// Decode ONE sequence at s[0..n). Returns the bytes consumed (>= 1 whenever n >= 1) and the code point
// in *cp -- U+FFFD for anything malformed (a stray continuation, a truncated or overlong sequence, an
// encoded surrogate), consuming exactly ONE byte then so the decoder resynchronises on the next lead.
inline int utf8_decode_one(const char *s, std::size_t n, std::uint32_t *cp) noexcept {
    if (n == 0) {
        *cp = 0;
        return 0;
    }
    const unsigned char b0 = (unsigned char)s[0];
    const int           k  = utf8_seq_len(b0);
    if (k == 1) {
        *cp = b0;
        return 1;
    }
    if (k == 0 || (std::size_t)k > n) {
        *cp = UTF16_REPLACEMENT;
        return 1;
    }
    std::uint32_t v = (k == 2) ? (b0 & 0x1Fu) : (k == 3) ? (b0 & 0x0Fu)
                                                         : (b0 & 0x07u);
    for (int i = 1; i < k; ++i) {
        const unsigned char b = (unsigned char)s[i];
        if (!utf8_is_cont(b)) {
            *cp = UTF16_REPLACEMENT;
            return 1;
        }
        v = (v << 6) | (b & 0x3Fu);
    }
    // Overlong 3/4-byte forms, encoded surrogates, and values past the Unicode range are all invalid.
    if ((k == 3 && v < 0x800u) || (k == 4 && (v < 0x10000u || v > 0x10FFFFu)) || (v >= 0xD800u && v <= 0xDFFFu)) {
        *cp = UTF16_REPLACEMENT;
        return 1;
    }
    *cp = v;
    return k;
}

// Decode `n` bytes of UTF-8 (stopping early at a NUL) into UTF-16 at dst, `cap` units INCLUDING the
// terminator; always NUL-terminates when cap > 0. Returns the units written, excluding the NUL. A
// supplementary code point becomes a surrogate pair, or is dropped whole if the pair does not fit.
//
// THE OUTPUT NEVER HAS MORE UNITS THAN THE INPUT HAS BYTES (every sequence is >= as many bytes as the
// units it decodes to), which is what lets this stand in for MultiByteToWideChar(len, len) into a
// buffer the game sized for the byte length.
inline std::size_t utf8_to_utf16(const char *src, std::size_t n, std::uint16_t *dst, std::size_t cap) noexcept {
    if (cap == 0) return 0;
    std::size_t i = 0, w = 0;
    while (i < n && src[i] != '\0') {
        std::uint32_t cp  = 0;
        const int     got = utf8_decode_one(src + i, n - i, &cp);
        i += (std::size_t)got;
        if (cp >= 0x10000u) {
            if (w + 2 >= cap) break;
            cp -= 0x10000u;
            dst[w++] = (std::uint16_t)(0xD800u | (cp >> 10));
            dst[w++] = (std::uint16_t)(0xDC00u | (cp & 0x3FFu));
        } else {
            if (w + 1 >= cap) break;
            dst[w++] = (std::uint16_t)cp;
        }
    }
    dst[w] = 0;
    return w;
}

// Encode `n` UTF-16 units (stopping early at a NUL) as UTF-8 into dst, `cap` bytes INCLUDING the
// terminator; always NUL-terminates when cap > 0 and never writes part of a character -- one that does
// not fit ends the output. A well-formed surrogate pair is one 4-byte character; a lone surrogate
// becomes U+FFFD. Returns the bytes written, excluding the NUL. (The narrow half of a round trip whose
// wide half is utf8_to_utf16: the edit field widens its buffer, the widget setter narrows it back.)
inline std::size_t utf16_to_utf8(const std::uint16_t *src, std::size_t n, char *dst, std::size_t cap) noexcept {
    if (cap == 0) return 0;
    std::size_t i = 0, w = 0;
    while (i < n && src[i] != 0) {
        std::uint32_t cp = src[i++];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i < n && src[i] >= 0xDC00u && src[i] <= 0xDFFFu)
            cp = 0x10000u + ((cp - 0xD800u) << 10) + ((std::uint32_t)src[i++] - 0xDC00u);
        else if (cp >= 0xD800u && cp <= 0xDFFFu)
            cp = UTF16_REPLACEMENT;
        char      b[4];
        const int k = utf8_encode(cp, b);
        if (k <= 0 || w + (std::size_t)k + 1 > cap) break;
        for (int j = 0; j < k; ++j) dst[w++] = b[j];
    }
    dst[w] = '\0';
    return w;
}

// How many UTF-16 units the first `nbytes` bytes of `s` decode to -- the conversion a byte-indexed
// cursor needs before it can be handed to anything that counts characters (the chat caret).
inline std::size_t utf8_utf16_units(const char *s, std::size_t nbytes) noexcept {
    std::size_t i = 0, units = 0;
    while (i < nbytes && s[i] != '\0') {
        std::uint32_t cp  = 0;
        const int     got = utf8_decode_one(s + i, nbytes - i, &cp);
        i += (std::size_t)got;
        units += (cp >= 0x10000u) ? 2u : 1u;
    }
    return units;
}

// The longest prefix of s[0..len) that is at most `max` bytes AND ends on a sequence boundary. A torn
// trailing sequence is dropped whole rather than left as a half character.
inline std::size_t utf8_truncate(const char *s, std::size_t len, std::size_t max) noexcept {
    if (len <= max) return len;
    std::size_t cut = max;
    while (cut > 0 && utf8_is_cont((unsigned char)s[cut])) --cut;
    return cut;
}

// Bytes of the character that ENDS at `pos` (what a backspace or a cursor-left must step over): 1 for
// ASCII or anything malformed, else the length of the well-formed sequence that finishes there.
inline int utf8_prev_len(const char *s, std::size_t pos) noexcept {
    if (pos == 0) return 0;
    std::size_t start = pos - 1;
    int         back  = 1;
    while (start > 0 && back < 4 && utf8_is_cont((unsigned char)s[start])) {
        --start;
        ++back;
    }
    const int k = utf8_seq_len((unsigned char)s[start]);
    return (k == back) ? back : 1;
}

// Bytes of the character that STARTS at `pos` (what a delete or a cursor-right must step over), never
// past `len`: the sequence length for a well-formed lead, 1 for anything else.
inline int utf8_next_len(const char *s, std::size_t pos, std::size_t len) noexcept {
    if (pos >= len) return 0;
    const int k = utf8_seq_len((unsigned char)s[pos]);
    if (k <= 1 || pos + (std::size_t)k > len) return 1;
    for (int i = 1; i < k; ++i)
        if (!utf8_is_cont((unsigned char)s[pos + (std::size_t)i])) return 1;
    return k;
}

// The byte offset at which the first `units` UTF-16 units of s[0..len) end -- the inverse of
// utf8_utf16_units, for a count that came back from something that measures in units (the game's
// line fitter) and has to be stored as a BYTE index again. Never lands inside a sequence: a unit
// count that would split a surrogate pair rounds down to the pair's start. Clamped to len.
inline std::size_t utf8_bytes_for_units(const char *s, std::size_t len, std::size_t units) noexcept {
    std::size_t i = 0, u = 0;
    while (i < len && s[i] != '\0') {
        std::uint32_t     cp  = 0;
        const int         got = utf8_decode_one(s + i, len - i, &cp);
        const std::size_t w   = (cp >= 0x10000u) ? 2u : 1u;
        if (u + w > units) break;
        u += w;
        i += (std::size_t)got;
    }
    return i;
}

// ---- a byte-indexed edit line holding UTF-8 ---------------------------------------------------------
//
// The game's text fields keep a NUL-terminated byte buffer plus a byte LENGTH and a byte CURSOR, and
// every one of their edit operations moves ONE byte. These two do the same edits a whole character at a
// time, so a UTF-8 line is never torn. `cap` is the buffer size INCLUDING the NUL.

// Insert `nb` bytes (one character's sequence) at *cur: all of them or none. Returns false -- buffer
// untouched -- when the line has no room for the whole sequence, or the indices are not sane.
inline bool utf8_edit_insert(char *buf, std::size_t cap, std::size_t *len, std::size_t *cur, const char *bytes,
                             std::size_t nb) noexcept {
    if (!buf || !len || !cur || !bytes || nb == 0 || *cur > *len || *len >= cap) return false;
    if (*len + nb + 1 > cap) return false;
    for (std::size_t i = *len + 1; i-- > *cur;) buf[i + nb] = buf[i]; // shift the tail, NUL included
    for (std::size_t i = 0; i < nb; ++i) buf[*cur + i] = bytes[i];
    *len += nb;
    *cur += nb;
    return true;
}

// Erase the character BEFORE the cursor (backspace, `before` = true) or AT it (delete). Returns the
// number of bytes removed (0 at the matching end of the line). The cursor stays on a boundary.
inline std::size_t utf8_edit_erase(char *buf, std::size_t *len, std::size_t *cur, bool before) noexcept {
    if (!buf || !len || !cur || *cur > *len) return 0;
    const int k = before ? utf8_prev_len(buf, *cur) : utf8_next_len(buf, *cur, *len);
    if (k <= 0) return 0;
    const std::size_t at = before ? *cur - (std::size_t)k : *cur;
    for (std::size_t i = at; i + (std::size_t)k <= *len; ++i) buf[i] = buf[i + (std::size_t)k]; // NUL too
    *len -= (std::size_t)k;
    *cur = at;
    return (std::size_t)k;
}

// ---- player (and game) names ------------------------------------------------------------------------

// The whole alphabet. No punctuation, no space: the ruling is "Latin letters and digits", and every
// name the test suite pins (host, client, client2, uitest ...) already fits it.
inline bool name_char_ok(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

inline bool name_is_valid(const char *s) noexcept {
    if (!s || !*s) return false;
    for (; *s; ++s)
        if (!name_char_ok((unsigned char)*s)) return false;
    return true;
}

// Normalize a NUL-terminated name IN PLACE (cap bytes including the NUL): every disallowed byte is
// DROPPED (never mapped -- a byte above 0x7F means a different letter in every codepage, so there is
// no honest Latin spelling of it), the result is capped at cap-1, and a name that ends up empty while
// the input was not (or the input was empty and `fallback` is non-null) becomes `fallback`. Returns
// true iff the buffer changed. The same function runs on every peer, so a forged name normalizes to
// the same bytes everywhere -- which is what keeps players[].name, a HASHED field, identical.
inline bool name_normalize(char *s, std::size_t cap, const char *fallback) noexcept {
    if (!s || cap == 0) return false;
    bool        changed = false;
    std::size_t w = 0, i = 0;
    for (; s[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (name_char_ok(c) && w + 1 < cap) {
            s[w++] = (char)c;
        } else {
            changed = true;
        }
    }
    if (w != i) changed = true;
    s[w] = '\0';
    if (w == 0 && fallback && *fallback && (changed || i == 0)) {
        std::size_t k = 0;
        for (; fallback[k] && k + 1 < cap; ++k) s[k] = fallback[k];
        s[k]    = '\0';
        changed = true;
    }
    return changed;
}

constexpr const char *NAME_FALLBACK = "Player"; // language-neutral; replaces an empty or all-foreign name

} // namespace mh_net_proto
