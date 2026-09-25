//
// map_transfer_selftest.cpp -- `net_selftest.exe maptest`: mp:X2, the MAP DOWNLOAD, with no game.
//
// WHY THIS SUITE EXISTS, stated as what the rig CANNOT show rather than as coverage. X2's four
// acceptance clauses are all rig clauses, and a red rig run says only "the two peers did not agree
// about a map": not whether the hash was computed over the wrong bytes, the stored name was built
// wrong, the resolver preferred a name over a content, the Start gate's predicate was inverted, or
// the delivered blob was written without being re-checked. Every one of those is a decision, and
// every one is a pure function here.
//
// THREE OF THE ARMS ARE THINGS A RIG CAN ONLY SHOW BY ABSENCE, which is the weakest evidence there
// is:
//   * "a joiner already holding the content transfers NOTHING" -- on the rig that is a counter that
//     stayed at zero, which is equally consistent with a transfer path that is simply broken. Here
//     it is `host_next_peer_needing_map() == -1` with a seated peer whose report matched, and the
//     arm right beside it shows the same function returning that peer's id when it did not.
//   * "the joiner's own file is byte-unchanged afterwards" -- the rig compares two log lines; here
//     the bytes are compared against a witness copy held in memory, which is the claim itself.
//   * "a delivered map that does not match the advert is refused" -- there is no way to make a
//     correct host send the wrong file, so the only place that refusal can be driven is here.
//
// NO SOCKET IN ARMS A..E; arm W is the one that puts a real map-sized file across two real UDP
// endpoints through mp:X1's snapshot pipeline, because "the transfer is the snapshot pipeline" is a
// claim worth executing once rather than assuming from the call site.
//
#include <winsock2.h>
#include <windows.h>

#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mh_net_udp/udp_endpoint.h"
#include "../mh_net_udp/udp_snapshot.h"

#include "mh_net_proto/net_crypto.h"
#include "mh_net_proto/session_info.h"
#include "seams/map_transfer.h"

namespace {

using mh::netudp::Config;
using mh::netudp::Endpoint;
using mh::netudp::bulk::CHUNK_BYTES;
namespace snap = mh::netudp::snapshot;
namespace maps = mh::seams::maps;
using namespace mh_net_proto;

int g_checks = 0, g_fails = 0;

void checkf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    b[500];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", b);
    }
}

// ---- the scratch directory ----------------------------------------------------------------------
//
// A REAL DIRECTORY WITH REAL FILES, not a mocked filesystem, and the reason is the subject: the thing
// under test is "what is on disk under this name", so a mock would be asserting that the mock agrees
// with itself. It is created under %TEMP% per run and removed at the end.
char g_dl[MAX_PATH];  // the suite's download directory: maps::set_dl_dir points the seam here
char g_dir[MAX_PATH]; // always ends in a backslash -- maps::* concatenates without inserting one

void scratch_make() {
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    wsprintfA(g_dir, "%smh_maptest_%lu\\", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_dir, nullptr);
    wsprintfA(g_dl, "%sdl\\", g_dir);
    maps::set_dl_dir(g_dl); // the seam writes downloads here instead of `mh_dl\\`
}

void scratch_path(char *out, size_t cap, const char *name) {
    wsprintfA(out, "%s%s", g_dir, name);
    (void)cap;
}

bool write_scratch(const char *name, const void *bytes, uint32_t len) {
    char path[MAX_PATH];
    scratch_path(path, sizeof(path), name);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, bytes, len, &w, nullptr);
    CloseHandle(h);
    return w == len;
}

void delete_scratch(const char *name) {
    char path[MAX_PATH];
    scratch_path(path, sizeof(path), name);
    DeleteFileA(path);
}

// ---- synthetic "map" content --------------------------------------------------------------------
//
// Two files of the SAME LENGTH and different bytes. The equal length is deliberate: a resolver that
// compared sizes instead of hashing would pass a lazier fixture, and a same-name-different-content
// pair in the wild is very often the same map lightly edited.
constexpr uint32_t MAPLEN = 180u * 1024u; // between the stock set's 115,943 and 467,065 bytes

uint8_t *g_host_map = nullptr; // what the host holds
uint8_t *g_mine_map = nullptr; // the joiner's own, different, same-named file
uint8_t *g_rx       = nullptr; // the delivery buffer

void fill(uint8_t *p, uint32_t n, uint32_t seed) {
    uint32_t x = seed;
    for (uint32_t i = 0; i < n; ++i) {
        x    = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

void hash_of(const void *p, uint32_t n, uint8_t out[MAP_HASH_BYTES]) {
    uint8_t full[SHA256_LEN];
    sha256((const uint8_t *)p, n, full);
    map_hash_from_sha256(full, out);
}

// THE DOWNLOAD DIRECTORY THIS SUITE USES, and the path a store lands at. The seam's default is
// `mh_dl\` beside the executable -- deliberately NOT under the map directory, because
// `llm_mp_mappicker_populate_list` scans `Maps\` for SUB-FOLDERS before it scans for `*.mpm` and a
// download folder there becomes the picker's pre-selected first row. The suite points the same
// knob at its own scratch so the arms below can assert both halves: the content-addressed NAME,
// and that nothing was written beside the player's file. Arm A still calls map_stored_name
// directly -- that is the CODEC's shape, and this is where the file goes.
const char *maps_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; ++q)
        if (*q == '\\' || *q == '/') b = q + 1;
    return b;
}

void stored_path(const char *base, const uint8_t h[MAP_HASH_BYTES], char *out, std::size_t cap) {
    char name[MAP_STORED_NAME_CAP];
    map_stored_name(base, h, name, sizeof(name));
    wsprintfA(out, "%s%s", g_dl, name);
    (void)cap;
}

// ---- arm W's link ---------------------------------------------------------------------------------
const unsigned char MT_PSK[32] = {'M', 'H', 't', 'e', 's', 't', 'k', 'e', 'y', ' ', 'm',
                                  'a', 'p', ' ', 'X', '2', ' ', 'f', 'i', 'x', 'e', 'd',
                                  '!', '!', '!', '!', '!', '!', '!', '!', '!', '!'};

Endpoint       g_ep_host, g_ep_cl;
snap::Sender   g_tx;
snap::Receiver g_rxr;

void mt_cfg(Config &c, int role, int port, unsigned short bind_port) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port          = port;
    c.net.player_id     = (role == 0) ? 0 : 1;
    c.net.log           = 1;
    c.net.host_assign   = 0;
    c.net.ping_ms       = 200;
    c.net.rx_timeout_ms = -1;
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

template <typename Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(5);
    }
}

} // namespace

int run_maptest(int port);

int run_maptest(int port) {
    // +400, the next free slot after udpbulktest's +200 and udpsnaptest's +300, so three suites that
    // each stand up loopback endpoints never collide when the gate runs them back to back.
    port += 400;
    printf("=== maptest (mp:X2 -- the map download: identity, naming, resolve, the gate) ===\n");
    scratch_make();
    printf("  scratch: %s\n", g_dir);

    g_host_map = (uint8_t *)VirtualAlloc(nullptr, MAPLEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_mine_map = (uint8_t *)VirtualAlloc(nullptr, MAPLEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_rx       = (uint8_t *)VirtualAlloc(nullptr, MAPLEN * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_host_map || !g_mine_map || !g_rx) {
        printf("  FAIL: could not allocate the fixtures\n");
        return 1;
    }
    fill(g_host_map, MAPLEN, 0xC01DBEEFu);
    fill(g_mine_map, MAPLEN, 0x5A1710ADu); // a different map, byte for byte, same LENGTH

    const char *BASE = "Cold War.mpm";
    uint8_t     host_h[MAP_HASH_BYTES], mine_h[MAP_HASH_BYTES];
    hash_of(g_host_map, MAPLEN, host_h);
    hash_of(g_mine_map, MAPLEN, mine_h);
    checkf(!map_hash_equal(host_h, mine_h), "the two fixtures hash differently (the whole premise)");

    // ---- arm A: THE NAME ------------------------------------------------------------------------
    // The stored name is the one string three places have to agree on -- the writer, the resolver and
    // a human reading a directory listing -- so it is built in mh_net_proto and only there.
    {
        char n[MAP_STORED_NAME_CAP];
        checkf(map_stored_name(BASE, host_h, n, sizeof(n)) != 0, "A: the stored name builds");
        char hex[MAP_HASH_HEX_CAP];
        map_hash_hex(host_h, hex, sizeof(hex));
        char expect[MAP_STORED_NAME_CAP];
        wsprintfA(expect, "Cold War.%s.mpm", hex);
        checkf(lstrcmpA(n, expect) == 0, "A: it is '%s' (got '%s')", expect, n);
        checkf(lstrcmpiA(n, BASE) != 0, "A: and it is NEVER the base name -- rule 1 of the header");

        // The hash goes before the LAST dot, so a dotted stem keeps its real extension. That matters
        // because the extension is what the loader reads to choose Maps\ over Dane\.
        char n2[MAP_STORED_NAME_CAP];
        map_stored_name("v1.2 map.mpm", host_h, n2, sizeof(n2));
        wsprintfA(expect, "v1.2 map.%s.mpm", hex);
        checkf(lstrcmpA(n2, expect) == 0, "A: a dotted stem keeps its extension ('%s')", n2);

        char n3[MAP_STORED_NAME_CAP];
        map_stored_name("MYMAP", host_h, n3, sizeof(n3));
        wsprintfA(expect, "MYMAP.%s", hex);
        checkf(lstrcmpA(n3, expect) == 0, "A: an extension-less base gets the hash appended");

        // A cap that cannot hold the name REFUSES rather than truncating or falling back, because
        // the fallback would be the base name and that is the one file we must not write.
        char tiny[8]; // NOT `small` -- windows.h's rpcndr.h #defines that to `char`
        checkf(map_stored_name(BASE, host_h, tiny, sizeof(tiny)) == 0,
               "A: too small a buffer REFUSES (it does not fall back to the base name)");
        checkf(map_stored_name("", host_h, n, sizeof(n)) == 0, "A: an empty base refuses");

        checkf(!map_name_is_stored_form(n3, BASE), "A: a DIFFERENT stem's stored name is not one of '%s'", BASE);
        char good[MAP_STORED_NAME_CAP];
        map_stored_name(BASE, host_h, good, sizeof(good));
        checkf(map_name_is_stored_form(good, BASE), "A: the stored name IS recognised as one");
        checkf(!map_name_is_stored_form(BASE, BASE), "A: the BASE name is not a stored form of itself");
        checkf(!map_name_is_stored_form("Cold War.ZZZZZZZZZZZZZZZZ.mpm", BASE),
               "A: a non-hex infix is not a stored form");
        checkf(!map_name_is_stored_form("Cold War.deadbeef.mpm", BASE),
               "A: a SHORT hex infix is not a stored form (the length is part of the shape)");
        // The character-class trap this test exists for: a base containing 'f' must not turn its own
        // 'f' into a wildcard (the first implementation compared against a rebuilt sample string).
        checkf(!map_name_is_stored_form("wolX.0011223344556677.mpm", "wolf.mpm"),
               "A: a base's own letters are compared literally, not as hex wildcards");
    }

    // ---- arm B: THE WIRE ------------------------------------------------------------------------
    // SESSION_INFO v5 and JOIN v5, and above all what an OLDER record decodes to: the no-claim zero,
    // which map_hash_is_none() reports as "this host said nothing" and never as a real hash.
    {
        SessionInfo si;
        si.tag = 0x1234u;
        lstrcpynA(si.name, "MH Host", sizeof(si.name));
        lstrcpynA(si.map, BASE, sizeof(si.map));
        si.has_map_header = true;
        si.codepage       = 1250;
        memcpy(si.map_hash, host_h, MAP_HASH_BYTES);
        si.map_size = MAPLEN;

        uint8_t     buf[SESSION_INFO_MAX_ENCODED];
        std::size_t n = session_info_encode(si, buf);
        SessionInfo got;
        checkf(session_info_decode(buf, n, got), "B: a v5 advert round-trips");
        checkf(map_hash_equal(got.map_hash, host_h), "B: ...carrying the map hash");
        checkf(got.map_size == MAPLEN, "B: ...and the map size");
        checkf(lstrcmpA(got.map, BASE) == 0, "B: ...beside the map NAME, which is still the label");

        // A v4 advert, written out as a v4 encoder would have: everything up to the codepage, and
        // then nothing. The decoder must read it and report NO CLAIM, so a pre-X2 host still lists.
        uint8_t v4[SESSION_INFO_MAX_ENCODED];
        memcpy(v4, buf, n);
        v4[0]                = 4;
        const std::size_t n4 = n - MAP_HASH_BYTES - 4;
        SessionInfo       got4;
        checkf(session_info_decode(v4, n4, got4), "B: a v4 advert still decodes");
        checkf(map_hash_is_none(got4.map_hash), "B: ...as NO CLAIM, not as a hash of zero");
        checkf(got4.map_size == 0, "B: ...with no size either");
        checkf(got4.codepage == 1250, "B: ...and its own last field intact (the prefix is stable)");

        // A v6 advert -- a build newer than this one -- is refused outright rather than parsed for
        // the fields it happens to share. That is the same rule every earlier bump set.
        uint8_t v6[SESSION_INFO_MAX_ENCODED];
        memcpy(v6, buf, n);
        v6[0] = SESSION_INFO_FORMAT + 1;
        SessionInfo got6;
        checkf(!session_info_decode(v6, n, got6), "B: a NEWER advert is refused, not best-effort read");

        // The JOIN carries what the JOINER holds -- never an echo of the advert.
        JoinRequest jr = join_request_for(si, 1250, mine_h);
        lstrcpynA(jr.player_name, "Bob", sizeof(jr.player_name));
        checkf(map_hash_equal(jr.map_hash, mine_h) && !map_hash_equal(jr.map_hash, si.map_hash),
               "B: the JOIN reports OUR hash, not the host's (an echo would hide every mismatch)");
        JoinRequest none = join_request_for(si, 1250);
        checkf(map_hash_is_none(none.map_hash),
               "B: the two-argument form declares NOTHING rather than the advert's value");

        uint8_t           jbuf[JOIN_REQUEST_MAX_ENCODED];
        const std::size_t jn = join_request_encode(jr, jbuf);
        JoinRequest       jgot;
        checkf(join_request_decode(jbuf, jn, jgot), "B: a v5 JOIN round-trips");
        checkf(map_hash_equal(jgot.map_hash, mine_h), "B: ...carrying the joiner's own map hash");
        checkf(join_admit(jbuf, jn, si, jgot) == JoinAdmit::Admit, "B: ...and it is admitted");

        // A v4 JOIN never says which map it holds, so the Start gate's predicate would be vacuously
        // true for that peer. Refused by name, on the same rule that refused a pre-codepage client.
        uint8_t j4[JOIN_REQUEST_MAX_ENCODED];
        memcpy(j4, jbuf, jn);
        j4[0] = 4;
        JoinRequest j4got;
        checkf(join_admit(j4, jn - MAP_HASH_BYTES, si, j4got) == JoinAdmit::RefusedOldProtocol,
               "B: a v4 JOIN (no map report) is refused as an OLD PROTOCOL, by name");
    }

    // ---- arm C: THE FILE LAYER + THE RESOLVER ----------------------------------------------------
    // Each step changes ONE thing on disk and re-asks, so a resolver that answered from a cached
    // decision (or from a name) would disagree with the very next line.
    {
        uint8_t  h[MAP_HASH_BYTES];
        uint32_t sz = 0;
        char     out[maps::STORED_PATH_CAP];

        delete_scratch(BASE);
        checkf(!maps::file_hash(g_dir, BASE, h, &sz), "C: an absent file has no hash");
        checkf(maps::resolve(g_dir, BASE, host_h, out, sizeof(out)) == maps::Resolve::Missing,
               "C: ...and nothing local holds the content");

        // The joiner's OWN file, same name, different bytes. This must NOT satisfy the wanted hash.
        checkf(write_scratch(BASE, g_mine_map, MAPLEN), "C: wrote the joiner's own file");
        checkf(maps::file_hash(g_dir, BASE, h, &sz) && sz == MAPLEN, "C: it hashes, at the right size");
        checkf(map_hash_equal(h, mine_h), "C: ...to its own content");
        checkf(maps::resolve(g_dir, BASE, host_h, out, sizeof(out)) == maps::Resolve::Missing,
               "C: A SAME-NAMED FILE OF DIFFERENT CONTENT IS NOT A MATCH -- the item's whole point");

        // The host's content, stored under its content-addressed name.
        char stored[maps::STORED_PATH_CAP];
        checkf(maps::store(BASE, host_h, g_host_map, MAPLEN, stored, sizeof(stored)),
               "C: the host's content stores");
        checkf(maps::resolve(g_dir, BASE, host_h, out, sizeof(out)) == maps::Resolve::Stored,
               "C: ...and now resolves to the STORED name");
        checkf(lstrcmpA(out, stored) == 0, "C: ...which is the file that was written");

        // AND IT WENT ONE DIRECTORY DOWN. Both halves matter and they are different claims: the
        // NAME must be the content-addressed form (that is the tracker's wording and what makes a
        // second download of the same content idempotent), and the LOCATION must be the `mh_dl\`
        // subdirectory (so the picker, which globs `Maps\*.mpm` without recursing, never lists it
        // and the shared rig `Maps` never gains a visible file).
        checkf(memcmp(stored, g_dl, lstrlenA(g_dl)) == 0,
               "C: the stored file lives in the DOWNLOAD directory, not among the player's maps");
        checkf(map_name_is_stored_form(maps_basename(stored), BASE),
               "C: ...under the content-addressed name");
        char beside[MAP_STORED_NAME_CAP];
        map_stored_name(BASE, host_h, beside, sizeof(beside));
        uint32_t nb = 0;
        uint8_t *pb = maps::read_file(g_dir, beside, MAPLEN * 2, &nb);
        checkf(pb == nullptr, "C: ...and NOTHING was written beside the base name");
        maps::free_bytes(pb);

        // AND THE JOINER'S OWN FILE IS BYTE-UNCHANGED. Compared against the witness still in memory,
        // which is the claim itself rather than a proxy for it.
        uint32_t n2  = 0;
        uint8_t *now = maps::read_file(g_dir, BASE, MAPLEN * 2, &n2);
        checkf(now != nullptr && n2 == MAPLEN && memcmp(now, g_mine_map, MAPLEN) == 0,
               "C: THE JOINER'S OWN FILE IS BYTE-FOR-BYTE UNCHANGED after the store");
        maps::free_bytes(now);

        // A local file that IS the content resolves to the base name and needs no redirect at all.
        checkf(write_scratch(BASE, g_host_map, MAPLEN), "C: overwrote the base with the host's content");
        checkf(maps::resolve(g_dir, BASE, host_h, out, sizeof(out)) == maps::Resolve::Base,
               "C: a base file that IS the content resolves to the base -- no redirect");
        checkf(write_scratch(BASE, g_mine_map, MAPLEN), "C: restored the joiner's own file");
    }

    // ---- arm D: THE REFUSALS AT THE WRITE --------------------------------------------------------
    // The transfer verifies that the bytes CROSSED intact. It cannot verify they are the bytes the
    // ADVERT named -- a different claim -- so store() re-checks, and this is the only place that
    // second check can be driven, because no correct host will ever send the wrong file.
    {
        char stored[maps::STORED_PATH_CAP];
        checkf(!maps::store(BASE, host_h, g_mine_map, MAPLEN, stored, sizeof(stored)),
               "D: content that does not hash to the claim is REFUSED at the write");
        checkf(!maps::store(BASE, host_h, g_host_map, 0, stored, sizeof(stored)),
               "D: a zero-length delivery is refused");
        checkf(!maps::store(BASE, host_h, nullptr, MAPLEN, stored, sizeof(stored)),
               "D: a null delivery is refused");
        // ...and the refusals left nothing behind: the stored name for `mine_h` must not exist.
        char ghost[maps::STORED_PATH_CAP];
        stored_path(BASE, mine_h, ghost, sizeof(ghost));
        uint32_t n = 0;
        uint8_t *p = maps::read_file("", ghost, MAPLEN * 2, &n);
        checkf(p == nullptr, "D: a refused store left NO file behind for the resolver to find");
        maps::free_bytes(p);
    }

    // ---- arm E: THE REDIRECT ---------------------------------------------------------------------
    // The redirect is what makes the load resolve by hash WITHOUT renaming the map in game memory --
    // which it must not do, because current_map_data reaches Planet[31] and `planets` is a hashed
    // determinism region. So the substitution has to be invisible above the file handle.
    {
        char stored[maps::STORED_PATH_CAP];
        stored_path(BASE, host_h, stored, sizeof(stored));
        char scratch[MAX_PATH], expect[MAX_PATH];

        maps::redirect_clear();
        checkf(lstrcmpA(maps::redirect_apply("Maps\\Cold War.mpm", scratch, sizeof(scratch)),
                        "Maps\\Cold War.mpm") == 0,
               "E: with nothing registered the path passes through UNCHANGED");

        maps::redirect_set(BASE, stored);
        lstrcpynA(expect, stored, sizeof(expect));
        checkf(lstrcmpA(maps::redirect_apply("Maps\\Cold War.mpm", scratch, sizeof(scratch)), expect) == 0,
               "E: the base name is served the stored file -- the WHOLE path, because the\n"
               "   download does not live in the map directory at all");
        checkf(lstrcmpA(maps::redirect_apply("Maps\\COLD WAR.MPM", scratch, sizeof(scratch)), expect) == 0,
               "E: ...case-insensitively, because Windows paths are");
        checkf(lstrcmpA(maps::redirect_apply("Msgs.dat", scratch, sizeof(scratch)), "Msgs.dat") == 0,
               "E: EVERY OTHER FILE THE GAME OPENS IS UNTOUCHED (this sits on the global open edge)");
        checkf(lstrcmpA(maps::redirect_apply("Maps\\Titi Taca.mpm", scratch, sizeof(scratch)),
                        "Maps\\Titi Taca.mpm") == 0,
               "E: ...including another map");
        checkf(lstrcmpA(maps::redirect_apply("Cold War.mpm", scratch, sizeof(scratch)), stored) == 0,
               "E: a bare name with no directory redirects too");
        maps::redirect_clear();
        checkf(lstrcmpA(maps::redirect_apply("Maps\\Cold War.mpm", scratch, sizeof(scratch)),
                        "Maps\\Cold War.mpm") == 0,
               "E: clearing it hands the name back");
    }

    // ---- arm F: THE START GATE -------------------------------------------------------------------
    // The gate is a predicate over what SEATED PEERS REPORTED, and the two halves are asserted
    // against each other: the same function must say "blocked, by Bob" and then "not blocked",
    // because a gate that is always shut and a gate that is always open both pass one of those.
    {
        maps::session_reset();
        maps::set_can_carry_for_test(1); // arm F is the UDP shape (channel C present); arm G is TCP's
        maps::host_set_claim_for_test(BASE, host_h, MAPLEN);
        char who[32];

        checkf(!maps::host_start_blocked(who, sizeof(who)),
               "F: with NO joiners seated the host may Start (the vacuous case is OPEN, not shut)");
        checkf(maps::host_next_peer_needing_map() == -1, "F: ...and nobody needs a transfer");

        maps::host_on_join(1, "Bob", mine_h); // Bob has a DIFFERENT file of the same name
        checkf(maps::host_start_blocked(who, sizeof(who)), "F: a joiner without the content SHUTS the gate");
        checkf(lstrcmpA(who, "Bob") == 0, "F: ...and the gate NAMES the peer ('%s')", who);
        checkf(maps::host_next_peer_needing_map() == 1, "F: ...and that peer is the transfer target");

        maps::host_on_join(2, "Eve", host_h); // Eve already holds it
        checkf(maps::host_next_peer_needing_map() == 1,
               "F: a peer that already holds the content is NOT chosen (one transfer at a time)");

        maps::host_on_join(1, "Bob", host_h); // Bob's completion report
        checkf(!maps::host_start_blocked(who, sizeof(who)),
               "F: once every seated peer reports our hash, the gate OPENS");
        checkf(maps::host_next_peer_needing_map() == -1,
               "F: A JOINER ALREADY HOLDING THE CONTENT TRANSFERS NOTHING -- nobody is chosen");

        // A peer that LEAVES must not hold the gate shut from beyond the grave.
        maps::host_on_join(3, "Mallory", mine_h);
        checkf(maps::host_start_blocked(who, sizeof(who)), "F: a third joiner without it shuts it again");
        maps::host_on_leave(3);
        checkf(!maps::host_start_blocked(who, sizeof(who)),
               "F: a DEPARTED peer does not hold the gate shut");

        // And with no claim at all (a stock map, or a host that could not read its file) the gate is
        // open for everyone -- the pre-X2 behaviour, which is what a no-claim must mean.
        maps::session_reset();
        uint8_t zero[MAP_HASH_BYTES] = {0};
        maps::host_set_claim_for_test(BASE, zero, 0);
        maps::host_on_join(1, "Bob", zero);
        checkf(!maps::host_start_blocked(who, sizeof(who)),
               "F: a host that makes NO CLAIM never blocks anyone (a stock map is install-identical)");
        maps::session_reset();
    }

    // ---- arm G: THE TCP SHAPE (mp:X2b) -----------------------------------------------------------
    // A transport with no channel C can never deliver a map, so a mismatch there is a REFUSAL, not a
    // wait -- and it must be one, because the first cut made no claim at all on TCP and a same-named
    // different map then desynced 8000 steps in. Asserted both ways: a mismatch (the
    // map_test_pretend=other shape) shuts the gate, names the peer, flags it unfetchable and arms no
    // transfer; a matching joiner opens it. A gate that is always shut on TCP passes the first half
    // and fails the second.
    {
        maps::session_reset();
        maps::set_can_carry_for_test(0);
        maps::host_set_claim_for_test(BASE, host_h, MAPLEN);
        char who[32];
        bool unf = false;

        maps::host_on_join(1, "Bob", mine_h); // same name, different content, over TCP
        checkf(maps::host_start_blocked(who, sizeof(who), &unf),
               "G: over TCP a joiner holding DIFFERENT content shuts the gate (refusal, not desync)");
        checkf(lstrcmpA(who, "Bob") == 0, "G: ...and names the peer ('%s')", who);
        checkf(unf, "G: ...and marks it UNFETCHABLE (the notice must not promise a download)");
        checkf(maps::host_next_peer_needing_map() == -1,
               "G: ...and NO transfer is ever armed on a link without channel C");
        // Wave 2 (G291): the widget's DISABLED bit is re-derived by retail every frame, so the
        // enforcement is the Start ACTIVATION -- launch.cpp's begin_map_load hook asks this.
        checkf(maps::host_refuse_start_click(),
               "G: ...and a Start CLICK is refused (the activation, not only the greyed widget)");

        uint8_t zero[MAP_HASH_BYTES] = {0};
        maps::host_on_join(2, "Eve", zero); // holds nothing at all under that name
        maps::host_on_join(1, "Bob", host_h);
        checkf(maps::host_start_blocked(who, sizeof(who), &unf) && lstrcmpA(who, "Eve") == 0 && unf,
               "G: a TCP joiner holding NOTHING is refused too, by name ('%s')", who);

        maps::host_on_leave(2);
        checkf(!maps::host_start_blocked(who, sizeof(who), &unf) && !unf,
               "G: a TCP joiner holding the SAME content does not block Start");
        checkf(!maps::host_refuse_start_click(), "G: ...and a Start CLICK goes through");

        maps::set_can_carry_for_test(1);
        maps::host_on_join(3, "Mallory", mine_h);
        checkf(maps::host_start_blocked(who, sizeof(who), &unf) && !unf,
               "G: the same mismatch over UDP is a WAIT (unfetchable stays false)");
        checkf(maps::host_refuse_start_click(),
               "G: ...and a Start CLICK during the UDP wait is refused too (the X2 gate had the same hole)");
        maps::set_can_carry_for_test(-1);
        maps::session_reset();
    }

    // ---- arm H: THE MAP-HAVE RACE (mp:T6) ---------------------------------------------------------
    // Two threads meet here in a game process: host_on_join on the transport's recv thread (the
    // joiner's map-have report) and the lobby tick's pump on the main thread (the snapshot sender).
    // The rig caught the pump arming a 462 KB snapshot ~4 ms after the host logged that the joiner
    // already held the map, about 1 run in 3. A rig run can only show that race by chance, so the
    // suite DRIVES each interleaving through the two hooks, with a counting stub in place of
    // MH_Net_SnapshotSend. Every "no snapshot" check has a "snapshot sent" twin: a pump that never
    // sends passes the first kind and fails the second.
    {
        struct Rec {
            int            sends;
            int            last_peer;
            int            join_sender; // who the hook reports as
            const uint8_t *join_hash;
            int            leave; // the between-hook sends a LEAVE instead of a JOIN
        };
        static Rec          r;
        const uint8_t       none_h[MAP_HASH_BYTES] = {0};
        auto                reset_rec              = [] { r = Rec{0, -1, 1, nullptr, 0}; };
        maps::PumpTestHooks hk{};
        hk.send = [](int peer, const void *, int, void *) {
            ++r.sends;
            r.last_peer = peer;
            return 1;
        };
        hk.body    = g_host_map;
        hk.len     = MAPLEN;
        auto begin = [&] {
            maps::session_reset();
            maps::set_can_carry_for_test(1);
            maps::host_set_claim_for_test(BASE, host_h, MAPLEN);
            reset_rec();
            hk.between         = nullptr;
            hk.join_prepublish = nullptr;
            maps::set_pump_hooks_for_test(&hk);
        };

        // H1/H2: the plain orders -- the report lands, THEN the pump runs.
        begin();
        maps::host_on_join(1, "Bob", host_h);
        maps::host_pump_for_test();
        checkf(r.sends == 0, "H1: report-then-pump, joiner HOLDS the map -> no snapshot (sends=%d)", r.sends);

        begin();
        maps::host_on_join(1, "Bob", mine_h); // the forced-snapshot shape: same name, other bytes
        maps::host_pump_for_test();
        checkf(r.sends == 1 && r.last_peer == 1,
               "H2: report-then-pump, joiner has a DIFFERENT same-named map -> snapshot sent to it (sends=%d)",
               r.sends);
        maps::host_pump_for_test();
        checkf(r.sends == 1, "H2: ...once: a second pump does not re-send while it runs (sends=%d)", r.sends);

        // H3/H4: THE RIG'S ORDER -- the pump runs INSIDE host_on_join, before the report is
        // published. The first cut had `seated` set and `holds` not yet set at exactly this point.
        begin();
        hk.join_prepublish = [](int, void *) { maps::host_pump_for_test(); };
        maps::host_on_join(1, "Bob", host_h);
        hk.join_prepublish = nullptr;
        maps::host_pump_for_test();
        checkf(r.sends == 0,
               "H3: pump INSIDE the report, joiner HOLDS the map -> no snapshot, then or after (sends=%d)",
               r.sends);

        begin();
        hk.join_prepublish = [](int, void *) { maps::host_pump_for_test(); };
        maps::host_on_join(1, "Bob", mine_h);
        hk.join_prepublish = nullptr;
        checkf(r.sends == 0, "H4: pump INSIDE the report sends nothing before the report is known (sends=%d)",
               r.sends);
        maps::host_pump_for_test();
        checkf(r.sends == 1 && r.last_peer == 1,
               "H4: ...and once it is known, a joiner WITHOUT the map gets the snapshot (sends=%d)", r.sends);

        // H5/H6/H7: the report lands INSIDE the pump, between its choice and its send (the re-check).
        begin();
        maps::host_on_join(1, "Bob", none_h); // first report: holds nothing -> chosen
        r.join_hash = host_h;
        hk.between  = [](int, void *) { maps::host_on_join(r.join_sender, "Bob", r.join_hash); };
        maps::host_pump_for_test();
        hk.between = nullptr;
        checkf(r.sends == 0,
               "H5: a report that the joiner HOLDS the map, landing mid-pump -> the send is withdrawn (sends=%d)",
               r.sends);
        char who[32];
        checkf(!maps::host_start_blocked(who, sizeof(who)), "H5: ...and the Start gate is open");
        maps::host_pump_for_test();
        checkf(r.sends == 0, "H5: ...and no later pump sends it either (sends=%d)", r.sends);

        begin();
        maps::host_on_join(1, "Bob", mine_h);
        r.join_hash = mine_h; // a re-sent JOIN that still LACKS it
        hk.between  = [](int, void *) { maps::host_on_join(r.join_sender, "Bob", r.join_hash); };
        maps::host_pump_for_test();
        hk.between = nullptr;
        checkf(r.sends == 1 && r.last_peer == 1,
               "H6: a mid-pump report that still LACKS the map does not withdraw the send (sends=%d)",
               r.sends);

        begin();
        maps::host_on_join(1, "Bob", mine_h);
        hk.between = [](int, void *) { maps::host_on_leave(1); };
        maps::host_pump_for_test();
        hk.between = nullptr;
        checkf(r.sends == 0, "H7: a joiner that LEAVES mid-pump is not sent the map (sends=%d)", r.sends);

        maps::set_pump_hooks_for_test(nullptr);
        maps::set_can_carry_for_test(-1);
        maps::session_reset();
    }

    // ---- arm W: THE WIRE -------------------------------------------------------------------------
    // One real map-sized file across two real UDP endpoints through mp:X1's snapshot pipeline,
    // delivered, stored, resolved and redirected -- and the joiner's own file compared to its witness
    // afterwards. This is the end-to-end claim the rig also makes, executed here so a red rig run can
    // be told apart from a broken mechanism.
    {
        delete_scratch(BASE);
        char stored0[maps::STORED_PATH_CAP];
        stored_path(BASE, host_h, stored0, sizeof(stored0));
        DeleteFileA(stored0);
        checkf(write_scratch(BASE, g_mine_map, MAPLEN), "W: the joiner starts holding its OWN file");

        checkf(g_tx.begin(g_host_map, MAPLEN) == snap::OK, "W: the sender manifests the map");
        printf("  -- W: %lu B, %lu body chunk(s) + %lu manifest chunk(s)\n", (unsigned long)MAPLEN,
               (unsigned long)g_tx.body_chunks(), (unsigned long)g_tx.manifest_chunks());
        g_rxr.reset(g_rx, MAPLEN * 2);
        memset(g_rx, 0xA5, MAPLEN);

        new (&g_ep_host) Endpoint();
        new (&g_ep_cl) Endpoint();
        Config ch, cc;
        mt_cfg(ch, 0, port, (unsigned short)port);
        mt_cfg(cc, 1, port, (unsigned short)(port + 1));
        const bool up = g_ep_host.start(ch, MT_PSK, true) && g_ep_cl.start(cc, MT_PSK, true) &&
                        wait_for([&] { return g_ep_host.peer_count() == 1; }, 10000);
        checkf(up, "W: the link came up");
        if (up) {
            const DWORD t0      = GetTickCount();
            const bool  started = wait_for(
                [&] {
                    return g_ep_host.bulk_send_src(1, &snap::Sender::source, &g_tx, g_tx.image_len());
                },
                5000);
            checkf(started, "W: the transfer started");
            const bool done = wait_for(
                [&] {
                    for (;;) {
                        uint32_t id  = 0;
                        int      len = (int)CHUNK_BYTES;
                        uint8_t  tmp[CHUNK_BYTES];
                        if (!g_ep_cl.bulk_recv(&id, tmp, &len)) break;
                        const int rc = g_rxr.on_chunk(id, tmp, (uint32_t)len);
                        if (rc == snap::ERR_CHUNK_HASH) g_ep_cl.bulk_resume_at(id);
                    }
                    return g_rxr.complete();
                },
                60000);
            const DWORD ms = GetTickCount() - t0;
            checkf(done, "W: the pipeline completed");
            printf("  -- W: delivered in %lu ms (%lu B)\n", (unsigned long)ms,
                   (unsigned long)g_rxr.body_len());
            if (done) {
                char st[maps::STORED_PATH_CAP];
                checkf(maps::store(BASE, host_h, g_rxr.body(), g_rxr.body_len(), st, sizeof(st)),
                       "W: the delivered bytes store under the content name");
                char out[maps::STORED_PATH_CAP];
                checkf(maps::resolve(g_dir, BASE, host_h, out, sizeof(out)) == maps::Resolve::Stored,
                       "W: ...and the load now resolves to it");
                maps::redirect_set(BASE, out);
                char scratch[MAX_PATH], expect[MAX_PATH];
                lstrcpynA(expect, out, sizeof(expect));
                checkf(lstrcmpA(maps::redirect_apply("Maps\\Cold War.mpm", scratch, sizeof(scratch)),
                                expect) == 0,
                       "W: ...so an open of the base name is served the downloaded file");
                maps::redirect_clear();

                uint32_t n   = 0;
                uint8_t *now = maps::read_file(g_dir, BASE, MAPLEN * 2, &n);
                checkf(now != nullptr && n == MAPLEN && memcmp(now, g_mine_map, MAPLEN) == 0,
                       "W: AND THE JOINER'S OWN FILE IS BYTE-UNCHANGED across the whole transfer");
                maps::free_bytes(now);

                uint32_t n3    = 0;
                uint8_t *fetch = maps::read_file("", out, MAPLEN * 2, &n3);
                checkf(fetch != nullptr && n3 == MAPLEN && memcmp(fetch, g_host_map, MAPLEN) == 0,
                       "W: ...and the stored file is the HOST's map, byte for byte");
                maps::free_bytes(fetch);
            }
        }
        g_ep_host.stop();
        g_ep_cl.stop();
        DeleteFileA(stored0);
        delete_scratch(BASE);
    }

    maps::set_dl_dir(nullptr);
    RemoveDirectoryA(g_dl); // store() made it on the first download; it is empty by now
    RemoveDirectoryA(g_dir);
    VirtualFree(g_host_map, 0, MEM_RELEASE);
    VirtualFree(g_mine_map, 0, MEM_RELEASE);
    VirtualFree(g_rx, 0, MEM_RELEASE);
    printf("=== maptest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
