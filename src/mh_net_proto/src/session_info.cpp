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

    // v2 (S9): the full 0x17c map header for the browser preview. Always emitted for FORMAT 2.
    std::memcpy(p, si.map_header, MAP_HEADER_SIZE); p += MAP_HEADER_SIZE;

    return std::size_t(p - out);
}

bool session_info_decode(const std::uint8_t* in, std::size_t len, SessionInfo& out) noexcept {
    if (len < 11) return false;                           // 11 scalar bytes (format+tag+hostver+proto+cur+max)
    const std::uint8_t* p = in;
    const std::uint8_t* end = in + len;
    std::uint8_t fmt = *p++;
    if (fmt != 1 && fmt != SESSION_INFO_FORMAT) return false;  // accept v1 (no header) or v2 (S9, with header)
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
    return std::size_t(p - out);
}

bool join_request_decode(const std::uint8_t* in, std::size_t len, JoinRequest& out) noexcept {
    if (len < 5) return false;                            // format byte + tag
    const std::uint8_t* p = in;
    const std::uint8_t* end = in + len;
    std::uint8_t fmt = *p++;
    if (fmt < 1 || fmt > JOIN_REQUEST_FORMAT) return false;  // accept v1 (lobby-id only) .. v2 (+ player_name)
    out.tag = get_u32(p); p += 4;
    if (p >= end) return false;                           // need name_len
    std::uint8_t nlen = *p++;
    if (nlen > SESSION_NAME_MAX || p + nlen > end) return false;
    std::memcpy(out.name, p, nlen); out.name[nlen] = '\0';
    p += nlen;
    out.player_name[0] = '\0';                            // default empty (v1, or v2 with no name)
    if (fmt >= 2 && p < end) {                            // S6 (v2): read player_name if present
        std::uint8_t pnlen = *p++;
        if (pnlen > PLAYER_NAME_MAX || p + pnlen > end) return false;
        std::memcpy(out.player_name, p, pnlen); out.player_name[pnlen] = '\0';
    }
    return true;
}

JoinRequest join_request_for(const SessionInfo& si) noexcept {
    JoinRequest jr;
    jr.tag = si.tag;
    std::size_t n = clamp_len(si.name, SESSION_NAME_MAX);
    std::memcpy(jr.name, si.name, n); jr.name[n] = '\0';
    return jr;
}

bool join_matches_session(const JoinRequest& jr, const SessionInfo& si) noexcept {
    return jr.tag == si.tag && std::strcmp(jr.name, si.name) == 0;
}

} // namespace mh_net_proto
