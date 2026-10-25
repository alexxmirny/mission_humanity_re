//
// ovf_selftest.cpp -- `net_selftest ovftest`: the overflow extension of the raised roster arrays
// (patching:PT-CAP8 S2, slice E; mh/caps/ovf.h).
//
// WHAT IT PROVES, over a calloc'd block that stands in for `.mhcapd` (caps::adopt_relocation), so no game
// memory is read or written:
//   (1) the slice table: lengths derived from the caps geometry equal HAND-COMPUTED numbers, each slice
//       fits the localiser's 512 KB, and the segment -> stream mapping is a bijection (every tail byte is
//       reached exactly once, through reads that straddle segment boundaries);
//   (2) the incremental tracker: sums equal a from-scratch recompute after random pokes (records of 273 bytes
//       straddle 64-byte blocks and the stream blocks straddle segments), pokes inside the vanilla window
//       move nothing, verify() catches a stale shadow, and the journal replays k steps back to the exact
//       sums of step S-k;
//   (3) the folds: the extension is a continuation of the OFF fold, one sum at a time, and the ON
//       fingerprint differs from the OFF one;
//   (4) the codec: a round trip over random sparse tails (projectiles included) is exact for the whole wide
//       block, nothing past the window encodes to NOTHING, and EVERY refusal code is reached by mutating a
//       valid blob -- with the wide arrays byte-identical afterwards (a refused blob writes nothing).
//
#include "caps/orders.h"
#include "caps/orders_layout.gen.h"
#include "caps/ovf.h"
#include "caps/ovf_save.h"
#include "addr/mh_structs.gen.h"
#include "caps/path_pool.h"
#include "caps/path_pool_layout.gen.h"
#include "ovf_rc2_blob.gen.h"

#include "addr/mh_regions.gen.h"
#include "desync/desync_watch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mh::caps;
using namespace mh::caps::ovf;
namespace inc = mh::state::inc;

namespace {

constexpr int NV       = ROSTER_SLICES; // the eleven roster slices (the U7d path/order slices are inert in these tests): the block below holds BOTH the .mhcapd and the .mhcapu stand-ins
int           g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

uint32_t g_rng = 0x9e3779b9u;
uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

uint8_t pat(unsigned a, unsigned p, unsigned r, unsigned b) { return static_cast<uint8_t>((a * 131u + p * 31u + r * 7u + b * 3u + 1u) | 1u); }

// Hand-computed slice lengths (spec section 1): players x (raised - vanilla) x elem.
struct want {
    const char *name;
    uint32_t    len;
};
const want W[NV] = {
    {"ovf_buildings_p0_3", 4u * 400u * 273u},
    {"ovf_buildings_p4_7", 4u * 400u * 273u},
    {"ovf_unit_storage", 8u * 102u * 244u},
    {"ovf_turrets", 8u * 95u * 55u},
    {"ovf_mines", 8u * 95u * 56u},
    {"ovf_labs", 8u * 102u * 8u},
    {"ovf_productions", 8u * 119u * 405u},
    {"ovf_projectiles", 2500u * 121u},
    {"ovf_units_p0_3", 4u * 400u * 233u},
    {"ovf_units_p4_7", 4u * 400u * 233u},
    {"ovf_soldiers", 8u * 400u * 29u},
};

uint32_t g_base = 0, g_ubase = 0; // .mhcapd / .mhcapu stand-ins (one calloc: g_ubase = g_base + UOFF)

// An independent address formula (not segment()/rec<>()): wide array a, player p, record r, byte b.
uint8_t *addr_of(unsigned a, unsigned p, unsigned r, unsigned b) {
    const uint32_t at = a < RAISED_COUNT ? g_base + CAPD_OFFSET[a] : g_ubase + CAPU_OFFSET[a - RAISED_COUNT];
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(at + (p * RAISED_ROW[a] + r) * stock(static_cast<arr>(a)).elem + b));
}

constexpr size_t UOFF    = 0x240000u;        // .mhcapd size; the .mhcapu stand-in follows it
constexpr size_t BLOCKSZ = UOFF + 0x100000u; // + the 0x100000 .mhcapu

uint32_t checksum_block(const uint8_t *blk) { return fnv32(blk, BLOCKSZ); }

// Stream byte `o` of slice e computed WITHOUT segment(): the independent oracle.
uint8_t stream_byte(int e, uint32_t o) {
    const slice_info &s = slice(e);
    const uint32_t    k = o / s.seg_len, w = o % s.seg_len;
    return *addr_of(s.arr, s.p0 + k, s.rec0 + w / s.elem, w % s.elem);
}

// The sum of explicit stream bytes of slice e (the tracker's definition, over a plain buffer).
uint64_t sum_of(int e, const uint8_t *bytes) {
    const slice_info &s = slice(e);
    uint64_t          t = 0;
    for (uint32_t b = 0; b * 64u < s.len; ++b) {
        const uint32_t n = (s.len - b * 64u) < 64u ? (s.len - b * 64u) : 64u;
        t += block_hash(e, b, bytes + b * 64u, n);
    }
    return t;
}

// A journal that records everything it is handed.
struct entry {
    int                  region;
    uint32_t             block;
    std::vector<uint8_t> bytes;
};
struct rec_journal final : inc::journal {
    std::vector<entry> e;
    void               old_block(int region, uint32_t block, const uint8_t *old_bytes, uint32_t n) override {
        e.push_back({region, block, std::vector<uint8_t>(old_bytes, old_bytes + n)});
    }
};

// Write one byte at stream offset o of slice e (through the independent formula).
void put_stream(int e, uint32_t o, uint8_t v) {
    const slice_info &s = slice(e);
    const uint32_t    k = o / s.seg_len, w = o % s.seg_len;
    *addr_of(s.arr, s.p0 + k, s.rec0 + w / s.elem, w % s.elem) = v;
}

void fill_tails_random(unsigned density_per_mille) {
    for (unsigned a = 0; a < WIDE_COUNT; ++a) {
        const geom &sk = stock(static_cast<arr>(a));
        for (uint32_t p = 0; p < sk.players; ++p)
            for (uint32_t r = sk.row; r < RAISED_ROW[a]; ++r) {
                if (rnd() % 1000u >= density_per_mille) continue;
                for (uint32_t b = 0; b < sk.elem; ++b) *addr_of(a, p, r, b) = (rnd() % 5u == 0) ? 0 : static_cast<uint8_t>(rnd());
                *addr_of(a, p, r, rnd() % sk.elem) = 0xA5; // never an all-zero record
            }
    }
}

void zero_tails() {
    for (unsigned a = 0; a < WIDE_COUNT; ++a) {
        const geom &sk = stock(static_cast<arr>(a));
        for (uint32_t p = 0; p < sk.players; ++p)
            for (uint32_t r = sk.row; r < RAISED_ROW[a]; ++r) memset(addr_of(a, p, r, 0), 0, sk.elem);
    }
}

void reseal(std::vector<uint8_t> &blob) {
    ovf_header h;
    memcpy(&h, blob.data(), sizeof(h));
    h.body_sum = fnv64(blob.data() + sizeof(h), blob.size() - sizeof(h));
    memcpy(blob.data(), &h, sizeof(h));
}
ovf_header hdr_of(const std::vector<uint8_t> &b) {
    ovf_header h;
    memcpy(&h, b.data(), sizeof(h));
    return h;
}
void put_hdr(std::vector<uint8_t> &b, const ovf_header &h) { memcpy(b.data(), &h, sizeof(h)); }

// Offsets of section `i` (0-based) and of its first record in a valid blob.
size_t section_at(const std::vector<uint8_t> &b, unsigned i) {
    size_t pos = sizeof(ovf_header);
    for (unsigned s = 0; s < i; ++s) {
        ovf_section sec;
        memcpy(&sec, b.data() + pos, sizeof(sec));
        pos += sizeof(sec) + static_cast<size_t>(sec.nrec) * (sizeof(ovf_record) + sec.elem);
    }
    return pos;
}


// ---- (5) the raised-caps SAVE (patching:PT-CAP8 S3, slice G; caps/ovf_save.h) --------------------------------------------
// A fake FILE (a byte vector with a position and call counters) stands in for the game's Watcom FILE.
struct fake_file {
    std::vector<uint8_t> d;
    int32_t              pos    = 0;
    int                  writes = 0, seeks = 0, reads = 0;
    int                  seek_fail = 0; // 1 = every seek fails
};
int32_t ff_tell(void *f) { return static_cast<fake_file *>(f)->pos; }
int     ff_seek(void *f, int32_t off, int whence) {
    auto *x = static_cast<fake_file *>(f);
    ++x->seeks;
    if (x->seek_fail) return -1;
    int64_t np = whence == 0 ? off : whence == 1 ? x->pos + (int64_t)off
                                                     : (int64_t)x->d.size() + off;
    if (np < 0 || np > (int64_t)x->d.size()) return -1;
    x->pos = (int32_t)np;
    return 0;
}
uint32_t ff_read(void *f, void *dst, uint32_t n) {
    auto *x = static_cast<fake_file *>(f);
    ++x->reads;
    const uint32_t have = (uint32_t)x->d.size() - (uint32_t)x->pos;
    const uint32_t k    = n < have ? n : have;
    if (k) memcpy(dst, x->d.data() + x->pos, k);
    x->pos += (int32_t)k;
    return k;
}
uint32_t ff_write(void *f, const void *src, uint32_t n) {
    auto *x = static_cast<fake_file *>(f);
    ++x->writes;
    if (x->pos + n > x->d.size()) x->d.resize((size_t)x->pos + n);
    memcpy(x->d.data() + x->pos, src, n);
    x->pos += (int32_t)n;
    return n;
}
const file_io FIO = {&ff_tell, &ff_seek, &ff_read, &ff_write};

// A synthetic six-slot version table shaped like the game's (40 bytes a slot; the real strings are asserted against the
// clean exe by tools/check_cap_save.py --selftest).
void make_table(uint8_t (&t)[6][40]) {
    static const char *const NAMES[6] = {" Extermination, demo version", " Extermination, ver 1.0", " Extermination, ver 1.01",
                                         " Extermination, ver 1.02", " Extermination, ver 1.03", " 'Mission: Humanity' ver 1.00"};
    for (int i = 0; i < 6; ++i) {
        memset(t[i], ' ', 40);
        memcpy(t[i], NAMES[i], strlen(NAMES[i]));
        memset(t[i] + 34, 0, 6);
    }
}

// The game_SaveGame member predicate over a fixture planet table.
struct planets {
    int32_t sys[40];
    int32_t status[40];
    bool    footer[40];
};
int32_t pl_sys(void *c, int i) { return static_cast<planets *>(c)->sys[i]; }
int32_t pl_status(void *c, int i) { return static_cast<planets *>(c)->status[i]; }
bool    pl_footer(void *c, int i) { return static_cast<planets *>(c)->footer[i]; }

void save_tests() {
    printf("ovftest (5) -- the raised-caps save\n");
    set_save_logger(nullptr);
    zero_tails();
    // a body in front of the trailer, as a planet file has
    auto make_body = [](fake_file &f, size_t n) {
        f.d.assign(n, 0);
        for (size_t i = 0; i < n; ++i) f.d[i] = (uint8_t)(i * 7u + 3u);
        f.pos = (int32_t)n;
    };

    // (5a) OFF writes NOTHING, even with a full set of tails
    {
        fill_tails_random(300);
        ck(!raised(), "(5a) the raise is OFF");
        fake_file f;
        make_body(f, 1000);
        const std::vector<uint8_t> before = f.d;
        ck(save_tail(FIO, &f, 7) == 0 && f.d == before && f.writes == 0 && f.seeks == 0 && f.reads == 0, "(5a) save_tail with the raise OFF: 0 bytes, no write, no seek, no read (a byte-vanilla file)");
        zero_tails();
        set_raised_for_test(true);
        ck(save_tail(FIO, &f, 7) == 0 && f.d == before && f.writes == 0, "(5a) raised but nothing past the window: 0 bytes appended (the file stays vanilla)");
    }

    // (5b) raised, tails present: body + MHOV blob + footer
    fill_tails_random(120);
    const uint64_t d0 = digest();
    fake_file      sf;
    make_body(sf, 1000);
    const int            wrote = save_tail(FIO, &sf, 7);
    std::vector<uint8_t> blob(max_bytes() + 8);
    const size_t         n = encode(blob.data(), blob.size());
    ck(n > 0 && n != ENCODE_FAIL && wrote == (int)(n + FOOTER_BYTES) && sf.d.size() == 1000 + n + FOOTER_BYTES, "(5b) raised: save_tail appends blob + 8-byte footer");
    {
        uint32_t foot[2] = {};
        memcpy(foot, sf.d.data() + sf.d.size() - 8, 8);
        ck(foot[0] == n && foot[1] == FOOTER_MAGIC && FOOTER_MAGIC == 0x464F484Du, "...the LAST 8 bytes are {ovf_len, 'MHOF'}");
        ck(memcmp(sf.d.data() + 1000, blob.data(), n) == 0 && check(sf.d.data() + 1000, n) == OVF_OK, "...and the bytes before them are the MHOV blob, which validates");
        ck(file_has_footer(FIO, &sf) && sf.pos == (int32_t)sf.d.size(), "...file_has_footer sees it and leaves the position where it was");
    }

    // (5c) load: position restored on EVERY path
    {
        set_raised_for_test(true);
        zero_tails();
        sf.pos         = 500;
        int32_t   fail = 0;
        const int w0   = sf.writes;
        ck(load_tail(FIO, &sf, &fail) == LT_APPLIED && fail == 0, "(5c) ON: load_tail applies the trailer");
        ck(sf.pos == 500 && sf.writes == w0, "...the file position is back where the vanilla reader left it, nothing was written");
        ck(digest() == d0, "...and the wide tails are byte-identical to the saved ones (digest equal)");

        // OFF: refused, nothing written to the arrays, flag set, position restored
        zero_tails();
        set_raised_for_test(false);
        set_refused_raised(false);
        const uint64_t dz = digest();
        sf.pos            = 123;
        fail              = 0;
        ck(load_tail(FIO, &sf, &fail) == LT_REFUSED_OFF && fail == 1 && refused_raised(), "(5c) OFF: a footer is REFUSED (fail++, refusal flag set)");
        ck(sf.pos == 123 && digest() == dz, "...position restored, the arrays untouched");
        ck(take_refused_raised() && !refused_raised() && !take_refused_raised(), "...take_refused_raised reads and clears the flag once");

        // no footer: exactly the vanilla reader, ON and OFF
        fake_file v;
        make_body(v, 2000);
        for (int on = 0; on < 2; ++on) {
            set_raised_for_test(on != 0);
            v.pos = 777;
            fail  = 0;
            ck(load_tail(FIO, &v, &fail) == LT_NONE && fail == 0 && v.pos == 777 && v.writes == 0 && !refused_raised(), on ? "(5c) ON, vanilla file: nothing happens, position restored" : "(5c) OFF, vanilla file: nothing happens, position restored");
        }
        // a file shorter than the footer: the seek fails, the position still comes back
        fake_file tiny;
        make_body(tiny, 5);
        tiny.pos = 2;
        fail     = 0;
        ck(load_tail(FIO, &tiny, &fail) == LT_NONE && tiny.pos == 2 && fail == 0, "(5c) a 5-byte file: no footer, position restored");
        // a stream that cannot seek at all
        fake_file noseek = v;
        noseek.seek_fail = 1;
        noseek.pos       = 44;
        ck(load_tail(FIO, &noseek, &fail) == LT_NONE && noseek.pos == 44 && fail == 0, "(5c) every seek failing: LT_NONE, position untouched");

        // raised, corrupt trailers: refused, nothing written, position restored
        set_raised_for_test(true);
        zero_tails();
        const uint64_t dz2     = digest();
        auto           try_bad = [&](fake_file bad, const char *what) {
            bad.pos       = 321;
            int32_t    fl = 0;
            const bool ok = load_tail(FIO, &bad, &fl) == LT_REFUSED_BAD && fl == 1 && bad.pos == 321 && digest() == dz2;
            ck(ok, what);
        };
        fake_file c1 = sf;
        c1.d[1000 + sizeof(ovf_header) + 20] ^= 0x40; // a body byte: body_sum
        try_bad(c1, "(5c) ON, a flipped body byte: LT_REFUSED_BAD, fail++, arrays untouched, position restored");
        fake_file c2 = sf;
        {
            uint32_t len = 0x7fffffff;
            memcpy(c2.d.data() + c2.d.size() - 8, &len, 4);
        }
        try_bad(c2, "(5c) ON, a footer length larger than the file: refused");
        fake_file c3 = sf;
        {
            uint32_t len = 0;
            memcpy(c3.d.data() + c3.d.size() - 8, &len, 4);
        }
        try_bad(c3, "(5c) ON, a footer length of 0: refused");
        fake_file c4 = sf;
        {
            uint32_t len = 20;
            memcpy(c4.d.data() + c4.d.size() - 8, &len, 4);
        }
        try_bad(c4, "(5c) ON, a footer length shorter than a header: refused");
        // the unmutated file still applies afterwards
        zero_tails();
        sf.pos = 10;
        fail   = 0;
        ck(load_tail(FIO, &sf, &fail) == LT_APPLIED && digest() == d0 && sf.pos == 10, "(5c) ...and the unmutated file still applies after all that");
    }

    // (5d) the container stamp
    {
        uint8_t table[6][40];
        make_table(table);
        uint8_t stamp[40];
        make_stamp(table[5], stamp);
        bool differs = true;
        for (int i = 0; i < 6; ++i) differs = differs && memcmp(stamp, table[i], 40) != 0;
        ck(differs, "(5d) the stamp differs from ALL six table slots");
        int ndiff = 0;
        for (int i = 0; i < 40; ++i) ndiff += stamp[i] != table[5][i];
        ck(ndiff == 4 && memcmp(stamp + 29, "MHC1", 4) == 0 && memcmp(stamp, table[5], 29) == 0 && memcmp(stamp + 33, table[5] + 33, 7) == 0,
           "...it is the build's own slot with bytes 29..32 replaced by \"MHC1\"");
        ck(is_stamp(stamp, table[5]), "...is_stamp recognises it");
        bool none = true;
        for (int i = 0; i < 6; ++i) none = none && !is_stamp(table[i], table[5]);
        ck(none, "...and none of the six genuine version strings");

        uint8_t hdr[40];
        for (int i = 0; i < 6; ++i) {
            memcpy(hdr, table[i], 40);
            set_raised_for_test(true);
            const bool a = version_check(hdr, table[5]) == VC_PASS && memcmp(hdr, table[i], 40) == 0;
            set_raised_for_test(false);
            memcpy(hdr, table[i], 40);
            const bool b = version_check(hdr, table[5]) == VC_PASS && memcmp(hdr, table[i], 40) == 0 && !refused_raised();
            ck(a && b, "(5d) version_check leaves a genuine version string untouched (ON and OFF)");
        }
        memcpy(hdr, stamp, 40);
        set_raised_for_test(true);
        ck(version_check(hdr, table[5]) == VC_ACCEPTED && memcmp(hdr, table[5], 40) == 0 && !refused_raised(), "(5d) ON: the stamp is rewritten to the table slot (the vanilla compare then accepts it)");
        memcpy(hdr, stamp, 40);
        set_raised_for_test(false);
        ck(version_check(hdr, table[5]) == VC_REFUSED && memcmp(hdr, stamp, 40) == 0 && refused_raised(), "(5d) OFF: the stamp is left (the vanilla compare fails) and the refusal flag is set");
        memcpy(hdr, table[5], 40);
        ck(version_check(hdr, table[5]) == VC_PASS && !refused_raised(), "(5d) the next, genuine header clears a stale refusal flag");
    }

    // (5e) stamp_needed re-implements the 0x44741a member predicate
    {
        planets pl;
        memset(&pl, 0, sizeof(pl));
        member_probe mp = {&pl, &pl_sys, &pl_status, &pl_footer, 3, 9};
        for (int i = 0; i < 40; ++i) pl.sys[i] = 3;
        set_raised_for_test(true);
        ck(!stamp_needed(mp), "(5e) no member carries a footer: no stamp");
        pl.footer[12] = true;
        ck(!stamp_needed(mp), "...a footer on a planet with status 0 that is not the current one: not a member");
        pl.status[12] = 1;
        ck(stamp_needed(mp), "...status != 0: a member -> stamp");
        pl.sys[12] = 4;
        ck(!stamp_needed(mp), "...in another system: not a member");
        pl.sys[12]    = 3;
        pl.status[12] = 0;
        pl.footer[12] = false;
        pl.footer[9]  = true;
        ck(stamp_needed(mp), "...the CURRENT planet is a member even with status 0");
        pl.footer[9] = false;
        pl.footer[0] = pl.footer[32] = true;
        pl.status[0] = pl.status[32] = 1;
        ck(!stamp_needed(mp), "...planets 0 and 32 are outside 1..0x1f");
        pl.footer[31] = true;
        pl.status[31] = 1;
        ck(stamp_needed(mp), "...planet 0x1f is inside");
        set_raised_for_test(false);
        ck(!stamp_needed(mp), "...with the raise OFF nothing is ever stamped");
        bool pred = true;
        for (int i = 1; i <= 31; ++i) {
            for (int s = 0; s < 2; ++s)
                for (int cur = 0; cur < 2; ++cur) {
                    planets q;
                    memset(&q, 0, sizeof(q));
                    for (int k = 0; k < 40; ++k) q.sys[k] = 5;
                    q.status[i]     = s;
                    member_probe m2 = {&q, &pl_sys, &pl_status, &pl_footer, 5, cur ? i : 0};
                    pred            = pred && member_included(m2, i) == (s != 0 || cur != 0);
                }
        }
        ck(pred, "...member_included == (system matches) && (status != 0 || current), over every index and combination");
    }

    set_raised_for_test(false);
    scratch_release();
}
} // namespace


// ---- (7) PT-CAP9-U7a: MHOV version 2 ------------------------------------------------------------------------------------------------
// The independent path-pool address formula (not the module's own accessors): host + offset + player stride + slot * 600 + entry * 2.
uint8_t *path_slot(uint8_t *pblk, unsigned p, unsigned s) {
    namespace PL = mh::caps::path_layout;
    return pblk + PL::BUF_OFFSET + p * PL::BUF_PLAYER_BYTES + s * PL::SLOT_BYTES;
}
uint8_t *path_flag(uint8_t *pblk, unsigned p, unsigned s) {
    namespace PL = mh::caps::path_layout;
    return pblk + PL::FLAGS_OFFSET + p * PL::ROW_SLOTS + s;
}
void fill_path_random(uint8_t *pblk, unsigned density_per_mille) {
    namespace PL = mh::caps::path_layout;
    for (unsigned p = 0; p < PL::PLAYERS; ++p)
        for (unsigned s = PL::STOCK_SLOTS; s < PL::ROW_SLOTS; ++s) {
            if (rnd() % 1000u >= density_per_mille) continue;
            *path_flag(pblk, p, s) = static_cast<uint8_t>(rnd() % 2u);
            uint8_t *w             = path_slot(pblk, p, s);
            for (unsigned b = 0; b < PL::SLOT_BYTES; ++b) w[b] = (rnd() % 3u == 0) ? 0 : static_cast<uint8_t>(rnd());
            w[rnd() % PL::SLOT_BYTES] = 0xA5; // never an all-zero slot (the flag may be 0, the waypoints are not)
        }
}
void zero_path_tails(uint8_t *pblk) {
    namespace PL = mh::caps::path_layout;
    for (unsigned p = 0; p < PL::PLAYERS; ++p)
        for (unsigned s = PL::STOCK_SLOTS; s < PL::ROW_SLOTS; ++s) {
            *path_flag(pblk, p, s) = 0;
            memset(path_slot(pblk, p, s), 0, PL::SLOT_BYTES);
        }
}
// The window (slots 0..99) gets a pattern a v2 apply must never touch.
void fill_path_window(uint8_t *pblk) {
    namespace PL = mh::caps::path_layout;
    for (unsigned p = 0; p < PL::PLAYERS; ++p)
        for (unsigned s = 0; s < PL::STOCK_SLOTS; ++s) {
            *path_flag(pblk, p, s) = static_cast<uint8_t>((p + s) & 1u);
            uint8_t *w             = path_slot(pblk, p, s);
            for (unsigned b = 0; b < PL::SLOT_BYTES; ++b) w[b] = static_cast<uint8_t>(p * 17u + s * 5u + b + 1u);
        }
}

// The MHOV blob a save container carries: scan for a `[blob][u32 len]['MHOF']` footer (the planet member rides in the container verbatim).
std::vector<uint8_t> blob_of_sav(const std::vector<uint8_t> &d) {
    for (size_t i = 4; i + 4 <= d.size(); ++i) {
        if (memcmp(&d[i], "MHOF", 4) != 0) continue;
        uint32_t len;
        memcpy(&len, &d[i - 4], 4);
        if (len < sizeof(ovf_header) || len > i - 4) continue;
        const size_t st = i - 4 - len;
        uint32_t     magic;
        memcpy(&magic, &d[st], 4);
        if (magic == MHOV_MAGIC) return std::vector<uint8_t>(d.begin() + (long)st, d.begin() + (long)(st + len));
    }
    return {};
}
// tools/uiscripts/saves/<name> found by walking up from the working directory and from the executable.
std::vector<uint8_t> read_repo_save(const char *name) {
    char roots[2][MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, roots[0]);
    GetModuleFileNameA(nullptr, roots[1], MAX_PATH);
    for (auto &root : roots) {
        std::string dir = root;
        for (int depth = 0; depth < 10 && !dir.empty(); ++depth) {
            const std::string path = dir + "\\tools\\uiscripts\\saves\\" + name;
            FILE             *f    = nullptr;
            if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
                std::vector<uint8_t> d;
                uint8_t              tmp[65536];
                size_t               got;
                while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) d.insert(d.end(), tmp, tmp + got);
                fclose(f);
                return d;
            }
            const size_t cut = dir.find_last_of("\\/");
            if (cut == std::string::npos) break;
            dir.resize(cut);
        }
    }
    return {};
}

// Every record of a (valid) blob matches the memory it was applied to. Independent of apply(): parses the struct layout itself.
bool blob_matches_memory(const std::vector<uint8_t> &b, uint8_t *pblk) {
    ovf_header h   = hdr_of(b);
    size_t     pos = sizeof(ovf_header);
    bool       ok  = true;
    for (unsigned s = 0; s < h.nsec; ++s) {
        ovf_section sec;
        memcpy(&sec, b.data() + pos, sizeof(sec));
        pos += sizeof(sec);
        for (uint32_t r = 0; r < sec.nrec; ++r) {
            ovf_record rh;
            memcpy(&rh, b.data() + pos, sizeof(rh));
            const uint8_t *body = b.data() + pos + sizeof(rh);
            if (sec.arr < WIDE_COUNT) {
                for (uint32_t i = 0; i < sec.elem; ++i) ok = ok && *addr_of(sec.arr, rh.player, rh.idx, i) == body[i];
            } else if (sec.arr == KIND_PATH_SLOT) {
                ok = ok && *path_flag(pblk, rh.player, rh.idx) == body[0] && memcmp(path_slot(pblk, rh.player, rh.idx), body + 1, 600) == 0;
            }
            pos += sizeof(rh) + sec.elem;
        }
    }
    return ok;
}

uint32_t sum_pblk(const uint8_t *pblk) { return fnv32(pblk, mh::caps::path_layout::HOST_VSIZE); }

void u7_tests() {
    namespace PL = mh::caps::path_layout;
    namespace P  = mh::caps::path;
    printf("  -- (7) PT-CAP9-U7a: MHOV version 2\n");
    uint8_t *block = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(g_base));
    uint8_t *pblk  = static_cast<uint8_t *>(calloc(PL::HOST_VSIZE + 64, 1));
    memset(block, 0, BLOCKSZ);
    reset_stock();
    P::reset();
    ck(adopt_relocation(g_base) && adopt_units_relocation(g_ubase) && P::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pblk))),
       "(7) arrays + units + the path pool adopted over calloc'd blocks");
    ck(ovf::max_bytes_save() == ovf::max_bytes() + sizeof(ovf_section) + 8u * 401u * (sizeof(ovf_record) + 601u) + sizeof(ovf_section) + 3796u * (sizeof(ovf_record) + 0x44u),
       "max_bytes_save() = max_bytes() + the path section's worst case + the order QUEUE section's (PT-CAP9-U7c)");
    std::vector<uint8_t> buf(max_bytes_save() + 16);
    const size_t         cap = buf.size() - 16;

    // ---- (7a) the REAL saves --------------------------------------------------------------------------------------
    {
        std::vector<uint8_t> rc2(ovf_fixture::RC2_BLOB, ovf_fixture::RC2_BLOB + sizeof(ovf_fixture::RC2_BLOB));
        ck(rc2.size() == ovf_fixture::RC2_LEN && hdr_of(rc2).version == MHOV_VERSION_1 && hdr_of(rc2).layout_fp == ovf_fixture::RC2_FP,
           "(7a) the rc2 fixture is the shipped v0.2.2-rc2 blob: version 1, fingerprint 0x2bdef41f");
        ck(check(rc2.data(), rc2.size()) == OVF_OK && record_count(rc2.data(), rc2.size()) == (int)ovf_fixture::RC2_RECORDS, "...passes check() with its 8 records");
        ck(layout_fp() == ovf_fixture::RC2_FP, "...and this build's layout_fp() is STILL the fingerprint rc2 wrote (a v1 reader of this build and an rc2 writer agree)");
        uint32_t feat = 99;
        ck(apply(rc2.data(), rc2.size(), &feat) == OVF_OK && feat == 0, "...applies (no features)");
        ck(blob_matches_memory(rc2, pblk), "...every record is in the wide arrays byte for byte");
        const size_t n = encode(buf.data(), cap);
        ck(n == rc2.size() && memcmp(buf.data(), rc2.data(), n) == 0, "...and re-encoding (the world mode) gives the rc2 blob BYTE FOR BYTE");
        const size_t ns = encode(buf.data(), cap, FOR_SAVE);
        ck(ns == rc2.size() && memcmp(buf.data(), rc2.data(), ns) == 0, "...and so does the SAVE mode with an empty path pool: v0.2.2-only content is written as version 1");
        memset(block, 0, BLOCKSZ);

        std::vector<uint8_t> sav = read_repo_save("capovf.sav");
        ck(!sav.empty(), "tools/uiscripts/saves/capovf.sav found (walking up from the working directory / the exe)");
        std::vector<uint8_t> cap_blob = blob_of_sav(sav);
        ck(cap_blob.size() == 225066u && hdr_of(cap_blob).version == MHOV_VERSION_1 && hdr_of(cap_blob).records == 1305u,
           "...and carries a 225066-byte version-1 blob with 1305 records");
        ck(!cap_blob.empty() && check(cap_blob.data(), cap_blob.size()) == OVF_OK, "the capovf blob passes check()");
        if (!cap_blob.empty()) {
            ck(apply(cap_blob.data(), cap_blob.size()) == OVF_OK && blob_matches_memory(cap_blob, pblk), "...applies, every record byte for byte");
            std::vector<uint8_t> big(max_bytes_save() + 16);
            const size_t         m = encode(big.data(), big.size() - 16, FOR_SAVE);
            ck(m == cap_blob.size() && memcmp(big.data(), cap_blob.data(), m) == 0, "...and re-encodes (SAVE mode) to the capovf blob BYTE FOR BYTE: still version 1");
        }
        // PT-CAP9-U7f: the user's shipped v0.2.2-rc2 save carries the rc2 blob (the cap_save_old row's rc2_on step expects its 8 records applied)
        std::vector<uint8_t> rcsav = read_repo_save("8-ayatus-rl-2.sav");
        ck(!rcsav.empty(), "tools/uiscripts/saves/8-ayatus-rl-2.sav found");
        std::vector<uint8_t> rc_blob = blob_of_sav(rcsav);
        ck(rc_blob.size() == ovf_fixture::RC2_LEN && hdr_of(rc_blob).version == MHOV_VERSION_1 && hdr_of(rc_blob).records == ovf_fixture::RC2_RECORDS &&
               memcmp(rc_blob.data(), ovf_fixture::RC2_BLOB, rc_blob.size()) == 0,
           "...and 8-ayatus-rl-2.sav carries the rc2 blob byte for byte (version 1, 8 records)");
        memset(block, 0, BLOCKSZ);
    }

    // ---- (7b) the writer picks the version -----------------------------------------------------------------------
    {
        fill_tails_random(150);
        const size_t         nw = encode(buf.data(), cap, FOR_WORLD);
        std::vector<uint8_t> world(buf.begin(), buf.begin() + (long)nw);
        const size_t         ns = encode(buf.data(), cap, FOR_SAVE);
        ck(nw != 0 && ns == nw && memcmp(buf.data(), world.data(), nw) == 0 && hdr_of(world).version == MHOV_VERSION_1,
           "(7b) arrays only: the SAVE blob is the WORLD blob byte for byte, version 1");
        // path content INSIDE the window only: still nothing for the path section
        fill_path_window(pblk);
        const size_t nw2 = encode(buf.data(), cap, FOR_SAVE);
        ck(nw2 == nw && memcmp(buf.data(), world.data(), nw) == 0, "...a busy pool inside the window (slots 0..99) adds nothing: the stock blocks carry it, the file stays version 1");
        // one stale byte in slot 100 flips to version 2
        path_slot(pblk, 2, 100)[17] = 0x42;
        const size_t nv2            = encode(buf.data(), cap, FOR_SAVE);
        ovf_header   h2             = hdr_of(std::vector<uint8_t>(buf.begin(), buf.begin() + 32));
        ck(nv2 != 0 && nv2 != ENCODE_FAIL && h2.version == MHOV_VERSION && h2.layout_fp == 0 && h2.features == FEAT_PATH_ID16,
           "...one byte in slot 100 makes it version 2: layout_fp 0, FEAT_PATH_ID16");
        ck(nv2 == nw + sizeof(ovf_section) + sizeof(ovf_record) + 601u && section_records(buf.data(), nv2, KIND_PATH_SLOT) == 1, "...costing exactly one section + one 601-byte record");
        const size_t nwv = encode(buf.data(), cap, FOR_WORLD);
        ck(nwv == nw && memcmp(buf.data(), world.data(), nw) == 0, "...and the WORLD mode never writes the path section");
        ck(encode(buf.data(), nv2 - 1, FOR_SAVE) == ENCODE_FAIL, "a buffer one byte short for the v2 blob is ENCODE_FAIL");
        zero_tails();
        zero_path_tails(pblk);
        memset(pblk, 0, PL::HOST_VSIZE + 64);
    }

    // ---- (7c) every kind round-trips ------------------------------------------------------------------------------
    std::vector<uint8_t> v2blob;
    {
        fill_tails_random(120);
        fill_path_random(pblk, 600);
        fill_path_window(pblk);
        path_slot(pblk, 7, 500)[3] = 0x31; // the sink slot is a record too (idx 500 = raised_row - 1)
        const std::vector<uint8_t> golden_arrays(block, block + BLOCKSZ);
        const std::vector<uint8_t> golden_pool(pblk, pblk + PL::HOST_VSIZE + 64);
        const size_t               n = encode(buf.data(), cap, FOR_SAVE);
        ck(n != 0 && n != ENCODE_FAIL, "(7c) a full fill (all nine arrays + the path pool) encodes");
        v2blob.assign(buf.begin(), buf.begin() + (long)n);
        const ovf_header h = hdr_of(v2blob);
        ck(h.version == MHOV_VERSION && h.nsec == 10 && h.layout_fp == 0 && h.features == FEAT_PATH_ID16 && h.body_len == n - sizeof(h), "...version 2, 10 sections (9 arrays + the path slots)");
        bool kv = true, fl = true;
        for (unsigned s = 0, pos = sizeof(ovf_header); s < h.nsec; ++s) {
            ovf_section sec;
            memcpy(&sec, v2blob.data() + pos, sizeof(sec));
            kv = kv && sec.kind_ver == KIND_VER;
            fl = fl && sec.flags == 0;
            pos += sizeof(sec) + sec.nrec * (sizeof(ovf_record) + sec.elem);
        }
        ck(kv && fl, "...every section carries kind_ver 1 and no flags");
        ck(check(v2blob.data(), n) == OVF_OK && record_count(v2blob.data(), n) == (int)(overflow_records() + (unsigned)section_records(v2blob.data(), n, KIND_PATH_SLOT)),
           "...validates; the header total = array overflow records + path records");
        ck(section_records(v2blob.data(), n, KIND_PATH_SLOT) > 1000 && section_records(v2blob.data(), n, 8) > 0, "...with path records and soldier records both present");
        // apply onto an empty world whose window carries the guard pattern
        zero_tails();
        zero_path_tails(pblk);
        uint32_t feat = 0;
        ck(apply(v2blob.data(), n, &feat) == OVF_OK && feat == FEAT_PATH_ID16, "apply() reports the PATH_ID16 feature");
        ck(memcmp(block, golden_arrays.data(), BLOCKSZ) == 0, "...the wide arrays are restored byte for byte");
        ck(memcmp(pblk, golden_pool.data(), golden_pool.size()) == 0, "...the WHOLE path pool is restored byte for byte (slots 100..500 from the blob, the window untouched, the sink included)");
        ck(blob_matches_memory(v2blob, pblk), "...and every record matches the independent parse");
        const size_t n2 = encode(buf.data(), cap, FOR_SAVE);
        ck(n2 == n && memcmp(buf.data(), v2blob.data(), n) == 0, "...re-encoding gives the same blob (deterministic)");
    }

    // ---- (7d) the reader's rules --------------------------------------------------------------------------------------
    {
        std::vector<uint8_t> blob     = v2blob;
        const size_t         path_sec = section_at(blob, hdr_of(blob).nsec - 1);
        zero_tails();
        zero_path_tails(pblk);
        fill_path_window(pblk);
        const uint32_t clean_a = checksum_block(block), clean_p = sum_pblk(pblk);
        uint32_t       full_psum = 0;
        {
            ck(apply(blob.data(), blob.size()) == OVF_OK, "(7d) the blob under test applies cleanly");
            full_psum = sum_pblk(pblk);
            zero_tails();
            zero_path_tails(pblk);
            ck(checksum_block(block) == clean_a && sum_pblk(pblk) == clean_p, "...and zeroing the tails restores the starting state");
        }
        auto refuses = [&](std::vector<uint8_t> &b, int want, const char *what) {
            const int c = check(b.data(), b.size());
            const int a = apply(b.data(), b.size());
            ck(c == want && a == want && checksum_block(block) == clean_a && sum_pblk(pblk) == clean_p, what);
            if (c != want) printf("    (check = %d, apply = %d, wanted %d)\n", c, a, want);
        };
        auto put_sec = [&](std::vector<uint8_t> &b, size_t at, const ovf_section &s) {
            memcpy(b.data() + at, &s, sizeof(s));
            reseal(b);
        };
        auto get_sec = [&](const std::vector<uint8_t> &b, size_t at) {
            ovf_section s;
            memcpy(&s, b.data() + at, sizeof(s));
            return s;
        };
        {
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.version              = 3;
            put_hdr(b, h);
            refuses(b, OVF_ERR_VERSION, "(7d) a NEWER file (version 3) is refused: OVF_ERR_VERSION");
            h = hdr_of(blob);
            h.features |= 2;
            put_hdr(b, h);
            refuses(b, OVF_ERR_FEATURE, "an unknown header FEATURE bit is refused: OVF_ERR_FEATURE");
            h           = hdr_of(blob);
            h.layout_fp = 1;
            put_hdr(b, h);
            refuses(b, OVF_ERR_LAYOUT, "a v2 header must carry layout_fp 0: OVF_ERR_LAYOUT");
        }
        {
            // an UNKNOWN kind (13; 10-12 are the order kinds since PT-CAP9-U7c) in place of the path section
            std::vector<uint8_t> b = blob;
            ovf_section          s = get_sec(blob, path_sec);
            s.arr                  = 13;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_KIND, "a required section of an UNKNOWN kind (13) is refused: OVF_ERR_KIND");
            s.flags = SEC_OPTIONAL;
            put_sec(b, path_sec, s);
            ck(check(b.data(), b.size()) == OVF_OK && apply(b.data(), b.size()) == OVF_OK, "...the same section marked OPTIONAL is skipped: check() and apply() accept the blob");
            ck(sum_pblk(pblk) == clean_p, "...and the skipped section wrote NOTHING (the pool is unchanged)");
            zero_tails();
        }
        {
            // a NEWER kind_ver on a known kind (the first array section)
            std::vector<uint8_t> b  = blob;
            const size_t         s0 = sizeof(ovf_header);
            ovf_section          s  = get_sec(blob, s0);
            s.kind_ver              = 2;
            put_sec(b, s0, s);
            refuses(b, OVF_ERR_KIND, "a required section with a NEWER kind_ver is refused: OVF_ERR_KIND");
            s.flags = SEC_OPTIONAL;
            put_sec(b, s0, s);
            ck(check(b.data(), b.size()) == OVF_OK && apply(b.data(), b.size()) == OVF_OK, "...marked OPTIONAL it is skipped");
            bool a0_untouched = true;
            for (uint32_t i = 0; i < s.nrec && i < 1; ++i) {
                ovf_record rh;
                memcpy(&rh, b.data() + s0 + sizeof(ovf_section), sizeof(rh));
                for (uint32_t k = 0; k < s.elem; ++k) a0_untouched = a0_untouched && *addr_of(s.arr, rh.player, rh.idx, k) == 0;
            }
            ck(a0_untouched, "...the skipped array's records were NOT applied");
            ck(sum_pblk(pblk) == full_psum && checksum_block(block) != clean_a, "...while the other sections WERE applied (the whole pool is back, the arrays changed)");
            zero_tails();
            zero_path_tails(pblk);
            fill_path_window(pblk);
        }
        {
            std::vector<uint8_t> b = blob;
            ovf_section          s = get_sec(blob, path_sec);
            s.flags                = 2;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_FEATURE, "an unknown section FLAG bit is refused: OVF_ERR_FEATURE");
            s = get_sec(blob, path_sec);
            s.elem += 1;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_ELEM, "a path section with another record size is refused: OVF_ERR_ELEM");
            s         = get_sec(blob, path_sec);
            s.players = 9;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_LAYOUT, "...another player count: OVF_ERR_LAYOUT");
            s            = get_sec(blob, path_sec);
            s.raised_row = 500;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_LAYOUT, "...another raised_row: OVF_ERR_LAYOUT");
            s          = get_sec(blob, path_sec);
            s.kind_ver = 0;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_KIND, "...kind_ver 0 on a v2 section: OVF_ERR_KIND");
            // a duplicate section, then an overlong OPTIONAL one
            const size_t s1 = section_at(blob, 1);
            b               = blob;
            b[s1]           = blob[sizeof(ovf_header)];
            reseal(b);
            refuses(b, OVF_ERR_SECTION_ORDER, "a duplicate section in a v2 blob: OVF_ERR_SECTION_ORDER");
            b       = blob;
            s       = get_sec(blob, path_sec);
            s.arr   = 12;
            s.flags = SEC_OPTIONAL;
            s.nrec += 1;
            put_sec(b, path_sec, s);
            refuses(b, OVF_ERR_TRUNCATED, "an OPTIONAL section whose nrec runs past the body is refused: OVF_ERR_TRUNCATED");
        }
        {
            // path record ranges
            std::vector<uint8_t> b  = blob;
            const size_t         r0 = path_sec + sizeof(ovf_section);
            ovf_record           r;
            memcpy(&r, blob.data() + r0, sizeof(r));
            const ovf_record keep = r;
            r.idx                 = 99;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "path record idx 99 (inside the window): OVF_ERR_RANGE");
            r     = keep;
            r.idx = 501;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "path record idx 501 (past the sink): OVF_ERR_RANGE");
            r        = keep;
            r.player = 8;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "path record player 8: OVF_ERR_RANGE");
        }
        {
            // a v1 header cannot carry a kind-9 section
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.version              = MHOV_VERSION_1;
            h.layout_fp            = layout_fp();
            h.features             = 0;
            put_hdr(b, h);
            for (unsigned s = 0, pos = sizeof(ovf_header); s < h.nsec; ++s) { // v1 sections carry kind_ver 0
                ovf_section sec = get_sec(b, pos);
                sec.kind_ver    = 0;
                memcpy(b.data() + pos, &sec, sizeof(sec));
                pos += sizeof(sec) + sec.nrec * (sizeof(ovf_record) + sec.elem);
            }
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "a VERSION-1 blob naming the path kind is refused: OVF_ERR_RANGE (the v1 reader knows nine kinds)");
            for (unsigned s = 0, pos = sizeof(ovf_header); s < h.nsec; ++s) {
                ovf_section sec = get_sec(b, pos);
                if (s == 0) sec.flags = 1;
                memcpy(b.data() + pos, &sec, sizeof(sec));
                pos += sizeof(sec) + sec.nrec * (sizeof(ovf_record) + sec.elem);
            }
            reseal(b);
            refuses(b, OVF_ERR_LAYOUT, "...and a v1 section with a flag set is OVF_ERR_LAYOUT (reserved must be 0)");
        }
        // the path pool NOT relocated: a required path section is refused, an OPTIONAL one skipped
        P::reset();
        {
            std::vector<uint8_t> b = blob;
            const int            c = check(b.data(), b.size());
            ck(c == OVF_ERR_NOT_RELOCATED && apply(b.data(), b.size()) == OVF_ERR_NOT_RELOCATED, "a build whose path pool is NOT relocated refuses a required path section: OVF_ERR_NOT_RELOCATED");
            ovf_section s = get_sec(blob, path_sec);
            s.flags       = SEC_OPTIONAL;
            put_sec(b, path_sec, s);
            ck(check(b.data(), b.size()) == OVF_OK, "...and skips it when it is OPTIONAL");
            ck(encode(buf.data(), cap, FOR_SAVE) != ENCODE_FAIL, "...(and encodes a save without a path section: the pool has no memory)");
        }
        ck(P::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pblk))), "...the path pool adopted again");
        ck(apply(blob.data(), blob.size()) == OVF_OK, "the unmutated v2 blob still applies after all that");
        zero_tails();
        zero_path_tails(pblk);
        memset(pblk, 0, PL::HOST_VSIZE + 64);
    }

    // ---- (7e) after_load + the save's free count + the trailer through a (fake) planet file --------------------------
    {
        set_raised_for_test(true);
        static int32_t cnt[8];
        P::set_stock_for_test(nullptr, nullptr, cnt);
        // two units of player 0: unit 1 holds slot 450 (word) with byte 0xfe; unit 2 holds slot 7 (byte 7, word 0); unit 3 holds a NOT busy slot 300 (word); unit 4 no path
        auto unit = [&](unsigned i) { return addr_of(MH_CAPS_IDX_UNITS, 0, i, 0); };
        auto mk   = [&](unsigned i, uint8_t byte, uint16_t word) {
            memset(unit(i), 0, 233);
            unit(i)[offsetof(mh::game::mh_map_object_unit, unit_proto_id)] = 1; // in use
            unit(i)[0xaa]                                                  = byte;
            memcpy(unit(i) + 0xda, &word, 2);
        };
        auto reset_units = [&]() {
            mk(1, 0xfe, 450);
            mk(2, 7, 0);
            mk(3, 0xfe, 300);
            mk(4, 0xff, 500);
            for (unsigned p = 0; p < 8; ++p) memset(path_flag(pblk, p, 0), 0, PL::ROW_SLOTS);
            *path_flag(pblk, 0, 450) = 1;
            *path_flag(pblk, 0, 7)   = 1;
            *path_flag(pblk, 0, 9)   = 1; // a busy slot nobody holds
        };
        auto word_of = [&](unsigned i) {
            uint16_t w;
            memcpy(&w, unit(i) + 0xda, 2);
            return w;
        };
        reset_units();
        P::after_load(true);
        ck(word_of(1) == 450 && unit(1)[0xaa] == 0xfe, "(7e) after_load(ids16): unit 1 keeps its busy slot 450, byte min(450, 0xfe) = 0xfe");
        ck(word_of(2) == 500 && unit(2)[0xaa] == 0xff, "...unit 2 (word 0, slot 0 not busy) loses its path: word 500, byte 0xff");
        ck(word_of(3) == 500 && unit(3)[0xaa] == 0xff, "...unit 3 (slot 300, flag clear) loses its path");
        ck(word_of(4) == 500 && unit(4)[0xaa] == 0xff, "...unit 4 (none) stays none");
        ck(cnt[0] == 500 - 3 && cnt[1] == 500, "...free counts are recomputed from the busy flags: 500 - busy (player 0: 3 busy)");
        reset_units();
        P::after_load(false);
        ck(word_of(1) == 500 && unit(1)[0xaa] == 0xff, "after_load(v1): unit 1's slot 450 is NOT trusted (byte 0xfe): dropped");
        ck(word_of(2) == 7 && unit(2)[0xaa] == 7, "...unit 2 is re-derived from its byte 7 (busy): word 7");
        ck(word_of(3) == 500 && word_of(4) == 500, "...the others are none");
        // the save's free count
        for (unsigned p = 0; p < 8; ++p) cnt[p] = 400 + (int)p;
        *path_flag(pblk, 0, 1)  = 1;
        *path_flag(pblk, 0, 2)  = 1;
        *path_flag(pblk, 0, 99) = 1;
        P::save_begin();
        ck(cnt[0] == 100 - 5 && cnt[1] == 100, "save_begin() writes a VANILLA-shaped count (100 - busy in slots 0..99): player 0 has 5 busy (1, 2, 7, 9, 99)");
        P::save_end();
        ck(cnt[0] == 400 && cnt[7] == 407, "save_end() puts the live counts back");
        P::save_end();
        ck(cnt[0] == 400, "...and is idempotent");
        set_raised_for_test(false);
        P::save_begin();
        ck(cnt[0] == 400, "save_begin() with the raise OFF touches nothing");
        P::set_stock_for_test(nullptr, nullptr, nullptr);
        memset(block, 0, BLOCKSZ);
        memset(pblk, 0, PL::HOST_VSIZE + 64);
    }
    {
        // a fake planet file: [vanilla body][v2 trailer][footer]; load_tail applies it and reports the feature
        set_raised_for_test(true);
        fill_tails_random(100);
        fill_path_random(pblk, 400);
        const std::vector<uint8_t> golden_arrays(block, block + BLOCKSZ);
        const std::vector<uint8_t> golden_pool(pblk, pblk + PL::HOST_VSIZE + 64);
        fake_file                  f;
        f.d.assign(1000, 0x5A);
        f.pos = 1000;
        std::string         log;
        static std::string *s_log;
        s_log = &log;
        set_save_logger([](const char *m) { s_log->append(m); });
        const int wrote = save_tail(FIO, &f, 3);
        ck(wrote > 0 && f.d.size() == 1000u + (size_t)wrote, "(7e) save_tail() appends a v2 trailer + footer to a planet file");
        ck(log.find("v2 path") != std::string::npos && log.find("trailer") != std::string::npos, "...and logs its size and the path record count");
        printf("      %s", log.c_str());
        zero_tails();
        zero_path_tails(pblk);
        f.pos        = 700; // mid-file: the position the load hook sits at
        int32_t fail = 0;
        ck(load_tail(FIO, &f, &fail) == LT_APPLIED && fail == 0 && f.pos == 700, "load_tail() applies it, restores the file position");
        ck(last_load_features() == FEAT_PATH_ID16, "...and reports FEAT_PATH_ID16");
        ck(memcmp(block, golden_arrays.data(), BLOCKSZ) == 0 && memcmp(pblk, golden_pool.data(), golden_pool.size()) == 0, "...with the arrays AND the whole pool restored");
        // a plain vanilla file clears the feature
        fake_file plain;
        plain.d.assign(500, 0x11);
        plain.pos = 100;
        ck(load_tail(FIO, &plain, &fail) == LT_NONE && last_load_features() == 0, "a file without a footer: LT_NONE and the features read 0 again");
        // the raise OFF refuses a v2 trailer with the raised-limits flag
        set_raised_for_test(false);
        f.pos = 700;
        fail  = 0;
        ck(load_tail(FIO, &f, &fail) == LT_REFUSED_OFF && fail == 1 && take_refused_raised() && last_load_features() == 0, "the raise OFF refuses the v2 trailer: LT_REFUSED_OFF, flag set, no features");
        // raise ON, a trailer a NEWER writer made (version 3): refused as BAD, no raised-limits flag
        set_raised_for_test(true);
        {
            fake_file      g = f;
            ovf_header     h;
            const uint32_t foot_len = [&] { uint32_t l; memcpy(&l, &g.d[g.d.size() - 8], 4); return l; }();
            const size_t   st       = g.d.size() - 8 - foot_len;
            memcpy(&h, &g.d[st], sizeof(h));
            h.version = 3;
            memcpy(&g.d[st], &h, sizeof(h));
            g.pos = 700;
            fail  = 0;
            ck(load_tail(FIO, &g, &fail) == LT_REFUSED_BAD && fail == 1 && !take_refused_raised(), "a trailer from a NEWER writer: LT_REFUSED_BAD, nothing applied, no raised-limits flag (the raise is ON)");
        }
        set_save_logger(nullptr);
        set_raised_for_test(false);
        scratch_release();
    }

    zero_tails();
    memset(pblk, 0, PL::HOST_VSIZE + 64);
    P::reset();
    reset_stock();
    free(pblk);
}

// ---- (8) PT-CAP9-U7c: the order arrays (queue / pending / staging) in the codec ------------------------------------------------------
// The order layer is adopted over a calloc'd `.mhcapo` stand-in and three calloc'd stock mirrors with their own count words (orders::set_stock_for_test),
// so the whole load protocol (scatter -> ovf::apply -> finish_scatter) runs on the host.
namespace {
struct order_fix {
    uint8_t *sect     = nullptr;
    uint8_t *stock[3] = {};
    int32_t  cnt[3]   = {};
};
std::string g_olog;
void        olog_sink(const char *m) { g_olog += m; }
void        fill_order(uint8_t *r, uint32_t seed) {
    for (unsigned b = 0; b < mh::caps::orders::REC_BYTES; ++b) r[b] = static_cast<uint8_t>((seed * 131u + b * 7u + 1u) | 1u);
}

} // namespace

void u7c_tests() {
    namespace O  = mh::caps::orders;
    namespace OL = mh::caps::orders_layout;
    namespace PL = mh::caps::path_layout;
    namespace P  = mh::caps::path;
    printf("  -- (8) PT-CAP9-U7c: the order arrays in the MHOV codec\n");
    uint8_t *block = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(g_base));
    memset(block, 0, BLOCKSZ);
    reset_stock();
    P::reset();
    O::reset_for_test();
    ck(adopt_relocation(g_base) && adopt_units_relocation(g_ubase), "(8) arrays + units adopted over calloc'd blocks");
    order_fix F;
    F.sect = static_cast<uint8_t *>(calloc(OL::SECTION_VSIZE + 64, 1));
    for (unsigned w = 0; w < 3; ++w) F.stock[w] = static_cast<uint8_t *>(calloc(OL::ARRAYS[w].stock_count + 4, O::REC_BYTES));
    ck(O::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(F.sect))), "...the order section adopted");
    for (unsigned w = 0; w < 3; ++w) O::set_stock_for_test(static_cast<O::which>(w), F.stock[w], &F.cnt[w]);
    O::set_on_for_test(true);
    O::set_log_sink(&olog_sink);
    set_raised_for_test(true);
    const size_t         bufsz = (max_bytes_world() > max_bytes_save() ? max_bytes_world() : max_bytes_save()) + 16;
    std::vector<uint8_t> buf(bufsz);
    const size_t         cap = bufsz - 16;
    const unsigned       REC = sizeof(ovf_record) + O::REC_BYTES;

    // ---- (8a) geometry and worst cases --------------------------------------------------------------------------
    ck(O::stock_cap(O::QUEUE) == 300 && O::wide_cap(O::QUEUE) == 4096 && O::stock_cap(O::PENDING) == 1000 && O::wide_cap(O::PENDING) == 32768 &&
           O::stock_cap(O::STAGING) == 300 && O::wide_cap(O::STAGING) == 2048,
       "(8a) the order arrays: 300/1000/300 stock, 4096/32768/2048 raised");
    const size_t qmax = sizeof(ovf_section) + 3796u * REC, pmax = sizeof(ovf_section) + 31768u * REC, smax = sizeof(ovf_section) + 1748u * REC;
    ck(max_bytes_world() == max_bytes() + qmax + pmax + smax, "max_bytes_world() = max_bytes() + the three order sections' worst case");
    ck(max_bytes_save() == max_bytes() + sizeof(ovf_section) + 8u * 401u * (sizeof(ovf_record) + 601u) + qmax, "max_bytes_save() = max_bytes() + the path section + the QUEUE section (pending/staging are not saved)");

    // ---- (8b) nothing past the windows (or the layer OFF): the blob is the v0.2.2 one, byte for byte -----------------
    fill_tails_random(100);
    const size_t         n0 = encode(buf.data(), cap, FOR_WORLD);
    std::vector<uint8_t> v1(buf.begin(), buf.begin() + (long)n0);
    ck(n0 != 0 && n0 != ENCODE_FAIL && hdr_of(v1).version == MHOV_VERSION_1, "(8b) a roster-only blob is version 1");
    F.cnt[O::QUEUE]   = 300;
    F.cnt[O::PENDING] = 1000;
    F.cnt[O::STAGING] = 300;
    const size_t n1   = encode(buf.data(), cap, FOR_WORLD);
    ck(n1 == n0 && memcmp(buf.data(), v1.data(), n0) == 0, "...counts AT the windows (300/1000/300) add nothing: still the version-1 blob");
    F.cnt[O::QUEUE] = 450;
    O::set_on_for_test(false);
    const size_t n2 = encode(buf.data(), cap, FOR_WORLD);
    ck(n2 == n0 && memcmp(buf.data(), v1.data(), n0) == 0, "...a queue count past 300 with the order layer OFF adds nothing either (the layer is not there to read it)");
    O::set_on_for_test(true);
    zero_tails();

    // ---- (8c) the writer: queue 450, pending 5000, staging 800 ---------------------------------------------------
    for (unsigned i = 0; i < 4096; ++i) fill_order(O::wide_record(O::QUEUE, i), 1000u + i);
    for (unsigned i = 0; i < 32768; ++i) fill_order(O::wide_record(O::PENDING, i), 70000u + i);
    for (unsigned i = 0; i < 2048; ++i) fill_order(O::wide_record(O::STAGING, i), 200000u + i);
    F.cnt[O::QUEUE]   = 450;
    F.cnt[O::PENDING] = 5000;
    F.cnt[O::STAGING] = 800;
    O::gather();
    ck(g_olog.find("queue count 450 is past the vanilla window") != std::string::npos, "(8c) gather() says a save now carries the queue tail in the trailer");
    const size_t         ns = encode(buf.data(), cap, FOR_SAVE);
    std::vector<uint8_t> sblob(buf.begin(), buf.begin() + (long)ns);
    ck(ns != 0 && ns != ENCODE_FAIL && check(sblob.data(), ns) == OVF_OK && hdr_of(sblob).version == MHOV_VERSION && hdr_of(sblob).layout_fp == 0, "...a SAVE blob validates, version 2");
    ck(hdr_of(sblob).nsec == 1 && hdr_of(sblob).records == 150u && hdr_of(sblob).features == 0u, "...with ONE section (the queue tail, 150 records), no path feature (the path pool is not relocated here)");
    ck(section_records(sblob.data(), ns, KIND_ORDER_QUEUE) == 150 && section_records(sblob.data(), ns, KIND_ORDER_PENDING) == 0 && section_records(sblob.data(), ns, KIND_ORDER_STAGING) == 0,
       "...kind 10 = 150, kinds 11 / 12 absent: PENDING and STAGING are world-only");
    ck(ns == sizeof(ovf_header) + sizeof(ovf_section) + 150u * REC, "...exactly one header + one section + 150 x (4 + 0x44) bytes");
    {
        ovf_section sec;
        memcpy(&sec, sblob.data() + sizeof(ovf_header), sizeof(sec));
        ck(sec.arr == KIND_ORDER_QUEUE && sec.players == 1 && sec.elem == 0x44 && sec.vanilla_row == 300 && sec.raised_row == 4096 && sec.nrec == 150 && sec.flags == 0 && sec.kind_ver == KIND_VER,
           "...the section names kind 10, 1 player, elem 0x44, rows 300..4096, kind_ver 1");
        bool ok = true;
        for (unsigned i = 0; i < 150; ++i) {
            ovf_record     r;
            const uint8_t *at = sblob.data() + sizeof(ovf_header) + sizeof(ovf_section) + i * REC;
            memcpy(&r, at, sizeof(r));
            ok = ok && r.player == 0 && r.pad == 0 && r.idx == 300u + i && memcmp(at + sizeof(r), O::wide_record(O::QUEUE, 300u + i), O::REC_BYTES) == 0;
        }
        ck(ok, "...records 300..449 in order, player 0, the wide queue's bytes");
    }
    const size_t         nw = encode(buf.data(), cap, FOR_WORLD);
    std::vector<uint8_t> wblob(buf.begin(), buf.begin() + (long)nw);
    ck(nw != 0 && nw != ENCODE_FAIL && check(wblob.data(), nw) == OVF_OK && hdr_of(wblob).nsec == 3 && hdr_of(wblob).records == 150u + 4000u + 500u, "...a WORLD blob carries all three: 150 + 4000 + 500 records");
    ck(section_records(wblob.data(), nw, KIND_ORDER_QUEUE) == 150 && section_records(wblob.data(), nw, KIND_ORDER_PENDING) == 4000 && section_records(wblob.data(), nw, KIND_ORDER_STAGING) == 500, "...(kind 10 / 11 / 12 counts)");
    ck(nw == sizeof(ovf_header) + 3u * sizeof(ovf_section) + 4650u * REC, "...and its size is exact");
    {
        fill_tails_random(100);
        const size_t         nr = encode(buf.data(), cap, FOR_WORLD);
        std::vector<uint8_t> mix(buf.begin(), buf.begin() + (long)nr);
        ck(nr != 0 && nr != ENCODE_FAIL && hdr_of(mix).version == MHOV_VERSION && hdr_of(mix).nsec >= 4 && check(mix.data(), nr) == OVF_OK, "...roster overflow + orders in one blob: version 2, validates");
        zero_tails();
    }
    // the path pool relocated: the same blob names the 16-bit path ids as valid
    {
        uint8_t *pblk = static_cast<uint8_t *>(calloc(PL::HOST_VSIZE + 64, 1));
        ck(P::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pblk))), "...(path pool adopted)");
        const size_t         np = encode(buf.data(), cap, FOR_SAVE);
        std::vector<uint8_t> pb(buf.begin(), buf.begin() + (long)np);
        ck(np == ns && hdr_of(pb).features == FEAT_PATH_ID16, "...with the path pool relocated the order-only v2 blob carries FEAT_PATH_ID16 (the ids in the file are the 16-bit ones)");
        P::reset();
        free(pblk);
    }

    // ---- (8d) the reader: the load protocol --------------------------------------------------------------------
    // the golden state holds only LIVE records: a record past its array's count is not carried (and not restored)
    for (unsigned i = 450; i < 4096; ++i) memset(O::wide_record(O::QUEUE, i), 0, O::REC_BYTES);
    for (unsigned i = 5000; i < 32768; ++i) memset(O::wide_record(O::PENDING, i), 0, O::REC_BYTES);
    for (unsigned i = 800; i < 2048; ++i) memset(O::wide_record(O::STAGING, i), 0, O::REC_BYTES);
    const std::vector<uint8_t> golden(F.sect, F.sect + OL::SECTION_VSIZE);
    const int32_t              c0 = F.cnt[0], c1 = F.cnt[1], c2 = F.cnt[2];
    {
        // what the vanilla blocks bring in: the first window of each array and the three counts -- then the load hook's scatter()
        O::gather();
        g_olog.clear();
        O::scatter();
        ck(F.cnt[0] == 300 && F.cnt[1] == 1000 && F.cnt[2] == 300, "(8d) scatter() clamps the counts to the stock windows");
        ck(memcmp(O::wide_record(O::QUEUE, 300), std::vector<uint8_t>(O::REC_BYTES, 0).data(), O::REC_BYTES) == 0 && memcmp(O::wide_record(O::QUEUE, 449), std::vector<uint8_t>(O::REC_BYTES, 0).data(), O::REC_BYTES) == 0,
           "...and zeroes the wide tails");
        ck(apply(wblob.data(), nw) == OVF_OK, "...the world blob applies");
        O::finish_scatter();
        ck(F.cnt[0] == c0 && F.cnt[1] == c1 && F.cnt[2] == c2, "...finish_scatter() RESTORES 450 / 5000 / 800: the tail came back whole");
        ck(memcmp(F.sect, golden.data(), golden.size()) == 0, "...and the whole order section is byte for byte what it was");
        ck(g_olog.find("queue count 450 restored (records 300..449") != std::string::npos && g_olog.find("pending count 5000 restored") != std::string::npos && g_olog.find("DROPPED") == std::string::npos,
           "...logged as restored, nothing dropped");
        O::finish_scatter();
        ck(F.cnt[0] == c0 && F.cnt[1] == c1, "...finish_scatter() is idempotent");
    }
    {
        // a queue record that is all zero INSIDE the tail: the section is contiguous, so the count still comes back
        std::vector<uint8_t> zr = sblob;
        // (rebuild from a world that has a zero record at 350)
        memset(O::wide_record(O::QUEUE, 350), 0, O::REC_BYTES);
        const size_t nz = encode(buf.data(), cap, FOR_SAVE);
        zr.assign(buf.begin(), buf.begin() + (long)nz);
        ck(nz == ns && section_records(zr.data(), nz, KIND_ORDER_QUEUE) == 150, "(8d) a ZERO record inside the tail is still written (the section is contiguous: the count is its extent)");
        O::gather();
        O::scatter();
        ck(apply(zr.data(), nz) == OVF_OK, "...applies");
        O::finish_scatter();
        ck(F.cnt[0] == 450 && F.cnt[1] == 1000 && memcmp(O::wide_record(O::QUEUE, 350), std::vector<uint8_t>(O::REC_BYTES, 0).data(), O::REC_BYTES) == 0, "...the queue count is restored to 450 with the zero record in place");
        // back to the golden state
        fill_order(O::wide_record(O::QUEUE, 350), 1000u + 350u);
    }
    {
        // the file says 450, the blob brings only 100 tail records: the count stays CLAMPED (a count over a hole would run stale orders)
        F.cnt[0]                = 400;
        F.cnt[1]                = 1000;
        F.cnt[2]                = 300;
        const size_t         n4 = encode(buf.data(), cap, FOR_SAVE);
        std::vector<uint8_t> short_blob(buf.begin(), buf.begin() + (long)n4);
        ck(section_records(short_blob.data(), n4, KIND_ORDER_QUEUE) == 100, "(8d) a queue of 400 writes 100 tail records");
        F.cnt[0] = 450;
        O::gather();
        g_olog.clear();
        O::scatter();
        ck(apply(short_blob.data(), n4) == OVF_OK, "...applies");
        O::finish_scatter();
        ck(F.cnt[0] == 300 && g_olog.find("did not come back (100 of 150 records)") != std::string::npos && g_olog.find("DROPPED") != std::string::npos,
           "...a count of 450 over 100 tail records stays CLAMPED to 300, and the drop is logged");
        // a REFUSED blob (bad sum) writes nothing: the clamp stands
        F.cnt[0] = 450;
        O::gather();
        g_olog.clear();
        O::scatter();
        std::vector<uint8_t> bad = sblob;
        bad[bad.size() - 1] ^= 0x55;
        ck(apply(bad.data(), bad.size()) == OVF_ERR_SUM, "...a corrupt blob is refused");
        O::finish_scatter();
        ck(F.cnt[0] == 300 && g_olog.find("(0 of 150 records)") != std::string::npos, "...and the count stays clamped, the drop logged");
        // no apply at all (the flow that never calls the codec)
        F.cnt[0] = 450;
        O::gather();
        O::scatter();
        g_olog.clear();
        O::scatter(); // a second scatter flushes the first one's unfinished clamp
        ck(g_olog.find("queue count 450") != std::string::npos && g_olog.find("DROPPED") != std::string::npos, "...a scatter nobody finished is flushed (logged) by the next scatter");
        O::finish_scatter();
        F.cnt[0] = c0;
        F.cnt[1] = c1;
        F.cnt[2] = c2;
        O::gather();
    }

    // ---- (8e) the reader's rules for the new kinds -----------------------------------------------------------------
    {
        zero_tails();
        auto sec_of = [&](const std::vector<uint8_t> &b) {
            ovf_section s;
            memcpy(&s, b.data() + sizeof(ovf_header), sizeof(s));
            return s;
        };
        auto put_sec0 = [&](std::vector<uint8_t> &b, const ovf_section &s) {
            memcpy(b.data() + sizeof(ovf_header), &s, sizeof(s));
            reseal(b);
        };
        auto refuses = [&](const std::vector<uint8_t> &b, int want, const char *what) {
            O::gather();
            O::scatter();
            const int cr = check(b.data(), b.size());
            const int ar = apply(b.data(), b.size());
            O::finish_scatter();
            ck(cr == want && ar == want, what);
            if (cr != want || ar != want) printf("    (check = %d, apply = %d, wanted %d)\n", cr, ar, want);
        };
        {
            std::vector<uint8_t> b = sblob;
            ovf_section          s = sec_of(b);
            s.elem                 = 0x45;
            put_sec0(b, s);
            refuses(b, OVF_ERR_ELEM, "(8e) an order section with another record size: OVF_ERR_ELEM");
            b            = sblob;
            s            = sec_of(b);
            s.raised_row = 8192;
            put_sec0(b, s);
            refuses(b, OVF_ERR_LAYOUT, "...another raised cap (a build with a different queue): OVF_ERR_LAYOUT");
            b         = sblob;
            s         = sec_of(b);
            s.players = 2;
            put_sec0(b, s);
            refuses(b, OVF_ERR_LAYOUT, "...two player rows: OVF_ERR_LAYOUT");
            b = sblob;
            ovf_record r;
            memcpy(&r, b.data() + sizeof(ovf_header) + sizeof(ovf_section), sizeof(r));
            r.idx = 299;
            memcpy(b.data() + sizeof(ovf_header) + sizeof(ovf_section), &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "...a record BELOW the stock window (idx 299): OVF_ERR_RANGE");
            b = sblob;
            memcpy(&r, b.data() + sizeof(ovf_header) + sizeof(ovf_section), sizeof(r));
            r.player = 1;
            memcpy(b.data() + sizeof(ovf_header) + sizeof(ovf_section), &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "...a record of player 1 (the queue has one row): OVF_ERR_RANGE");
            b = sblob;
            ovf_record r2;
            memcpy(&r, b.data() + sizeof(ovf_header) + sizeof(ovf_section), sizeof(r));
            memcpy(&r2, b.data() + sizeof(ovf_header) + sizeof(ovf_section) + REC, sizeof(r2));
            memcpy(b.data() + sizeof(ovf_header) + sizeof(ovf_section), &r2, sizeof(r2));
            memcpy(b.data() + sizeof(ovf_header) + sizeof(ovf_section) + REC, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RECORD_ORDER, "...records out of order: OVF_ERR_RECORD_ORDER");
            // an UNKNOWN kind (13) in place of the queue section
            b     = sblob;
            s     = sec_of(b);
            s.arr = 13;
            put_sec0(b, s);
            refuses(b, OVF_ERR_KIND, "an unknown kind (13) is refused: OVF_ERR_KIND (10-12 are known now)");
            // a v1 header cannot carry a kind 10
            b            = sblob;
            ovf_header h = hdr_of(b);
            h.version    = MHOV_VERSION_1;
            h.layout_fp  = layout_fp();
            h.features   = 0;
            put_hdr(b, h);
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "a version-1 blob cannot carry kind 10: OVF_ERR_RANGE");
        }
        // the order layer NOT on: a required order section is refused (a queue over a hole), an OPTIONAL one skipped
        O::set_on_for_test(false);
        {
            const int c = check(sblob.data(), sblob.size());
            ck(c == OVF_ERR_NOT_RELOCATED && apply(sblob.data(), sblob.size()) == OVF_ERR_NOT_RELOCATED, "a build whose order layer is NOT on refuses the required queue section: OVF_ERR_NOT_RELOCATED");
            std::vector<uint8_t> b = sblob;
            ovf_section          s = sec_of(b);
            s.flags                = SEC_OPTIONAL;
            put_sec0(b, s);
            ck(check(b.data(), b.size()) == OVF_OK, "...and skips it when it is OPTIONAL");
            ck(encode(buf.data(), cap, FOR_WORLD) == 0, "...(and with nothing else to write, encodes nothing)");
        }
        O::set_on_for_test(true);
    }

    // ---- (8f) the planet-file trailer: save_tail / load_tail / the stamp -------------------------------------------
    {
        for (unsigned i = 0; i < 450; ++i) fill_order(O::wide_record(O::QUEUE, i), 1000u + i);
        F.cnt[0] = 450;
        F.cnt[1] = 300;
        F.cnt[2] = 300;
        O::gather();
        fake_file f;
        f.d.assign(1000, 0x5A);
        f.pos = 1000;
        std::string         log;
        static std::string *s_log;
        s_log = &log;
        set_save_logger([](const char *m) { s_log->append(m); });
        const int wrote = save_tail(FIO, &f, 3);
        ck(wrote > 0 && f.d.size() == 1000u + (size_t)wrote, "(8f) save_tail() appends a trailer + footer for a queue past 300");
        ck(log.find("v2 path 0 orders 150") != std::string::npos, "...and logs the queue records it carries");
        printf("      %s", log.c_str());
        ck(file_has_footer(FIO, &f), "...the file ends in a valid footer (the stamp predicate sees it)");
        // the container stamp: some included member carries a footer
        planets pl;
        memset(&pl, 0, sizeof(pl));
        pl.sys[3]       = 7;
        pl.status[3]    = 1;
        pl.footer[3]    = file_has_footer(FIO, &f);
        member_probe mp = {&pl, &pl_sys, &pl_status, &pl_footer, 7, 3};
        ck(stamp_needed(mp), "...so the container is STAMPED (a retail / OFF build refuses it before reading any block)");
        // load
        f.pos = 700;
        O::gather();
        O::scatter();
        int32_t fail = 0;
        ck(load_tail(FIO, &f, &fail) == LT_APPLIED && fail == 0 && f.pos == 700, "load_tail() applies it and restores the position");
        O::finish_scatter();
        ck(F.cnt[0] == 450 && F.cnt[1] == 300 && F.cnt[2] == 300, "...the queue count is back to 450");
        // raise OFF refuses (the stamp's backstop)
        set_raised_for_test(false);
        f.pos = 700;
        fail  = 0;
        ck(load_tail(FIO, &f, &fail) == LT_REFUSED_OFF && fail == 1 && take_refused_raised(), "a build with the raise OFF refuses the file: LT_REFUSED_OFF, flag set");
        set_raised_for_test(true);
        // no queue past 300: no trailer at all (a v0.2.2-only save is a vanilla file)
        F.cnt[0] = 300;
        fake_file g;
        g.d.assign(500, 0x11);
        g.pos = 500;
        ck(save_tail(FIO, &g, 3) == 0 && g.d.size() == 500u, "a queue of 300 or fewer writes NO trailer: the file stays vanilla");
        F.cnt[0] = 450;
        set_save_logger(nullptr);
        scratch_release();
    }

    // ---- (8g) the worst case fits max_bytes_world() --------------------------------------------------------------
    {
        zero_tails();
        F.cnt[0] = 4096;
        F.cnt[1] = 32768;
        F.cnt[2] = 2048;
        std::vector<uint8_t> full(max_bytes_world() + 16);
        const size_t         nf = encode(full.data(), max_bytes_world(), FOR_WORLD);
        ck(nf == sizeof(ovf_header) + qmax + pmax + smax && nf <= max_bytes_world(), "(8g) full arrays: the WORLD blob is exactly header + the three worst-case sections");
        const int fcr = check(full.data(), nf);
        ck(fcr == OVF_OK && hdr_of(std::vector<uint8_t>(full.begin(), full.begin() + 32)).records == 3796u + 31768u + 1748u, "...and it validates");
        if (fcr != OVF_OK) printf("    (check = %d, records %u)\n", fcr, hdr_of(std::vector<uint8_t>(full.begin(), full.begin() + 32)).records);
        ck(encode(full.data(), nf - 1, FOR_WORLD) == ENCODE_FAIL, "...one byte short (a failed encode scribbles the buffer: it is checked AFTER the validation) is ENCODE_FAIL");
        // a count past the raised cap (never legal) is clamped, not overrun
        F.cnt[0]        = 99999;
        const size_t nc = encode(full.data(), max_bytes_world(), FOR_WORLD);
        ck(nc == nf, "...a count word past the raised cap is clamped to it (never reads past the array)");
        F.cnt[0] = -5;
        F.cnt[1] = 0;
        F.cnt[2] = 0;
        ck(encode(full.data(), max_bytes_world(), FOR_WORLD) == 0, "...a negative count writes nothing");
    }

    // teardown
    set_raised_for_test(false);
    O::set_log_sink(nullptr);
    O::reset_for_test();
    for (unsigned w = 0; w < 3; ++w) free(F.stock[w]);
    free(F.sect);
    zero_tails();
    reset_stock();
}

// ---- (9) PT-CAP9-U7d: the path pool and the order arrays join the overflow hash ---------------------------------------------------------
void u7d_tests() {
    namespace O  = mh::caps::orders;
    namespace OL = mh::caps::orders_layout;
    namespace PL = mh::caps::path_layout;
    namespace P  = mh::caps::path;
    printf("  -- (9) PT-CAP9-U7d: path pool + order arrays in the overflow hash\n");
    uint8_t *block = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(g_base));
    memset(block, 0, BLOCKSZ);
    reset_stock();
    P::reset();
    O::reset_for_test();
    uint8_t  *pblk = static_cast<uint8_t *>(calloc(PL::HOST_VSIZE + 64, 1));
    order_fix F;
    F.sect = static_cast<uint8_t *>(calloc(OL::SECTION_VSIZE + 64, 1));
    for (unsigned w = 0; w < 3; ++w) F.stock[w] = static_cast<uint8_t *>(calloc(OL::ARRAYS[w].stock_count + 4, O::REC_BYTES));
    ck(adopt_relocation(g_base) && adopt_units_relocation(g_ubase) && P::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pblk))) &&
           O::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(F.sect))),
       "(9) arrays + units + path pool + order section adopted over calloc'd blocks");
    for (unsigned w = 0; w < 3; ++w) O::set_stock_for_test(static_cast<O::which>(w), F.stock[w], &F.cnt[w]);
    O::set_on_for_test(true);
    set_raised_for_test(true);

    // the geometry, hand-computed
    ck(SLICES == 21 && ROSTER_SLICES == 11, "SLICES = 11 roster + 10 (4 path tails, flags, 4 path windows, the order queue tail)");
    bool lens = true, fits = true;
    for (int e = 11; e < 15; ++e) lens = lens && slice(e).len == 2u * 401u * 600u;
    lens = lens && slice(15).len == 8u * 501u;
    for (int e = 16; e < 20; ++e) lens = lens && slice(e).len == 2u * 100u * 600u;
    lens = lens && slice(20).len == 3796u * 0x44u;
    for (int e = 0; e < SLICES; ++e) fits = fits && slice(e).len <= MAX_SLICE_BYTES && slice(e).len == (uint32_t)(slice(e).p1 - slice(e).p0) * slice(e).seg_len;
    ck(lens, "...lengths: tails 481200, flags 4008, windows 120000, queue 258128 (pending and staging are not hashed: peer-local)");
    ck(fits, "...every slice fits the localiser's 512 KB");
    bool act = true;
    for (int e = 0; e < SLICES; ++e) act = act && active(e);
    ck(act, "...with the path pool relocated and the order layer ON every slice is active");

    const size_t ab    = tracker::arena_bytes();
    void        *arena = calloc(ab, 1);
    tracker      t;
    t.attach(arena);
    ck(t.prime(), "the tracker primes");
    uint64_t sm[SLICES], rc[SLICES];
    t.sums(sm);
    recompute_all(rc);
    ck(memcmp(sm, rc, sizeof(sm)) == 0 && t.verify() == 0, "...sums equal the from-scratch recompute");

    // a planted 1-byte divergence in each region moves exactly its own slice's sum
    struct plant {
        const char *what;
        uint8_t    *at;
        int         slice;
    };
    const plant planted[] = {
        {"queue record 350", O::wide_record(O::QUEUE, 350) + 5, 20},
        {"queue record 4095 (the last)", O::wide_record(O::QUEUE, 4095) + 67, 20},
        {"path slot 50 (player 2)", P::waypoint(2, 50, 3), 17},
        {"path slot 300 (player 5)", P::waypoint(5, 300, 7), 13},
        {"path sink slot 500 (player 7)", P::waypoint(7, 500, 0), 14},
        {"path busy flag (player 3, slot 400)", P::flags() + 3u * 501u + 400u, 15},
    };
    for (const plant &x : planted) {
        uint64_t before[SLICES], after[SLICES];
        t.update(nullptr);
        t.sums(before);
        *x.at ^= 0x21;
        t.update(nullptr);
        t.sums(after);
        bool only = true;
        for (int e = 0; e < SLICES; ++e) only = only && ((after[e] != before[e]) == (e == x.slice));
        char what[160];
        snprintf(what, sizeof(what), "a 1-byte change in %s moves slice %d's sum and no other", x.what, x.slice);
        ck(only, what);
        ck(t.verify() == 0, "...and the incremental sum still equals the recompute");
        char d[96];
        describe(x.slice, 0, d, sizeof(d));
        ck(d[0] != 0, "...describe() names the slice's bytes");
        *x.at ^= 0x21;
    }

    // the per-step cost with every slice active (info only): a clean scan of all slices, and with 20 changed bytes
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        const int REPS = 100;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < REPS; ++i) t.update(nullptr);
        QueryPerformanceCounter(&b);
        const double clean = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / REPS;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < REPS; ++i) {
            for (int k = 0; k < 20; ++k) *(O::wide_record(O::QUEUE, 300 + (unsigned)k) + 3) = (uint8_t)(i + k);
            t.update(nullptr);
        }
        QueryPerformanceCounter(&b);
        const double busy = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / REPS;
        printf("  (info) U7d update() of %lu bytes (%.2f MB/step; roster %lu): %.0f us/step unchanged, %.0f us/step with 20 changed bytes\n", (unsigned long)total_bytes(),
               (double)total_bytes() / 1048576.0, (unsigned long)roster_bytes(), clean, busy);
        ck(t.verify() == 0, "...and the sums still equal the recompute after the timing loop");
        for (int k = 0; k < 20; ++k) *(O::wide_record(O::QUEUE, 300 + (unsigned)k) + 3) = 0;
        t.update(nullptr);
    }

    // the fingerprint folds the slice set: a different active set reads as a different build
    const uint64_t fp_all = fp_on(0x1234);
    O::set_on_for_test(false);
    const uint64_t fp_noord = fp_on(0x1234);
    bool           inert    = true;
    for (int e = 20; e < SLICES; ++e) inert = inert && !active(e) && recompute(e) == 0;
    ck(!active(20) && active(15) && active(11), "...the order slice is inert, the path slices are not");
    ck(fp_all != fp_noord && inert, "fp_on() differs when the order layer is not ON (its slice goes inert, sum 0)");
    O::set_on_for_test(true);
    ck(fp_on(0x1234) == fp_all && fp_on(0x1234) != fp_on(0x1235), "...and is stable for one set and still depends on the manifest fingerprint");
    // the save round-trip digests do not see the new slices
    const uint64_t dg = digest();
    *P::waypoint(1, 70, 0) ^= 1;
    *O::wide_record(O::QUEUE, 350) ^= 1;
    ck(digest() == dg && digest_persisted() == digest_persisted(), "digest() covers the roster tails only (the new slices are hash-only)");
    *P::waypoint(1, 70, 0) ^= 1;
    *O::wide_record(O::QUEUE, 350) ^= 1;

    free(arena);
    O::set_on_for_test(false);
    O::reset_for_test();
    P::reset();
    free(pblk);
    free(F.sect);
    for (unsigned w = 0; w < 3; ++w) free(F.stock[w]);
    set_raised_for_test(false);
    reset_stock();
}

int run_ovftest() {
    printf("ovftest -- the overflow extension of the raised roster arrays\n");
    reset_stock();
    uint8_t *block = static_cast<uint8_t *>(calloc(BLOCKSZ, 1));
    g_base         = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
    g_ubase        = g_base + static_cast<uint32_t>(UOFF);

    // ---- (1) the slice table ------------------------------------------------------------------------------
    {
        bool     names = true, lens = true, fits = true, geo = true;
        uint32_t sum = 0;
        for (int e = 0; e < NV; ++e) {
            const slice_info &s = slice(e);
            names               = names && strcmp(s.name, W[e].name) == 0;
            lens                = lens && s.len == W[e].len;
            fits                = fits && s.len <= MAX_SLICE_BYTES;
            geo                 = geo && s.len == static_cast<uint32_t>(s.p1 - s.p0) * (s.rec1 - s.rec0) * s.elem && s.seg_len == (s.rec1 - s.rec0) * s.elem;
            sum += s.len;
        }
        ck(names, "(1) the eleven slices carry the spec names in order");
        ck(lens, "...and their lengths equal the HAND-COMPUTED players x (raised - vanilla) x elem");
        ck(sum == 2690052u && roster_bytes() == 2690052u && total_bytes() > roster_bytes(), "...2,690,052 bytes in all (1,851,652 v0.2.2 + 838,400 units/soldiers)");
        ck(fits, "...every slice fits the localiser's 512 KB (128 x 64 blocks)");
        ck(geo, "...seg_len / len are consistent with the rec window");
        ck(slice(0).p0 == 0 && slice(0).p1 == 4 && slice(1).p0 == 4 && slice(1).p1 == 8 && slice(7).p1 == 1 && slice(7).rec0 == 1500u && slice(7).rec1 == 4000u,
           "...buildings split p0-3 / p4-7; the projectile pool is ONE row [1500, 4000)");
        ck(slice(0).rec0 == 100u && slice(0).rec1 == 500u && slice(2).rec0 == 25u && slice(3).rec0 == 32u && slice(5).rec0 == 25u && slice(6).rec0 == 8u && slice(6).rec1 == 127u,
           "...windows [100,500) [25,127) [32,127) [32,127) [25,127) [8,127)");
        ck(!inc::has_mask(mh::state::HIDX_BUILDINGS) && !inc::has_mask(mh::state::HIDX_UNIT_STORAGE) && !inc::has_mask(mh::state::HIDX_TURRETS) &&
               !inc::has_mask(mh::state::HIDX_MINES) && !inc::has_mask(mh::state::HIDX_LABS) && !inc::has_mask(mh::state::HIDX_PRODUCTIONS) &&
               !inc::has_mask(mh::state::HIDX_PROJECTILE_POOL),
           "...none of the seven arrays has a VERDICT keep mask today (the extension hashes with keep = all 0xff)");
    }

    ck(adopt_relocation(g_base), "adopt_relocation(calloc block) accepted");
    ck(relocated() && !raised(), "...relocated, not raised");
    ck(adopt_units_relocation(g_ubase) && units_relocated(), "adopt_units_relocation(calloc block + 0x240000) accepted: all eleven slices are active");

    // ---- (1b) the segment -> stream mapping is a bijection ---------------------------------------------------
    {
        for (unsigned a = 0; a < WIDE_COUNT; ++a) {
            const geom &sk = stock(static_cast<arr>(a));
            for (uint32_t p = 0; p < sk.players; ++p)
                for (uint32_t r = sk.row; r < RAISED_ROW[a]; ++r)
                    for (uint32_t b = 0; b < sk.elem; ++b) *addr_of(a, p, r, b) = pat(a, p, r, b);
        }
        bool    all = true, seg = true;
        uint8_t buf[200];
        for (int e = 0; e < NV; ++e) {
            const slice_info &s = slice(e);
            for (uint32_t o = 0; o < s.len;) {
                const uint32_t n = std::min<uint32_t>(1u + rnd() % 190u, s.len - o);
                gather(e, o, n, buf);
                for (uint32_t i = 0; i < n; ++i) all = all && buf[i] == stream_byte(e, o + i);
                o += n;
            }
            for (int k = 0; k < s.p1 - s.p0; ++k)
                seg = seg && segment(e, k) == addr_of(s.arr, s.p0 + k, s.rec0, 0);
        }
        ck(all, "(1b) gather() over every slice, in random odd-sized pieces that straddle segments, reproduces the independent address formula byte for byte");
        ck(seg, "...and segment(e, k) is the first tail record of player p0 + k");
        char d[64];
        describe(0, 0, d, sizeof(d));
        ck(strcmp(d, "buildings p0 #100 +0x0") == 0, "describe: slice 0 offset 0 = buildings p0 #100 +0x0");
        describe(0, slice(0).seg_len + 37u * 273u + 0x12u, d, sizeof(d));
        ck(strcmp(d, "buildings p1 #137 +0x12") == 0, "describe: second segment, record 37 of the tail = buildings p1 #137 +0x12");
        describe(1, 3u * slice(1).seg_len + 399u * 273u + 272u, d, sizeof(d));
        ck(strcmp(d, "buildings p7 #499 +0x110") == 0, "describe: the very last byte of slice 1 = buildings p7 #499 +0x110");
        describe(7, 121u * 3u + 5u, d, sizeof(d));
        ck(strcmp(d, "projectiles p0 #1503 +0x5") == 0, "describe: projectiles p0 #1503 +0x5");
        describe(2, slice(2).len, d, sizeof(d));
        ck(d[0] == 0, "describe: an offset past the slice is empty");
        zero_tails();
    }

    // ---- (2) the incremental tracker ---------------------------------------------------------------------------
    {
        const size_t ab = tracker::arena_bytes();
        ck(ab > total_bytes() && ab < total_bytes() + total_bytes() / 4u, "(2) the arena is the shadows + block hashes (~9 MB since U7d: 7.64 MB of slices)");
        void   *arena = calloc(ab, 1);
        tracker t;
        t.attach(arena);
        ck(!t.primed(), "a fresh tracker is unprimed");
        fill_tails_random(150);
        ck(t.prime() && t.primed(), "prime() copies every segment into the shadow");
        uint64_t sm[SLICES], rc[SLICES];
        t.sums(sm);
        recompute_all(rc);
        ck(memcmp(sm, rc, sizeof(sm)) == 0 && t.verify() == 0, "...and its sums equal a from-scratch recompute");
        bool shadow_ok = true;
        for (int e = 0; e < NV; ++e)
            for (uint32_t o = 0; o < slice(e).len; o += 211) shadow_ok = shadow_ok && t.shadow(e)[o] == stream_byte(e, o);
        ck(shadow_ok, "...the shadow is the logical stream");

        // clean update: no change, no journal
        {
            rec_journal j;
            t.update(&j);
            ck(t.last_changed_blocks == 0 && j.e.empty(), "an update with nothing changed rehashes nothing and journals nothing");
        }
        // pokes inside the vanilla window are invisible
        {
            *addr_of(MH_CAPS_IDX_BUILDINGS, 0, 99, 5) ^= 0x55;
            *addr_of(MH_CAPS_IDX_PRODUCTIONS, 7, 7, 400) ^= 0x55;
            *addr_of(MH_CAPS_IDX_PROJECTILES, 0, 1499, 0) ^= 0x55;
            rec_journal j;
            t.update(&j);
            ck(t.last_changed_blocks == 0 && j.e.empty() && t.verify() == 0, "a poke INSIDE the vanilla window (record 99 / 7 / 1499) moves nothing in the extension");
            *addr_of(MH_CAPS_IDX_BUILDINGS, 0, 99, 5) ^= 0x55;
            *addr_of(MH_CAPS_IDX_PRODUCTIONS, 7, 7, 400) ^= 0x55;
            *addr_of(MH_CAPS_IDX_PROJECTILES, 0, 1499, 0) ^= 0x55;
            t.update(nullptr);
        }
        // random pokes, incl. both sides of every segment boundary
        bool eq = true;
        for (int round = 0; round < 60; ++round) {
            const int npoke = (round % 7 == 0) ? 0 : 1 + static_cast<int>(rnd() % 40u);
            for (int i = 0; i < npoke; ++i) {
                const int         e = static_cast<int>(rnd() % NV);
                const slice_info &s = slice(e);
                uint32_t          o = rnd() % s.len;
                if (i % 5 == 0 && s.p1 - s.p0 > 1) { // a boundary byte
                    const uint32_t k = 1u + rnd() % static_cast<uint32_t>(s.p1 - s.p0 - 1);
                    o                = k * s.seg_len - 1u + (rnd() & 1u);
                }
                put_stream(e, o, static_cast<uint8_t>(rnd()));
            }
            t.update(nullptr);
            t.sums(sm);
            recompute_all(rc);
            eq = eq && memcmp(sm, rc, sizeof(sm)) == 0;
        }
        ck(eq, "60 rounds of random pokes (boundary bytes included): the incremental sums equal the recompute after EVERY update");
        ck(t.verify() == 0, "...verify() agrees");
        {
            bool sh = true;
            for (int e = 0; e < NV; ++e)
                for (uint32_t o = 0; o < slice(e).len; o += 97) sh = sh && t.shadow(e)[o] == stream_byte(e, o);
            ck(sh, "...and the shadow still equals the live stream");
        }
        // a stale shadow is caught
        {
            const uint32_t o = slice(3).len / 2;
            put_stream(3, o, static_cast<uint8_t>(stream_byte(3, o) ^ 0xFF));
            int first = -2;
            ck(t.verify(&first) == 1 && first == 3, "verify() catches a live change the tracker has not seen (stale shadow), naming the slice");
            t.update(nullptr);
            ck(t.verify() == 0, "...and an update repairs it");
        }
        // a straddling block: change only the segment-0 side of a block that spans two segments
        {
            const slice_info &s = slice(0);
            const uint32_t    o = s.seg_len - 1u; // last byte of segment 0; its stream block also holds segment-1 bytes
            ck((s.seg_len % 64u) != 0, "(a 400 x 273 tail is not a multiple of 64: stream blocks straddle segments)");
            put_stream(0, o, static_cast<uint8_t>(stream_byte(0, o) ^ 0x3C));
            rec_journal j;
            t.update(&j);
            t.sums(sm);
            recompute_all(rc);
            ck(j.e.size() == 1 && j.e[0].region == (int)mh::state::HASH_REGION_COUNT + 0 && j.e[0].block == o / 64u && sm[0] == rc[0],
               "a byte at the end of segment 0 journals exactly ONE stream block (region = HASH_REGION_COUNT + e) and the sum follows");
            put_stream(0, o + 1u, static_cast<uint8_t>(stream_byte(0, o + 1u) ^ 0x3C)); // first byte of segment 1, SAME block
            rec_journal j2;
            t.update(&j2);
            t.sums(sm);
            recompute_all(rc);
            ck(j2.e.size() == 1 && j2.e[0].block == j.e[0].block && sm[0] == rc[0], "...and the segment-1 side of that block is the same block");
        }
        // the journal replays k steps back to the sums of step S-k
        {
            const int                         K = 4;
            std::vector<std::vector<uint8_t>> cur(NV);
            for (int e = 0; e < NV; ++e) cur[e].assign(t.shadow(e), t.shadow(e) + slice(e).len);
            std::vector<std::vector<uint8_t>> at_step[K + 1];
            uint64_t                          sums_at[K + 1][SLICES];
            rec_journal                       jr[K];
            for (int e = 0; e < NV; ++e) at_step[0].push_back(cur[e]);
            t.sums(sums_at[0]);
            for (int k = 0; k < K; ++k) {
                for (int i = 0; i < 25; ++i) {
                    const int e = static_cast<int>(rnd() % NV);
                    put_stream(e, rnd() % slice(e).len, static_cast<uint8_t>(rnd()));
                }
                t.update(&jr[k]);
                for (int e = 0; e < NV; ++e) at_step[k + 1].push_back(std::vector<uint8_t>(t.shadow(e), t.shadow(e) + slice(e).len));
                t.sums(sums_at[k + 1]);
            }
            // undo from the shadow: newest step first, entries newest first
            std::vector<std::vector<uint8_t>> rb(NV);
            for (int e = 0; e < NV; ++e) rb[e] = at_step[K][e];
            bool undo_ok = true, sums_ok = true;
            for (int k = K - 1; k >= 0; --k) {
                for (size_t i = jr[k].e.size(); i-- > 0;) {
                    const entry &d = jr[k].e[i];
                    memcpy(rb[d.region - (int)mh::state::HASH_REGION_COUNT].data() + d.block * 64u, d.bytes.data(), d.bytes.size());
                }
                for (int e = 0; e < NV; ++e) {
                    undo_ok = undo_ok && rb[e] == at_step[k][e];
                    sums_ok = sums_ok && sum_of(e, rb[e].data()) == sums_at[k][e];
                }
            }
            ck(undo_ok, "the journal replayed newest-first rebuilds the exact shadow of step S-k for k = 1..4");
            ck(sums_ok, "...and the block hashes of the rebuilt state are the sums recorded at S-k (the localiser's self-check)");
        }
        // The per-step cost (informational: the spec budget is 0.5 ms/step on the dev box; the rig COST line is the real number).
        {
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            const int REPS = 200;
            QueryPerformanceCounter(&a);
            for (int i = 0; i < REPS; ++i) t.update(nullptr);
            QueryPerformanceCounter(&b);
            const double clean = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / REPS;
            QueryPerformanceCounter(&a);
            for (int i = 0; i < REPS; ++i) {
                for (int k = 0; k < 20; ++k) put_stream(k % NV, (rnd() % slice(k % NV).len), (uint8_t)rnd());
                t.update(nullptr);
            }
            QueryPerformanceCounter(&b);
            const double busy = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / REPS;
            printf("  (info) update() of %lu bytes (roster slices only): %.0f us/step unchanged, %.0f us/step with 20 changed bytes\n", (unsigned long)roster_bytes(), clean, busy);
            ck(t.verify() == 0, "...and the sums still equal the recompute after the timing loop");
        }
        free(arena);
    }
    zero_tails();

    // ---- (3) the folds ----------------------------------------------------------------------------------------
    {
        uint64_t per[8], base = 0x0123456789abcdefull, ext[SLICES];
        for (int i = 0; i < 8; ++i) per[i] = 0x1111111111111111ull * (uint64_t)(i + 1);
        bool           excl[8] = {false, true, false, false, false, false, false, false};
        const uint64_t off     = mh::desync::fold_state(per, excl, 8);
        for (int e = 0; e < SLICES; ++e) ext[e] = 0x9e3779b97f4a7c15ull * (uint64_t)(e + 1);
        const uint64_t on = fold_ext(off, ext);
        uint64_t       h  = off;
        for (int e = 0; e < SLICES; ++e) h = mh::desync::fnv1a(&ext[e], 8, h);
        ck(on == h, "(3) fold_ext continues the OFF fold byte for byte (FNV-1a-64, little-endian sums, slice order)");
        ck(on != off, "...and moves it");
        bool each = true;
        for (int e = 0; e < SLICES; ++e) {
            uint64_t x[SLICES];
            memcpy(x, ext, sizeof(x));
            x[e] ^= 1;
            each = each && fold_ext(off, x) != on;
        }
        ck(each, "...every one of the sums (all SLICES of them) moves the ON hash");
        uint64_t zero[SLICES] = {};
        ck(fold_ext(off, zero) != off, "...even all-zero sums are folded (an ON peer never reads as an OFF one)");
        ck(fp_on(base) != base && fp_on(base) != mh::desync::fnv1a(&OVF_KIND, 4, base) && fp_on(base) == fp_on(base), "the ON fingerprint is the arm value with the extension kind and the slice set (U7d) folded in; ON != OFF");
        ck(fp_on(base) != fp_on(base ^ 1), "...and it still depends on the manifest fingerprint");
        // the block hash is the tracker's region id convention
        uint8_t blk[64];
        for (int i = 0; i < 64; ++i) blk[i] = (uint8_t)(i * 3);
        ck(block_hash(5, 7, blk, 64) == inc::block_hash(mh::state::HASH_REGION_COUNT + 5, 7, blk, 64, nullptr), "block_hash(e, ...) is inc::block_hash(HASH_REGION_COUNT + e, ..., keep = null)");
    }

    // ---- (4) the codec -------------------------------------------------------------------------------------------
    {
        const size_t         cap = max_bytes();
        std::vector<uint8_t> buf(cap + 16);
        ck(cap > 2700000u && cap < 3000000u, "(4) max_bytes() is the worst case (~2.8 MB)");
        ck(encode(buf.data(), cap) == 0, "NOTHING past the window encodes to 0 bytes (the caller appends nothing)");
        ck(overflow_records() == 0, "...(overflow_records() agrees)");
        size_t got = 100;
        ck(append_to_world_blob(buf.data(), &got, cap) && got == 100, "append_to_world_blob with the raise OFF is a successful no-op");

        // a zero record is not a record; a single non-zero byte is
        *addr_of(MH_CAPS_IDX_LABS, 3, 60, 4) = 1;
        {
            const size_t n = encode(buf.data(), cap);
            ck(n == sizeof(ovf_header) + sizeof(ovf_section) + sizeof(ovf_record) + 8u && record_count(buf.data(), n) == 1, "one non-zero byte = ONE record (header + one section + one 8-byte lab)");
        }
        zero_tails();

        fill_tails_random(220);
        // make sure the projectile row and several arrays carry records
        *addr_of(MH_CAPS_IDX_PROJECTILES, 0, 3999, 120) = 0x77;
        *addr_of(MH_CAPS_IDX_PROJECTILES, 0, 1500, 0)   = 0x78;
        *addr_of(MH_CAPS_IDX_BUILDINGS, 7, 499, 272)    = 0x79;
        *addr_of(MH_CAPS_IDX_BUILDINGS, 0, 100, 0)      = 0x7a;
        const uint32_t expect_records                   = overflow_records();
        const size_t   n                                = encode(buf.data(), cap);
        ck(n != 0 && n != ENCODE_FAIL && n <= cap, "a random sparse fill encodes");
        ck(check(buf.data(), n) == OVF_OK && record_count(buf.data(), n) == (int)expect_records, "...validates, and the header counts the same records overflow_records() does");
        ovf_header h0;
        memcpy(&h0, buf.data(), sizeof(h0));
        ck(h0.magic == MHOV_MAGIC && h0.version == MHOV_VERSION_1 && h0.nsec == 9 && h0.layout_fp == layout_fp() && h0.features == 0 && h0.body_len == n - sizeof(h0),
           "...with a sane header (9 sections, the layout fingerprint)");
        std::vector<uint8_t> golden(block, block + BLOCKSZ);
        ck(encode(buf.data(), n - 1) == ENCODE_FAIL, "a buffer one byte short is ENCODE_FAIL, never a truncated blob");
        std::vector<uint8_t> blob(buf.begin(), buf.begin() + (long)n);

        zero_tails();
        ck(apply(blob.data(), blob.size()) == OVF_OK, "apply() on zeroed tails");
        ck(memcmp(block, golden.data(), BLOCKSZ) == 0, "...restores the WHOLE wide block byte for byte (round trip is exact)");
        {
            std::vector<uint8_t> again(cap + 16);
            const size_t         n2 = encode(again.data(), cap);
            ck(n2 == n && memcmp(again.data(), blob.data(), n) == 0, "...and re-encoding gives the same blob (deterministic)");
        }

        // ---- refusals: each mutation reaches ITS code, and a refused blob writes nothing ----
        zero_tails();
        const uint32_t clean   = checksum_block(block);
        auto           refuses = [&](std::vector<uint8_t> &b, int want_rc, const char *what) {
            const int c = check(b.data(), b.size());
            const int a = apply(b.data(), b.size());
            ck(c == want_rc && a == want_rc && checksum_block(block) == clean, what);
            if (c != want_rc) printf("    (check = %d, apply = %d, wanted %d)\n", c, a, want_rc);
        };
        {
            std::vector<uint8_t> b = blob;
            b[0] ^= 1;
            refuses(b, OVF_ERR_MAGIC, "mutation: magic -> OVF_ERR_MAGIC, nothing written");
        }
        {
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.version              = 3;
            put_hdr(b, h);
            refuses(b, OVF_ERR_VERSION, "mutation: a NEWER version (3) -> OVF_ERR_VERSION");
            h.version = 0;
            put_hdr(b, h);
            refuses(b, OVF_ERR_VERSION, "mutation: version 0 -> OVF_ERR_VERSION");
        }
        {
            std::vector<uint8_t> b = blob;
            b[sizeof(ovf_header) + 30] ^= 0x10;
            refuses(b, OVF_ERR_SUM, "mutation: a body byte flipped -> OVF_ERR_SUM");
        }
        {
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.layout_fp ^= 1;
            put_hdr(b, h);
            refuses(b, OVF_ERR_LAYOUT, "mutation: layout fingerprint -> OVF_ERR_LAYOUT");
        }
        {
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.features             = 1;
            put_hdr(b, h);
            refuses(b, OVF_ERR_LAYOUT, "mutation: a v1 header with a feature bit -> OVF_ERR_LAYOUT");
        }
        {
            std::vector<uint8_t> b(blob.begin(), blob.end() - 1);
            refuses(b, OVF_ERR_TRUNCATED, "mutation: one byte cut off the end -> OVF_ERR_TRUNCATED");
            std::vector<uint8_t> c(blob.begin(), blob.begin() + 20);
            refuses(c, OVF_ERR_TRUNCATED, "mutation: shorter than a header -> OVF_ERR_TRUNCATED");
        }
        {
            std::vector<uint8_t> b = blob;
            b.push_back(0);
            refuses(b, OVF_ERR_TRAILING, "mutation: a byte after body_len -> OVF_ERR_TRAILING");
            std::vector<uint8_t> c = blob;
            ovf_header           h = hdr_of(c);
            h.body_len += 4;
            put_hdr(c, h);
            for (int i = 0; i < 4; ++i) c.push_back(0);
            reseal(c);
            refuses(c, OVF_ERR_TRAILING, "mutation: 4 spare bytes INSIDE body_len (resealed) -> OVF_ERR_TRAILING");
        }
        ck(check(nullptr, 10) == OVF_ERR_ARG, "check(null) -> OVF_ERR_ARG");
        {
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.body_len += 1;
            put_hdr(b, h);
            refuses(b, OVF_ERR_TRUNCATED, "mutation: body_len larger than the data -> OVF_ERR_TRUNCATED");
        }
        {
            std::vector<uint8_t> b = blob;
            ovf_header           h = hdr_of(b);
            h.records += 1;
            put_hdr(b, h);
            refuses(b, OVF_ERR_COUNT, "mutation: the header's record total -> OVF_ERR_COUNT");
        }
        const size_t s1 = section_at(blob, 1);
        {
            std::vector<uint8_t> b = blob;
            b[s1]                  = blob[sizeof(ovf_header)]; // second section claims the FIRST section's array: a duplicate
            reseal(b);
            refuses(b, OVF_ERR_SECTION_ORDER, "mutation: a duplicate section (resealed) -> OVF_ERR_SECTION_ORDER");
            std::vector<uint8_t> c = blob;
            c[s1]                  = 0; // arrays are 0,1,...: the second section now goes BACKWARDS
            reseal(c);
            refuses(c, OVF_ERR_SECTION_ORDER, "mutation: a section out of order (resealed) -> OVF_ERR_SECTION_ORDER");
        }
        {
            std::vector<uint8_t> b = blob;
            b[s1]                  = 9;
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "mutation: an unknown array id (resealed) -> OVF_ERR_RANGE");
        }
        {
            std::vector<uint8_t> b = blob;
            ovf_section          s;
            memcpy(&s, b.data() + sizeof(ovf_header), sizeof(s));
            s.elem += 1;
            memcpy(b.data() + sizeof(ovf_header), &s, sizeof(s));
            reseal(b);
            refuses(b, OVF_ERR_ELEM, "mutation: a section's elem size (resealed) -> OVF_ERR_ELEM");
            memcpy(&s, blob.data() + sizeof(ovf_header), sizeof(s));
            s.players += 1;
            memcpy(b.data() + sizeof(ovf_header), &s, sizeof(s));
            reseal(b);
            refuses(b, OVF_ERR_LAYOUT, "mutation: a section's player count (resealed) -> OVF_ERR_LAYOUT");
        }
        {
            const size_t         r0 = sizeof(ovf_header) + sizeof(ovf_section); // first record of the first section
            std::vector<uint8_t> b  = blob;
            ovf_record           r;
            memcpy(&r, b.data() + r0, sizeof(r));
            ovf_record keep = r;
            r.idx           = 99;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "mutation: idx = vanilla_row - 1 (resealed) -> OVF_ERR_RANGE");
            r     = keep;
            r.idx = 500;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "mutation: idx = raised_row (resealed) -> OVF_ERR_RANGE");
            r        = keep;
            r.player = 8;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "mutation: player = 8 (resealed) -> OVF_ERR_RANGE");
            r     = keep;
            r.pad = 1;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RANGE, "mutation: a non-zero pad (resealed) -> OVF_ERR_RANGE");
            // records out of order: give the FIRST record the idx of the SECOND one (a duplicate key), then a swap
            ovf_record   second;
            const size_t r1 = r0 + sizeof(ovf_record) + 273u;
            memcpy(&second, blob.data() + r1, sizeof(second));
            r        = keep;
            r.idx    = second.idx;
            r.player = second.player;
            memcpy(b.data() + r0, &r, sizeof(r));
            reseal(b);
            refuses(b, OVF_ERR_RECORD_ORDER, "mutation: two records with one key (resealed) -> OVF_ERR_RECORD_ORDER");
            r = keep;
            memcpy(b.data() + r0, &r, sizeof(r));
            memcpy(b.data() + r1, &keep, sizeof(keep)); // second record := the first's key: descending/duplicate
            reseal(b);
            refuses(b, OVF_ERR_RECORD_ORDER, "mutation: a record behind its successor's key (resealed) -> OVF_ERR_RECORD_ORDER");
        }
        // the clean blob still applies after all that
        ck(apply(blob.data(), blob.size()) == OVF_OK && memcmp(block, golden.data(), BLOCKSZ) == 0, "the unmutated blob still applies and restores the block exactly");

        {
            uint64_t h = FNV64_OFFSET;
            for (int e = 0; e < NV; ++e)
                for (uint32_t o = 0; o < slice(e).len; ++o) {
                    const uint8_t b = stream_byte(e, o);
                    h               = fnv64(&b, 1, h);
                }
            ck(digest() == h, "digest() is FNV-1a-64 over the eleven streams in slice order (the save round-trip witness)");
        }
        {
            // the world blob trailer: nav_offset + nav_len + fixup_len is where it starts
            std::vector<uint8_t> w(5000, 0);
            size_t               tl = 0;
            ck(world_trailer(w.data(), 796, 96, 500, 200, &tl) == nullptr, "world_trailer: a blob that ends at the fixup trailer carries NO overflow");
            const uint8_t *t = world_trailer(w.data(), 1001, 96, 500, 200, &tl);
            ck(t == w.data() + 796 && tl == 205, "...one byte more and the trailer starts at nav_offset + nav_len + fixup_len (796)");
            ck(world_trailer(w.data(), 5000, 0, 500, 200, &tl) == nullptr, "...nav_offset 0 (no trailers at all) never has one");
            ck(world_rc_refused(OVF_ERR_SUM) == -45 && world_rc_refused(OVF_ERR_MAGIC) == -42 && WORLD_ERR_CAPS_OVF == -40, "world_rc_refused maps ovf codes to -40 + code (-41 .. -54)");
        }
        // not relocated: nothing to write into, nothing to read
        reset_stock();
        ck(apply(blob.data(), blob.size()) == OVF_ERR_NOT_RELOCATED && encode(buf.data(), cap) == ENCODE_FAIL, "with the arrays NOT relocated apply() refuses and encode() fails");
        ck(!tracker().prime(), "...and a tracker cannot prime");
    }

    // ---- (5) the raised-caps save ----
    memset(block, 0, BLOCKSZ);
    ck(adopt_relocation(g_base) && adopt_units_relocation(g_ubase), "(5) re-adopted the calloc block for the save tests");
    save_tests();
    reset_stock();

    // ---- (6) PT-CAP9-U1: the v0.2.2 pair relocated, the units manifest REFUSED -> the units slices are inert ----
    {
        memset(block, 0, BLOCKSZ);
        ck(adopt_relocation(g_base) && !units_relocated(), "(6) only the v0.2.2 arrays relocated");
        bool inert = true;
        for (int e = 0; e < NV; ++e) {
            const bool units = e >= 8;
            inert            = inert && active(e) == !units && (units ? recompute(e) == 0 : true);
        }
        ck(inert, "...slices 8..10 (units, soldiers) are inactive, recompute() 0");
        *addr_of(MH_CAPS_IDX_UNITS, 1, 300, 5) = 0x42; // memory the build does not own in this layout: must NOT be encoded
        *addr_of(MH_CAPS_IDX_LABS, 3, 60, 4)   = 1;
        std::vector<uint8_t> buf(max_bytes() + 16);
        const size_t         n = encode(buf.data(), buf.size());
        ck(n != ENCODE_FAIL && n != 0 && check(buf.data(), n) == OVF_OK && record_count(buf.data(), n) == 1 && hdr_of(buf).nsec == 1,
           "...encode() skips the units arrays: only the lab record is written");
        // a blob that DOES name the units (a save from a units-raised run) is refused when units are not relocated
        reset_stock();
        ck(adopt_units_relocation(g_ubase) && adopt_relocation(g_base), "...both adopted again");
        std::vector<uint8_t> big(max_bytes() + 16);
        const size_t         m = encode(big.data(), big.size());
        ck(m != ENCODE_FAIL && m != 0 && hdr_of(big).nsec == 2, "...with the units relocated the same memory encodes as a second section");
        reset_stock();
        ck(adopt_relocation(g_base) && apply(big.data(), m) == OVF_ERR_NOT_RELOCATED, "...and that blob is REFUSED by a build whose units are not relocated (OVF_ERR_NOT_RELOCATED)");
        reset_stock();
    }

    u7_tests();
    u7c_tests();
    u7d_tests();

    free(block);
    reset_stock();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
