// mh_net_proto -- SESSION_INFO (de)serialization + lobby-id. Portable, no platform dependencies.
#include "mh_net_proto/session_info.h"
#include "byteio.h"
#include <cstring>
#include <cstdio>

namespace mh_net_proto {

using detail::put_u16;
using detail::put_u32;
using detail::get_u16;
using detail::get_u32;

namespace {
// Copy a NUL-terminated field into a fixed buffer, clamping to `cap-1` chars + NUL. Returns length.
std::uint8_t clamp_len(const char* s, int cap) noexcept {
    std::size_t n = 0;
    while (n < std::size_t(cap) && s[n]) ++n;
    return std::uint8_t(n);
}
} // namespace

std::size_t session_info_encode(const SessionInfo& si, std::uint8_t* out) noexcept {
    std::uint8_t* p = out;
    *p++ = SESSION_INFO_FORMAT;
    put_u32(p, si.tag);          p += 4;
    put_u16(p, si.host_version); p += 2;
    put_u16(p, si.protocol);     p += 2;
    *p++ = si.cur_players;
    *p++ = si.max_players;

    std::uint8_t nlen = clamp_len(si.name, SESSION_NAME_MAX);
    *p++ = nlen;
    std::memcpy(p, si.name, nlen); p += nlen;

    std::uint8_t mlen = clamp_len(si.map, SESSION_MAP_MAX);
    *p++ = mlen;
    std::memcpy(p, si.map, mlen);  p += mlen;

    // v2 (S9): the full 0x17c map header for the browser preview. Always emitted for FORMAT >= 2.
    std::memcpy(p, si.map_header, MAP_HEADER_SIZE); p += MAP_HEADER_SIZE;

    // v3 (SES0): the match_id, and then v4 (F3): the codepage. Each new field goes LAST, so every
    // earlier version's prefix stays byte-identical to what it always was and an older decoder that
    // stops early has read a correct record rather than a shifted one.
    std::memcpy(p, si.match_id, UUID7_BYTES); p += UUID7_BYTES;
    put_u16(p, si.codepage); p += 2;
    // v5 (X2): the map's content identity, last for the same reason every field before it was.
    std::memcpy(p, si.map_hash, MAP_HASH_BYTES); p += MAP_HASH_BYTES;
    put_u32(p, si.map_size); p += 4;

    return std::size_t(p - out);
}

bool session_info_decode(const std::uint8_t* in, std::size_t len, SessionInfo& out) noexcept {
    if (len < 11) return false;                           // 11 scalar bytes (format+tag+hostver+proto+cur+max)
    const std::uint8_t* p = in;
    const std::uint8_t* end = in + len;
    std::uint8_t fmt = *p++;
    // accept v1 (no header), v2 (S9, with header), v3 (SES0, + match_id). Anything else is refused
    // rather than best-effort parsed -- an unknown trailing field is exactly what must not be ignored.
    if (fmt < 1 || fmt > SESSION_INFO_FORMAT) return false;
    out.tag         = get_u32(p); p += 4;
    out.host_version = get_u16(p); p += 2;
    out.protocol    = get_u16(p); p += 2;
    out.cur_players = *p++;
    out.max_players = *p++;

    if (p >= end) return false;                           // need name_len
    std::uint8_t nlen = *p++;
    if (nlen > SESSION_NAME_MAX || p + nlen > end) return false;
    std::memcpy(out.name, p, nlen); out.name[nlen] = '\0'; p += nlen;

    if (p >= end) return false;                           // need map_len
    std::uint8_t mlen = *p++;
    if (mlen > SESSION_MAP_MAX || p + mlen > end) return false;
    std::memcpy(out.map, p, mlen); out.map[mlen] = '\0';  p += mlen;

    // v2 (S9): the trailing 0x17c map header. Optional so a v1 advert still decodes (has_map_header=false).
    if (fmt >= 2 && p + MAP_HEADER_SIZE <= end) {
        std::memcpy(out.map_header, p, MAP_HEADER_SIZE); p += MAP_HEADER_SIZE;
        out.has_map_header = true;
    } else {
        out.has_map_header = false;
    }

    // v3 (SES0): the trailing match_id. Optional in the same sense as the header above -- a v1/v2
    // advert leaves it NIL, which uuid7_is_nil() reports as "this host mints no match_id".
    if (fmt >= 3 && p + UUID7_BYTES <= end) {
        std::memcpy(out.match_id, p, UUID7_BYTES); p += UUID7_BYTES;
    } else {
        std::memset(out.match_id, 0, UUID7_BYTES);
    }

    // v4 (F3): the trailing codepage. Optional on the same rule -- a pre-v4 advert leaves it 0, which
    // the admit policy reads as "declares no pin", never as a codepage.
    if (fmt >= 4 && p + 2 <= end) {
        out.codepage = get_u16(p); p += 2;
    } else {
        out.codepage = 0;
    }

    // v5 (X2): the map content hash + size. Optional on the same rule -- a pre-v5 advert leaves the
    // hash all-zero, which map_hash_is_none() reports as "this host makes no content claim", never
    // as a map whose bytes hash to zero.
    if (fmt >= 5 && p + MAP_HASH_BYTES + 4 <= end) {
        std::memcpy(out.map_hash, p, MAP_HASH_BYTES); p += MAP_HASH_BYTES;
        out.map_size = get_u32(p); p += 4;
    } else {
        std::memset(out.map_hash, 0, MAP_HASH_BYTES);
        out.map_size = 0;
    }

    return true;
}

bool session_same_lobby(const SessionInfo& a, const SessionInfo& b) noexcept {
    return a.tag == b.tag && std::strcmp(a.name, b.name) == 0;
}

const char* lobby_id_str(const SessionInfo& si, char* out, std::size_t cap) noexcept {
    if (cap == 0) return out;
    std::snprintf(out, cap, "%s#%08X", si.name, si.tag);
    return out;
}

const char* session_match_id_log_line(const std::uint8_t match_id[UUID7_BYTES], char* out, std::size_t cap) noexcept {
    if (cap < SESSION_LOG_LINE_CAP) { if (cap) out[0] = '\0'; return out; }
    char hex[UUID7_HEX_CAP];
    uuid7_hex(match_id, hex, sizeof(hex));
    // snprintf, not a hand-built concat: the format string below IS the contract mp_analyze.py reads.
    std::snprintf(out, cap, "; [session] match_id=%s\n", hex);
    return out;
}

// ---- JOIN request -------------------------------------------------------------------------------
std::size_t join_request_encode(const JoinRequest& jr, std::uint8_t* out) noexcept {
    std::uint8_t* p = out;
    *p++ = JOIN_REQUEST_FORMAT;
    put_u32(p, jr.tag); p += 4;
    std::uint8_t nlen = clamp_len(jr.name, SESSION_NAME_MAX);
    *p++ = nlen;
    std::memcpy(p, jr.name, nlen); p += nlen;
    std::uint8_t pnlen = clamp_len(jr.player_name, PLAYER_NAME_MAX);   // S6 (v2): the joining player's name
    *p++ = pnlen;
    std::memcpy(p, jr.player_name, pnlen); p += pnlen;
    std::memcpy(p, jr.match_id, UUID7_BYTES); p += UUID7_BYTES;        // SES0 (v3): echo the advert's match_id
    put_u16(p, jr.codepage); p += 2;                                   // F3 (v4): OUR pin, not the host's
    std::memcpy(p, jr.map_hash, MAP_HASH_BYTES); p += MAP_HASH_BYTES;  // X2 (v5): what WE hold, not what they said
    return std::size_t(p - out);
}

bool join_request_decode(const std::uint8_t* in, std::size_t len, JoinRequest& out) noexcept {
    if (len < 5) return false;                            // format byte + tag
    const std::uint8_t* p = in;
    const std::uint8_t* end = in + len;
    std::uint8_t fmt = *p++;
    // PARSE v1..v3. The version POLICY is join_admit()'s, not this function's: a host that refuses an
    // old client still wants the lobby-id out of its JOIN so the refusal line can name it.
    if (fmt < 1 || fmt > JOIN_REQUEST_FORMAT) return false;
    out.format = fmt;
    out.tag = get_u32(p); p += 4;
    if (p >= end) return false;                           // need name_len
    std::uint8_t nlen = *p++;
    if (nlen > SESSION_NAME_MAX || p + nlen > end) return false;
    std::memcpy(out.name, p, nlen); out.name[nlen] = '\0';
    p += nlen;
    out.player_name[0] = '\0';                            // default empty (v1, or v2 with no name)
    std::memset(out.match_id, 0, UUID7_BYTES);            // default nil (v1/v2 carry none)
    if (fmt >= 2 && p < end) {                            // S6 (v2): read player_name if present
        std::uint8_t pnlen = *p++;
        if (pnlen > PLAYER_NAME_MAX || p + pnlen > end) return false;
        std::memcpy(out.player_name, p, pnlen); out.player_name[pnlen] = '\0';
        p += pnlen;
    }
    out.codepage = 0;                                     // default: v1..v3 declare no pin
    if (fmt >= 3) {                                       // SES0 (v3): the echoed match_id
        if (p + UUID7_BYTES > end) return false;          // REQUIRED at v3 -- a short one is malformed
        std::memcpy(out.match_id, p, UUID7_BYTES); p += UUID7_BYTES;
    }
    if (fmt >= 4) {                                       // F3 (v4): the joiner's own codepage
        if (p + 2 > end) return false;                    // REQUIRED at v4, same rule as the id
        out.codepage = get_u16(p); p += 2;
    }
    std::memset(out.map_hash, 0, MAP_HASH_BYTES);         // default: v1..v4 hold no map claim
    if (fmt >= 5) {                                       // X2 (v5): the map content the joiner holds
        if (p + MAP_HASH_BYTES > end) return false;       // REQUIRED at v5, same rule as the id
        std::memcpy(out.map_hash, p, MAP_HASH_BYTES); p += MAP_HASH_BYTES;
    }
    return true;
}

JoinRequest join_request_for(const SessionInfo& si, std::uint16_t my_codepage,
                             const std::uint8_t* my_map_hash) noexcept {
    JoinRequest jr;
    jr.tag = si.tag;
    std::size_t n = clamp_len(si.name, SESSION_NAME_MAX);
    std::memcpy(jr.name, si.name, n); jr.name[n] = '\0';
    std::memcpy(jr.match_id, si.match_id, UUID7_BYTES); // SES0: echo the advert's id straight back
    jr.codepage = my_codepage;                          // F3: OUR pin -- deliberately NOT si.codepage
    // X2: OUR file's hash -- deliberately NOT si.map_hash, for the codepage field's exact reason.
    if (my_map_hash != nullptr) std::memcpy(jr.map_hash, my_map_hash, MAP_HASH_BYTES);
    return jr;
}

JoinRequest join_request_for(const SessionInfo& si, std::uint16_t my_codepage) noexcept {
    return join_request_for(si, my_codepage, nullptr);
}

JoinRequest join_request_for(const SessionInfo& si) noexcept { return join_request_for(si, 0, nullptr); }

// ---- mp:X2 -- the map-identity helpers ----------------------------------------------------------

void map_hash_from_sha256(const std::uint8_t sha[32], std::uint8_t out[MAP_HASH_BYTES]) noexcept {
    std::memcpy(out, sha, MAP_HASH_BYTES); // the FIRST eight, once, in one place
}

bool map_hash_is_none(const std::uint8_t h[MAP_HASH_BYTES]) noexcept {
    for (int i = 0; i < MAP_HASH_BYTES; ++i)
        if (h[i] != 0) return false;
    return true;
}

bool map_hash_equal(const std::uint8_t a[MAP_HASH_BYTES], const std::uint8_t b[MAP_HASH_BYTES]) noexcept {
    return std::memcmp(a, b, MAP_HASH_BYTES) == 0;
}

const char* map_hash_hex(const std::uint8_t h[MAP_HASH_BYTES], char* out, std::size_t cap) noexcept {
    if (cap < MAP_HASH_HEX_CAP) { if (cap) out[0] = '\0'; return out; }
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < MAP_HASH_BYTES; ++i) {
        out[i * 2]     = kHex[(h[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[h[i] & 0xf];
    }
    out[MAP_HASH_BYTES * 2] = '\0';
    return out;
}

std::size_t map_stored_name(const char* base, const std::uint8_t hash[MAP_HASH_BYTES], char* out,
                            std::size_t cap) noexcept {
    if (base == nullptr || out == nullptr || cap == 0) return 0;
    std::size_t blen = 0;
    while (base[blen] != '\0' && blen < MAP_STORED_NAME_CAP) ++blen;
    if (blen == 0) return 0;
    // The insertion point is the LAST dot, so "Cold War.mpm" splits after "Cold War" and a dotted
    // stem ("v1.2 map.mpm") still keeps its real extension.
    std::size_t dot = blen; // no dot -> append
    for (std::size_t i = blen; i-- > 0;) {
        if (base[i] == '.') { dot = i; break; }
    }
    char hex[MAP_HASH_HEX_CAP];
    map_hash_hex(hash, hex, sizeof(hex));
    const std::size_t need = blen + 1 + (MAP_HASH_BYTES * 2); // + one '.' + the hex
    if (need + 1 > cap) return 0;                             // REFUSE; see the header on why not a fallback
    std::size_t w = 0;
    for (std::size_t i = 0; i < dot; ++i) out[w++] = base[i];
    out[w++] = '.';
    for (std::size_t i = 0; i < MAP_HASH_BYTES * 2; ++i) out[w++] = hex[i];
    for (std::size_t i = dot; i < blen; ++i) out[w++] = base[i]; // the extension, dot included
    out[w] = '\0';
    return w;
}

bool map_name_is_stored_form(const char* name, const char* base) noexcept {
    if (name == nullptr || base == nullptr) return false;
    std::size_t blen = 0;
    while (base[blen] != '\0' && blen < MAP_STORED_NAME_CAP) ++blen;
    if (blen == 0) return false;
    std::size_t dot = blen;
    for (std::size_t i = blen; i-- > 0;) {
        if (base[i] == '.') { dot = i; break; }
    }
    // POSITIONAL, against the split map_stored_name uses -- NOT a character-class scan over a
    // rebuilt sample string. A sample would have to mark the hex run somehow, and any marker char
    // ('f', say) also occurs in real map names, so "wolf.mpm" would make its own 'f' a wildcard.
    std::size_t w = 0;
    for (std::size_t i = 0; i < dot; ++i)
        if (name[w++] != base[i]) return false;
    if (name[w++] != '.') return false;
    for (int i = 0; i < MAP_HASH_BYTES * 2; ++i) {
        const char c = name[w++];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    for (std::size_t i = dot; i < blen; ++i)
        if (name[w++] != base[i]) return false;
    return name[w] == '\0';
}

bool join_matches_session(const JoinRequest& jr, const SessionInfo& si) noexcept {
    return jr.tag == si.tag && std::strcmp(jr.name, si.name) == 0;
}

const char* join_admit_reason(JoinAdmit a) noexcept {
    switch (a) {
        case JoinAdmit::Admit:               return "ok";
        case JoinAdmit::RefusedMalformed:    return "malformed JOIN";
        case JoinAdmit::RefusedOldProtocol:  return "old protocol (no match_id)";
        case JoinAdmit::RefusedNewerProtocol:return "newer protocol than this host";
        case JoinAdmit::RefusedWrongLobby:   return "not our lobby";
        case JoinAdmit::RefusedCodepage:     return "input codepage mismatch";
    }
    return "unknown";
}

JoinAdmit join_admit(const std::uint8_t* in, std::size_t len, const SessionInfo& mine, JoinRequest& out) noexcept {
    out = JoinRequest{};
    if (!join_request_decode(in, len, out)) {
        // Either the bytes are junk, or the format byte is outside 1..JOIN_REQUEST_FORMAT. A byte
        // ABOVE our maximum is a genuinely newer client and worth saying so; decode cannot report the
        // two apart, so read the format byte back here rather than guessing.
        if (len >= 1 && in[0] > JOIN_REQUEST_FORMAT) { out.format = in[0]; return JoinAdmit::RefusedNewerProtocol; }
        return JoinAdmit::RefusedMalformed;
    }
    if (out.format < JOIN_REQUEST_MIN_FORMAT) return JoinAdmit::RefusedOldProtocol;
    if (!join_matches_session(out, mine))     return JoinAdmit::RefusedWrongLobby;
    // F3: the encodings must MATCH, and the lobby test has to come first -- a peer dialling somebody
    // else's lobby is refused for dialling the wrong lobby, not for its codepage. The comparison is
    // exact equality of two declared numbers, not a compatibility table: CP1250 and CP1251 agree on
    // every byte below 0x80 and disagree above it, so "close enough" is precisely the state in which a
    // chat line looks fine until somebody types a letter with an accent.
    //
    // A host that declares nothing (0, i.e. not built with F3, or a codepage it could not resolve)
    // accepts anyone: the field would otherwise refuse every peer on the strength of a value this side
    // never set. That asymmetry is deliberate and it is the compatibility direction -- a host WITH a
    // pin still refuses a peer whose pin differs, including one that declares none at v4.
    if (mine.codepage != 0 && out.codepage != mine.codepage) return JoinAdmit::RefusedCodepage;
    return JoinAdmit::Admit;
}


// ---- mp:F3c -- the REFUSED announce ---------------------------------------------------------------

std::size_t announce_refused_encode(std::uint8_t target_player_id, const char* reason, std::uint8_t* out) noexcept {
    out[0] = ANNOUNCE_REFUSED;
    out[1] = target_player_id;
    std::size_t n = 0;
    if (reason) {
        for (; reason[n] && n < ANNOUNCE_TEXT_CAP - 1; ++n) out[2 + n] = (std::uint8_t)reason[n];
    }
    out[2 + n] = 0;
    return 2 + n + 1;
}

bool announce_refused_decode(const std::uint8_t* in, std::size_t len, std::uint8_t* target_player_id,
                             char* reason, std::size_t cap) noexcept {
    if (!in || len < 3 || in[0] != ANNOUNCE_REFUSED || cap == 0) return false;
    // The text must be NUL-terminated INSIDE the frame: a reason that runs to the end of the buffer
    // is a truncated frame, not a long reason, and reading past `len` for the NUL would be the bug.
    std::size_t n = 0;
    while (2 + n < len && in[2 + n] != 0) ++n;
    if (2 + n >= len) return false;
    if (target_player_id) *target_player_id = in[1];
    std::size_t w = n < cap - 1 ? n : cap - 1;
    std::memcpy(reason, in + 2, w);
    reason[w] = 0;
    return true;
}

// ---- mp:L1f -- the host's per-slot ping summary (see session_info.h for the layout + the why) ----

std::size_t announce_ping_encode(const AnnouncePingEntry* in, std::size_t n, std::uint8_t* out) noexcept {
    if (!out) return 0;
    if (!in) n = 0;
    if (n > ANNOUNCE_PING_MAX_ENTRIES) n = ANNOUNCE_PING_MAX_ENTRIES;
    out[0] = ANNOUNCE_PING;
    out[1] = (std::uint8_t)n;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint16_t ms = in[i].srtt_ms > ANNOUNCE_PING_SRTT_CLAMP ? ANNOUNCE_PING_SRTT_CLAMP : in[i].srtt_ms;
        std::uint8_t  r  = in[i].relay;
        if (r > ANNOUNCE_PING_RELAY_UNKNOWN) r = ANNOUNCE_PING_RELAY_UNKNOWN;
        out[2 + i * 4] = in[i].player_id;
        out[3 + i * 4] = (std::uint8_t)(ms & 0xff);
        out[4 + i * 4] = (std::uint8_t)(ms >> 8);
        out[5 + i * 4] = r;
    }
    return 2 + n * 4;
}

bool announce_ping_decode(const std::uint8_t* in, std::size_t len, AnnouncePingEntry* out,
                          std::size_t cap, std::size_t* out_n) noexcept {
    if (out_n) *out_n = 0;
    if (!in || len < 2 || in[0] != ANNOUNCE_PING) return false;
    const std::size_t n = in[1];
    if (n > ANNOUNCE_PING_MAX_ENTRIES) return false;
    // ALL OR NOTHING. `len` must hold every entry the count declares; a short frame is a truncated
    // table, and reading the entries that did fit would leave the rest of the lobby showing the
    // PREVIOUS table's numbers while the header says this one arrived.
    if (len < 2 + n * 4) return false;
    const std::size_t w = n < cap ? n : cap;
    for (std::size_t i = 0; i < w && out; ++i) {
        out[i].player_id = in[2 + i * 4];
        out[i].srtt_ms   = (std::uint16_t)(in[3 + i * 4] | ((std::uint16_t)in[4 + i * 4] << 8));
        std::uint8_t r   = in[5 + i * 4];
        out[i].relay     = r > ANNOUNCE_PING_RELAY_UNKNOWN ? ANNOUNCE_PING_RELAY_UNKNOWN : r;
    }
    if (out_n) *out_n = out ? w : 0;
    return true;
}

const char* join_refusal_text(JoinAdmit a, const SessionInfo& mine, const JoinRequest& theirs, char* out,
                              std::size_t cap) noexcept {
    if (cap == 0) return out;
    switch (a) {
        case JoinAdmit::RefusedCodepage:
            // Both numbers, host's first, because the fix is on ONE of the two machines and
            // "mismatch" alone does not say which. 20 characters at the widest (5-digit codepages).
            std::snprintf(out, cap, "codepage %u/%u", (unsigned)mine.codepage, (unsigned)theirs.codepage);
            break;
        case JoinAdmit::RefusedNewerProtocol:
            std::snprintf(out, cap, "protocol %u > %u", (unsigned)theirs.format, (unsigned)JOIN_REQUEST_FORMAT);
            break;
        case JoinAdmit::RefusedOldProtocol:
            std::snprintf(out, cap, "protocol %u < %u", (unsigned)theirs.format, (unsigned)JOIN_REQUEST_MIN_FORMAT);
            break;
        default:
            std::snprintf(out, cap, "%s", join_admit_reason(a));
            break;
    }
    out[cap - 1] = 0;
    return out;
}

} // namespace mh_net_proto
