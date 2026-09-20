//
// udp_wire_selftest.cpp -- `net_selftest.exe udpwiretest`: the UDP packet format (mp:T0, plan D2).
//
// WHY THIS SUITE EXISTS, AND WHY IT READS FILES. Everything under test is a pure function over
// bytes, so an in-process round-trip is cheap and proves nothing interesting on its own: an encoder
// agreeing with its own decoder is one implementation agreeing with itself, and both would agree
// just as happily on a format the Rust relay cannot read. T0's whole point is that TWO independent
// implementations -- this one and `cargo test -p relay` -- accept and refuse the SAME committed
// bytes. So the centre of this file is the FIXTURE arm: it loads
// src/mh_net_proto/test/fixtures/udp/, decodes each case and asserts the recorded verdict.
//
// THREE ARMS, IN INCREASING DISTANCE FROM THE FIXTURES:
//   (1) fixtures   -- the committed cases, their verdicts and their decoded contents.
//   (2) properties -- round-trips and refusals the fixtures cannot express because they are about
//                     RANGES (every K from 1 to 8, every replay-window position, a chunk fragmented
//                     at each boundary). A fixture per value would be a directory nobody reads.
//   (3) fuzz       -- `udpwiretest --fuzz <seconds>`: seeded mutation of the fixture packets fed to
//                     the decoder, run under the Release_asan build. Its claim is narrow and worth
//                     stating: NOT that the decoder gets the right answer on garbage, but that it
//                     never reads or writes outside its buffer while deciding. That is the property
//                     a hand-written bounds test cannot cover and ASan reports precisely.
//
// AND `--emit <dir>`: regenerate the fixtures from this encoder. The committed files are BUILT, not
// hand-typed, and the Rust side never writes them -- if the format changes, the C++ encoder emits
// and the Rust decoder must then still agree, which is the direction that catches a drift.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mh_net_proto/net_udp.h"
#include "mh_net_proto/uuid7.h"
// mp:R1c -- the relay leg's key derivation. HEADER ONLY: udp_relay.cpp owns sockets and a thread
// and is not linked here, which is precisely why the derivation was put in the header.
#include "../mh_net_udp/udp_relay.h"

using namespace mh_net_proto;
using namespace mh_net_proto::udp;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// ---- tiny hex + the restricted JSON the index uses -------------------------------------------
//
// THE INDEX IS A JSON ARRAY OF FLAT OBJECTS WHOSE VALUES ARE ALL STRINGS -- numbers included,
// written in decimal. That is a deliberate narrowing, for one reason: this side has no JSON library
// and the Rust side has serde_json. A grammar this small is ~60 lines here and still a real JSON
// document there, so the asymmetry costs nothing and the FILE stays something a person can read.
// Every deviation (a bare number, a nested object) is REFUSED rather than best-effort parsed --
// an index this file misreads is exactly the failure a shared fixture set exists to prevent.

void hex_encode(const unsigned char *b, size_t n, char *out) {
    static const char *D = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = D[b[i] >> 4];
        out[i * 2 + 1] = D[b[i] & 0xf];
    }
    out[n * 2] = 0;
}

int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *s, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const int hi = hex_nib(s[i * 2]), lo = hex_nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return s[n * 2] == 0;
}

struct Kv {
    char key[32];
    char val[160];
};
struct Case {
    Kv          kv[24];
    int         n = 0;
    const char *get(const char *k) const {
        for (int i = 0; i < n; ++i)
            if (strcmp(kv[i].key, k) == 0) return kv[i].val;
        return NULL;
    }
    void put(const char *k, const char *v) {
        if (n >= (int)(sizeof(kv) / sizeof(kv[0]))) return;
        snprintf(kv[n].key, sizeof(kv[n].key), "%s", k);
        snprintf(kv[n].val, sizeof(kv[n].val), "%s", v);
        ++n;
    }
    void put_u(const char *k, unsigned long long v) {
        char b[32];
        snprintf(b, sizeof(b), "%llu", v);
        put(k, b);
    }
};

unsigned long long get_u(const Case &c, const char *k, unsigned long long dflt) {
    const char *s = c.get(k);
    return s ? strtoull(s, NULL, 10) : dflt;
}

// A JSON string body (no escapes -- the fixture keys and values are ASCII identifiers, hex and
// decimal by construction, and the writer refuses anything else).
bool json_string(const char *&p, char *out, size_t cap) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') return false; // escapes are outside the grammar, so a file using one is refused
        if (i + 1 >= cap) return false;
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    ++p;
    out[i] = 0;
    return true;
}

bool json_skip(const char *&p, char want) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    if (*p != want) return false;
    ++p;
    return true;
}

// Parse the whole index. Returns the case count, or -1 on any grammar deviation.
int parse_index(const char *text, Case *out, int cap) {
    const char *p = text;
    if (!json_skip(p, '[')) return -1;
    int n = 0;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
        if (*p == ']') return n;
        if (n >= cap) return -1;
        if (!json_skip(p, '{')) return -1;
        Case &c = out[n];
        c.n     = 0;
        for (;;) {
            char k[32], v[160];
            if (!json_string(p, k, sizeof(k))) return -1;
            if (!json_skip(p, ':')) return -1;
            if (!json_string(p, v, sizeof(v))) return -1;
            c.put(k, v);
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == '}') {
                ++p;
                break;
            }
            return -1;
        }
        ++n;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') return n;
        return -1;
    }
    return -1;
}

// ---- file helpers -------------------------------------------------------------------------------

size_t read_file(const char *path, unsigned char *out, size_t cap, bool &ok) {
    ok      = false;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n    = fread(out, 1, cap, f);
    const bool   more = fgetc(f) != EOF; // a file bigger than the buffer must not read as truncated
    fclose(f);
    if (more) return 0;
    ok = true;
    return n;
}

bool write_file(const char *path, const unsigned char *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = (n == 0) || (fwrite(b, 1, n, f) == n);
    fclose(f);
    return ok;
}

// WHERE THE FIXTURES ARE. The gate runs the exe with cwd = the repo root (tools/run_selftests.py),
// but the exe itself is staged into %TEMP%, so an exe-relative path would be wrong and a bare
// relative path is right only by the driver's convention. Both are therefore tried, plus an
// explicit override -- and the suite FAILS LOUDLY when it finds nothing rather than reporting zero
// fixture checks, which would be a green run that tested nothing.
const char *fixture_dir(char *buf, size_t cap) {
    if (const char *env = getenv("MH_UDP_FIXTURES")) {
        snprintf(buf, cap, "%s", env);
        return buf;
    }
    const char *suffix = "src/mh_net_proto/test/fixtures/udp";
    char        probe[512];
    char        prefix[256];
    prefix[0] = 0;
    for (int up = 0; up < 6; ++up) {
        snprintf(probe, sizeof(probe), "%s%s/index.json", prefix, suffix);
        FILE *f = fopen(probe, "rb");
        if (f) {
            fclose(f);
            snprintf(buf, cap, "%s%s", prefix, suffix);
            return buf;
        }
        strncat(prefix, "../", sizeof(prefix) - strlen(prefix) - 1);
    }
    return NULL;
}

// ---- the fixture material ------------------------------------------------------------------------
// Fixed vectors, so the emitted bytes are a function of this file alone. Nothing here comes from a
// clock or an RNG: a fixture whose contents move is not a fixture.

const unsigned char ENC_KEY[KEY_LEN]  = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                                         0xcc, 0xdd, 0xee, 0xff, 0x00, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
                                         0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
const unsigned char MAC_KEY[KEY_LEN]  = {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69, 0x5a,
                                         0x4b, 0x3c, 0x2d, 0x1e, 0x0f, 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb,
                                         0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
const unsigned char HOST_KEY[KEY_LEN] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
                                         0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5,
                                         0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf};

// The match_id every fixture belongs to: a UUIDv7 minted from fixed inputs, exactly as
// session_id_selftest.cpp does, so the conn_id below is derivable by hand from this file.
void fixture_match_id(unsigned char out[UUID7_BYTES]) {
    unsigned char r[UUID7_RAND_MAX];
    for (int i = 0; i < (int)sizeof(r); ++i) r[i] = (unsigned char)(0xa0 + i);
    uuid7_make(1789646400000ULL, r, out);
}
void fixture_conn_id(unsigned char out[CONN_ID_BYTES], unsigned char slot) {
    unsigned char mid[UUID7_BYTES];
    fixture_match_id(mid);
    conn_id_from_match(mid, slot, out);
}

// The step-input payload every input fixture carries: three steps, newest first, 4 bytes each.
size_t build_input_payload(unsigned char *out, size_t cap, unsigned int newest, unsigned char k) {
    static unsigned char bytes[INPUT_K_MAX][8];
    InputEntry           e[INPUT_K_MAX];
    for (unsigned char i = 0; i < k; ++i) {
        for (int j = 0; j < 4; ++j) bytes[i][j] = (unsigned char)(0x40 + i * 4 + j);
        e[i].step  = newest - i;
        e[i].bytes = bytes[i];
        e[i].len   = 4;
    }
    return input_encode(e, k, out, cap);
}

size_t build_records_payload(unsigned char *out, size_t cap) {
    size_t     used = 0;
    PingRecord ping = {0x11223344u, 0x55667788u};
    record_append_ping(out, cap, &used, REC_PING, ping);
    StepHashRecord sh = {4242u, 0xdeadbeefu};
    record_append_step_hash(out, cap, &used, sh);
    TelemetryRecord tm = {81000u, 12000u, 655u, 4242u};
    record_append_telemetry(out, cap, &used, tm);
    return used;
}

// The bulk chunk the piece fixture is cut from: 2600 deterministic bytes -> 3 pieces.
const unsigned int FIXTURE_CHUNK_LEN = 2600;
void               build_chunk(unsigned char *chunk) {
    for (unsigned int i = 0; i < FIXTURE_CHUNK_LEN; ++i)
        chunk[i] = (unsigned char)((i * 31u + (i >> 5)) & 0xff);
}

// ---- the emitter -----------------------------------------------------------------------------------

struct Emitter {
    char dir[512];
    Case cases[64];
    int  n  = 0;
    bool ok = true;

    void write_bin(const char *name, const unsigned char *b, size_t len) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (!write_file(path, b, len)) {
            printf("  FAIL: cannot write %s\n", path);
            ok = false;
        }
    }

    // One packet case: seal `body`, record the wire bytes and the plaintext body, then apply an
    // optional single-byte mutation (the tamper cases) BEFORE recording -- so the committed file is
    // exactly what the decoder will be handed.
    void packet_case(const char *key, unsigned char type, const unsigned char *conn, unsigned long long seq,
                     const unsigned char *body, size_t body_len, const char *verdict,
                     const char *window, int mutate_off, unsigned char mutate_xor,
                     const unsigned char *declared_conn) {
        unsigned char pkt[MAX_DATAGRAM + 4];
        Verdict       why = Verdict::Ok;
        const size_t  n2  = packet_encode(type, conn, seq, ENC_KEY, MAC_KEY, body, body_len, pkt, why);
        if (n2 == 0) {
            printf("  FAIL: emitter could not seal %s (%s)\n", key, verdict_name(why));
            ok = false;
            return;
        }
        if (mutate_off >= 0 && (size_t)mutate_off < n2) pkt[mutate_off] ^= mutate_xor;

        char wire[64];
        snprintf(wire, sizeof(wire), "%s.bin", key);
        write_bin(wire, pkt, n2);

        Case &c = cases[n++];
        c.n     = 0;
        c.put("kind", "packet");
        c.put("key", key);
        c.put("wire", wire);
        c.put("verdict", verdict);
        c.put("window", window);
        char hexb[KEY_LEN * 2 + 1];
        hex_encode(ENC_KEY, KEY_LEN, hexb);
        c.put("enc_key", hexb);
        hex_encode(MAC_KEY, KEY_LEN, hexb);
        c.put("mac_key", hexb);
        hex_encode(declared_conn ? declared_conn : conn, CONN_ID_BYTES, hexb);
        c.put("conn_id", hexb);
        c.put_u("seq", seq);
        c.put_u("type", type);
        if (strcmp(verdict, "ok") == 0) {
            char plain[64];
            snprintf(plain, sizeof(plain), "%s.plain.bin", key);
            write_bin(plain, body, body_len);
            c.put("plain", plain);
        }
    }

    void token_case(const char *key, const unsigned char *wirebytes, size_t len, const char *verdict,
                    unsigned long long now_ms, const ConnectToken &tok, const char *expired) {
        char wire[64];
        snprintf(wire, sizeof(wire), "%s.bin", key);
        write_bin(wire, wirebytes, len);
        Case &c = cases[n++];
        c.n     = 0;
        c.put("kind", "token");
        c.put("key", key);
        c.put("wire", wire);
        c.put("verdict", verdict);
        char hexb[KEY_LEN * 2 + 1];
        hex_encode(HOST_KEY, KEY_LEN, hexb);
        c.put("host_key", hexb);
        c.put_u("now_ms", now_ms);
        c.put("expired", expired);
        if (strcmp(verdict, "ok") == 0) {
            hex_encode(tok.match_id, UUID7_BYTES, hexb);
            c.put("match_id", hexb);
            hex_encode(tok.conn_id, CONN_ID_BYTES, hexb);
            c.put("conn_id", hexb);
            c.put_u("slot", tok.slot);
            c.put_u("expire_ms", tok.expire_unix_ms);
            hex_encode(tok.keys.enc_c2s, KEY_LEN, hexb);
            c.put("enc_c2s", hexb);
            hex_encode(tok.keys.mac_s2c, KEY_LEN, hexb);
            c.put("mac_s2c", hexb);
        }
    }

    bool write_index() {
        char path[640];
        snprintf(path, sizeof(path), "%s/index.json", dir);
        FILE *f = fopen(path, "wb");
        if (!f) {
            printf("  FAIL: cannot write %s\n", path);
            return false;
        }
        fprintf(f, "[\n");
        for (int i = 0; i < n; ++i) {
            fprintf(f, "  {");
            for (int j = 0; j < cases[i].n; ++j)
                fprintf(f, "%s\"%s\": \"%s\"", j ? ", " : "", cases[i].kv[j].key, cases[i].kv[j].val);
            fprintf(f, "}%s\n", i + 1 < n ? "," : "");
        }
        fprintf(f, "]\n");
        fclose(f);
        return true;
    }
};

// Build the whole fixture set into `em`. The SAME function drives `--emit`; there is no second
// description of what the cases are.
void build_fixtures(Emitter &em) {
    unsigned char conn[CONN_ID_BYTES];
    fixture_conn_id(conn, 1);
    unsigned char body[MAX_BODY];
    unsigned char pay[MAX_BODY];

    // (1) channel A, K = 3. The default redundancy (plan D2).
    {
        const size_t pn   = build_input_payload(pay, sizeof(pay), 1000, INPUT_K_DEFAULT);
        size_t       used = 0;
        frame_append(body, sizeof(body), &used, CH_INPUT, pay, pn);
        em.packet_case("p01_input_k3", PKT_DATA, conn, 1, body, used, "ok", "s1", -1, 0, NULL);
        // (7) the replay: byte-identical, same window, so it must be dropped the second time.
        em.packet_case("p07_replay_of_p01", PKT_DATA, conn, 1, body, used, "replay", "s1", -1, 0, NULL);
        // (9) conn_id rewritten in flight, but the case still DECLARES the real one -- so the
        // conn test fires before the MAC would, which is the ordering the header exists for.
        em.packet_case("p09_wrong_conn", PKT_DATA, conn, 7, body, used, "wrong_conn", "", 2, 0x01, NULL);
        // (10) the tag itself flipped.
        em.packet_case("p10_tamper_tag", PKT_DATA, conn, 8, body, used, "bad_mac", "",
                       (int)(HDR_SIZE + used + 3), 0x80, NULL);
        // (14/15/16) header-byte refusals, each its own verdict.
        em.packet_case("p14_bad_magic", PKT_DATA, conn, 9, body, used, "bad_magic", "", 0, 0x10, NULL);
        em.packet_case("p15_bad_version", PKT_DATA, conn, 10, body, used, "bad_version", "", 0, 0x03, NULL);
        em.packet_case("p16_bad_type", PKT_DATA, conn, 11, body, used, "bad_type", "", 1, 0x09, NULL);
    }
    // (2) channel B records.
    {
        const size_t pn   = build_records_payload(pay, sizeof(pay));
        size_t       used = 0;
        frame_append(body, sizeof(body), &used, CH_STATE, pay, pn);
        em.packet_case("p02_state_records", PKT_DATA, conn, 2, body, used, "ok", "s1", -1, 0, NULL);
        // (8) a tampered BODY byte. The MAC covers the ciphertext, so this is the acceptance
        // clause's "a tampered byte fails the MAC" in its most literal form.
        em.packet_case("p08_tamper_body", PKT_DATA, conn, 12, body, used, "bad_mac", "",
                       (int)HDR_SIZE + 2, 0x01, NULL);
    }
    // (3)(4) channel C: one piece of a 3-piece chunk, and the ack for it.
    {
        static unsigned char chunk[16 * 1024];
        build_chunk(chunk);
        unsigned char sha[SHA256_LEN];
        sha256(chunk, FIXTURE_CHUNK_LEN, sha);
        Piece p;
        piece_for(chunk, FIXTURE_CHUNK_LEN, 0x0bu, 1, 77, p, sha);
        const size_t pn   = piece_encode(p, pay, sizeof(pay));
        size_t       used = 0;
        frame_append(body, sizeof(body), &used, CH_BULK, pay, pn);
        em.packet_case("p03_bulk_piece", PKT_DATA, conn, 3, body, used, "ok", "s1", -1, 0, NULL);

        Ack          a  = {77u, 0x00000003u};
        const size_t an = ack_encode(a, pay, sizeof(pay));
        used            = 0;
        frame_append(body, sizeof(body), &used, CH_BULK, pay, an);
        em.packet_case("p04_bulk_ack", PKT_DATA, conn, 4, body, used, "ok", "s1", -1, 0, NULL);
    }
    // (5) keepalive: a sealed packet with NO body at all. MIN_DATAGRAM is real traffic, not a
    // degenerate case, so it has a fixture.
    em.packet_case("p05_keepalive", PKT_KEEPALIVE, conn, 5, NULL, 0, "ok", "s1", -1, 0, NULL);
    // (6) two channels in one datagram -- the mux, which is the only thing frame framing is for.
    {
        const size_t an   = build_input_payload(pay, sizeof(pay), 1003, 2);
        size_t       used = 0;
        frame_append(body, sizeof(body), &used, CH_INPUT, pay, an);
        const size_t bn = build_records_payload(pay, sizeof(pay));
        frame_append(body, sizeof(body), &used, CH_STATE, pay, bn);
        em.packet_case("p06_mux_a_and_b", PKT_DATA, conn, 6, body, used, "ok", "s1", -1, 0, NULL);
    }
    // (12) the LARGEST legal datagram, exactly MAX_DATAGRAM. Committed next to the 1201-byte
    // refusal so the two together pin the boundary: an off-by-one that refused 1200 would turn this
    // case red, and one that accepted 1201 would turn the other red. Its single frame carries an
    // UNKNOWN channel id, so the case does double duty as the forward-compatibility one: a decoder
    // must skip a channel it does not know and still reach the end of the body cleanly.
    {
        for (size_t i = 0; i < sizeof(pay); ++i) pay[i] = (unsigned char)(i * 7 + 1);
        size_t used = 0;
        frame_append(body, sizeof(body), &used, 200, pay, MAX_BODY - FRAME_HDR);
        em.packet_case("p12_max_size", PKT_DATA, conn, 20, body, used, "ok", "", -1, 0, NULL);
    }
    // (17)(18) the replay window's FLOOR, in its own window: accept a far-ahead sequence, then a
    // sequence more than 64 behind it. Not a duplicate -- never seen at all -- and still dropped,
    // because a window cannot prove it is not one.
    {
        const size_t pn   = build_input_payload(pay, sizeof(pay), 2000, 1);
        size_t       used = 0;
        frame_append(body, sizeof(body), &used, CH_INPUT, pay, pn);
        em.packet_case("p17_far_ahead", PKT_DATA, conn, 200, body, used, "ok", "s2", -1, 0, NULL);
        em.packet_case("p18_below_floor", PKT_DATA, conn, 1, body, used, "replay", "s2", -1, 0, NULL);
    }
    // (19) a sealed packet whose BODY is not well-formed framing: the MAC is valid, so the packet
    // is authentic, and the frame walk must still refuse it. Authentic and well-formed are two
    // different questions and a decoder that conflates them hands the game a truncated frame.
    {
        body[0] = CH_INPUT;
        body[1] = 0xff; // a frame length of 255 ...
        body[2] = 0x00;
        body[3] = 0x01; // ... over a 4-byte body
        em.packet_case("p19_body_malformed", PKT_DATA, conn, 21, body, 4, "ok", "", -1, 0, NULL);
    }

    // (11)(13) the two length refusals. Neither is a sealed packet -- they are raw byte counts, and
    // that is the point: the decoder must reject them on size before it parses anything.
    {
        unsigned char over[MAX_DATAGRAM + 1];
        for (size_t i = 0; i < sizeof(over); ++i) over[i] = (unsigned char)(i & 0xff);
        over[0] = MAGIC_VER;
        over[1] = PKT_DATA;
        memcpy(over + 2, conn, CONN_ID_BYTES);
        em.write_bin("p11_too_long.bin", over, sizeof(over));
        Case &c = em.cases[em.n++];
        c.n     = 0;
        c.put("kind", "packet");
        c.put("key", "p11_too_long");
        c.put("wire", "p11_too_long.bin");
        c.put("verdict", "too_long");
        c.put("window", "");
        char hexb[KEY_LEN * 2 + 1];
        hex_encode(ENC_KEY, KEY_LEN, hexb);
        c.put("enc_key", hexb);
        hex_encode(MAC_KEY, KEY_LEN, hexb);
        c.put("mac_key", hexb);
        hex_encode(conn, CONN_ID_BYTES, hexb);
        c.put("conn_id", hexb);
        c.put_u("seq", 0);

        unsigned char shortp[MIN_DATAGRAM - 1];
        memcpy(shortp, over, sizeof(shortp));
        em.write_bin("p13_too_short.bin", shortp, sizeof(shortp));
        Case &d = em.cases[em.n++];
        d.n     = 0;
        d.put("kind", "packet");
        d.put("key", "p13_too_short");
        d.put("wire", "p13_too_short.bin");
        d.put("verdict", "too_short");
        d.put("window", "");
        hex_encode(ENC_KEY, KEY_LEN, hexb);
        d.put("enc_key", hexb);
        hex_encode(MAC_KEY, KEY_LEN, hexb);
        d.put("mac_key", hexb);
        hex_encode(conn, CONN_ID_BYTES, hexb);
        d.put("conn_id", hexb);
        d.put_u("seq", 0);
    }

    // ---- connect tokens ------------------------------------------------------------------------
    {
        ConnectToken tok;
        memset(&tok, 0, sizeof(tok));
        fixture_match_id(tok.match_id);
        fixture_conn_id(tok.conn_id, 1);
        tok.slot           = 1;
        tok.expire_unix_ms = 1789646400000ULL + 3600000ULL; // an hour after the match_id's stamp
        for (int i = 0; i < KEY_LEN; ++i) {
            tok.keys.enc_c2s[i] = (unsigned char)(0x10 + i);
            tok.keys.enc_s2c[i] = (unsigned char)(0x40 + i);
            tok.keys.mac_c2s[i] = (unsigned char)(0x70 + i);
            tok.keys.mac_s2c[i] = (unsigned char)(0xa0 + i);
        }
        unsigned char wire[TOKEN_WIRE];
        token_seal(HOST_KEY, 0x0123456789abcdefULL, tok, wire);
        em.token_case("t01_token_ok", wire, TOKEN_WIRE, "ok", 0, tok, "0");
        em.token_case("t03_token_expired", wire, TOKEN_WIRE, "bad_mac",
                      tok.expire_unix_ms + 1, tok, "1");

        unsigned char bad[TOKEN_WIRE];
        memcpy(bad, wire, TOKEN_WIRE);
        bad[40] ^= 0x01; // one ciphertext byte
        em.token_case("t02_token_tampered", bad, TOKEN_WIRE, "bad_mac", 0, tok, "0");

        // The routing copy of conn_id disagreeing with the sealed one -- FORGED WITH THE HOST KEY,
        // because the MAC covers the routing copy, so an in-flight edit would only ever read as
        // bad_mac. This is the case the double-carried field exists for: a party that HOLDS the key
        // minting an inconsistent token, which token_open must still refuse.
        unsigned char mism[TOKEN_WIRE];
        memcpy(mism, wire, TOKEN_WIRE);
        mism[10] ^= 0x40;
        unsigned char enc[KEY_LEN], mac[KEY_LEN], full[SHA256_LEN];
        token_keys(HOST_KEY, enc, mac);
        hmac_sha256(mac, KEY_LEN, mism, 26 + TOKEN_PRIVATE, full);
        memcpy(mism + 26 + TOKEN_PRIVATE, full, MAC_LEN);
        em.token_case("t04_token_conn_mismatch", mism, TOKEN_WIRE, "malformed", 0, tok, "0");
    }
}

// ---- the fixture verifier ---------------------------------------------------------------------------

struct WindowSlot {
    char         name[32];
    ReplayWindow win;
};

// Assert the channel framing of a decoded body against the case's optional expectation fields.
void verify_body(const Case &c, const unsigned char *body, size_t len) {
    const char *key = c.get("key");
    size_t      off = 0;
    Frame       fr;
    bool        walk_ok = true;
    int         frames  = 0;
    char        what[160];

    while (frame_next(body, len, &off, fr, walk_ok)) {
        ++frames;
        if (fr.id == CH_INPUT) {
            InputEntry          e[INPUT_K_MAX];
            Verdict             why = Verdict::Ok;
            const unsigned char k   = input_decode(fr.data, fr.len, e, why);
            snprintf(what, sizeof(what), "%s: channel A decodes", key);
            check(what, k != 0 && why == Verdict::Ok);
            if (k) {
                snprintf(what, sizeof(what), "%s: channel A steps descend from the newest", key);
                bool desc = true;
                for (unsigned char i = 1; i < k; ++i)
                    if (e[i].step != e[0].step - i) desc = false;
                check(what, desc);
            }
        } else if (fr.id == CH_STATE) {
            LatestSlots slots;
            Verdict     why = Verdict::Ok;
            snprintf(what, sizeof(what), "%s: channel B records absorb", key);
            check(what, records_absorb(fr.data, fr.len, slots, why) && why == Verdict::Ok);
            PingRecord      p;
            StepHashRecord  sh;
            TelemetryRecord tm;
            snprintf(what, sizeof(what), "%s: channel B carries a ping, a step hash and telemetry", key);
            check(what, ping_from(slots, REC_PING, p) && step_hash_from(slots, sh) &&
                            telemetry_from(slots, tm));
        } else if (fr.id == CH_BULK) {
            if (fr.len == ACK_LEN && fr.data[0] == BULK_ACK) {
                Ack a;
                snprintf(what, sizeof(what), "%s: channel C ack decodes", key);
                check(what, ack_decode(fr.data, fr.len, a));
            } else {
                Piece   p;
                Verdict why = Verdict::Ok;
                snprintf(what, sizeof(what), "%s: channel C piece decodes", key);
                check(what, piece_decode(fr.data, fr.len, p, why) && why == Verdict::Ok);
            }
        }
    }
    const bool want_malformed = strstr(key, "malformed") != NULL;
    snprintf(what, sizeof(what), "%s: frame walk %s", key, want_malformed ? "refuses" : "completes");
    check(what, walk_ok != want_malformed);
    if (!want_malformed && len > 0) {
        snprintf(what, sizeof(what), "%s: at least one frame", key);
        check(what, frames > 0);
    }
}

int run_fixtures(const char *dir, Case *cases, int *out_n) {
    char path[640];
    snprintf(path, sizeof(path), "%s/index.json", dir);
    static char  text[65536];
    bool         ok = false;
    const size_t n  = read_file(path, (unsigned char *)text, sizeof(text) - 1, ok);
    if (!ok) {
        printf("  FAIL: cannot read %s\n", path);
        return 1;
    }
    text[n] = 0;

    const int count = parse_index(text, cases, 64);
    if (count <= 0) {
        printf("  FAIL: %s is not the restricted index grammar (or is empty)\n", path);
        return 1;
    }
    *out_n = count;
    printf("  fixtures: %d cases from %s\n", count, dir);

    WindowSlot windows[8];
    int        nwin = 0;

    for (int i = 0; i < count; ++i) {
        const Case &c    = cases[i];
        const char *key  = c.get("key");
        const char *kind = c.get("kind");
        const char *want = c.get("verdict");
        char        what[160];

        static unsigned char wire[MAX_DATAGRAM + 64];
        snprintf(path, sizeof(path), "%s/%s", dir, c.get("wire"));
        bool         rok = false;
        const size_t wn  = read_file(path, wire, sizeof(wire), rok);
        snprintf(what, sizeof(what), "%s: wire file loads", key);
        check(what, rok);
        if (!rok) continue;

        if (strcmp(kind, "token") == 0) {
            unsigned char hk[KEY_LEN];
            check("token host_key is hex", hex_decode(c.get("host_key"), hk, KEY_LEN));
            ConnectToken  got;
            bool          expired = false;
            const Verdict v       = token_open(hk, wire, wn, get_u(c, "now_ms", 0), got, expired);
            snprintf(what, sizeof(what), "%s: verdict %s", key, want);
            check(what, strcmp(verdict_name(v), want) == 0);
            snprintf(what, sizeof(what), "%s: expired flag", key);
            check(what, expired == (strcmp(c.get("expired"), "1") == 0));
            if (v == Verdict::Ok) {
                unsigned char mid[UUID7_BYTES], cid[CONN_ID_BYTES], k32[KEY_LEN];
                hex_decode(c.get("match_id"), mid, UUID7_BYTES);
                hex_decode(c.get("conn_id"), cid, CONN_ID_BYTES);
                snprintf(what, sizeof(what), "%s: match_id round-trips", key);
                check(what, memcmp(mid, got.match_id, UUID7_BYTES) == 0);
                snprintf(what, sizeof(what), "%s: conn_id round-trips", key);
                check(what, memcmp(cid, got.conn_id, CONN_ID_BYTES) == 0);
                snprintf(what, sizeof(what), "%s: slot round-trips", key);
                check(what, got.slot == (unsigned char)get_u(c, "slot", 99));
                snprintf(what, sizeof(what), "%s: expiry round-trips", key);
                check(what, got.expire_unix_ms == get_u(c, "expire_ms", 0));
                hex_decode(c.get("enc_c2s"), k32, KEY_LEN);
                snprintf(what, sizeof(what), "%s: enc_c2s round-trips", key);
                check(what, memcmp(k32, got.keys.enc_c2s, KEY_LEN) == 0);
                hex_decode(c.get("mac_s2c"), k32, KEY_LEN);
                snprintf(what, sizeof(what), "%s: mac_s2c round-trips", key);
                check(what, memcmp(k32, got.keys.mac_s2c, KEY_LEN) == 0);
            }
            continue;
        }

        unsigned char ek[KEY_LEN], mk[KEY_LEN], cid[CONN_ID_BYTES];
        check("enc_key is hex", hex_decode(c.get("enc_key"), ek, KEY_LEN));
        check("mac_key is hex", hex_decode(c.get("mac_key"), mk, KEY_LEN));
        check("conn_id is hex", hex_decode(c.get("conn_id"), cid, CONN_ID_BYTES));

        // Windows are shared by NAME and processed in index order, so a replay case is a genuine
        // second delivery to a window the first case already moved -- not a flag the harness sets.
        ReplayWindow *win   = NULL;
        const char   *wname = c.get("window");
        if (wname && wname[0]) {
            for (int w = 0; w < nwin; ++w)
                if (strcmp(windows[w].name, wname) == 0) win = &windows[w].win;
            if (!win && nwin < 8) {
                snprintf(windows[nwin].name, sizeof(windows[nwin].name), "%s", wname);
                windows[nwin].win.reset();
                win = &windows[nwin++].win;
            }
        }

        Header        hdr;
        size_t        blen = 0;
        const Verdict v    = packet_decode(wire, wn, cid, ek, mk, win, hdr, &blen);
        snprintf(what, sizeof(what), "%s: verdict %s", key, want);
        check(what, strcmp(verdict_name(v), want) == 0);
        if (v != Verdict::Ok) continue;

        snprintf(what, sizeof(what), "%s: seq matches the index", key);
        check(what, hdr.seq == get_u(c, "seq", 0xffffffffULL));
        if (c.get("type")) {
            snprintf(what, sizeof(what), "%s: type matches the index", key);
            check(what, hdr.type == (unsigned char)get_u(c, "type", 255));
        }

        if (const char *pf = c.get("plain")) {
            static unsigned char plain[MAX_DATAGRAM];
            snprintf(path, sizeof(path), "%s/%s", dir, pf);
            bool         pok = false;
            const size_t pn  = read_file(path, plain, sizeof(plain), pok);
            snprintf(what, sizeof(what), "%s: plaintext matches the committed body", key);
            check(what, pok && pn == blen && (blen == 0 || memcmp(plain, wire + HDR_SIZE, blen) == 0));
        }
        verify_body(c, wire + HDR_SIZE, blen);
    }
    return 0;
}

// ---- arm 2: the properties the fixtures cannot express ----------------------------------------------

void run_properties() {
    unsigned char conn[CONN_ID_BYTES];
    fixture_conn_id(conn, 1);
    unsigned char body[MAX_BODY], pay[MAX_BODY], pkt[MAX_DATAGRAM + 8];
    Verdict       why = Verdict::Ok;

    // EVERY redundancy K, not just the default. K is configurable 1..8 (plan D2) and a format that
    // only round-trips its default is a format with seven untested branches.
    for (unsigned char k = INPUT_K_MIN; k <= INPUT_K_MAX; ++k) {
        const size_t pn = build_input_payload(pay, sizeof(pay), 5000 + k, k);
        char         what[96];
        snprintf(what, sizeof(what), "channel A encodes K=%u", k);
        check(what, pn > 0);
        InputEntry e[INPUT_K_MAX];
        snprintf(what, sizeof(what), "channel A round-trips K=%u", k);
        check(what, input_decode(pay, pn, e, why) == k && why == Verdict::Ok);
    }
    // K outside the range, and a trailing byte. Both are refusals a peer on a different build could
    // produce, so both are decoder business rather than encoder business.
    {
        InputEntry e[INPUT_K_MAX];
        pay[0] = 0;
        memset(pay + 1, 0, 4);
        check("channel A refuses K=0", input_decode(pay, 5, e, why) == 0);
        pay[0] = INPUT_K_MAX + 1;
        check("channel A refuses K=9", input_decode(pay, 5, e, why) == 0);
        const size_t pn = build_input_payload(pay, sizeof(pay), 100, 2);
        check("channel A refuses a trailing byte", input_decode(pay, pn + 1, e, why) == 0);
        check("channel A refuses truncation", input_decode(pay, pn - 1, e, why) == 0);
    }
    // The encoder refuses a step sequence that is not the implied run (net_udp.cpp's check).
    {
        unsigned char b[4] = {1, 2, 3, 4};
        InputEntry    e[2];
        e[0].step  = 10;
        e[0].bytes = b;
        e[0].len   = 4;
        e[1].step  = 8; // should be 9
        e[1].bytes = b;
        e[1].len   = 4;
        check("channel A refuses a broken implied run", input_encode(e, 2, pay, sizeof(pay)) == 0);
    }

    // LATEST-WINS, materialised: two pings in one payload, the second must be what a reader sees.
    {
        size_t     used = 0;
        PingRecord a = {1, 2}, b = {9, 8};
        record_append_ping(pay, sizeof(pay), &used, REC_PING, a);
        record_append_ping(pay, sizeof(pay), &used, REC_PING, b);
        LatestSlots slots;
        check("channel B absorbs two pings", records_absorb(pay, used, slots, why));
        PingRecord got;
        check("channel B keeps the LATEST ping", ping_from(slots, REC_PING, got) &&
                                                     got.t_origin_ms == 9 && got.t_echo_ms == 8);
        // An unknown record id is SKIPPED, and the known record after it still arrives.
        used                  = 0;
        unsigned char junk[3] = {7, 7, 7};
        record_append(pay, sizeof(pay), &used, 200, junk, sizeof(junk));
        record_append_ping(pay, sizeof(pay), &used, REC_PONG, b);
        LatestSlots s2;
        check("channel B skips an unknown record id", records_absorb(pay, used, s2, why));
        check("channel B still reads the record after it", ping_from(s2, REC_PONG, got));
        // Truncation is refused, not treated as the end.
        check("channel B refuses truncation", !records_absorb(pay, used - 1, s2, why));
    }

    // CHANNEL C over a whole chunk: every piece round-trips, the pieces reassemble to the original,
    // and the per-chunk SHA-256 in the header is the hash of what was reassembled. A piece codec
    // that dropped the last partial piece would still pass a single-piece test.
    {
        static unsigned char chunk[16 * 1024], rebuilt[16 * 1024];
        build_chunk(chunk);
        unsigned char sha[SHA256_LEN];
        sha256(chunk, FIXTURE_CHUNK_LEN, sha);
        const unsigned short total = piece_count(FIXTURE_CHUNK_LEN);
        check("chunk of 2600 bytes is 3 pieces", total == 3);
        size_t got = 0;
        bool   all = true;
        for (unsigned short i = 0; i < total; ++i) {
            Piece p, q;
            if (!piece_for(chunk, FIXTURE_CHUNK_LEN, 0x0b, i, 100 + i, p, sha)) all = false;
            const size_t n = piece_encode(p, pay, sizeof(pay));
            if (n == 0 || !piece_decode(pay, n, q, why)) all = false;
            else {
                if (q.index != i || q.total != total || q.chunk_len != FIXTURE_CHUNK_LEN) all = false;
                if (memcmp(q.chunk_sha, sha, SHA256_LEN) != 0) all = false;
                memcpy(rebuilt + got, q.bytes, q.len);
                got += q.len;
            }
        }
        check("channel C round-trips every piece", all);
        check("channel C pieces reassemble the chunk",
              got == FIXTURE_CHUNK_LEN && memcmp(rebuilt, chunk, FIXTURE_CHUNK_LEN) == 0);
        unsigned char again[SHA256_LEN];
        sha256(rebuilt, FIXTURE_CHUNK_LEN, again);
        check("the chunk header's SHA-256 is the reassembled chunk's",
              memcmp(again, sha, SHA256_LEN) == 0);
        check("a chunk over 16 KiB has no piece count", piece_count(CHUNK_MAX + 1) == 0);
        check("an empty chunk has no piece count", piece_count(0) == 0);
        // A piece claiming an index past its own total is refused.
        Piece p;
        piece_for(chunk, FIXTURE_CHUNK_LEN, 0x0b, 0, 1, p, sha);
        const size_t n = piece_encode(p, pay, sizeof(pay));
        pay[5]         = 9; // index := 9, total is 3
        Piece q;
        check("channel C refuses index >= total", !piece_decode(pay, n, q, why));
    }

    // THE SIZE CEILING, from the encoder's side. The fixtures pin the decoder at 1200/1201; this is
    // the other half -- a body one byte too big must be refused at the source, so the transport
    // never has to discover the limit by sending something that will be dropped.
    {
        memset(body, 0x5a, sizeof(body));
        check("encode accepts a body of exactly MAX_BODY",
              packet_encode(PKT_DATA, conn, 1, ENC_KEY, MAC_KEY, body, MAX_BODY, pkt, why) ==
                  MAX_DATAGRAM);
        check("encode refuses MAX_BODY + 1",
              packet_encode(PKT_DATA, conn, 1, ENC_KEY, MAC_KEY, body, MAX_BODY + 1, pkt, why) == 0 &&
                  why == Verdict::BodyTooLong);
    }

    // THE REPLAY WINDOW, position by position. The fixtures carry one duplicate and one below-floor
    // case; these are the 64 in between, plus the two boundaries that an off-by-one moves.
    {
        ReplayWindow w;
        check("a fresh window accepts anything", w.check(12345));
        w.commit(1000);
        check("the same sequence twice is a replay", !w.check(1000));
        bool all_fresh = true, all_dup = true;
        for (unsigned long long s = 1001; s <= 1000 + REPLAY_WINDOW; ++s) {
            if (!w.check(s)) all_fresh = false;
            w.commit(s);
            if (w.check(s)) all_dup = false;
        }
        check("every sequence in the window is accepted once", all_fresh);
        check("every sequence in the window is refused twice", all_dup);
        // newest is now 1064. 1064-63 = 1001 is the last still inside; 1000 is exactly the floor.
        check("the oldest in-window sequence is still known", !w.check(1001));
        check("a sequence at the floor is refused", !w.check(1000));
        check("a sequence below the floor is refused", !w.check(500));
        // A jump of more than a window clears the bitmap: nothing older can be judged any more.
        ReplayWindow j;
        j.commit(10);
        j.commit(10 + REPLAY_WINDOW + 5);
        check("a jump past the window forgets the old bitmap", !j.check(10));
        check("a jump past the window accepts a fresh mid sequence", j.check(10 + REPLAY_WINDOW));
    }

    // AN UNAUTHENTICATED PACKET MUST NOT MOVE THE WINDOW. The reason the commit is split from the
    // check (net_udp.h): if a forged packet at sequence 500 advanced the window, the REAL packet at
    // 500 would then be refused as a replay -- a one-datagram denial of service.
    {
        size_t       used = 0;
        const size_t pn   = build_input_payload(pay, sizeof(pay), 300, 1);
        frame_append(body, sizeof(body), &used, CH_INPUT, pay, pn);
        const size_t         n = packet_encode(PKT_DATA, conn, 500, ENC_KEY, MAC_KEY, body, used, pkt, why);
        static unsigned char forged[MAX_DATAGRAM];
        memcpy(forged, pkt, n);
        forged[HDR_SIZE + 1] ^= 0x01;
        ReplayWindow w;
        Header       h;
        size_t       bl = 0;
        check("a forged packet fails the MAC",
              packet_decode(forged, n, conn, ENC_KEY, MAC_KEY, &w, h, &bl) == Verdict::BadMac);
        check("the forged packet did not move the window", w.check(500));
        static unsigned char real[MAX_DATAGRAM];
        memcpy(real, pkt, n);
        check("the real packet at that sequence is still accepted",
              packet_decode(real, n, conn, ENC_KEY, MAC_KEY, &w, h, &bl) == Verdict::Ok);
    }

    // hdr_peek is the RELAY's view: conn_id and sequence with no key at all.
    {
        size_t       used = 0;
        const size_t pn   = build_input_payload(pay, sizeof(pay), 700, 1);
        frame_append(body, sizeof(body), &used, CH_INPUT, pay, pn);
        const size_t n = packet_encode(PKT_DATA, conn, 4242, ENC_KEY, MAC_KEY, body, used, pkt, why);
        Header       h;
        check("a relay reads the header without a key", hdr_peek(pkt, n, h));
        check("the header it reads is the right one",
              h.seq == 4242 && h.type == PKT_DATA && memcmp(h.conn_id, conn, CONN_ID_BYTES) == 0);
        pkt[0] ^= 0x10;
        check("a relay refuses a foreign magic", !hdr_peek(pkt, n, h));
    }

    // TOKEN refusals the fixtures do not carry, because they are about the codec's own guards.
    {
        ConnectToken tok;
        memset(&tok, 0, sizeof(tok));
        fixture_match_id(tok.match_id);
        fixture_conn_id(tok.conn_id, 3);
        tok.slot           = 3;
        tok.expire_unix_ms = 1789646400000ULL;
        unsigned char wire[TOKEN_WIRE];
        check("a token seals to TOKEN_WIRE bytes", token_seal(HOST_KEY, 7, tok, wire) == TOKEN_WIRE);
        tok.slot = TOKEN_SLOT_MAX + 1;
        check("a token refuses a slot past the lobby's 8", token_seal(HOST_KEY, 7, tok, wire) == 0);
        ConnectToken got;
        bool         exp = false;
        check("a short token is refused", token_open(HOST_KEY, wire, TOKEN_WIRE - 1, 0, got, exp) ==
                                              Verdict::TooShort);
        unsigned char other[KEY_LEN];
        memcpy(other, HOST_KEY, KEY_LEN);
        other[0] ^= 0x01;
        check("a token minted under another host key is refused",
              token_open(other, wire, TOKEN_WIRE, 0, got, exp) == Verdict::BadMac);
    }

    // ---- mp:R1c: THE PER-PEER RELAY LEG KEY ------------------------------------------------------
    //
    // WHY THIS ARM IS HERE and not in a relay-shaped suite: the leg key is derived from a CONNECT
    // TOKEN, which is this file's subject, and `udp_relay.cpp` -- the only other place the
    // derivation lives -- owns sockets and a thread and is not linked into this exe. The derivation
    // itself is socket-free and lives in `udp_relay.h` for exactly that reason.
    //
    // THE PROPERTY UNDER TEST IS THE ITEM'S: a peer holding the deployment PSK, and even a peer
    // holding a VALID TOKEN OF ITS OWN, cannot produce the tag another peer's handle is now checked
    // against. Before mp:R1c every peer shared one key, so forging a handle was free and the relay
    // would re-point that peer's traffic at the forger's address. The relay counts the refusal as
    // `leg_bad_mac`; this is the same refusal at the codec, where it can be asserted without a
    // network.
    {
        unsigned char mac_c2s[KEY_LEN], mac_s2c[KEY_LEN], other_c2s[KEY_LEN];
        for (int i = 0; i < (int)KEY_LEN; ++i) {
            mac_c2s[i]   = (unsigned char)i;
            mac_s2c[i]   = (unsigned char)(i * 7 + 3);
            other_c2s[i] = (unsigned char)(i * 31 + 11);
        }
        unsigned char kc[KEY_LEN], kh[KEY_LEN], kc2[KEY_LEN], dep[KEY_LEN];
        mh::udprelay::peer_leg_key(mac_c2s, true, kc);
        mh::udprelay::peer_leg_key(mac_s2c, false, kh);
        mh::udprelay::peer_leg_key(other_c2s, true, kc2);
        mh::udprelay::deployment_leg_key(HOST_KEY, dep);

        check("a client's leg key is not its host's", memcmp(kc, kh, KEY_LEN) != 0);
        check("neither is the deployment key", memcmp(kc, dep, KEY_LEN) != 0 &&
                                                   memcmp(kh, dep, KEY_LEN) != 0);
        check("a DIFFERENT token gives a different client key", memcmp(kc, kc2, KEY_LEN) != 0);
        // The direction labels are not interchangeable: the same bytes under the other label must
        // not land on the same key, or the two halves of a pair could replay each other.
        unsigned char swapped[KEY_LEN];
        mh::udprelay::peer_leg_key(mac_c2s, false, swapped);
        check("the two labels are really two keys over the same material",
              memcmp(kc, swapped, KEY_LEN) != 0);

        // THE CROSS-LANGUAGE FIXTURE. `src/relay/src/leg.rs` derives these same two keys for the
        // same two inputs, and the relay and the peer have to agree byte for byte or the leg simply
        // stops verifying. Pinned as hex on BOTH sides (leg.rs has the matching assertion), because
        // "both implementations changed together" is the failure a round-trip test inside one of
        // them cannot see.
        static const char *const K_CLIENT =
            "ead16d7f41a7407ea694ca092468c24f4993189a7e621f598051442289ad6f34";
        static const char *const K_HOST =
            "d716d119872ec8f7e03a0462d06a2fc01d94a0718db3eb612566bd4431cbed85";
        char hexc[KEY_LEN * 2 + 1], hexh[KEY_LEN * 2 + 1];
        for (int i = 0; i < (int)KEY_LEN; ++i) {
            snprintf(hexc + i * 2, 3, "%02x", kc[i]);
            snprintf(hexh + i * 2, 3, "%02x", kh[i]);
        }
        check("the client leg key matches the Rust relay's, byte for byte",
              strcmp(hexc, K_CLIENT) == 0);
        check("the host leg key matches the Rust relay's, byte for byte",
              strcmp(hexh, K_HOST) == 0);

        // AND THE REFUSAL ITSELF, over the leg's own tag rule (HMAC-SHA256 truncated to 128 bits
        // over the 18-byte header and payload). A forger writes the victim's handle into `src` and
        // tags it with everything it has: the shared deployment key, and a key from a token of its
        // own. Neither produces the tag the victim's handle is checked against.
        unsigned char hdr[18 + 4];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 0x51; // the leg family byte, version 1
        hdr[1] = 4;    // OP_PING
        hdr[2] = 0x2a; // src = the victim's handle
        unsigned char want[SHA256_LEN], forged_dep[SHA256_LEN], forged_tok[SHA256_LEN];
        hmac_sha256(kc, KEY_LEN, hdr, sizeof(hdr), want);
        hmac_sha256(dep, KEY_LEN, hdr, sizeof(hdr), forged_dep);
        hmac_sha256(kc2, KEY_LEN, hdr, sizeof(hdr), forged_tok);
        check("a PSK holder cannot tag another peer's handle", memcmp(want, forged_dep, 16) != 0);
        check("nor can a peer holding a token of its own", memcmp(want, forged_tok, 16) != 0);
    }
}

// ---- arm 3: the decoder fuzz ------------------------------------------------------------------------
//
// A seeded xorshift, not rand(): the run has to be reproducible from its seed, and the C runtime's
// generator differs between the two build configurations this is run under.
struct Rng {
    unsigned long long s;
    unsigned long long next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    unsigned int below(unsigned int n) { return n ? (unsigned int)(next() % n) : 0; }
};

int run_fuzz(const char *dir, int seconds, unsigned long long seed) {
    Case cases[64];
    int  count = 0;
    {
        char        path[640];
        static char text[65536];
        snprintf(path, sizeof(path), "%s/index.json", dir);
        bool         ok = false;
        const size_t n  = read_file(path, (unsigned char *)text, sizeof(text) - 1, ok);
        if (!ok) {
            printf("  FAIL: fuzz cannot read %s\n", path);
            return 1;
        }
        text[n] = 0;
        count   = parse_index(text, cases, 64);
        if (count <= 0) {
            printf("  FAIL: fuzz cannot parse the index\n");
            return 1;
        }
    }

    // The corpus: every committed wire file. Real packets are the only interesting starting point --
    // random bytes are rejected on the magic byte within nanoseconds and never reach the framing.
    static unsigned char corpus[64][MAX_DATAGRAM + 64];
    static size_t        corpus_len[64];
    int                  ncorp = 0;
    for (int i = 0; i < count && ncorp < 64; ++i) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", dir, cases[i].get("wire"));
        bool         ok = false;
        const size_t n  = read_file(path, corpus[ncorp], sizeof(corpus[0]), ok);
        if (ok && n) corpus_len[ncorp++] = n;
    }
    if (ncorp == 0) {
        printf("  FAIL: fuzz corpus is empty\n");
        return 1;
    }

    unsigned char ek[KEY_LEN], mk[KEY_LEN], cid[CONN_ID_BYTES], hk[KEY_LEN];
    memcpy(ek, ENC_KEY, KEY_LEN);
    memcpy(mk, MAC_KEY, KEY_LEN);
    memcpy(hk, HOST_KEY, KEY_LEN);
    fixture_conn_id(cid, 1);

    Rng                rng = {seed ? seed : 0x9e3779b97f4a7c15ULL};
    ReplayWindow       win;
    unsigned long long iters = 0;
    unsigned long long verdict_count[16];
    memset(verdict_count, 0, sizeof(verdict_count));

    const time_t t0 = time(NULL);
    printf("  fuzz: %d corpus packets, seed %llu, %d s\n", ncorp, (unsigned long long)rng.s, seconds);
    for (;;) {
        // The clock is read every 4096 iterations rather than every one: time() dominated the loop
        // otherwise, which would have made the iteration count a measurement of the clock.
        if ((iters & 0xfff) == 0 && (long)(time(NULL) - t0) >= seconds) break;
        ++iters;

        static unsigned char buf[MAX_DATAGRAM + 64];
        const int            pick = (int)rng.below((unsigned int)ncorp);
        size_t               len  = corpus_len[pick];
        memcpy(buf, corpus[pick], len);

        // Four mutation shapes. Truncation and extension matter as much as bit flips: most of the
        // bounds arithmetic in the decoder is about a declared length disagreeing with the real one.
        const unsigned int shape = rng.below(4);
        if (shape == 0) {
            const unsigned int nflips = 1 + rng.below(4);
            for (unsigned int i = 0; i < nflips; ++i)
                buf[rng.below((unsigned int)len)] ^= (unsigned char)(1u << rng.below(8));
        } else if (shape == 1) {
            len = 1 + rng.below((unsigned int)len);
        } else if (shape == 2) {
            const unsigned int add = rng.below(64);
            for (unsigned int i = 0; i < add && len < sizeof(buf); ++i)
                buf[len++] = (unsigned char)rng.below(256);
        } else {
            const unsigned int off = rng.below((unsigned int)len);
            const unsigned int run = 1 + rng.below(16);
            for (unsigned int i = 0; i < run && off + i < len; ++i)
                buf[off + i] = (unsigned char)rng.below(256);
        }
        // Keep a fraction of the corpus's identity: without this, almost everything dies on the
        // first byte and the framing below never runs.
        if (rng.below(4) != 0) buf[0] = MAGIC_VER;

        Header        hdr;
        size_t        blen = 0;
        const Verdict v    = packet_decode(buf, len, rng.below(2) ? cid : NULL, ek, mk,
                                        rng.below(2) ? &win : NULL, hdr, &blen);
        verdict_count[(unsigned)v & 15]++;
        if (v == Verdict::Ok) {
            // An authentic packet: walk its framing, which is where the interesting bounds live.
            size_t off = 0;
            Frame  fr;
            bool   ok = true;
            while (frame_next(buf + HDR_SIZE, blen, &off, fr, ok)) {
                if (fr.id == CH_INPUT) {
                    InputEntry e[INPUT_K_MAX];
                    Verdict    why = Verdict::Ok;
                    input_decode(fr.data, fr.len, e, why);
                } else if (fr.id == CH_STATE) {
                    LatestSlots slots;
                    Verdict     why = Verdict::Ok;
                    records_absorb(fr.data, fr.len, slots, why);
                } else if (fr.id == CH_BULK) {
                    Piece   p;
                    Ack     a;
                    Verdict why = Verdict::Ok;
                    piece_decode(fr.data, fr.len, p, why);
                    ack_decode(fr.data, fr.len, a);
                }
            }
        }
        // The token codec gets the same treatment -- it is a second parser over attacker bytes.
        ConnectToken tok;
        bool         exp = false;
        token_open(hk, buf, len, rng.below(2) ? 1789646400000ULL : 0, tok, exp);

        // And the window, so a long run actually exercises the slide rather than one position.
        if ((iters & 0x3ff) == 0) win.reset();
    }

    const long elapsed = (long)(time(NULL) - t0);
    printf("  fuzz: %llu iterations in %ld s, 0 crashes\n", iters, elapsed);
    printf("  fuzz verdicts:");
    for (unsigned v = 0; v < 12; ++v)
        if (verdict_count[v]) printf(" %s=%llu", verdict_name((Verdict)v), verdict_count[v]);
    printf("\n");
    // A run that never produced an Ok would be a run that only tested the magic byte.
    check("fuzz reached an authentic packet at least once", verdict_count[(unsigned)Verdict::Ok] > 0);
    check("fuzz produced at least one MAC failure", verdict_count[(unsigned)Verdict::BadMac] > 0);
    return 0;
}

} // namespace

int run_udpwiretest(int argc, char **argv) {
    char        dirbuf[512];
    const char *mode = (argc > 2) ? argv[2] : "";

    if (strcmp(mode, "--emit") == 0) {
        Emitter em;
        snprintf(em.dir, sizeof(em.dir), "%s", (argc > 3) ? argv[3] : ".");
        build_fixtures(em);
        if (!em.write_index()) return 1;
        printf("=== udpwiretest --emit: %d cases into %s ===\n", em.n, em.dir);
        return em.ok ? 0 : 1;
    }

    const char *dir = fixture_dir(dirbuf, sizeof(dirbuf));
    if (!dir) {
        printf("  FAIL: no fixture directory found (set MH_UDP_FIXTURES, or run from the repo root)\n");
        printf("=== udpwiretest: 1 checks, 1 failures ===\n");
        return 1;
    }

    if (strcmp(mode, "--fuzz") == 0) {
        const int                secs = (argc > 3) ? atoi(argv[3]) : 10;
        const unsigned long long seed = (argc > 4) ? strtoull(argv[4], NULL, 10) : 0;
        const int                rc   = run_fuzz(dir, secs > 0 ? secs : 10, seed);
        printf("=== udpwiretest(fuzz): %d checks, %d failures ===\n", g_checks, g_fails);
        return (rc || g_fails) ? 1 : 0;
    }

    Case      cases[64];
    int       n  = 0;
    const int rc = run_fixtures(dir, cases, &n);
    run_properties();
    printf("=== udpwiretest: %d checks, %d failures ===\n", g_checks, g_fails);
    return (rc || g_fails) ? 1 : 0;
}
