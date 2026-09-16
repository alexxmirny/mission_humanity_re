// mh_net_proto -- authenticated + encrypted link layer for the MH multiplayer transport.
//
// WHY THIS EXISTS. Peers connect over the raw internet: the host publishes a port (directly, or via
// an `ssh -R` reverse tunnel from a VPS when it is behind NAT), and anyone who can reach that port
// speaks to the game. Before this, "anyone" was literal -- a TCP connect with the right two magic
// bytes got a peer slot, and its payloads went straight into mh.exe's 2001-era lobby/lockstep
// parser. Note the tunnel does NOT help here and cannot: `ssh -R` terminates on the host box, so a
// remote attacker's connection arrives at the game as 127.0.0.1, indistinguishable from a local one.
// No address-based gate can work. Authentication has to live on the wire.
//
// WHAT IT PROVIDES. A pre-shared-key handshake (mutual, nonce-based, so neither side's proof
// replays) followed by ChaCha20 encryption with encrypt-then-MAC integrity on every record. That
// gives: unauthenticated peers are rejected before they can address the game at all; traffic is
// confidential across the public path (the SSH hop only covers host<->VPS, not peer<->VPS); and
// records cannot be forged, reordered, duplicated or truncated undetected -- the MAC covers an
// implicit per-direction sequence number that both sides count independently.
//
// WHAT IT IS NOT. Not a substitute for the game being robust against a MALICIOUS PEER: an
// authenticated player still speaks the retail protocol, and holding the key means you were invited.
// This raises the bar from "anyone on the internet" to "someone you gave the key to", which is the
// threat model that matters for a friends-only session.
//
// Portable by the mh_net_proto contract: standard library + fixed-width ints only, no platform or
// socket headers, so the DLL, the selftest, and a future Linux relay share the identical bytes.
#pragma once
#include <cstdint>
#include <cstddef>

namespace mh_net_proto {

// ---- primitives ---------------------------------------------------------------------------------
constexpr std::size_t SHA256_LEN = 32;
constexpr std::size_t KEY_LEN    = 32; // pre-shared key, and each derived directional key
constexpr std::size_t NONCE_LEN  = 16; // per-side handshake nonce
constexpr std::size_t MAC_LEN    = 16; // per-record tag: HMAC-SHA256 truncated (128-bit)

void sha256(const std::uint8_t *msg, std::size_t len, std::uint8_t out[SHA256_LEN]) noexcept;
void hmac_sha256(const std::uint8_t *key, std::size_t key_len, const std::uint8_t *msg,
                 std::size_t msg_len, std::uint8_t out[SHA256_LEN]) noexcept;

// ChaCha20 keystream XOR (RFC 8439, 96-bit nonce + 32-bit block counter). In-place safe.
void chacha20_xor(const std::uint8_t key[KEY_LEN], std::uint64_t seq, const std::uint8_t *in,
                  std::uint8_t *out, std::size_t len) noexcept;

// Constant-time equality -- a byte-at-a-time memcmp on a MAC leaks its prefix through timing.
bool ct_equal(const std::uint8_t *a, const std::uint8_t *b, std::size_t len) noexcept;

// ---- handshake ----------------------------------------------------------------------------------
// C->S  HELLO      : "MHK1" | ver(2) | flags(2) | client_nonce(16)          = 24 bytes
// S->C  CHALLENGE  : "MHK1" | ver(2) | flags(2) | server_nonce(16) | proof_s(32) = 56 bytes
// C->S  RESPONSE   : proof_c(32)                                            = 32 bytes
// Each proof is HMAC(psk, label | client_nonce | server_nonce) with a distinct label per direction,
// so a proof captured from one side can never be replayed as the other's, and a full transcript
// replay fails because both nonces are fresh per connection.
constexpr std::uint32_t HS_MAGIC    = 0x314B484Du; // "MHK1" little-endian
constexpr std::uint16_t HS_VERSION  = 1;
constexpr std::size_t   HS_HELLO_LEN     = 24;
constexpr std::size_t   HS_CHALLENGE_LEN = 56;
constexpr std::size_t   HS_RESPONSE_LEN  = SHA256_LEN;

// Derived per-connection material. Directions are keyed separately so a record can never be
// reflected back at its sender and still verify.
struct SessionKeys {
    std::uint8_t enc_c2s[KEY_LEN];
    std::uint8_t enc_s2c[KEY_LEN];
    std::uint8_t mac_c2s[KEY_LEN];
    std::uint8_t mac_s2c[KEY_LEN];
};

void hs_build_hello(const std::uint8_t client_nonce[NONCE_LEN], std::uint8_t out[HS_HELLO_LEN]) noexcept;
// Returns false on bad magic or an unsupported version (report the two apart: a version mismatch is
// "update your build", a magic mismatch is "that is not this game").
bool hs_parse_hello(const std::uint8_t in[HS_HELLO_LEN], std::uint8_t client_nonce[NONCE_LEN],
                    std::uint16_t &version) noexcept;

void hs_build_challenge(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                        const std::uint8_t server_nonce[NONCE_LEN], std::uint8_t out[HS_CHALLENGE_LEN]) noexcept;
// Client side: verify the host really holds the key, and recover its nonce.
bool hs_check_challenge(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                        const std::uint8_t in[HS_CHALLENGE_LEN], std::uint8_t server_nonce[NONCE_LEN],
                        std::uint16_t &version) noexcept;

void hs_build_response(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                       const std::uint8_t server_nonce[NONCE_LEN], std::uint8_t out[HS_RESPONSE_LEN]) noexcept;
bool hs_check_response(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                       const std::uint8_t server_nonce[NONCE_LEN], const std::uint8_t in[HS_RESPONSE_LEN]) noexcept;

void hs_derive_keys(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                    const std::uint8_t server_nonce[NONCE_LEN], SessionKeys &out) noexcept;

// ---- record layer -------------------------------------------------------------------------------
// On the wire: len(4, LE) | ciphertext(len) | mac(16). `seq` is NOT transmitted -- both ends count
// their own direction, so a replayed or dropped record fails the MAC instead of being accepted.
constexpr std::size_t REC_LEN_SIZE     = 4;
constexpr std::size_t REC_OVERHEAD     = REC_LEN_SIZE + MAC_LEN;
constexpr std::uint32_t REC_MAX_PLAIN  = 64 * 1024; // sanity bound on a declared length

// Seal `plain` into `out` (needs len + REC_OVERHEAD bytes). Returns the total bytes written.
std::size_t rec_seal(const std::uint8_t enc_key[KEY_LEN], const std::uint8_t mac_key[KEY_LEN],
                     std::uint64_t seq, const std::uint8_t *plain, std::uint32_t len,
                     std::uint8_t *out) noexcept;
// Verify + decrypt a body of `len` ciphertext bytes followed by its MAC. `io` is decrypted in place.
bool rec_open(const std::uint8_t enc_key[KEY_LEN], const std::uint8_t mac_key[KEY_LEN],
              std::uint64_t seq, std::uint8_t *io, std::uint32_t len,
              const std::uint8_t mac[MAC_LEN]) noexcept;

// ---- key text -----------------------------------------------------------------------------------
// The key travels between players as text (a Discord message), so it is 64 lowercase hex chars.
constexpr std::size_t KEY_HEX_LEN = KEY_LEN * 2;
void key_to_hex(const std::uint8_t key[KEY_LEN], char out[KEY_HEX_LEN + 1]) noexcept;
// Tolerant of surrounding whitespace and of a trailing newline; rejects anything else.
bool key_from_hex(const char *hex, std::uint8_t out[KEY_LEN]) noexcept;

} // namespace mh_net_proto
