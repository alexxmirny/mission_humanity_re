// mh_net_proto -- SESSION_INFO: the session descriptor advertised by a host and listed in a client's
// browser (over LAN or the relay). Portable, no platform dependencies. See S1.
//
// Design (2026-07-12 discussion): the descriptor carries IDENTITY, not LOCATION. There is deliberately
// NO host address -- for the relay the IP is private/meaningless (only the relay keeps it), and for LAN
// the socket is already open; a join routes by lobby-id over the existing channel. The lobby-id = game
// name + a host-generated random tag (so two same-named games don't collide + it is the join/dedup key).
#pragma once
#include <cstdint>
#include <cstddef>

namespace mh_net_proto {

constexpr int SESSION_NAME_MAX = 31;   // max game-name chars (excl. NUL)
constexpr int SESSION_MAP_MAX  = 31;   // max map-name chars (excl. NUL)
constexpr int PLAYER_NAME_MAX  = 31;   // max player-name chars (excl. NUL); matches mh.exe's 32-byte name field
constexpr int MAP_HEADER_SIZE  = 0x17c;// mh.exe map_data_1 header size (the browser preview renders from this)

// The SESSION_INFO wire format version (bumped if the layout below changes). Distinct from `protocol`,
// which is the mh.exe GAME protocol version.
// v1: identity + map name string. v2 (S9): + the full 0x17c map_header (real biome/size for the browser
// preview). Decode accepts BOTH -- a v1 advert lists fine, just without a real preview header.
constexpr std::uint8_t SESSION_INFO_FORMAT = 2;

struct SessionInfo {
    std::uint32_t tag         = 0;   // host-generated random -> disambiguates same-named games (dedup key)
    std::uint16_t host_version = 0;  // host build version (compat gate / display)
    std::uint16_t protocol    = 0;   // mh.exe game protocol version
    std::uint8_t  cur_players = 0;   // players currently in the lobby
    std::uint8_t  max_players = 0;   // lobby capacity
    char name[SESSION_NAME_MAX + 1] = {0};  // game name (host-typed), NUL-terminated
    char map [SESSION_MAP_MAX  + 1] = {0};  // map name, NUL-terminated
    bool has_map_header = false;            // true iff map_header carries the host's real 0x17c map header (v2+)
    unsigned char map_header[MAP_HEADER_SIZE] = {0};  // full map_data_1 header -> the browser map PREVIEW (S9)
};

// Worst-case encoded size: 11 scalar bytes + two length-prefixed strings + the fixed 0x17c map header (v2).
constexpr std::size_t SESSION_INFO_MAX_ENCODED =
    11 + (1 + SESSION_NAME_MAX) + (1 + SESSION_MAP_MAX) + MAP_HEADER_SIZE; // 75 + 380 = 455

// Serialize `si` little-endian into `out` (must hold >= SESSION_INFO_MAX_ENCODED bytes). name/map are
// length-prefixed (only the used bytes go on the wire). Returns the number of bytes written.
std::size_t session_info_encode(const SessionInfo& si, std::uint8_t* out) noexcept;

// Deserialize a SESSION_INFO from `in`/`len`. Returns false on a truncated buffer, an unknown format
// version, or an over-long string field. `out` is left well-formed (NUL-terminated) only on success.
bool session_info_decode(const std::uint8_t* in, std::size_t len, SessionInfo& out) noexcept;

// ---- lobby-id (name + tag) ----------------------------------------------------------------------
// Two sessions denote the SAME game (lobby) iff both name AND tag match. Same name + different tag =>
// two distinct games (the tag is what prevents a duplicate-name collision).
bool session_same_lobby(const SessionInfo& a, const SessionInfo& b) noexcept;

// Format the lobby-id as "name#XXXXXXXX" (tag in hex) into `out` (cap bytes). Returns `out`.
const char* lobby_id_str(const SessionInfo& si, char* out, std::size_t cap) noexcept;

// ---- JOIN request (client -> host) --------------------------------------------------------------
// "I want to join the lobby identified by <name + tag>, and here is my player name." Carries the
// lobby-id (identity, no address -- the channel implies location, like SessionInfo) PLUS the joining
// player's name (S6: so the host can show the client's real name in its lobby slot instead of "Player2").
// The host matches the lobby-id against its own advertised session; a match admits the sender. See S4/S6.
// v1: lobby-id only. v2 (S6): + player_name. Decode accepts BOTH (a v1 JOIN admits fine, name blank).
constexpr std::uint8_t JOIN_REQUEST_FORMAT = 2;

struct JoinRequest {
    std::uint32_t tag = 0;                          // lobby-id tag (must equal the host's SessionInfo.tag)
    char name[SESSION_NAME_MAX + 1] = {0};          // lobby-id game name, NUL-terminated
    char player_name[PLAYER_NAME_MAX + 1] = {0};    // the joining player's name (S6), NUL-terminated
};

// Worst-case encoded size: format byte + tag + length-prefixed name + length-prefixed player_name.
constexpr std::size_t JOIN_REQUEST_MAX_ENCODED = 1 + 4 + (1 + SESSION_NAME_MAX) + (1 + PLAYER_NAME_MAX); // 69

// Serialize `jr` little-endian into `out` (must hold >= JOIN_REQUEST_MAX_ENCODED bytes). Returns bytes written.
std::size_t join_request_encode(const JoinRequest& jr, std::uint8_t* out) noexcept;

// Deserialize a JOIN request from `in`/`len`. Returns false on truncation, an unknown format version,
// or an over-long name. `out` is left well-formed (NUL-terminated) only on success.
bool join_request_decode(const std::uint8_t* in, std::size_t len, JoinRequest& out) noexcept;

// Build a JOIN request naming the same lobby (name + tag) as `si`.
JoinRequest join_request_for(const SessionInfo& si) noexcept;

// True iff `jr` names the same lobby (name AND tag) as host session `si` -- the host's admit test.
bool join_matches_session(const JoinRequest& jr, const SessionInfo& si) noexcept;

} // namespace mh_net_proto
