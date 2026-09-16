// mh_net_proto -- portable, cross-platform networking protocol for the MH multiplayer restoration.
//
// This library is PURE LOGIC: message formats, framing, and (de)serialization shared by the Windows
// injected DLL (WinSock transport) and the headless-Linux relay (Boost.Asio / POSIX transport). It
// MUST NOT include any platform or socket headers (no <windows.h>, <winsock2.h>, <sys/socket.h>, no
// Boost) -- only the C++ standard library + fixed-width integer types. Each platform provides its own
// thin socket adapter and calls into this library to encode/decode. See MP-DISCOVERY (S0/S1).
//
// SESSION_INFO / lobby-id / join / player-name messages (S1..S6) will land here next.
#pragma once
#include <cstdint>
#include <cstddef>

namespace mh_net_proto {

// ---- wire framing -------------------------------------------------------------------------------
// Every record on the stream is a 12-byte routing header + payload. src/dst let the host (and a
// future relay) route by player id and let the receiver tag the datagram, WITHOUT parsing the
// mh.exe game payload -- exactly what the relay reads to bridge. Encoded LITTLE-ENDIAN on the wire,
// byte-for-byte compatible with the transport's original x86 packed-struct layout, so existing peers
// interoperate unchanged.
constexpr std::uint16_t WIRE_MAGIC = 0x484D;   // 'MH' -- stream sanity / desync guard

enum WireFlag : std::uint16_t {
    FLAG_DATA         = 0,   // application datagram (goes to the recv queue)
    FLAG_HELLO        = 1,   // control: "I am player <src>" (binds socket->id on host)
    FLAG_SESSION_INFO = 2,   // control: host advertises its SessionInfo (discovery; NOT a game datagram)
    FLAG_JOIN         = 3,   // control: client -> host "I want to join lobby <name+tag>" (S4; NOT a game datagram)
    FLAG_START        = 4,   // control: host -> client "I clicked Start, enter the game now" (U2; NOT a game datagram)
    FLAG_LEAVE        = 5,   // control: client -> host "I left the lobby (back to browser)" (U12; NOT a game datagram)
    FLAG_WELCOME      = 6,   // control: host -> client "you are player <dst>" (N1 host-assign; NOT a game datagram)
    FLAG_ANNOUNCE     = 7,   // control: host -> all "player <name> joined/left the lobby" (U16 announce text; NOT a game datagram)
    FLAG_PING         = 8,   // control: link keepalive, either direction, empty payload (R-live; NOT a game datagram)
    FLAG_HASH         = 9,   // control: a peer's (step, state-hash, per-region hashes) desync sample (D21; NOT a game datagram)
};
// Receiver rule: deliver ONLY FLAG_DATA to the game, and ignore any flag you do not recognise. The
// tempting "default: treat as data" turns every control frame added later into garbage in an older
// peer's lockstep queue -- i.e. a desync. (R-live, 2026-07-26.)

constexpr std::int16_t BROADCAST = -1;         // dst sentinel (mirrors MH_NET_BROADCAST)

struct WireHdr {
    std::uint16_t magic;
    std::uint16_t flags;
    std::int16_t  src;   // sender player id
    std::int16_t  dst;   // dest player id, or BROADCAST (-1)
    std::uint32_t len;   // payload length
};

constexpr std::size_t WIRE_HDR_SIZE = 12;      // on-the-wire size, independent of C++ struct padding

// Encode a header into `out` (must have room for WIRE_HDR_SIZE bytes), little-endian.
void wire_hdr_encode(const WireHdr& h, std::uint8_t* out) noexcept;

// Decode a WIRE_HDR_SIZE-byte little-endian header from `in`. Returns false if the magic mismatches
// (the caller should drop the connection, matching the transport's bad-magic guard).
bool wire_hdr_decode(const std::uint8_t* in, WireHdr& out) noexcept;

} // namespace mh_net_proto
