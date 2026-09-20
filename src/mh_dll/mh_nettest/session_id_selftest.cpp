//
// session_id_selftest.cpp -- `net_selftest.exe sessionidtest`: the match_id (SES0) and the pinned
// input codepage (mp:F3). UUIDv7 construction and text, the SESSION_INFO/JOIN v4 wire records, and
// the host's ADMIT decision -- with no game, no rig and no socket.
//
// mp:F3 EXTENDED IT RATHER THAN ADDING A SUITE, for the reason the file already gives: both of its
// wire fields ride the same two records and both of its refusals are the same `join_admit()` the
// seam calls. The codepage's negative case is even harder to stage on the rig than SES0's -- it needs
// two peers that disagree about their ANSI codepage, i.e. two differently-localised Windows installs
// -- so an offline arm is not a convenience here, it is the only form the assertion has.
//
// WHY THIS SUITE EXISTS. SES0's acceptance has four clauses and the rig can demonstrate exactly
// two of them: both peers logging one id, and a re-created lobby logging a different one. The other
// two are REFUSALS, and there is no old client to run:
//
//   * a pre-SES0 JOIN (format 2) must be refused with a NAMED reason rather than seated without a
//     match_id -- the tracker's negative case;
//   * a pre-SES0 DECODER must refuse a v3 advert outright rather than best-effort parsing it and
//     reporting a match under no id. That one is asserted here by decoding with the OLD acceptance
//     rule written out longhand, which is the only way to say what a binary we no longer build does.
//
// Both are pure functions, which is why the seam (mh/seams/net_discovery.cpp, on_join_recv) does
// nothing but call join_admit() and print join_admit_reason(): the decision the rig cannot reach and
// the decision this file asserts are the same code, not two encodings of one rule.
//
// The mint itself is split for the same reason. mh_net_proto::uuid7_make() takes the clock and the
// entropy as ARGUMENTS (mh_common/include/mh_session_id.h supplies them on Windows), so fixed
// vectors give a byte-exact expectation -- a self-seeding minter could only ever be checked for
// plausibility.
//
#include <stdio.h>
#include <string.h>

#include "mh_net_proto/session_info.h"
#include "mh_net_proto/uuid7.h"

using namespace mh_net_proto;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// mp:X2's two trailing fields, as fixed vectors. A real advert's would be the SHA-256 of a real map
// file; what this suite is about is the RECORD, so the bytes only have to be distinctive enough that
// a mis-offset read cannot look right.
const unsigned char    MAPH[MAP_HASH_BYTES] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr unsigned int MAPSZ                = 0x000715a3u; // 464,291 -- a plausible .mpm

// A SessionInfo shaped like a real host advert, so the round-trips are over the record the wire
// actually carries rather than a mostly-zero one.
SessionInfo make_session(const char *name, unsigned int tag, const unsigned char *match_id,
                         unsigned short codepage = 0) {
    SessionInfo si;
    memcpy(si.map_hash, MAPH, MAP_HASH_BYTES); // X2: the map's content identity (v5)
    si.map_size     = MAPSZ;
    si.codepage     = codepage;
    si.tag          = tag;
    si.host_version = 2;
    si.protocol     = 1;
    si.cur_players  = 2;
    si.max_players  = 4;
    memcpy(si.name, name, strlen(name) + 1);
    memcpy(si.map, "TUTORIAL.MP", 12);
    si.has_map_header = true;
    for (int i = 0; i < MAP_HEADER_SIZE; ++i) si.map_header[i] = (unsigned char)(i & 0xff);
    if (match_id) memcpy(si.match_id, match_id, UUID7_BYTES);
    return si;
}

// The PRE-SES0 SessionInfo decoder's acceptance rule, written out rather than referenced, because
// the code it describes no longer exists. This is the only thing in the suite that is a copy of
// something: it is a copy of a DELETED line (`fmt != 1 && fmt != 2`), and copying it is the point.
bool old_decoder_accepts(const unsigned char *buf, size_t len) {
    return len >= 11 && (buf[0] == 1 || buf[0] == 2);
}

// A v2 JOIN, encoded by hand -- there is no encoder for the old format any more, and one written to
// serve this test would be a second encoder rather than a record of the old one.
size_t encode_join_v2(unsigned char *out, unsigned int tag, const char *lobby, const char *player) {
    unsigned char *p = out;
    *p++             = 2; // format
    *p++             = (unsigned char)(tag & 0xff);
    *p++             = (unsigned char)((tag >> 8) & 0xff);
    *p++             = (unsigned char)((tag >> 16) & 0xff);
    *p++             = (unsigned char)((tag >> 24) & 0xff);
    unsigned char n  = (unsigned char)strlen(lobby);
    *p++             = n;
    memcpy(p, lobby, n);
    p += n;
    unsigned char pn = (unsigned char)strlen(player);
    *p++             = pn;
    memcpy(p, player, pn);
    p += pn;
    return (size_t)(p - out);
}

} // namespace

int run_sessionidtest() {
    printf("=== sessionidtest (SES0: the UUIDv7 match_id, its wire records and the admit decision) ===\n");

    // ---- 1. uuid7_make: the RFC 9562 §5.7 layout ------------------------------------------------
    {
        // 2026-09-17T12:00:00Z == 1789646400000 ms. A real-looking stamp rather than a small integer,
        // so a 32-bit truncation anywhere in the 48-bit field shows up as a wrong year rather than a
        // wrong small number.
        const unsigned long long MS = 1789646400000ULL;
        unsigned char            rnd[UUID7_RAND_MAX];
        for (int i = 0; i < (int)sizeof(rnd); ++i) rnd[i] = (unsigned char)(0xa0 + i);
        unsigned char id[UUID7_BYTES];
        uuid7_make(MS, rnd, id);

        check("version nibble is 7", uuid7_version(id) == 7);
        check("variant bits are 0b10", uuid7_variant(id) == 2);
        check("the timestamp decodes back to the input ms", uuid7_unix_ms(id) == MS);
        check("a minted id is not nil", !uuid7_is_nil(id));

        // Byte-exact, so a change to the bit packing is a failure rather than a silent re-layout:
        // ts 0x01a0_af3c_ea00, then 0x70|(0xa0&0x0f)=0x70, 0xa1, 0x80|(0xa2&0x3f)=0xa2, then a3..a9.
        const unsigned char WANT[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                                 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        check("the packed layout is byte-exact", memcmp(id, WANT, UUID7_BYTES) == 0);

        // The version nibble OVERWRITES four of the caller's random bits; asserting that keeps a
        // future "use all 80 bits" edit from quietly producing non-conforming ids.
        check("rand_a's top nibble is replaced by the version", (id[6] & 0xf0) == 0x70);
        check("rand_b's top two bits are replaced by the variant", (id[8] & 0xc0) == 0x80);

        // A nil buffer is distinguishable from a real id -- the sentinel every downstream reader uses.
        unsigned char nil[UUID7_BYTES] = {0};
        check("an all-zero buffer reads as nil", uuid7_is_nil(nil));
        check("nil has no version 7", uuid7_version(nil) != 7);

        // Only the low 48 bits are used; the field is documented as saturating, not wrapping into
        // the version nibble.
        unsigned char big[UUID7_BYTES];
        uuid7_make(0xffff'0000'0000'0000ULL | MS, rnd, big);
        check("the high 16 bits of the stamp are dropped, not folded", uuid7_unix_ms(big) == MS);
        check("an over-large stamp still yields version 7", uuid7_version(big) == 7);
    }

    // ---- 2. two mints differ --------------------------------------------------------------------
    {
        unsigned char a[UUID7_BYTES], b[UUID7_BYTES], c[UUID7_BYTES];
        unsigned char r1[UUID7_RAND_MAX], r2[UUID7_RAND_MAX];
        for (int i = 0; i < (int)sizeof(r1); ++i) {
            r1[i] = (unsigned char)(i + 1);
            r2[i] = (unsigned char)(0xf0 - i);
        }
        uuid7_make(1789646400000ULL, r1, a);
        uuid7_make(1789646400000ULL, r2, b); // same millisecond, different entropy
        uuid7_make(1789646400001ULL, r1, c); // same entropy, next millisecond

        check("two mints in the SAME ms with different entropy differ", memcmp(a, b, UUID7_BYTES) != 0);
        check("two mints with the same entropy in different ms differ", memcmp(a, c, UUID7_BYTES) != 0);
        // k-sortability is the reason for v7 over v4: memcmp order must be time order.
        check("a later id sorts after an earlier one", memcmp(a, c, UUID7_BYTES) < 0);
    }

    // ---- 3. text forms ---------------------------------------------------------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        char                hex[UUID7_HEX_CAP];
        uuid7_hex(ID, hex, sizeof(hex));
        check("hex is 32 characters", strlen(hex) == 32);
        check("hex is the lowercase dash-less form", strcmp(hex, "01a0af3cea0070a1a2a3a4a5a6a7a8a9") == 0);

        char dashed[UUID7_TEXT_CAP];
        uuid7_dashed(ID, dashed, sizeof(dashed));
        check("the dashed form is the canonical 8-4-4-4-12",
              strcmp(dashed, "01a0af3c-ea00-70a1-a2a3-a4a5a6a7a8a9") == 0);

        unsigned char back[UUID7_BYTES];
        check("hex parses back", uuid7_parse(hex, back) && memcmp(back, ID, UUID7_BYTES) == 0);
        memset(back, 0, sizeof(back));
        check("the dashed form parses back", uuid7_parse(dashed, back) && memcmp(back, ID, UUID7_BYTES) == 0);

        // Refusals leave `out` alone -- a partially-decoded id that looked real is the failure here.
        unsigned char keep[UUID7_BYTES];
        memcpy(keep, ID, UUID7_BYTES);
        check("a short string is refused", !uuid7_parse("01a0af3c", keep));
        check("a non-hex character is refused", !uuid7_parse("01a0af3cea0070a1a2a3a4a5a6a7a8az", keep));
        check("a dash in the wrong place is refused", !uuid7_parse("01a0af3c2-ea0-70a1-a2a3-a4a5a6a7a8a9", keep));
        check("a refused parse leaves the buffer untouched", memcmp(keep, ID, UUID7_BYTES) == 0);
        check("a null string is refused", !uuid7_parse(nullptr, keep));

        // A buffer too small writes NOTHING rather than a truncated id that would read as real.
        char small[8];
        memset(small, 'X', sizeof(small));
        uuid7_hex(ID, small, sizeof(small));
        check("hex into a short buffer writes nothing", small[0] == 'X');
    }

    // ---- 4. THE LOG LINE -- the contract tools/mp_analyze.py parses ------------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        char                line[SESSION_LOG_LINE_CAP];
        session_match_id_log_line(ID, line, sizeof(line));
        check("the log line is exactly the documented format",
              strcmp(line, "; [session] match_id=01a0af3cea0070a1a2a3a4a5a6a7a8a9\n") == 0);
    }

    // ---- 5. the relay connection id (D4/D8) ------------------------------------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        unsigned char       c0[CONN_ID_BYTES], c3[CONN_ID_BYTES];
        conn_id_from_match(ID, 0, c0);
        conn_id_from_match(ID, 3, c3);
        check("slot 0's conn_id is the match_id's first 8 bytes", memcmp(c0, ID, CONN_ID_BYTES) == 0);
        check("the slot XORs into the LOW byte only", memcmp(c3, ID, CONN_ID_BYTES - 1) == 0 && c3[7] == (ID[7] ^ 3));
        check("two slots of one match get different conn_ids", memcmp(c0, c3, CONN_ID_BYTES) != 0);
        // Slots are 0..7, so the derivation is injective over the whole legal range -- two peers of
        // one match can never collide on the relay's demux key.
        unsigned char seen[8][CONN_ID_BYTES];
        bool          distinct = true;
        for (int s = 0; s < 8; ++s) conn_id_from_match(ID, (unsigned char)s, seen[s]);
        for (int i = 0; i < 8 && distinct; ++i)
            for (int j = i + 1; j < 8 && distinct; ++j)
                if (memcmp(seen[i], seen[j], CONN_ID_BYTES) == 0) distinct = false;
        check("all 8 slots of one match derive distinct conn_ids", distinct);
    }

    // ---- 6. SESSION_INFO v4 round-trip ------------------------------------------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        SessionInfo         si              = make_session("MH Host", 0xdeadbeefu, ID, 1251);
        unsigned char       buf[SESSION_INFO_MAX_ENCODED];
        size_t              n = session_info_encode(si, buf);

        check("the advert declares format 5", buf[0] == 5);
        check("SESSION_INFO_FORMAT is 5", SESSION_INFO_FORMAT == 5);
        // The record is its parts and nothing else -- 11 scalars + "MH Host"(1+7) + "TUTORIAL.MP"(1+11)
        // + 0x17c header + the 16-byte id + the 2-byte codepage + X2's 8-byte map hash and 4-byte size.
        check("the encoded length is id + codepage + map identity longer than v2",
              n == 11 + 8 + 12 + MAP_HEADER_SIZE + UUID7_BYTES + 2 + MAP_HASH_BYTES + 4);
        // EACH TRAILING FIELD AT ITS OWN OFFSET, counted from the END. Every bump appends, so this
        // is the assertion that says the earlier fields did not move -- which is the whole reason
        // the layout grows that way.
        check("the map SIZE is the LAST 4 bytes on the wire, little-endian",
              buf[n - 4] == (MAPSZ & 0xff) && buf[n - 3] == ((MAPSZ >> 8) & 0xff) &&
                  buf[n - 2] == ((MAPSZ >> 16) & 0xff) && buf[n - 1] == ((MAPSZ >> 24) & 0xff));
        check("the map content HASH is the 8 bytes before it",
              memcmp(buf + n - 4 - MAP_HASH_BYTES, MAPH, MAP_HASH_BYTES) == 0);
        check("the codepage is the 2 bytes before that, little-endian",
              buf[n - 4 - MAP_HASH_BYTES - 2] == (1251 & 0xff) &&
                  buf[n - 4 - MAP_HASH_BYTES - 1] == (1251 >> 8));
        check("the id is the 16 bytes before that",
              memcmp(buf + n - 4 - MAP_HASH_BYTES - 2 - UUID7_BYTES, ID, UUID7_BYTES) == 0);

        SessionInfo back;
        check("a v5 advert decodes", session_info_decode(buf, n, back));
        check("the map content claim survives the round-trip",
              map_hash_equal(back.map_hash, MAPH) && back.map_size == MAPSZ);
        check("the match_id survives the round-trip", memcmp(back.match_id, ID, UUID7_BYTES) == 0);
        check("the codepage survives the round-trip", back.codepage == 1251);
        check("the pre-existing fields still survive",
              back.tag == si.tag && strcmp(back.name, si.name) == 0 && strcmp(back.map, si.map) == 0 &&
                  back.has_map_header && memcmp(back.map_header, si.map_header, MAP_HEADER_SIZE) == 0);

        // Truncation: an advert cut short of its trailing fields must not be accepted WITH half of
        // one. Cutting inside the codepage leaves it 0; cutting inside the id leaves the id nil too.
        SessionInfo cut;
        session_info_decode(buf, n - 1, cut);
        check("an advert truncated inside the map identity leaves it a NO CLAIM",
              map_hash_is_none(cut.map_hash) && cut.map_size == 0);
        check("...and the codepage in front of it still reads correctly", cut.codepage == 1251);
        SessionInfo cut2;
        session_info_decode(buf, n - MAP_HASH_BYTES - 4 - 1, cut2);
        check("an advert truncated inside the codepage leaves it 0", cut2.codepage == 0);
        SessionInfo cut3;
        session_info_decode(buf, n - MAP_HASH_BYTES - 4 - 4, cut3);
        check("an advert truncated inside the id leaves it nil", uuid7_is_nil(cut3.match_id));

        // A v2 advert (an older host) still lists, with no id and no codepage -- the compatibility
        // half of both bumps, asserted at the oldest version that still carries a map header.
        unsigned char v2[SESSION_INFO_MAX_ENCODED];
        memcpy(v2, buf, n);
        v2[0]          = 2;
        size_t      n2 = n - UUID7_BYTES - 2 - MAP_HASH_BYTES - 4;
        SessionInfo o;
        check("a v2 advert still decodes", session_info_decode(v2, n2, o));
        check("a v2 advert carries a nil match_id", uuid7_is_nil(o.match_id));
        check("a v2 advert declares no codepage", o.codepage == 0);
        check("a v2 advert makes no map content claim", map_hash_is_none(o.map_hash));

        // A v3 advert (a SES0-era host that predates F3) lists with its id and no codepage -- the
        // case F3's own bump has to keep working, and the one join_admit reads as "no claim".
        unsigned char v3[SESSION_INFO_MAX_ENCODED];
        memcpy(v3, buf, n);
        v3[0]          = 3;
        size_t      n3 = n - 2 - MAP_HASH_BYTES - 4;
        SessionInfo o3;
        check("a v3 advert still decodes", session_info_decode(v3, n3, o3));
        check("a v3 advert keeps its match_id", memcmp(o3.match_id, ID, UUID7_BYTES) == 0);
        check("a v3 advert declares no codepage", o3.codepage == 0);
        check("a v3 advert makes no map content claim", map_hash_is_none(o3.map_hash));

        // A v4 advert (a pre-X2 host) keeps its id and its codepage and declares NO MAP CLAIM. The
        // distinction `map_hash_is_none` draws is the load-bearing one: the field reads as "this
        // host said nothing about its map", never as "a map whose bytes hash to zero".
        unsigned char v4[SESSION_INFO_MAX_ENCODED];
        memcpy(v4, buf, n);
        v4[0]          = 4;
        size_t      n4 = n - MAP_HASH_BYTES - 4;
        SessionInfo o4;
        check("a v4 advert still decodes", session_info_decode(v4, n4, o4));
        check("a v4 advert keeps its codepage", o4.codepage == 1251);
        check("a v4 advert makes no map content claim",
              map_hash_is_none(o4.map_hash) && o4.map_size == 0);

        check("an unknown FUTURE format is refused", (buf[0] = 6, !session_info_decode(buf, n, o)));
        buf[0] = 5;
    }

    // ---- 7. THE NEGATIVE CASE (a): a pre-SES0 DECODER refuses a v3 advert -------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        SessionInfo         si              = make_session("MH Host", 0x1234u, ID);
        unsigned char       buf[SESSION_INFO_MAX_ENCODED];
        size_t              n = session_info_encode(si, buf);
        check("an old client REFUSES a v5 advert (rather than mis-parsing it)", !old_decoder_accepts(buf, n));
        // ...and the guard is not vacuous: the same rule accepts what it was written to accept.
        buf[0] = 2;
        check("the same old rule still accepts a v2 advert", old_decoder_accepts(buf, n));
    }

    // ---- 8. JOIN v5 round-trip --------------------------------------------------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        SessionInfo         si              = make_session("MH Host", 0x5150u, ID, 1251);
        JoinRequest         jr              = join_request_for(si, 1250);
        memcpy(jr.player_name, "Ripley", 7);
        check("join_request_for echoes the advert's match_id", memcmp(jr.match_id, ID, UUID7_BYTES) == 0);
        // THE ONE THING THIS FIELD MUST NOT DO. If join_request_for copied the ADVERT's codepage the
        // way it copies the advert's id, every mismatch on the wire would arrive looking like
        // agreement and the refusal below could never fire. So the codepage argument is the JOINER's.
        check("join_request_for carries the JOINER's codepage, not the advert's", jr.codepage == 1250);
        check("the one-argument form declares no pin", join_request_for(si).codepage == 0);

        unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
        size_t        n = join_request_encode(jr, buf);
        check("the JOIN declares format 5", buf[0] == 5);
        check("JOIN_REQUEST_FORMAT is 5", JOIN_REQUEST_FORMAT == 5);
        check("the JOIN is id + codepage + map hash longer than v2",
              n == 1 + 4 + 8 + 7 + UUID7_BYTES + 2 + MAP_HASH_BYTES);

        JoinRequest back;
        check("a v5 JOIN decodes", join_request_decode(buf, n, back));
        check("the echoed match_id survives", memcmp(back.match_id, ID, UUID7_BYTES) == 0);
        check("the joiner's codepage survives", back.codepage == 1250);
        check("the player name still survives", strcmp(back.player_name, "Ripley") == 0);
        check("decode reports the format it read", back.format == 5);

        // X2's field, and the identical rule the codepage above is asserted under, one field along:
        // the two-argument form declares NOTHING, which is not the same as declaring the advert's
        // value. A form that echoed would make every disagreement arrive looking like agreement.
        check("the two-argument join_request_for declares no map", map_hash_is_none(jr.map_hash));
        {
            const unsigned char MINE[MAP_HASH_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8};
            JoinRequest         withmap              = join_request_for(si, 1250, MINE);
            check("join_request_for carries the JOINER's map hash, not the advert's",
                  map_hash_equal(withmap.map_hash, MINE) &&
                      !map_hash_equal(withmap.map_hash, si.map_hash));
            unsigned char mb[JOIN_REQUEST_MAX_ENCODED];
            size_t        mn = join_request_encode(withmap, mb);
            JoinRequest   mback;
            check("the joiner's map hash survives the round-trip",
                  join_request_decode(mb, mn, mback) && map_hash_equal(mback.map_hash, MINE));
        }

        // A v5 JOIN cut short of ANY trailing field is MALFORMED, not "a JOIN with no map claim" --
        // at v5 all three are required, so a short one is a corrupt frame and must not be seated.
        // The asymmetry with the ADVERT, whose trailing fields are optional, is deliberate: an
        // advert is read from anyone, a JOIN is a request to be given a seat.
        JoinRequest shortj;
        check("a v5 JOIN truncated inside its map hash is refused",
              !join_request_decode(buf, n - 1, shortj));
        check("a v5 JOIN truncated inside its codepage is refused",
              !join_request_decode(buf, n - MAP_HASH_BYTES - 1, shortj));
        check("a v5 JOIN truncated inside its id is refused",
              !join_request_decode(buf, n - MAP_HASH_BYTES - 4, shortj));
    }

    // ---- 9. THE NEGATIVE CASE (b): the host's ADMIT decision --------------------------------------
    {
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        SessionInfo         mine            = make_session("MH Host", 0x5150u, ID, 1251);

        // (i) the good path -- a current client naming this lobby, with the SAME pin.
        {
            JoinRequest jr = join_request_for(mine, 1251);
            memcpy(jr.player_name, "Ripley", 7);
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = join_request_encode(jr, buf);
            JoinRequest   got;
            JoinAdmit     v = join_admit(buf, n, mine, got);
            check("a current JOIN naming this lobby is ADMITTED", v == JoinAdmit::Admit);
            check("the admitted request carries the player name", strcmp(got.player_name, "Ripley") == 0);
            check("Admit's reason string is ok", strcmp(join_admit_reason(v), "ok") == 0);
        }

        // (ii) THE TRACKER'S NEGATIVE CASE: a pre-SES0 (v2) client. It parses perfectly -- that is
        // the danger -- and must still be refused, by version, with a reason the log can name.
        {
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = encode_join_v2(buf, mine.tag, mine.name, "Ripley");
            JoinRequest   got;
            JoinAdmit     v = join_admit(buf, n, mine, got);
            check("a pre-SES0 JOIN naming the RIGHT lobby is REFUSED on protocol", v == JoinAdmit::RefusedOldProtocol);
            check("the refusal reason names the missing id",
                  strcmp(join_admit_reason(v), "old protocol (no match_id)") == 0);
            check("the refused request still carries its lobby-id (so the log can name it)",
                  got.tag == mine.tag && strcmp(got.name, mine.name) == 0);
            check("the refused request reports the old format", got.format == 2);
            check("the refused request has no match_id", uuid7_is_nil(got.match_id));
            // Non-vacuous: the SAME bytes with the format byte raised are admitted only if the id is
            // there too, so the refusal is about the version and not about the byte count.
            check("bumping only the format byte does NOT sneak a v2 JOIN in",
                  (buf[0] = JOIN_REQUEST_FORMAT, join_admit(buf, n, mine, got) == JoinAdmit::RefusedMalformed));
        }

        // (ii-b) F3: a SES0-era (v3) client. It has a match_id and parses perfectly -- and is still
        // refused, because it declares no codepage and the host has a pin. "Assume it agrees" is the
        // silent corruption the field exists to prevent, so the version floor does the refusing.
        {
            JoinRequest   jr = join_request_for(mine, 1251);
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = join_request_encode(jr, buf);
            buf[0]          = 3; // a v3 client's format byte...
            JoinRequest got;
            JoinAdmit   v = join_admit(buf, n - 2, mine, got); // ...and a v3 client's shorter frame
            check("a v3 (pre-F3) JOIN is refused on protocol", v == JoinAdmit::RefusedOldProtocol);
            check("the v3 refusal still yields the lobby-id for the log",
                  got.tag == mine.tag && strcmp(got.name, mine.name) == 0);
            check("JOIN_REQUEST_MIN_FORMAT is 5", JOIN_REQUEST_MIN_FORMAT == 5);
        }

        // (ii-c) F3's OWN NEGATIVE CASE: a current client, our lobby, DIFFERENT pinned codepage.
        // This is the one the rig cannot stage -- it would take two differently-localised Windows
        // installs -- and it is the whole reason the decision is a pure function.
        {
            JoinRequest jr = join_request_for(mine, 1250); // CP1250 joiner vs our CP1251 lobby
            memcpy(jr.player_name, "Ripley", 7);
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = join_request_encode(jr, buf);
            JoinRequest   got;
            JoinAdmit     v = join_admit(buf, n, mine, got);
            check("a JOIN with a DIFFERENT pinned codepage is refused", v == JoinAdmit::RefusedCodepage);
            check("the codepage refusal names itself",
                  strcmp(join_admit_reason(v), "input codepage mismatch") == 0);
            check("the refused request carries BOTH values for the log",
                  got.codepage == 1250 && mine.codepage == 1251);
            check("the refused request still carries its lobby-id",
                  got.tag == mine.tag && strcmp(got.name, mine.name) == 0);
            // Non-vacuous twice over: the same frame with the pin corrected is admitted, and a
            // v4 client that declares NO pin is still refused against a host that has one (0 is "no
            // claim", and no claim is not a match).
            JoinRequest ok = join_request_for(mine, 1251);
            memcpy(ok.player_name, "Ripley", 7);
            n = join_request_encode(ok, buf);
            check("the same JOIN with the matching pin is admitted", join_admit(buf, n, mine, got) == JoinAdmit::Admit);
            JoinRequest none = join_request_for(mine, 0);
            n                = join_request_encode(none, buf);
            check("a v4 JOIN declaring NO pin is refused by a pinned host",
                  join_admit(buf, n, mine, got) == JoinAdmit::RefusedCodepage);
            // ...and the compatibility direction: a host with no pin of its own admits anyone. Without
            // this a peer built before F3's ini key existed would be locked out by a field neither
            // side ever set.
            SessionInfo unpinned = make_session("MH Host", 0x5150u, ID, 0);
            JoinRequest any      = join_request_for(unpinned, 1251);
            n                    = join_request_encode(any, buf);
            check("an UNPINNED host admits a pinned joiner",
                  join_admit(buf, n, unpinned, got) == JoinAdmit::Admit);
        }

        // (iii) a client NEWER than this host.
        {
            JoinRequest   jr = join_request_for(mine, 1251);
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = join_request_encode(jr, buf);
            buf[0]          = (unsigned char)(JOIN_REQUEST_FORMAT + 1);
            JoinRequest got;
            JoinAdmit   v = join_admit(buf, n, mine, got);
            check("a JOIN from a NEWER client is refused as such", v == JoinAdmit::RefusedNewerProtocol);
            check("the newer-client verdict reports the format it saw", got.format == JOIN_REQUEST_FORMAT + 1);
        }

        // (iv) the wrong lobby -- unchanged behaviour, asserted so the version work cannot have
        // quietly widened admission.
        {
            SessionInfo   other = make_session("MH Host", 0x9999u, ID, 1251);
            JoinRequest   jr    = join_request_for(other, 1251);
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = join_request_encode(jr, buf);
            JoinRequest   got;
            check("a JOIN for a different TAG is refused", join_admit(buf, n, mine, got) == JoinAdmit::RefusedWrongLobby);

            SessionInfo named = make_session("Other Game", mine.tag, ID, 1251);
            jr                = join_request_for(named, 1251);
            n                 = join_request_encode(jr, buf);
            check("a JOIN for a different NAME is refused", join_admit(buf, n, mine, got) == JoinAdmit::RefusedWrongLobby);

            // ORDER MATTERS between the two new-ish refusals: a peer dialling SOMEBODY ELSE'S lobby
            // is refused for that, not for its codepage. Otherwise the log would send the reader to
            // an ini file over what is really a stale browser entry.
            SessionInfo elsewhere = make_session("Other Game", 0x9999u, ID, 1251);
            jr                    = join_request_for(elsewhere, 1250);
            n                     = join_request_encode(jr, buf);
            check("the wrong lobby is refused as the wrong lobby even with a codepage mismatch",
                  join_admit(buf, n, mine, got) == JoinAdmit::RefusedWrongLobby);
        }

        // (v) junk.
        {
            unsigned char junk[4] = {0, 0, 0, 0};
            JoinRequest   got;
            check("an empty JOIN is malformed", join_admit(junk, 0, mine, got) == JoinAdmit::RefusedMalformed);
            check("a JOIN with format 0 is malformed", join_admit(junk, 4, mine, got) == JoinAdmit::RefusedMalformed);
        }

        // (vi) a re-created lobby: the host re-mints BOTH the tag and the id, so a joiner still
        // holding the old advert is refused on the lobby test that already existed. This is the
        // offline shadow of the rig's host_recreate scenario.
        {
            unsigned char ID2[UUID7_BYTES];
            unsigned char r[UUID7_RAND_MAX];
            for (int i = 0; i < (int)sizeof(r); ++i) r[i] = (unsigned char)(0x10 + i);
            uuid7_make(1789646400500ULL, r, ID2);
            check("a re-created lobby's id differs from the first", memcmp(ID2, ID, UUID7_BYTES) != 0);

            SessionInfo   recreated = make_session("MH Host", 0x6161u, ID2, 1251);
            JoinRequest   stale     = join_request_for(mine, 1251); // the client's view is the OLD lobby
            unsigned char buf[JOIN_REQUEST_MAX_ENCODED];
            size_t        n = join_request_encode(stale, buf);
            JoinRequest   got;
            check("a JOIN for the PREVIOUS lobby is refused after a re-create",
                  join_admit(buf, n, recreated, got) == JoinAdmit::RefusedWrongLobby);
        }
    }

    // ---- mp:F3c -- THE REFUSAL REACHES THE JOINER: the REFUSED announce and its text --------------
    // Until F3c a refused JOIN was a host log line and nothing else; the joiner sat in a lobby that
    // showed it seated. The reply rides FLAG_ANNOUNCE as a third kind, addressed by player id, with
    // the host's reason as text. This is the codec + the text; the rig scenario (codepage_refused)
    // proves the bounce and the notice on a real frame.
    {
        printf("--- F3c: the REFUSED announce ---\n");
        const unsigned char ID[UUID7_BYTES] = {0x01, 0xa0, 0xaf, 0x3c, 0xea, 0x00, 0x70, 0xa1,
                                               0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
        SessionInfo         mine            = make_session("MH Host", 0x5150u, ID, 1252);
        JoinRequest         theirs          = join_request_for(mine, 1251);
        char                reason[ANNOUNCE_TEXT_CAP];
        join_refusal_text(JoinAdmit::RefusedCodepage, mine, theirs, reason, sizeof(reason));
        check("the codepage refusal text names BOTH codepages, host first",
              strcmp(reason, "codepage 1252/1251") == 0);
        // THE SCREEN BUDGET: the joiner draws "Join refused: " + this on a 32-character status
        // line, so the widest form of every branch must fit JOIN_REFUSAL_TEXT_MAX.
        check("the codepage text fits the screen budget", strlen(reason) <= JOIN_REFUSAL_TEXT_MAX);
        SessionInfo wide  = make_session("MH Host", 0x5150u, ID, 65001);
        JoinRequest widej = join_request_for(wide, 65000);
        join_refusal_text(JoinAdmit::RefusedCodepage, wide, widej, reason, sizeof(reason));
        check("the WIDEST codepage text (two 5-digit values) still fits", strlen(reason) <= JOIN_REFUSAL_TEXT_MAX);
        theirs.format = 3;
        join_refusal_text(JoinAdmit::RefusedOldProtocol, mine, theirs, reason, sizeof(reason));
        check("the old-protocol text names the format seen and the minimum",
              strcmp(reason, "protocol 3 < 5") == 0 && strlen(reason) <= JOIN_REFUSAL_TEXT_MAX);
        theirs.format = 250;
        join_refusal_text(JoinAdmit::RefusedNewerProtocol, mine, theirs, reason, sizeof(reason));
        check("the newer-protocol text names the format seen and the maximum",
              strcmp(reason, "protocol 250 > 5") == 0 && strlen(reason) <= JOIN_REFUSAL_TEXT_MAX);
        join_refusal_text(JoinAdmit::RefusedWrongLobby, mine, theirs, reason, sizeof(reason));
        check("the wrong-lobby text is the admit reason itself, within budget",
              strcmp(reason, "not our lobby") == 0 && strlen(reason) <= JOIN_REFUSAL_TEXT_MAX);

        unsigned char ab[ANNOUNCE_MAX_ENCODED];
        size_t        n = announce_refused_encode(3, "codepage 1252/1251", ab);
        check("REFUSED announce: kind byte, then the target id", ab[0] == ANNOUNCE_REFUSED && ab[1] == 3);
        check("REFUSED announce: text is NUL-terminated inside the frame", n == 2 + 18 + 1 && ab[n - 1] == 0);
        unsigned char target = 0;
        char          got[ANNOUNCE_TEXT_CAP];
        check("REFUSED announce decodes to the same target + text",
              announce_refused_decode(ab, n, &target, got, sizeof(got)) && target == 3 &&
                  strcmp(got, "codepage 1252/1251") == 0);
        // Negatives: a U16 join/left announce is NOT a refusal (kind byte); a frame whose text runs
        // to the end without a NUL is truncated, not a long reason; and an over-long reason is cut
        // at the cap rather than overrunning the frame.
        unsigned char u16[6] = {ANNOUNCE_JOINED, 2, 'b', 'o', 'b', 0};
        check("a JOINED announce is not decoded as a refusal",
              !announce_refused_decode(u16, 6, &target, got, sizeof(got)));
        check("a REFUSED frame with no NUL inside `len` is refused",
              !announce_refused_decode(ab, n - 1, &target, got, sizeof(got)));
        check("a 2-byte REFUSED frame (no text at all) is refused",
              !announce_refused_decode(ab, 2, &target, got, sizeof(got)));
        char longr[200];
        memset(longr, 'x', sizeof(longr) - 1);
        longr[sizeof(longr) - 1] = 0;
        n                        = announce_refused_encode(1, longr, ab);
        check("an over-long reason is truncated to the cap, NUL included",
              n == ANNOUNCE_MAX_ENCODED && ab[n - 1] == 0);
        // Every peer receives the broadcast; only the addressed one acts. That gate is the seam's
        // (net_seams.cpp on_announce_recv compares [1] with MH_Net_LocalPlayerId) -- what the codec
        // guarantees is that [1] survives the round trip, checked above.
    }

    printf("=== sessionidtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
