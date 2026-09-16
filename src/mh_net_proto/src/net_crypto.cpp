// net_crypto.cpp -- see include/mh_net_proto/net_crypto.h.
//
// Self-contained SHA-256 / HMAC-SHA256 / ChaCha20. Written out rather than linked because the
// consumer is a DLL injected into a 2001 Watcom game: it must not drag in a crypto library, must
// build identically for the portable relay, and must have no initialization order or CRT surprises.
// These are small, well-specified, heavily cross-checked algorithms (RFC 6234 / RFC 8439) and the
// unit tests pin them to the published vectors -- that is the guard against a hand-rolled slip.
#include "mh_net_proto/net_crypto.h"

#include <cstring>

namespace mh_net_proto {
namespace {

inline std::uint32_t ror32(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline std::uint32_t rol32(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline std::uint32_t load_be32(const std::uint8_t *p) {
    return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) | ((std::uint32_t)p[2] << 8) | p[3];
}
inline void store_be32(std::uint8_t *p, std::uint32_t v) {
    p[0] = (std::uint8_t)(v >> 24);
    p[1] = (std::uint8_t)(v >> 16);
    p[2] = (std::uint8_t)(v >> 8);
    p[3] = (std::uint8_t)v;
}
inline std::uint32_t load_le32(const std::uint8_t *p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
inline void store_le32(std::uint8_t *p, std::uint32_t v) {
    p[0] = (std::uint8_t)v;
    p[1] = (std::uint8_t)(v >> 8);
    p[2] = (std::uint8_t)(v >> 16);
    p[3] = (std::uint8_t)(v >> 24);
}

// ---- SHA-256 (FIPS 180-4) -----------------------------------------------------------------------
const std::uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct Sha256Ctx {
    std::uint32_t h[8];
    std::uint64_t total;
    std::uint8_t  buf[64];
    std::size_t   used;
};

void sha_init(Sha256Ctx &c) {
    c.h[0] = 0x6a09e667; c.h[1] = 0xbb67ae85; c.h[2] = 0x3c6ef372; c.h[3] = 0xa54ff53a;
    c.h[4] = 0x510e527f; c.h[5] = 0x9b05688c; c.h[6] = 0x1f83d9ab; c.h[7] = 0x5be0cd19;
    c.total = 0;
    c.used  = 0;
}

void sha_block(Sha256Ctx &c, const std::uint8_t *p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]             = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    std::uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        std::uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        std::uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        std::uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

void sha_update(Sha256Ctx &c, const std::uint8_t *p, std::size_t n) {
    c.total += n;
    while (n) {
        std::size_t take = 64 - c.used;
        if (take > n) take = n;
        std::memcpy(c.buf + c.used, p, take);
        c.used += take;
        p += take;
        n -= take;
        if (c.used == 64) {
            sha_block(c, c.buf);
            c.used = 0;
        }
    }
}

void sha_final(Sha256Ctx &c, std::uint8_t out[SHA256_LEN]) {
    std::uint64_t bits = c.total * 8;
    std::uint8_t  pad  = 0x80;
    sha_update(c, &pad, 1);
    std::uint8_t zero = 0;
    while (c.used != 56) sha_update(c, &zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (std::uint8_t)(bits >> (56 - i * 8));
    sha_update(c, len, 8);
    for (int i = 0; i < 8; ++i) store_be32(out + i * 4, c.h[i]);
}

// ---- ChaCha20 (RFC 8439) ------------------------------------------------------------------------
inline void qr(std::uint32_t &a, std::uint32_t &b, std::uint32_t &c, std::uint32_t &d) {
    a += b; d ^= a; d = rol32(d, 16);
    c += d; b ^= c; b = rol32(b, 12);
    a += b; d ^= a; d = rol32(d, 8);
    c += d; b ^= c; b = rol32(b, 7);
}

void chacha_block(const std::uint8_t key[KEY_LEN], std::uint32_t counter,
                  const std::uint8_t nonce[12], std::uint8_t out[64]) {
    std::uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i) s[4 + i] = load_le32(key + i * 4);
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = load_le32(nonce + i * 4);
    std::uint32_t x[16];
    std::memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; ++i) { // 20 rounds = 10 double-rounds
        qr(x[0], x[4], x[8], x[12]);
        qr(x[1], x[5], x[9], x[13]);
        qr(x[2], x[6], x[10], x[14]);
        qr(x[3], x[7], x[11], x[15]);
        qr(x[0], x[5], x[10], x[15]);
        qr(x[1], x[6], x[11], x[12]);
        qr(x[2], x[7], x[8], x[13]);
        qr(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) store_le32(out + i * 4, x[i] + s[i]);
}

// One HMAC over a list of pieces -- the handshake proofs and the record MAC are all
// "key + a few concatenated fields", and building a scratch buffer for each was the only
// alternative.
struct Piece {
    const std::uint8_t *p;
    std::size_t         n;
};

void hmac_pieces(const std::uint8_t *key, std::size_t key_len, const Piece *pieces, int npieces,
                 std::uint8_t out[SHA256_LEN]) {
    std::uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha256(key, key_len, k); // long keys are hashed down first (RFC 2104)
    } else {
        std::memcpy(k, key, key_len);
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (std::uint8_t)(k[i] ^ 0x36);
        opad[i] = (std::uint8_t)(k[i] ^ 0x5c);
    }
    Sha256Ctx c;
    sha_init(c);
    sha_update(c, ipad, 64);
    for (int i = 0; i < npieces; ++i)
        if (pieces[i].n) sha_update(c, pieces[i].p, pieces[i].n);
    std::uint8_t inner[SHA256_LEN];
    sha_final(c, inner);

    sha_init(c);
    sha_update(c, opad, 64);
    sha_update(c, inner, SHA256_LEN);
    sha_final(c, out);
}

// Handshake proof / key-derivation labels. Distinct per role and per direction, so no derived value
// is ever usable in another position (proof reflection, key reuse across directions).
const char *LBL_SERVER  = "mh-proof-server";
const char *LBL_CLIENT  = "mh-proof-client";
const char *LBL_ENC_C2S = "mh-enc-c2s";
const char *LBL_ENC_S2C = "mh-enc-s2c";
const char *LBL_MAC_C2S = "mh-mac-c2s";
const char *LBL_MAC_S2C = "mh-mac-s2c";

void kdf(const std::uint8_t psk[KEY_LEN], const char *label, const std::uint8_t cn[NONCE_LEN],
         const std::uint8_t sn[NONCE_LEN], std::uint8_t out[SHA256_LEN]) {
    Piece pieces[3] = {{(const std::uint8_t *)label, std::strlen(label)}, {cn, NONCE_LEN}, {sn, NONCE_LEN}};
    hmac_pieces(psk, KEY_LEN, pieces, 3, out);
}

} // namespace

// ---- public primitives --------------------------------------------------------------------------
void sha256(const std::uint8_t *msg, std::size_t len, std::uint8_t out[SHA256_LEN]) noexcept {
    Sha256Ctx c;
    sha_init(c);
    sha_update(c, msg, len);
    sha_final(c, out);
}

void hmac_sha256(const std::uint8_t *key, std::size_t key_len, const std::uint8_t *msg,
                 std::size_t msg_len, std::uint8_t out[SHA256_LEN]) noexcept {
    Piece p = {msg, msg_len};
    hmac_pieces(key, key_len, &p, 1, out);
}

void chacha20_xor(const std::uint8_t key[KEY_LEN], std::uint64_t seq, const std::uint8_t *in,
                  std::uint8_t *out, std::size_t len) noexcept {
    std::uint8_t nonce[12];
    std::memset(nonce, 0, sizeof(nonce));
    for (int i = 0; i < 8; ++i) nonce[4 + i] = (std::uint8_t)(seq >> (i * 8)); // record seq IS the nonce
    std::uint8_t  ks[64];
    std::uint32_t counter = 0;
    std::size_t   done    = 0;
    while (done < len) {
        chacha_block(key, counter++, nonce, ks);
        std::size_t take = len - done;
        if (take > 64) take = 64;
        for (std::size_t i = 0; i < take; ++i) out[done + i] = (std::uint8_t)(in[done + i] ^ ks[i]);
        done += take;
    }
}

bool ct_equal(const std::uint8_t *a, const std::uint8_t *b, std::size_t len) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < len; ++i) diff = (std::uint8_t)(diff | (a[i] ^ b[i]));
    return diff == 0;
}

// ---- handshake ----------------------------------------------------------------------------------
void hs_build_hello(const std::uint8_t client_nonce[NONCE_LEN], std::uint8_t out[HS_HELLO_LEN]) noexcept {
    store_le32(out, HS_MAGIC);
    out[4] = (std::uint8_t)HS_VERSION;
    out[5] = (std::uint8_t)(HS_VERSION >> 8);
    out[6] = out[7] = 0; // flags, reserved
    std::memcpy(out + 8, client_nonce, NONCE_LEN);
}

bool hs_parse_hello(const std::uint8_t in[HS_HELLO_LEN], std::uint8_t client_nonce[NONCE_LEN],
                    std::uint16_t &version) noexcept {
    if (load_le32(in) != HS_MAGIC) return false;
    version = (std::uint16_t)(in[4] | (in[5] << 8));
    if (version != HS_VERSION) return false;
    std::memcpy(client_nonce, in + 8, NONCE_LEN);
    return true;
}

void hs_build_challenge(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                        const std::uint8_t server_nonce[NONCE_LEN], std::uint8_t out[HS_CHALLENGE_LEN]) noexcept {
    store_le32(out, HS_MAGIC);
    out[4] = (std::uint8_t)HS_VERSION;
    out[5] = (std::uint8_t)(HS_VERSION >> 8);
    out[6] = out[7] = 0;
    std::memcpy(out + 8, server_nonce, NONCE_LEN);
    kdf(psk, LBL_SERVER, client_nonce, server_nonce, out + 24);
}

bool hs_check_challenge(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                        const std::uint8_t in[HS_CHALLENGE_LEN], std::uint8_t server_nonce[NONCE_LEN],
                        std::uint16_t &version) noexcept {
    if (load_le32(in) != HS_MAGIC) return false;
    version = (std::uint16_t)(in[4] | (in[5] << 8));
    if (version != HS_VERSION) return false;
    std::memcpy(server_nonce, in + 8, NONCE_LEN);
    std::uint8_t expect[SHA256_LEN];
    kdf(psk, LBL_SERVER, client_nonce, server_nonce, expect);
    return ct_equal(expect, in + 24, SHA256_LEN);
}

void hs_build_response(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                       const std::uint8_t server_nonce[NONCE_LEN], std::uint8_t out[HS_RESPONSE_LEN]) noexcept {
    kdf(psk, LBL_CLIENT, client_nonce, server_nonce, out);
}

bool hs_check_response(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                       const std::uint8_t server_nonce[NONCE_LEN], const std::uint8_t in[HS_RESPONSE_LEN]) noexcept {
    std::uint8_t expect[SHA256_LEN];
    kdf(psk, LBL_CLIENT, client_nonce, server_nonce, expect);
    return ct_equal(expect, in, SHA256_LEN);
}

void hs_derive_keys(const std::uint8_t psk[KEY_LEN], const std::uint8_t client_nonce[NONCE_LEN],
                    const std::uint8_t server_nonce[NONCE_LEN], SessionKeys &out) noexcept {
    kdf(psk, LBL_ENC_C2S, client_nonce, server_nonce, out.enc_c2s);
    kdf(psk, LBL_ENC_S2C, client_nonce, server_nonce, out.enc_s2c);
    kdf(psk, LBL_MAC_C2S, client_nonce, server_nonce, out.mac_c2s);
    kdf(psk, LBL_MAC_S2C, client_nonce, server_nonce, out.mac_s2c);
}

// ---- record layer -------------------------------------------------------------------------------
namespace {
void rec_mac(const std::uint8_t mac_key[KEY_LEN], std::uint64_t seq, const std::uint8_t *ct,
             std::uint32_t len, std::uint8_t out[SHA256_LEN]) {
    std::uint8_t hdr[12]; // seq(8) | len(4) -- both authenticated, so neither can be tampered
    for (int i = 0; i < 8; ++i) hdr[i] = (std::uint8_t)(seq >> (i * 8));
    store_le32(hdr + 8, len);
    Piece pieces[2] = {{hdr, sizeof(hdr)}, {ct, len}};
    hmac_pieces(mac_key, KEY_LEN, pieces, 2, out);
}
} // namespace

std::size_t rec_seal(const std::uint8_t enc_key[KEY_LEN], const std::uint8_t mac_key[KEY_LEN],
                     std::uint64_t seq, const std::uint8_t *plain, std::uint32_t len,
                     std::uint8_t *out) noexcept {
    store_le32(out, len);
    chacha20_xor(enc_key, seq, plain, out + REC_LEN_SIZE, len);
    std::uint8_t full[SHA256_LEN];
    rec_mac(mac_key, seq, out + REC_LEN_SIZE, len, full); // encrypt-then-MAC
    std::memcpy(out + REC_LEN_SIZE + len, full, MAC_LEN);
    return REC_LEN_SIZE + len + MAC_LEN;
}

bool rec_open(const std::uint8_t enc_key[KEY_LEN], const std::uint8_t mac_key[KEY_LEN],
              std::uint64_t seq, std::uint8_t *io, std::uint32_t len,
              const std::uint8_t mac[MAC_LEN]) noexcept {
    std::uint8_t full[SHA256_LEN];
    rec_mac(mac_key, seq, io, len, full);
    if (!ct_equal(full, mac, MAC_LEN)) return false; // verify BEFORE decrypting
    chacha20_xor(enc_key, seq, io, io, len);
    return true;
}

// ---- key text -----------------------------------------------------------------------------------
void key_to_hex(const std::uint8_t key[KEY_LEN], char out[KEY_HEX_LEN + 1]) noexcept {
    static const char *H = "0123456789abcdef";
    for (std::size_t i = 0; i < KEY_LEN; ++i) {
        out[i * 2]     = H[key[i] >> 4];
        out[i * 2 + 1] = H[key[i] & 0xf];
    }
    out[KEY_HEX_LEN] = '\0';
}

bool key_from_hex(const char *hex, std::uint8_t out[KEY_LEN]) noexcept {
    if (!hex) return false;
    while (*hex == ' ' || *hex == '\t' || *hex == '\r' || *hex == '\n') ++hex;
    std::uint8_t v[KEY_LEN];
    for (std::size_t i = 0; i < KEY_LEN; ++i) {
        int nib[2];
        for (int k = 0; k < 2; ++k) {
            char ch = hex[i * 2 + k];
            if (ch >= '0' && ch <= '9') nib[k] = ch - '0';
            else if (ch >= 'a' && ch <= 'f') nib[k] = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') nib[k] = ch - 'A' + 10;
            else return false;
        }
        v[i] = (std::uint8_t)((nib[0] << 4) | nib[1]);
    }
    for (const char *t = hex + KEY_HEX_LEN; *t; ++t) // trailing whitespace only
        if (*t != ' ' && *t != '\t' && *t != '\r' && *t != '\n') return false;
    std::memcpy(out, v, KEY_LEN);
    return true;
}

} // namespace mh_net_proto
