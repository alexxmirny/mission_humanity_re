// mh_net_proto -- the UDP packet format's encoder/decoder (mp:T0, plan D2). See net_udp.h for the
// layout, the reasoning and what consumes it.
//
// EVERY BOUNDS TEST HERE IS LOAD-BEARING. This code's whole input is hostile by construction: a
// datagram arrives from the internet before anything has authenticated it, and the header is read
// BEFORE the MAC is checked because the MAC's own key is selected by what the header says. So each
// read is preceded by its length test, the tests are ordered so that the cheapest and most
// contents-independent (length, magic) run first, and the fuzz arm of `net_selftest.exe udpwiretest`
// exists to keep that honest under ASan rather than by inspection.
#include "mh_net_proto/net_udp.h"

#include <cstring>

#include "byteio.h"

namespace mh_net_proto {
namespace udp {

using detail::get_u16;
using detail::get_u32;
using detail::put_u16;
using detail::put_u32;

namespace {

void put_u64(std::uint8_t *p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = (std::uint8_t)(v >> (i * 8));
}
std::uint64_t get_u64(const std::uint8_t *p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (std::uint64_t)p[i] << (i * 8);
    return v;
}

// The packet tag: HMAC-SHA256 over header||ciphertext, truncated to 128 bits exactly as the TCP
// record layer does (net_crypto.h MAC_LEN). The input is contiguous in the datagram buffer, which
// is why the header sits in front of the ciphertext rather than after it.
void packet_tag(const std::uint8_t mac_key[KEY_LEN], const std::uint8_t *pkt, std::size_t aad_plus_ct,
                std::uint8_t out[MAC_LEN]) noexcept {
    std::uint8_t full[SHA256_LEN];
    hmac_sha256(mac_key, KEY_LEN, pkt, aad_plus_ct, full);
    std::memcpy(out, full, MAC_LEN);
}

const char *const TOKEN_LBL_ENC = "mh-token-enc";
const char *const TOKEN_LBL_MAC = "mh-token-mac";

} // namespace

const char *verdict_name(Verdict v) noexcept {
    switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::TooLong: return "too_long";
    case Verdict::TooShort: return "too_short";
    case Verdict::BadMagic: return "bad_magic";
    case Verdict::BadVersion: return "bad_version";
    case Verdict::BadType: return "bad_type";
    case Verdict::WrongConn: return "wrong_conn";
    case Verdict::Replay: return "replay";
    case Verdict::BadMac: return "bad_mac";
    case Verdict::Malformed: return "malformed";
    case Verdict::BodyTooLong: return "body_too_long";
    }
    return "?";
}

// ---- replay window ------------------------------------------------------------------------------

bool ReplayWindow::check(std::uint64_t seq) const noexcept {
    if (!seeded) return true;
    if (seq > newest) return true;
    const std::uint64_t age = newest - seq;
    if (age >= REPLAY_WINDOW) return false; // below the floor: too old to prove it is not a replay
    return (bits & (std::uint64_t(1) << age)) == 0;
}

void ReplayWindow::commit(std::uint64_t seq) noexcept {
    if (!check(seq)) return;
    if (!seeded) {
        seeded = true;
        newest = seq;
        bits   = 1;
        return;
    }
    if (seq > newest) {
        const std::uint64_t shift = seq - newest;
        bits                      = (shift >= REPLAY_WINDOW) ? 0 : (bits << shift);
        bits |= 1;
        newest = seq;
        return;
    }
    bits |= (std::uint64_t(1) << (newest - seq));
}

// ---- header -------------------------------------------------------------------------------------

bool hdr_peek(const std::uint8_t *pkt, std::size_t len, Header &out) noexcept {
    if (len < MIN_DATAGRAM || len > MAX_DATAGRAM) return false;
    if ((pkt[0] >> 4) != MAGIC_NIBBLE) return false;
    if ((pkt[0] & 0x0f) != VERSION) return false;
    if (pkt[1] > PKT_TYPE_MAX) return false;
    out.type = pkt[1];
    std::memcpy(out.conn_id, pkt + 2, CONN_ID_BYTES);
    out.seq = get_u64(pkt + 10);
    return true;
}

// ---- packet -------------------------------------------------------------------------------------

std::size_t packet_encode(std::uint8_t type, const std::uint8_t conn_id[CONN_ID_BYTES],
                          std::uint64_t seq, const std::uint8_t enc_key[KEY_LEN],
                          const std::uint8_t mac_key[KEY_LEN], const std::uint8_t *body,
                          std::size_t body_len, std::uint8_t *out, Verdict &why) noexcept {
    if (type > PKT_TYPE_MAX) {
        why = Verdict::BadType;
        return 0;
    }
    if (body_len > MAX_BODY) {
        why = Verdict::BodyTooLong;
        return 0;
    }
    out[0] = MAGIC_VER;
    out[1] = type;
    std::memcpy(out + 2, conn_id, CONN_ID_BYTES);
    put_u64(out + 10, seq);
    if (body_len) chacha20_xor(enc_key, seq, body, out + HDR_SIZE, body_len);
    packet_tag(mac_key, out, HDR_SIZE + body_len, out + HDR_SIZE + body_len);
    why = Verdict::Ok;
    return HDR_SIZE + body_len + TAG_SIZE;
}

Verdict packet_decode(std::uint8_t *pkt, std::size_t len, const std::uint8_t *expect_conn,
                      const std::uint8_t enc_key[KEY_LEN], const std::uint8_t mac_key[KEY_LEN],
                      ReplayWindow *win, Header &hdr, std::size_t *body_len) noexcept {
    // ORDER MATTERS AND IS PART OF THE FORMAT. The length ceiling is first so a 1201-byte datagram
    // is refused for its size whatever it contains -- if the magic test ran first, the same
    // oversize datagram would be reported as BadMagic when its first byte happened to be junk, and
    // the acceptance clause "a 1201-byte payload is refused" would be testing the wrong thing.
    if (len > MAX_DATAGRAM) return Verdict::TooLong;
    if (len < MIN_DATAGRAM) return Verdict::TooShort;
    if ((pkt[0] >> 4) != MAGIC_NIBBLE) return Verdict::BadMagic;
    if ((pkt[0] & 0x0f) != VERSION) return Verdict::BadVersion;
    if (pkt[1] > PKT_TYPE_MAX) return Verdict::BadType;

    hdr.type = pkt[1];
    std::memcpy(hdr.conn_id, pkt + 2, CONN_ID_BYTES);
    hdr.seq = get_u64(pkt + 10);

    if (expect_conn && std::memcmp(hdr.conn_id, expect_conn, CONN_ID_BYTES) != 0)
        return Verdict::WrongConn;

    // Cheap pre-check before the HMAC (see net_udp.h): a duplicate flood costs no crypto.
    if (win && !win->check(hdr.seq)) return Verdict::Replay;

    const std::size_t ct_len = len - HDR_SIZE - TAG_SIZE;
    std::uint8_t      want[MAC_LEN];
    packet_tag(mac_key, pkt, HDR_SIZE + ct_len, want);
    if (!ct_equal(want, pkt + HDR_SIZE + ct_len, MAC_LEN)) return Verdict::BadMac;

    if (ct_len) chacha20_xor(enc_key, hdr.seq, pkt + HDR_SIZE, pkt + HDR_SIZE, ct_len);
    // Commit only now: an unauthenticated packet may not move the window (net_udp.h).
    if (win) win->commit(hdr.seq);
    *body_len = ct_len;
    return Verdict::Ok;
}

// ---- channel mux ---------------------------------------------------------------------------------

bool frame_append(std::uint8_t *body, std::size_t cap, std::size_t *used, std::uint8_t id,
                  const std::uint8_t *payload, std::size_t len) noexcept {
    if (len > 0xffffu) return false;
    if (*used + FRAME_HDR + len > cap) return false;
    body[*used]     = id;
    put_u16(body + *used + 1, (std::uint16_t)len);
    if (len) std::memcpy(body + *used + FRAME_HDR, payload, len);
    *used += FRAME_HDR + len;
    return true;
}

bool frame_next(const std::uint8_t *body, std::size_t len, std::size_t *off, Frame &out,
                bool &ok) noexcept {
    ok = true;
    if (*off == len) return false;
    if (*off + FRAME_HDR > len) { // a stub too short to be a frame header
        ok = false;
        return false;
    }
    const std::uint16_t flen = get_u16(body + *off + 1);
    if (*off + FRAME_HDR + flen > len) { // a length running past the body
        ok = false;
        return false;
    }
    out.id   = body[*off];
    out.data = body + *off + FRAME_HDR;
    out.len  = flen;
    *off += FRAME_HDR + flen;
    return true;
}

// ---- channel A: step inputs ------------------------------------------------------------------------

std::size_t input_encode(const InputEntry *entries, std::uint8_t count, std::uint8_t *out,
                         std::size_t cap) noexcept {
    if (count < INPUT_K_MIN || count > INPUT_K_MAX) return 0;
    std::size_t need = 5;
    for (std::uint8_t i = 0; i < count; ++i) {
        if (entries[i].len > INPUT_ENTRY_MAX) return 0;
        // The implied numbering, CHECKED rather than assumed. A caller that hands over entries in
        // the wrong order produces a payload no decoder can tell from a correct one, so the only
        // place this can be caught is here.
        if (entries[i].step != entries[0].step - i) return 0;
        need += 1 + entries[i].len;
    }
    if (need > cap) return 0;

    out[0] = count;
    put_u32(out + 1, entries[0].step);
    std::size_t off = 5;
    for (std::uint8_t i = 0; i < count; ++i) {
        out[off++] = (std::uint8_t)entries[i].len;
        if (entries[i].len) std::memcpy(out + off, entries[i].bytes, entries[i].len);
        off += entries[i].len;
    }
    return off;
}

std::uint8_t input_decode(const std::uint8_t *payload, std::size_t len, InputEntry *out,
                          Verdict &why) noexcept {
    why = Verdict::Malformed;
    if (len < 5) return 0;
    const std::uint8_t count = payload[0];
    if (count < INPUT_K_MIN || count > INPUT_K_MAX) return 0;
    const std::uint32_t newest = get_u32(payload + 1);

    std::size_t off = 5;
    for (std::uint8_t i = 0; i < count; ++i) {
        if (off + 1 > len) return 0;
        const std::uint8_t elen = payload[off++];
        if (off + elen > len) return 0;
        out[i].step  = newest - i;
        out[i].bytes = payload + off;
        out[i].len   = elen;
        off += elen;
    }
    if (off != len) return 0; // trailing bytes: a frame whose length disagrees with its contents
    why = Verdict::Ok;
    return count;
}

// ---- channel B: latest-wins records ------------------------------------------------------------------

std::size_t record_append(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                          std::uint8_t id, const std::uint8_t *bytes, std::size_t len) noexcept {
    if (len > REC_LEN_MAX) return 0;
    if (*used + 2 + len > cap) return 0;
    payload[*used]     = id;
    payload[*used + 1] = (std::uint8_t)len;
    if (len) std::memcpy(payload + *used + 2, bytes, len);
    *used += 2 + len;
    return 2 + len;
}

std::size_t record_append_ping(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                               std::uint8_t id, const PingRecord &r) noexcept {
    std::uint8_t b[REC_PING_LEN];
    put_u32(b, r.t_origin_ms);
    put_u32(b + 4, r.t_echo_ms);
    return record_append(payload, cap, used, id, b, sizeof(b));
}

std::size_t record_append_step_hash(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                                    const StepHashRecord &r) noexcept {
    std::uint8_t b[REC_STEP_HASH_LEN];
    put_u32(b, r.step);
    put_u32(b + 4, r.hash);
    return record_append(payload, cap, used, REC_STEP_HASH, b, sizeof(b));
}

std::size_t record_append_telemetry(std::uint8_t *payload, std::size_t cap, std::size_t *used,
                                    const TelemetryRecord &r) noexcept {
    std::uint8_t b[REC_TELEMETRY_LEN];
    put_u32(b, r.srtt_us);
    put_u32(b + 4, r.rttvar_us);
    put_u32(b + 8, r.loss_q16);
    put_u32(b + 12, r.last_step);
    return record_append(payload, cap, used, REC_TELEMETRY, b, sizeof(b));
}

bool records_absorb(const std::uint8_t *payload, std::size_t len, LatestSlots &slots,
                    Verdict &why) noexcept {
    std::size_t off = 0;
    while (off != len) {
        if (off + 2 > len) {
            why = Verdict::Malformed;
            return false;
        }
        const std::uint8_t id   = payload[off];
        const std::uint8_t rlen = payload[off + 1];
        if (off + 2 + rlen > len) {
            why = Verdict::Malformed;
            return false;
        }
        // An unknown id is SKIPPED, not an error -- that is what makes the channel extensible
        // without a flag day. The length byte is carried for exactly this walk.
        if (id >= 1 && id <= REC_ID_MAX && rlen <= REC_LEN_MAX) {
            slots.present[id] = true;
            slots.len[id]     = rlen;
            if (rlen) std::memcpy(slots.bytes[id], payload + off + 2, rlen);
        }
        off += 2 + rlen;
    }
    why = Verdict::Ok;
    return true;
}

bool ping_from(const LatestSlots &s, std::uint8_t id, PingRecord &out) noexcept {
    if (id != REC_PING && id != REC_PONG) return false;
    if (!s.present[id] || s.len[id] != REC_PING_LEN) return false;
    out.t_origin_ms = get_u32(s.bytes[id]);
    out.t_echo_ms   = get_u32(s.bytes[id] + 4);
    return true;
}

bool step_hash_from(const LatestSlots &s, StepHashRecord &out) noexcept {
    if (!s.present[REC_STEP_HASH] || s.len[REC_STEP_HASH] != REC_STEP_HASH_LEN) return false;
    out.step = get_u32(s.bytes[REC_STEP_HASH]);
    out.hash = get_u32(s.bytes[REC_STEP_HASH] + 4);
    return true;
}

bool telemetry_from(const LatestSlots &s, TelemetryRecord &out) noexcept {
    if (!s.present[REC_TELEMETRY] || s.len[REC_TELEMETRY] != REC_TELEMETRY_LEN) return false;
    const std::uint8_t *b = s.bytes[REC_TELEMETRY];
    out.srtt_us           = get_u32(b);
    out.rttvar_us         = get_u32(b + 4);
    out.loss_q16          = get_u32(b + 8);
    out.last_step         = get_u32(b + 12);
    return true;
}

// ---- channel C: bulk reliable -----------------------------------------------------------------------

std::uint16_t piece_count(std::uint32_t chunk_len, std::size_t piece_max) noexcept {
    if (chunk_len == 0 || chunk_len > CHUNK_MAX) return 0;
    if (piece_max == 0 || piece_max > PIECE_MAX) return 0;
    return (std::uint16_t)((chunk_len + piece_max - 1) / piece_max);
}

std::size_t piece_encode(const Piece &p, std::uint8_t *out, std::size_t cap) noexcept {
    if (p.len > PIECE_MAX) return 0;
    if (p.chunk_len == 0 || p.chunk_len > CHUNK_MAX) return 0;
    if (p.total == 0 || p.total > PIECE_TOTAL_MAX || p.index >= p.total) return 0;
    if (PIECE_HDR + p.len > cap) return 0;
    out[0] = BULK_PIECE;
    put_u32(out + 1, p.chunk_id);
    put_u16(out + 5, p.index);
    put_u16(out + 7, p.total);
    put_u32(out + 9, p.chunk_len);
    std::memcpy(out + 13, p.chunk_sha, SHA256_LEN);
    put_u32(out + 45, p.piece_seq);
    put_u16(out + 49, (std::uint16_t)p.len);
    if (p.len) std::memcpy(out + PIECE_HDR, p.bytes, p.len);
    return PIECE_HDR + p.len;
}

bool piece_decode(const std::uint8_t *payload, std::size_t len, Piece &out, Verdict &why,
                  std::size_t piece_max) noexcept {
    why = Verdict::Malformed;
    if (len < PIECE_HDR || payload[0] != BULK_PIECE) return false;
    out.chunk_id  = get_u32(payload + 1);
    out.index     = get_u16(payload + 5);
    out.total     = get_u16(payload + 7);
    out.chunk_len = get_u32(payload + 9);
    std::memcpy(out.chunk_sha, payload + 13, SHA256_LEN);
    out.piece_seq            = get_u32(payload + 45);
    const std::uint16_t plen = get_u16(payload + 49);
    if (piece_max == 0 || piece_max > PIECE_MAX) return false;
    if (plen > piece_max) return false;
    if (PIECE_HDR + plen != len) return false; // the frame must be exactly this piece
    if (out.chunk_len == 0 || out.chunk_len > CHUNK_MAX) return false;
    if (out.total == 0 || out.total > PIECE_TOTAL_MAX || out.index >= out.total) return false;
    if (out.total != piece_count(out.chunk_len, piece_max)) return false;
    out.bytes = payload + PIECE_HDR;
    out.len   = plen;
    why       = Verdict::Ok;
    return true;
}

std::size_t ack_encode(const Ack &a, std::uint8_t *out, std::size_t cap) noexcept {
    if (cap < ACK_LEN) return 0;
    out[0] = BULK_ACK;
    put_u32(out + 1, a.ack_seq);
    put_u32(out + 5, a.ack_bits);
    return ACK_LEN;
}

bool ack_decode(const std::uint8_t *payload, std::size_t len, Ack &out) noexcept {
    if (len != ACK_LEN || payload[0] != BULK_ACK) return false;
    out.ack_seq  = get_u32(payload + 1);
    out.ack_bits = get_u32(payload + 5);
    return true;
}

bool piece_for(const std::uint8_t *chunk, std::uint32_t chunk_len, std::uint32_t chunk_id,
               std::uint16_t index, std::uint32_t piece_seq, Piece &out,
               const std::uint8_t sha[SHA256_LEN], std::size_t piece_max) noexcept {
    const std::uint16_t total = piece_count(chunk_len, piece_max);
    if (total == 0 || index >= total) return false;
    const std::uint32_t start = (std::uint32_t)index * (std::uint32_t)piece_max;
    std::uint32_t       take  = chunk_len - start;
    if (take > piece_max) take = (std::uint32_t)piece_max;
    out.chunk_id  = chunk_id;
    out.index     = index;
    out.total     = total;
    out.chunk_len = chunk_len;
    std::memcpy(out.chunk_sha, sha, SHA256_LEN);
    out.piece_seq = piece_seq;
    out.bytes     = chunk + start;
    out.len       = take;
    return true;
}

// ---- connect token ----------------------------------------------------------------------------------

void token_keys(const std::uint8_t host_key[KEY_LEN], std::uint8_t enc_out[KEY_LEN],
                std::uint8_t mac_out[KEY_LEN]) noexcept {
    hmac_sha256(host_key, KEY_LEN, (const std::uint8_t *)TOKEN_LBL_ENC, std::strlen(TOKEN_LBL_ENC),
                enc_out);
    hmac_sha256(host_key, KEY_LEN, (const std::uint8_t *)TOKEN_LBL_MAC, std::strlen(TOKEN_LBL_MAC),
                mac_out);
}

std::size_t token_seal(const std::uint8_t host_key[KEY_LEN], std::uint64_t nonce,
                       const ConnectToken &tok, std::uint8_t *out) noexcept {
    if (tok.slot > TOKEN_SLOT_MAX) return 0;
    std::uint8_t enc[KEY_LEN], mac[KEY_LEN];
    token_keys(host_key, enc, mac);

    std::uint8_t priv[TOKEN_PRIVATE];
    std::memset(priv, 0, sizeof(priv));
    std::memcpy(priv, tok.match_id, UUID7_BYTES);
    std::memcpy(priv + 16, tok.conn_id, CONN_ID_BYTES);
    priv[24] = tok.slot;
    // priv[25..31] reserved, zero -- token_open requires them zero, so the field cannot quietly
    // become a place to smuggle bytes past a verifier written before it meant anything.
    put_u64(priv + 32, tok.expire_unix_ms);
    std::memcpy(priv + 40, tok.keys.enc_c2s, KEY_LEN);
    std::memcpy(priv + 72, tok.keys.enc_s2c, KEY_LEN);
    std::memcpy(priv + 104, tok.keys.mac_c2s, KEY_LEN);
    std::memcpy(priv + 136, tok.keys.mac_s2c, KEY_LEN);

    out[0] = MAGIC_VER;
    out[1] = TOKEN_KIND;
    put_u64(out + 2, nonce);
    std::memcpy(out + 10, tok.conn_id, CONN_ID_BYTES);
    put_u64(out + 18, tok.expire_unix_ms);
    chacha20_xor(enc, nonce, priv, out + 26, TOKEN_PRIVATE);
    std::uint8_t full[SHA256_LEN];
    hmac_sha256(mac, KEY_LEN, out, 26 + TOKEN_PRIVATE, full);
    std::memcpy(out + 26 + TOKEN_PRIVATE, full, MAC_LEN);
    return TOKEN_WIRE;
}

Verdict token_open(const std::uint8_t host_key[KEY_LEN], const std::uint8_t *in, std::size_t len,
                   std::uint64_t now_unix_ms, ConnectToken &out, bool &expired) noexcept {
    expired = false;
    if (len != TOKEN_WIRE) return Verdict::TooShort;
    if ((in[0] >> 4) != MAGIC_NIBBLE) return Verdict::BadMagic;
    if ((in[0] & 0x0f) != VERSION) return Verdict::BadVersion;
    if (in[1] != TOKEN_KIND) return Verdict::BadType;

    std::uint8_t enc[KEY_LEN], mac[KEY_LEN];
    token_keys(host_key, enc, mac);
    std::uint8_t full[SHA256_LEN];
    hmac_sha256(mac, KEY_LEN, in, 26 + TOKEN_PRIVATE, full);
    if (!ct_equal(full, in + 26 + TOKEN_PRIVATE, MAC_LEN)) return Verdict::BadMac;

    const std::uint64_t nonce = get_u64(in + 2);
    std::uint8_t        priv[TOKEN_PRIVATE];
    chacha20_xor(enc, nonce, in + 26, priv, TOKEN_PRIVATE);

    std::memcpy(out.match_id, priv, UUID7_BYTES);
    std::memcpy(out.conn_id, priv + 16, CONN_ID_BYTES);
    out.slot = priv[24];
    for (int i = 25; i < 32; ++i)
        if (priv[i] != 0) return Verdict::Malformed;
    out.expire_unix_ms = get_u64(priv + 32);
    std::memcpy(out.keys.enc_c2s, priv + 40, KEY_LEN);
    std::memcpy(out.keys.enc_s2c, priv + 72, KEY_LEN);
    std::memcpy(out.keys.mac_c2s, priv + 104, KEY_LEN);
    std::memcpy(out.keys.mac_s2c, priv + 136, KEY_LEN);

    // The routing copies must equal the sealed ones (net_udp.h): a middlebox that rewrote either is
    // caught here rather than being believed by the relay and not by the host.
    if (std::memcmp(out.conn_id, in + 10, CONN_ID_BYTES) != 0) return Verdict::Malformed;
    if (out.expire_unix_ms != get_u64(in + 18)) return Verdict::Malformed;
    if (out.slot > TOKEN_SLOT_MAX) return Verdict::Malformed;

    if (now_unix_ms != 0 && now_unix_ms > out.expire_unix_ms) {
        expired = true;
        return Verdict::BadMac; // deliberately indistinguishable on the wire -- see net_udp.h
    }
    return Verdict::Ok;
}

} // namespace udp
} // namespace mh_net_proto
