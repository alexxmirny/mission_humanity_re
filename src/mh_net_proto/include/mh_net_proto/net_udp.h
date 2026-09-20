// mh_net_proto -- the UDP packet format (MP refinement plan decision D2, tracker mp:T0).
//
// WHAT THIS IS. One datagram format for the restored multiplayer, modelled on the netcode.io
// framing spec (https://github.com/networkprotocol/netcode/blob/master/STANDARD.md) -- a plaintext
// header authenticated as associated data, a per-direction sequence number used directly as the
// encryption nonce, a 64-entry replay window, and a connect token minted by the host and sealed
// with a key the client never sees. What it does NOT take from netcode.io is the cryptography:
// this keeps the PSK handshake, ChaCha20 and truncated HMAC-SHA256 already in net_crypto.h. There
// is no new primitive here, and adding one would mean a second thing to get right.
//
// WHY HAND-ROLLED. Surveyed and rejected (plan D2): GameNetworkingSockets needs OpenSSL + protobuf
// inside a 32-bit injected DLL and documents x64 only; ENet has no crypto; every mature Rust option
// is Rust-only and both ends of this system have to speak it -- the injected DLL in C++ and the
// relay in Rust. So the format is ours, and the thing that keeps the two implementations honest is
// a directory of FIXTURE FILES (src/mh_net_proto/test/fixtures/udp/) that `net_selftest.exe
// udpwiretest` and `cargo test -p relay` both read. An encoder agreeing with its own decoder proves
// nothing; two independent decoders agreeing on committed bytes is the claim.
//
// PORTABILITY CONTRACT (unchanged): standard library + fixed-width integers only. No <windows.h>,
// no sockets, no allocation. Everything here is explicit little-endian byte work, like net_wire.cpp
// -- a packed struct would be a different format on a different compiler.
//
// WHAT CONSUMES IT. mp:T1 (mh_net_udp.dll) uses the encoder/decoder and the channels; mp:R1 (the
// Rust relay) uses the header and the replay window to demux and forward without holding a session
// key, and the token record to decide who is allowed in. Neither exists yet: this item is the
// format and its two-language proof, nothing more.
//
#pragma once
#include <cstdint>
#include <cstddef>

#include "mh_net_proto/net_crypto.h"
#include "mh_net_proto/uuid7.h"

namespace mh_net_proto {
namespace udp {

// ---- sizes ------------------------------------------------------------------------------------
// 1200 is RFC 9000's conservative IPv6 minimum-MTU figure minus headers -- the size QUIC picked so
// that a datagram crosses the internet without depending on PMTU discovery. It is a HARD ceiling
// here, not a target: encode refuses a body that would exceed it and decode refuses a datagram of
// 1201 bytes before it looks at anything else, so the refusal cannot depend on the contents.
constexpr std::size_t MAX_DATAGRAM = 1200;
constexpr std::size_t HDR_SIZE     = 18; // magic/ver(1) type(1) conn_id(8) seq(8)
constexpr std::size_t TAG_SIZE     = MAC_LEN;
constexpr std::size_t MAX_BODY     = MAX_DATAGRAM - HDR_SIZE - TAG_SIZE; // 1166
constexpr std::size_t MIN_DATAGRAM = HDR_SIZE + TAG_SIZE;                // 34, an empty sealed body

// ---- header -----------------------------------------------------------------------------------
// byte 0  magic/version: high nibble MAGIC_NIBBLE, low nibble VERSION
// byte 1  packet type (PacketType)
// byte 2  conn_id[8]      -- the relay's demux key, uuid7.h conn_id_from_match()
// byte 10 seq[8] LE       -- per DIRECTION, strictly increasing, never reused
// byte 18 ciphertext ... then the 16-byte tag at the end of the datagram
//
// The whole 18-byte header is PLAINTEXT and AUTHENTICATED: a relay must read conn_id without a key,
// and nothing in the header may be editable in flight. The MAC therefore covers header||ciphertext
// -- header as associated data in the AEAD sense, even though the construction is encrypt-then-MAC
// rather than a packaged AEAD.
//
// SEQUENCE IS THE NONCE. chacha20_xor() takes the 64-bit seq and places it in the RFC 8439 nonce
// (net_crypto.cpp), so a repeated seq in one direction would repeat a keystream. That is why seq is
// per-direction, why the two directions have separate keys, and why the sender must never rewind:
// the replay window below protects the RECEIVER, and a sender that reuses a sequence breaks the
// cipher regardless of what any receiver does.
constexpr std::uint8_t MAGIC_NIBBLE = 0x4; // 'M' >> 4 -- the family byte of every MH wire format
constexpr std::uint8_t VERSION      = 1;
constexpr std::uint8_t MAGIC_VER    = (MAGIC_NIBBLE << 4) | VERSION; // 0x41

enum PacketType : std::uint8_t {
    PKT_DATA       = 0, // sealed body of channel frames (the only type carrying game traffic)
    PKT_TOKEN      = 1, // client -> host/relay: a connect token, sealed with the HOST key
    PKT_TOKEN_ACK  = 2, // host -> client: the token was accepted; body is empty
    PKT_KEEPALIVE  = 3, // either direction, empty body, 20-25 s (plan D4: NAT binding floor is 2 min)
    PKT_DISCONNECT = 4, // either direction, empty body, best-effort
    PKT_TYPE_MAX   = 4,
};

struct Header {
    std::uint8_t  type;
    std::uint8_t  conn_id[CONN_ID_BYTES];
    std::uint64_t seq;
};

// Read the header out of a datagram WITHOUT verifying anything cryptographic. This is what a relay
// calls: it has no session key, so conn_id and seq are all it can and should see. Returns false on
// a short datagram, a bad magic nibble, an unknown version or an unknown packet type.
bool hdr_peek(const std::uint8_t *pkt, std::size_t len, Header &out) noexcept;

// ---- replay window ----------------------------------------------------------------------------
// netcode.io's rule, 64 entries: a sequence at or below the floor (newest - 64) is too old, and one
// already seen is a duplicate. Both are dropped. `bits` is a bitmap of the 64 sequences ending at
// `newest`, bit i meaning (newest - i) was accepted.
//
// TEST-THEN-COMMIT IS A DELIBERATE SPLIT. `check` is cheap and runs BEFORE the MAC so a flood of
// duplicates costs no HMAC; `commit` runs only AFTER the MAC verifies, so a forged packet carrying
// a plausible future sequence cannot push the window forward and make the REAL packet at that
// sequence look like a replay. Doing both before the MAC is the obvious simplification and it is a
// denial-of-service hole.
constexpr std::size_t REPLAY_WINDOW = 64;

struct ReplayWindow {
    std::uint64_t newest = 0;
    std::uint64_t bits   = 0;
    bool          seeded = false;

    void reset() noexcept {
        newest = 0;
        bits   = 0;
        seeded = false;
    }
    // True when `seq` is acceptable (fresh and not below the floor). Pure -- no state change.
    bool check(std::uint64_t seq) const noexcept;
    // Record `seq` as received. Caller must have verified the MAC first. No-op if check() is false.
    void commit(std::uint64_t seq) noexcept;
};

// ---- verdicts ---------------------------------------------------------------------------------
// Every refusal is its own value. A single bool would make "the peer is on an old build" and "we
// are being attacked" the same event in a log, and the transport's whole job is to tell them apart.
enum class Verdict : std::uint8_t {
    Ok = 0,
    TooLong,     // > MAX_DATAGRAM. Checked first, so it never depends on the contents
    TooShort,    // < MIN_DATAGRAM
    BadMagic,    // not the MH family byte
    BadVersion,  // our family, a version we do not speak
    BadType,     // unknown packet type
    WrongConn,   // conn_id is not this session's
    Replay,      // duplicate, or below the replay window's floor
    BadMac,      // authentication failed: tampering, or the wrong key
    Malformed,   // the sealed body is not well-formed channel framing
    BodyTooLong, // encode side: the body would push the datagram past MAX_DATAGRAM
};

const char *verdict_name(Verdict v) noexcept;

// ---- packet encode / decode -------------------------------------------------------------------
// Seal `body` into `out` (needs HDR_SIZE + body_len + TAG_SIZE bytes, at most MAX_DATAGRAM).
// Returns the datagram length, or 0 with `why` set. The caller owns `seq`: it must be this
// direction's next unused value, and reusing one repeats a ChaCha20 keystream.
std::size_t packet_encode(std::uint8_t type, const std::uint8_t conn_id[CONN_ID_BYTES],
                          std::uint64_t seq, const std::uint8_t enc_key[KEY_LEN],
                          const std::uint8_t mac_key[KEY_LEN], const std::uint8_t *body,
                          std::size_t body_len, std::uint8_t *out, Verdict &why) noexcept;

// Verify and decrypt one datagram. `pkt` is decrypted IN PLACE: on Ok, the body is
// pkt[HDR_SIZE .. HDR_SIZE + *body_len). `expect_conn` may be null to skip the conn_id test (a
// relay demuxing by conn_id has already done it; a peer with one session should pass its own).
//
// `win` is this direction's window. It is consulted before the MAC and committed after it, per the
// note above; pass null in a context with no window (a fixture check, a fuzz driver) and the replay
// test is skipped rather than silently passing.
Verdict packet_decode(std::uint8_t *pkt, std::size_t len, const std::uint8_t *expect_conn,
                      const std::uint8_t enc_key[KEY_LEN], const std::uint8_t mac_key[KEY_LEN],
                      ReplayWindow *win, Header &hdr, std::size_t *body_len) noexcept;

// ---- channel mux ------------------------------------------------------------------------------
// The sealed body is a sequence of frames, each `id(1) | len(2 LE) | payload(len)`. One datagram
// can carry a step-input frame and a ping frame; the receiver dispatches by id and IGNORES an id it
// does not know -- the same forward-compatibility rule net_wire.h's FLAG_ comment states, and for
// the same reason: "unknown means data" turns every channel added later into a desync.
constexpr std::size_t FRAME_HDR = 3;

enum ChannelId : std::uint8_t {
    CH_INPUT = 1, // A: step inputs, unreliable, redundant. No acks, no retransmit.
    CH_STATE = 2, // B: latest-wins small records (ping, step hash, telemetry)
    CH_BULK  = 3, // C: reliable chunked transfer (map, snapshot) with an ack bitfield
};

struct Frame {
    std::uint8_t        id;
    const std::uint8_t *data;
    std::size_t         len;
};

// Append a frame to `body` at `*used`, which is advanced. `cap` is the body capacity (<= MAX_BODY).
bool frame_append(std::uint8_t *body, std::size_t cap, std::size_t *used, std::uint8_t id,
                  const std::uint8_t *payload, std::size_t len) noexcept;

// Walk the frames of a body. `*off` starts at 0 and is advanced. Returns false at the end, and sets
// `ok` false if the remainder is not a well-formed frame (a truncated length, a length running past
// the body) -- a caller that ignores `ok` would treat truncation as a clean end of stream.
bool frame_next(const std::uint8_t *body, std::size_t len, std::size_t *off, Frame &out,
                bool &ok) noexcept;

// ---- channel A: step inputs -------------------------------------------------------------------
// Each packet carries the inputs for the last K steps, so one lost datagram is repaired by the next
// without a retransmit and without an added round trip -- lockstep inputs are tens of bytes, so the
// redundancy is nearly free (plan D2; AoE's "1500 archers" made the same trade).
//
// payload: count(1) | newest_step(4 LE) | count x { len(1) | bytes }
// Entry i is the input for step (newest_step - i), newest first. The step numbers are IMPLIED,
// which is the compact part: 5 bytes of header for the whole run instead of 4 per entry.
constexpr std::uint8_t INPUT_K_MIN     = 1;
constexpr std::uint8_t INPUT_K_MAX     = 8;
constexpr std::uint8_t INPUT_K_DEFAULT = 3;
constexpr std::size_t  INPUT_ENTRY_MAX = 255; // a step's input bytes; `len` is one byte

struct InputEntry {
    std::uint32_t       step;
    const std::uint8_t *bytes;
    std::size_t         len;
};

// Encode K entries, newest first, into `out`. `entries[i].step` must equal newest_step - i; the
// encoder CHECKS that rather than trusting it, because the implied numbering is exactly the kind of
// thing a caller gets subtly wrong and no decoder could detect. Returns bytes written, 0 on refusal.
std::size_t input_encode(const InputEntry *entries, std::uint8_t count, std::uint8_t *out,
                         std::size_t cap) noexcept;

// Decode into `out` (capacity INPUT_K_MAX). Entries point INTO `payload`. Returns the count, or 0
// with `why` set on a malformed payload (K of 0 or > 8, truncation, trailing bytes).
std::uint8_t input_decode(const std::uint8_t *payload, std::size_t len, InputEntry *out,
                          Verdict &why) noexcept;

// ---- channel B: latest-wins records ------------------------------------------------------------
// payload: repeated { id(1) | len(1) | bytes }. The receiver keeps only the NEWEST record per id --
// there is no value in an old ping sample or a stale telemetry line, so there is no queue and
// nothing to retransmit. Ids are fixed-width by construction; `len` is carried anyway so an unknown
// id can be skipped rather than ending the walk.
enum RecordId : std::uint8_t {
    REC_PING      = 1, // t_origin_ms(4 LE) | t_echo_ms(4 LE) -- t_echo 0 in a fresh probe
    REC_PONG      = 2, // t_origin_ms(4 LE) | t_echo_ms(4 LE) -- the probe's origin, echoed
    REC_STEP_HASH = 3, // step(4 LE) | hash(4 LE) -- the desync sample (D21's UDP carrier)
    REC_TELEMETRY = 4, // srtt_us(4) | rttvar_us(4) | loss_q16(4) | last_step(4), all LE
    REC_ID_MAX    = 4,
};
constexpr std::size_t REC_PING_LEN      = 8;
constexpr std::size_t REC_PONG_LEN      = 8;
constexpr std::size_t REC_STEP_HASH_LEN = 8;
constexpr std::size_t REC_TELEMETRY_LEN = 16;
constexpr std::size_t REC_LEN_MAX       = 64;

struct PingRecord {
    std::uint32_t t_origin_ms;
    std::uint32_t t_echo_ms;
};
struct StepHashRecord {
    std::uint32_t step;
    std::uint32_t hash;
};
struct TelemetryRecord {
    std::uint32_t srtt_us;
    std::uint32_t rttvar_us;
    std::uint32_t loss_q16; // loss fraction in Q16 (65536 == 100%)
    std::uint32_t last_step;
};

std::size_t record_append(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                          std::uint8_t id, const std::uint8_t *bytes, std::size_t len) noexcept;
std::size_t record_append_ping(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                               std::uint8_t id, const PingRecord &r) noexcept;
std::size_t record_append_step_hash(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                                    const StepHashRecord &r) noexcept;
std::size_t record_append_telemetry(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                                    const TelemetryRecord &r) noexcept;

// The materialised latest-wins state: one slot per id, last writer wins. Walking a payload into
// this is what makes "latest wins" a property of the code rather than of a comment.
struct LatestSlots {
    bool         present[REC_ID_MAX + 1] = {};
    std::uint8_t len[REC_ID_MAX + 1]     = {};
    std::uint8_t bytes[REC_ID_MAX + 1][REC_LEN_MAX] = {};
};

// Walk `payload`, overwriting `slots` per id. Unknown ids are skipped (not an error). Returns false
// with `why` set on truncation.
bool records_absorb(const std::uint8_t *payload, std::size_t len, LatestSlots &slots,
                    Verdict &why) noexcept;

bool ping_from(const LatestSlots &s, std::uint8_t id, PingRecord &out) noexcept;
bool step_hash_from(const LatestSlots &s, StepHashRecord &out) noexcept;
bool telemetry_from(const LatestSlots &s, TelemetryRecord &out) noexcept;

// ---- channel C: bulk reliable ------------------------------------------------------------------
// A chunk is at most 16 KiB (plan D7) and is fragmented into pieces of at most PIECE_MAX bytes so a
// piece plus its header plus the packet header stays under MAX_DATAGRAM. Each piece carries the
// WHOLE chunk's SHA-256, which costs 32 bytes per piece and buys the property D7 asks for: a piece
// is self-describing, so a transfer is resumable by index after a restart with no side channel
// saying what was being sent.
//
// PIECE  : kind(1)=0 | chunk_id(4) | index(2) | total(2) | chunk_len(4) | sha256(32) | piece_seq(4)
//          | piece_len(2) | bytes
// ACK    : kind(1)=1 | ack_seq(4) | ack_bits(4)
// ack_bits is Gaffer's 32-bit bitfield: bit i acknowledges (ack_seq - 1 - i), so one ack covers 33
// pieces and a lost ack is repaired by the next one.
constexpr std::uint32_t CHUNK_MAX      = 16 * 1024;
constexpr std::size_t   PIECE_MAX      = 1100;
constexpr std::size_t   PIECE_HDR      = 51;
constexpr std::size_t   ACK_LEN        = 9;
constexpr std::uint8_t  BULK_PIECE     = 0;
constexpr std::uint8_t  BULK_ACK       = 1;
// ceil(CHUNK_MAX / PIECE_MAX) is 15, and this is 16 because the STRIDE IS A LINK PROPERTY since
// mp:R1d: a relayed link runs channel C at a smaller piece so that the leg envelope's 34 bytes still
// leave the datagram inside T0's 1200-byte no-PMTU-discovery ceiling, and 16 KiB at that stride is
// 16 pieces. It is a CEILING on what a peer may declare, not the count anybody computes -- every
// site that needs the real one calls piece_count() with the stride it is using -- so raising it
// widens what is accepted by exactly the one value a relayed sender emits and nothing else. (The
// module's own piece_seq addressing has always been 16-wide: udp_channel_c.h PIECES_PER_CHUNK.)
constexpr std::uint16_t PIECE_TOTAL_MAX = 16;
// mp:R1d. The relayed stride. 1024 rather than "PIECE_MAX minus the leg's 34" because the number
// that matters is the DATAGRAM: 18 (T0 header) + 3 (frame) + 51 (piece header) + 1024 + 16 (tag)
// = 1112, and 1112 + 34 = 1146, comfortably inside 1200 with room for the envelope to grow a field.
// It also divides CHUNK_MAX exactly, so a full chunk is 16 whole pieces and the last-piece
// remainder case is never the common one.
constexpr std::size_t PIECE_MAX_RELAYED = 1024;

struct Piece {
    std::uint32_t       chunk_id;
    std::uint16_t       index;
    std::uint16_t       total;
    std::uint32_t       chunk_len;
    std::uint8_t        chunk_sha[SHA256_LEN];
    std::uint32_t       piece_seq;
    const std::uint8_t *bytes;
    std::size_t         len;
};

struct Ack {
    std::uint32_t ack_seq;
    std::uint32_t ack_bits;
};

std::size_t piece_encode(const Piece &p, std::uint8_t *out, std::size_t cap) noexcept;
bool        piece_decode(const std::uint8_t *payload, std::size_t len, Piece &out, Verdict &why,
                         std::size_t piece_max = PIECE_MAX) noexcept;
std::size_t ack_encode(const Ack &a, std::uint8_t *out, std::size_t cap) noexcept;
bool        ack_decode(const std::uint8_t *payload, std::size_t len, Ack &out) noexcept;

// How many pieces `chunk_len` takes at `piece_max` bytes a piece. 0 if the chunk is empty, over
// CHUNK_MAX, or the stride is not a usable one (zero, or above PIECE_MAX).
//
// THE STRIDE IS A PARAMETER BECAUSE IT IS A PROPERTY OF THE LINK, not of the format (mp:R1d). Both
// ends of one link derive it the same way -- a relayed endpoint uses PIECE_MAX_RELAYED, a direct one
// PIECE_MAX -- and the default keeps every pre-R1d caller (the fixtures, the wire selftest) reading
// exactly as it did. What a receiver must NOT do is assume its own stride of a peer's pieces: the
// three entry points that touch the stride all take it, so a site that forgets one fails to compile
// rather than reassembling a chunk at the wrong offsets.
std::uint16_t piece_count(std::uint32_t chunk_len, std::size_t piece_max = PIECE_MAX) noexcept;
// Build piece `index` of `chunk`. Fills the header, points `bytes` into `chunk`.
bool piece_for(const std::uint8_t *chunk, std::uint32_t chunk_len, std::uint32_t chunk_id,
               std::uint16_t index, std::uint32_t piece_seq, Piece &out,
               const std::uint8_t sha[SHA256_LEN], std::size_t piece_max = PIECE_MAX) noexcept;

// ---- connect token ------------------------------------------------------------------------------
// netcode.io's central idea, kept: the host mints a token at lobby time and seals its private part
// with a key only the host (and the relay it authorises) holds. A client presents the token it was
// given; it cannot read, forge or extend one. The keys the session will use travel INSIDE the
// sealed part, so possession of a token is what admits a peer -- no PSK typed by a stranger.
//
// The HANDSHAKE that presents a token is mp:T1's; this is the record and its codec.
//
// wire (210 bytes):
//   0   magic/version(1) | kind(1)=TOKEN_KIND
//   2   nonce(8 LE)          -- the ChaCha20 nonce; unique per minted token
//   10  conn_id(8)           -- PLAINTEXT: a relay demuxes before it can open anything
//   18  expire_unix_ms(8 LE) -- PLAINTEXT: a relay drops an expired token with no key at all
//   26  ciphertext(168)      -- the private part below
//   194 mac(16)              -- over bytes [0, 194)
// private part (168 bytes):
//   match_id(16) | conn_id(8) | slot(1) | reserved(7) | expire_unix_ms(8)
//   | enc_c2s(32) | enc_s2c(32) | mac_c2s(32) | mac_s2c(32)
//
// conn_id and expire are carried TWICE, outside and inside. That is not redundancy for its own
// sake: the outside copy is what an unauthenticated relay routes on, the inside copy is what the
// host trusts, and token_open() requires them to be EQUAL -- so a middlebox rewriting the routing
// copy is caught rather than being believed by one party and not the other.
constexpr std::uint8_t  TOKEN_KIND     = 1;
constexpr std::size_t   TOKEN_PRIVATE  = 168;
constexpr std::size_t   TOKEN_WIRE     = 26 + TOKEN_PRIVATE + MAC_LEN; // 210
constexpr std::size_t   TOKEN_SLOT_MAX = 7;                            // the lobby's 8 physical slots

struct ConnectToken {
    std::uint8_t  match_id[UUID7_BYTES];
    std::uint8_t  conn_id[CONN_ID_BYTES];
    std::uint8_t  slot;
    std::uint64_t expire_unix_ms;
    SessionKeys   keys;
};

// The host key is one 32-byte secret; the enc and mac halves are derived from it by label, so a
// deployment has one thing to configure and the two uses can never be accidentally the same bytes.
void token_keys(const std::uint8_t host_key[KEY_LEN], std::uint8_t enc_out[KEY_LEN],
                std::uint8_t mac_out[KEY_LEN]) noexcept;

// Seal a token into `out[TOKEN_WIRE]`. Returns TOKEN_WIRE, or 0 if slot > TOKEN_SLOT_MAX.
std::size_t token_seal(const std::uint8_t host_key[KEY_LEN], std::uint64_t nonce,
                       const ConnectToken &tok, std::uint8_t *out) noexcept;

// Open a token. `now_unix_ms` of 0 skips the expiry test (a fixture checking the codec, not the
// clock); otherwise an expired token is Verdict::BadMac -- deliberately NOT its own verdict on the
// wire, because telling an attacker which of "wrong key" and "too late" applied is free information.
// The caller learns the difference from `expired`.
Verdict token_open(const std::uint8_t host_key[KEY_LEN], const std::uint8_t *in, std::size_t len,
                   std::uint64_t now_unix_ms, ConnectToken &out, bool &expired) noexcept;

} // namespace udp
} // namespace mh_net_proto
