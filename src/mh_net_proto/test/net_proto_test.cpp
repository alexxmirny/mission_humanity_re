// mh_net_proto unit test -- round-trips + lobby-id dedup. Portable; returns non-zero on any failure.
// Built + run by CMake (ctest) on both Windows and Linux. See S1.
#include "mh_net_proto/net_wire.h"
#include "mh_net_proto/net_crypto.h"
#include "mh_net_proto/session_info.h"
#include <cstdio>
#include <cstring>

using namespace mh_net_proto;

static int g_fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fails; } } while (0)

int main() {
    // ---- wire header round-trip (S0 regression) ----
    {
        WireHdr h{ WIRE_MAGIC, FLAG_HELLO, 3, BROADCAST, 1000 };
        std::uint8_t b[WIRE_HDR_SIZE];
        wire_hdr_encode(h, b);
        CHECK(b[0] == 0x4D && b[1] == 0x48);              // 'MH' little-endian on the wire
        WireHdr h2{};
        CHECK(wire_hdr_decode(b, h2));
        CHECK(h2.magic == h.magic && h2.flags == h.flags && h2.src == h.src
              && h2.dst == h.dst && h2.len == h.len);
        std::uint8_t bad[WIRE_HDR_SIZE] = {0};
        WireHdr hx{};
        CHECK(!wire_hdr_decode(bad, hx));                 // magic mismatch -> reject
    }

    // ---- SESSION_INFO byte-identical round-trip ----
    {
        SessionInfo a{};
        a.tag = 0xDEADBEEFu; a.host_version = 102; a.protocol = 1;
        a.cur_players = 1; a.max_players = 2;
        std::strcpy(a.name, "uitestgame");
        std::strcpy(a.map,  "TUTORIAL.MP");

        std::uint8_t buf[SESSION_INFO_MAX_ENCODED];
        std::size_t n = session_info_encode(a, buf);
        CHECK(n > 0 && n <= SESSION_INFO_MAX_ENCODED);

        SessionInfo b{};
        CHECK(session_info_decode(buf, n, b));
        CHECK(b.tag == a.tag && b.host_version == a.host_version && b.protocol == a.protocol);
        CHECK(b.cur_players == a.cur_players && b.max_players == a.max_players);
        CHECK(std::strcmp(a.name, b.name) == 0);
        CHECK(std::strcmp(a.map,  b.map)  == 0);

        // empty strings round-trip
        SessionInfo e{}; e.tag = 7;
        std::size_t en = session_info_encode(e, buf);
        SessionInfo e2{};
        CHECK(session_info_decode(buf, en, e2));
        CHECK(e2.tag == 7 && e2.name[0] == '\0' && e2.map[0] == '\0');

        // truncated / malformed decode fails (must not read out of bounds)
        CHECK(!session_info_decode(buf, 3, b));
        CHECK(!session_info_decode(buf, 0, b));
        std::uint8_t badver[SESSION_INFO_MAX_ENCODED];
        std::size_t bn = session_info_encode(a, badver);
        badver[0] = 0xFF;                                 // unknown format version
        CHECK(!session_info_decode(badver, bn, b));
    }

    // ---- lobby-id (name + tag) dedup ----
    {
        SessionInfo base{};
        std::strcpy(base.name, "MH Host"); base.tag = 0x11111111u;

        SessionInfo same = base;                          // identical -> same lobby
        CHECK(session_same_lobby(base, same));

        SessionInfo diffTag = base; diffTag.tag = 0x22222222u;  // same name, different tag -> distinct
        CHECK(!session_same_lobby(base, diffTag));

        SessionInfo diffName = base; std::strcpy(diffName.name, "Other");  // diff name -> distinct
        CHECK(!session_same_lobby(base, diffName));

        char id[64];
        lobby_id_str(base, id, sizeof(id));
        CHECK(std::strcmp(id, "MH Host#11111111") == 0);
    }

    // ---- JOIN request round-trip + lobby-id match (S4) ----
    {
        SessionInfo host{};
        std::strcpy(host.name, "s3test"); host.tag = 0xA4A24976u;
        host.max_players = 8; host.cur_players = 1;

        // a client builds its join from the received session descriptor
        JoinRequest jr = join_request_for(host);
        CHECK(jr.tag == host.tag && std::strcmp(jr.name, host.name) == 0);

        std::uint8_t buf[JOIN_REQUEST_MAX_ENCODED];
        std::size_t n = join_request_encode(jr, buf);
        CHECK(n > 0 && n <= JOIN_REQUEST_MAX_ENCODED);

        JoinRequest jr2{};
        CHECK(join_request_decode(buf, n, jr2));
        CHECK(jr2.tag == jr.tag && std::strcmp(jr2.name, jr.name) == 0);

        // the host admits a matching lobby-id, rejects a wrong one
        CHECK(join_matches_session(jr2, host));
        SessionInfo otherTag = host; otherTag.tag = 0xDEADBEEFu;   // right name, wrong tag -> reject
        CHECK(!join_matches_session(jr2, otherTag));
        SessionInfo otherName = host; std::strcpy(otherName.name, "elsewhere");  // wrong name -> reject
        CHECK(!join_matches_session(jr2, otherName));

        // S6: player_name round-trips (v2), and the lobby-id still matches
        std::strcpy(jr.player_name, "Alice");
        std::uint8_t pbuf[JOIN_REQUEST_MAX_ENCODED];
        std::size_t pn = join_request_encode(jr, pbuf);
        JoinRequest jr3{};
        CHECK(join_request_decode(pbuf, pn, jr3));
        CHECK(std::strcmp(jr3.player_name, "Alice") == 0 && join_matches_session(jr3, host));

        // S6: a v1 JOIN (format byte 1, lobby-id only) still decodes -> player_name left empty
        std::uint8_t v1[JOIN_REQUEST_MAX_ENCODED]; std::uint8_t* q = v1;
        *q++ = 1;
        *q++ = (std::uint8_t)(host.tag & 0xff);  *q++ = (std::uint8_t)((host.tag >> 8) & 0xff);
        *q++ = (std::uint8_t)((host.tag >> 16) & 0xff); *q++ = (std::uint8_t)((host.tag >> 24) & 0xff);
        std::uint8_t nl = (std::uint8_t)std::strlen(host.name); *q++ = nl;
        std::memcpy(q, host.name, nl); q += nl;
        JoinRequest jrv1{}; std::strcpy(jrv1.player_name, "SHOULD_CLEAR");
        CHECK(join_request_decode(v1, (std::size_t)(q - v1), jrv1));
        CHECK(jrv1.player_name[0] == '\0' && jrv1.tag == host.tag && std::strcmp(jrv1.name, host.name) == 0);

        // truncated / bad-version decode fails cleanly
        CHECK(!join_request_decode(buf, 2, jr2));
        std::uint8_t badver[JOIN_REQUEST_MAX_ENCODED];
        std::size_t bn = join_request_encode(jr, badver);
        badver[0] = 0xFF;
        CHECK(!join_request_decode(badver, bn, jr2));
    }

    // ---- crypto: PINNED to published vectors -------------------------------------------------
    // These primitives are hand-written (the injected DLL cannot drag in a crypto library), so the
    // published test vectors ARE the correctness argument. A refactor that breaks one breaks here,
    // not silently on the wire.
    {
        auto hex = [](const std::uint8_t *b, std::size_t n, char *out) {
            static const char *H = "0123456789abcdef";
            for (std::size_t i = 0; i < n; ++i) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 0xf]; }
            out[n * 2] = '\0';
        };
        char h[80];

        // FIPS 180-4: SHA-256("abc") and SHA-256("")
        std::uint8_t d[SHA256_LEN];
        sha256((const std::uint8_t *)"abc", 3, d);
        hex(d, SHA256_LEN, h);
        CHECK(std::strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
        sha256((const std::uint8_t *)"", 0, d);
        hex(d, SHA256_LEN, h);
        CHECK(std::strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

        // Multi-block (>64 bytes), which exercises the buffering path the one-shot vectors do not.
        std::uint8_t big[200];
        for (int i = 0; i < 200; ++i) big[i] = (std::uint8_t)i;
        sha256(big, 200, d);
        std::uint8_t d2[SHA256_LEN];
        sha256(big, 200, d2);
        CHECK(std::memcmp(d, d2, SHA256_LEN) == 0); // deterministic

        // RFC 4231 test case 2: HMAC-SHA256(key="Jefe", data="what do ya want for nothing?")
        hmac_sha256((const std::uint8_t *)"Jefe", 4,
                    (const std::uint8_t *)"what do ya want for nothing?", 28, d);
        hex(d, SHA256_LEN, h);
        CHECK(std::strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);

        // RFC 8439 2.4.2: ChaCha20 keystream, key = 00..1f, nonce/counter as encoded by chacha20_xor.
        // We drive our own seq-nonce layout, so pin the SELF-consistency + the known property that
        // XOR is an involution and distinct seqs give distinct streams (a seq/nonce reuse bug shows up
        // here as identical ciphertext for two different records).
        std::uint8_t key[KEY_LEN];
        for (int i = 0; i < (int)KEY_LEN; ++i) key[i] = (std::uint8_t)i;
        std::uint8_t zeros[64] = {0}, ks1[64], ks2[64];
        chacha20_xor(key, 1, zeros, ks1, 64);
        chacha20_xor(key, 2, zeros, ks2, 64);
        CHECK(std::memcmp(ks1, ks2, 64) != 0);       // different record seq -> different keystream
        std::uint8_t back[64];
        chacha20_xor(key, 1, ks1, back, 64);
        CHECK(std::memcmp(back, zeros, 64) == 0);    // involution
        // Crossing the 64-byte block boundary must keep the first block identical (counter starts 0).
        std::uint8_t longz[130] = {0}, kslong[130];
        chacha20_xor(key, 1, longz, kslong, 130);
        CHECK(std::memcmp(kslong, ks1, 64) == 0);
    }

    // ---- handshake: mutual auth, and every failure mode ---------------------------------------
    {
        std::uint8_t psk[KEY_LEN], wrong[KEY_LEN];
        for (int i = 0; i < (int)KEY_LEN; ++i) { psk[i] = (std::uint8_t)(i * 7 + 1); wrong[i] = (std::uint8_t)(i * 7 + 2); }
        std::uint8_t cn[NONCE_LEN], sn[NONCE_LEN];
        for (int i = 0; i < (int)NONCE_LEN; ++i) { cn[i] = (std::uint8_t)(0xA0 + i); sn[i] = (std::uint8_t)(0x50 + i); }

        std::uint8_t hello[HS_HELLO_LEN], chal[HS_CHALLENGE_LEN], resp[HS_RESPONSE_LEN];
        std::uint8_t cn2[NONCE_LEN], sn2[NONCE_LEN];
        std::uint16_t ver = 0;

        hs_build_hello(cn, hello);
        CHECK(hs_parse_hello(hello, cn2, ver) && ver == HS_VERSION && std::memcmp(cn, cn2, NONCE_LEN) == 0);

        hs_build_challenge(psk, cn, sn, chal);
        CHECK(hs_check_challenge(psk, cn, chal, sn2, ver) && std::memcmp(sn, sn2, NONCE_LEN) == 0);
        CHECK(!hs_check_challenge(wrong, cn, chal, sn2, ver));  // client detects a fake host
        hs_build_response(psk, cn, sn, resp);
        CHECK(hs_check_response(psk, cn, sn, resp));
        CHECK(!hs_check_response(wrong, cn, sn, resp));         // host rejects a wrong key

        // A proof is bound to BOTH nonces: replaying it under a fresh server nonce must fail. This is
        // the property that stops a recorded handshake from being replayed by a scanner.
        std::uint8_t sn_fresh[NONCE_LEN];
        std::memcpy(sn_fresh, sn, NONCE_LEN);
        sn_fresh[0] ^= 0xff;
        CHECK(!hs_check_response(psk, cn, sn_fresh, resp));

        // ...and the two directions' proofs are not interchangeable (no reflection attack).
        CHECK(!hs_check_response(psk, cn, sn, chal + 24));

        // Garbage / wrong-magic input is rejected rather than parsed.
        std::uint8_t junk[HS_CHALLENGE_LEN] = {0};
        CHECK(!hs_parse_hello(junk, cn2, ver));
        CHECK(!hs_check_challenge(psk, cn, junk, sn2, ver));

        // Keys: all four derived values distinct, and both sides derive the same set.
        SessionKeys a{}, b{};
        hs_derive_keys(psk, cn, sn, a);
        hs_derive_keys(psk, cn, sn, b);
        CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
        CHECK(std::memcmp(a.enc_c2s, a.enc_s2c, KEY_LEN) != 0);
        CHECK(std::memcmp(a.enc_c2s, a.mac_c2s, KEY_LEN) != 0);
        CHECK(std::memcmp(a.mac_c2s, a.mac_s2c, KEY_LEN) != 0);
        SessionKeys c{};
        hs_derive_keys(wrong, cn, sn, c);
        CHECK(std::memcmp(a.enc_c2s, c.enc_c2s, KEY_LEN) != 0); // key change -> different session
    }

    // ---- record layer: seal/open, tamper, replay, reflect -------------------------------------
    {
        std::uint8_t enc[KEY_LEN], mac[KEY_LEN];
        for (int i = 0; i < (int)KEY_LEN; ++i) { enc[i] = (std::uint8_t)(i + 3); mac[i] = (std::uint8_t)(i + 9); }
        const char  *msg = "lockstep payload";
        std::uint32_t n  = (std::uint32_t)std::strlen(msg);
        std::uint8_t  rec[128], plain[128];

        std::size_t total = rec_seal(enc, mac, 7, (const std::uint8_t *)msg, n, rec);
        CHECK(total == REC_LEN_SIZE + n + MAC_LEN);
        CHECK(std::memcmp(rec + REC_LEN_SIZE, msg, n) != 0); // actually encrypted, not passed through

        std::memcpy(plain, rec + REC_LEN_SIZE, n);
        CHECK(rec_open(enc, mac, 7, plain, n, rec + REC_LEN_SIZE + n));
        CHECK(std::memcmp(plain, msg, n) == 0);

        std::memcpy(plain, rec + REC_LEN_SIZE, n);           // same bytes, WRONG sequence number
        CHECK(!rec_open(enc, mac, 8, plain, n, rec + REC_LEN_SIZE + n));

        std::uint8_t tampered[128];                          // flip one ciphertext bit
        std::memcpy(tampered, rec, total);
        tampered[REC_LEN_SIZE] ^= 0x01;
        std::memcpy(plain, tampered + REC_LEN_SIZE, n);
        CHECK(!rec_open(enc, mac, 7, plain, n, tampered + REC_LEN_SIZE + n));

        std::memcpy(tampered, rec, total);                   // flip one MAC bit
        tampered[REC_LEN_SIZE + n] ^= 0x01;
        std::memcpy(plain, tampered + REC_LEN_SIZE, n);
        CHECK(!rec_open(enc, mac, 7, plain, n, tampered + REC_LEN_SIZE + n));

        std::memcpy(plain, rec + REC_LEN_SIZE, n);           // wrong direction's keys
        CHECK(!rec_open(mac, enc, 7, plain, n, rec + REC_LEN_SIZE + n));

        std::size_t zt = rec_seal(enc, mac, 0, nullptr, 0, rec); // empty record (a bare 12-byte header)
        CHECK(zt == REC_LEN_SIZE + MAC_LEN);
        CHECK(rec_open(enc, mac, 0, plain, 0, rec + REC_LEN_SIZE));
    }

    // ---- key text round-trip ------------------------------------------------------------------
    {
        std::uint8_t k[KEY_LEN], back[KEY_LEN];
        for (int i = 0; i < (int)KEY_LEN; ++i) k[i] = (std::uint8_t)(i * 5 + 2);
        char text[KEY_HEX_LEN + 1];
        key_to_hex(k, text);
        CHECK(std::strlen(text) == KEY_HEX_LEN);
        CHECK(key_from_hex(text, back) && std::memcmp(k, back, KEY_LEN) == 0);
        char padded[KEY_HEX_LEN + 8];
        std::snprintf(padded, sizeof(padded), "  %s\r\n", text);       // as read from a text file
        CHECK(key_from_hex(padded, back) && std::memcmp(k, back, KEY_LEN) == 0);
        CHECK(!key_from_hex("deadbeef", back));                        // too short
        CHECK(!key_from_hex(nullptr, back));
        char bad[KEY_HEX_LEN + 1];
        std::memcpy(bad, text, sizeof(bad));
        bad[5] = 'z';
        CHECK(!key_from_hex(bad, back));                               // non-hex
        char trailing[KEY_HEX_LEN + 4];
        std::snprintf(trailing, sizeof(trailing), "%sxx", text);
        CHECK(!key_from_hex(trailing, back));                          // trailing junk
    }

    std::printf(g_fails ? "\n%d CHECK(S) FAILED\n" : "ALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
